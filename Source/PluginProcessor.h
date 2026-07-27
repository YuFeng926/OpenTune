#pragma once

/**
 * OpenTune 核心音频处理器
 * 
 * OpenTuneAudioProcessor 是 JUCE runtime 外壳，负责：
 * - 组合 SourceStore、content owner、StandaloneArrangement 与 VST3 ARA session
 * - 实时音频播放和混音（processBlock）
 * - AI 推理调度（通过独立的 F0InferenceService 与 VocoderRenderScheduler）
 * - 项目状态序列化/反序列化
 * 
 * 线程安全说明：
 * - source truth 由 SourceStore 管理，editable truth 由 content owner 管理
 * - Standalone placement/mix truth 由 StandaloneArrangement 管理
 * - 音频线程只读取 immutable playback snapshot 与 clip core 读取源
 */

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <atomic>
#include <cstdint>
#include <vector>
#include <array>
#include <map>
#include <mutex>
#include <optional>
#include "SourceStore.h"
#include "StandaloneArrangement.h"
#include "DSP/ResamplingManager.h"
#include "Utils/PitchCurve.h"
#include "DSP/ChromaKeyDetector.h"
#include "Inference/RenderCache.h"
#include "Inference/F0InferenceService.h"
#include "Services/F0ExtractionService.h"
#include "Services/ReferenceAnalysisService.h"
#include "Utils/ContentAnalysisState.h"
#include "Utils/SourceWindow.h"
#include "Utils/SilentGapDetector.h"
#include "Utils/TimeCoordinate.h"
#include "Utils/UndoManager.h"
#include "Utils/VocoderModelWeight.h"
#include "Utils/PianoKeyAudition.h"
#include "Inference/INoteGenerator.h"
#include "Utils/AppPreferences.h"
#include "Utils/PlacementClipboard.h"
#include "Utils/TrackConstants.h"
#include "Utils/PitchShiftSettings.h"
#include "Utils/PlaybackAudioReader.h"
#include "Content/ContentKey.h"
#include "Content/ContentEditCommands.h"
#include "Content/StandaloneContentRepository.h"
#include "Render/ContentRenderService.h"
#include "Runtime/ProcessF0Runtime.h"
#include "Runtime/ProcessRenderRuntime.h"
#include <functional>

namespace OpenTune {

// ============================================================================
// Placement Operation Outcome Structures
// ============================================================================

struct SplitOutcome {
    int      trackId{0};
    uint64_t sourceId{0};
    uint64_t originalPlacementId{0};
    ContentKey originalContentKey;
    uint64_t leadingPlacementId{0};
    uint64_t trailingPlacementId{0};
    ContentKey leadingContentKey;
    ContentKey trailingContentKey;
};

struct MergeOutcome {
    int      trackId{0};
    uint64_t sourceId{0};
    uint64_t leadingPlacementId{0};
    uint64_t trailingPlacementId{0};
    ContentKey leadingContentKey;
    ContentKey trailingContentKey;
    uint64_t mergedPlacementId{0};
    ContentKey mergedContentKey;
};

struct DeleteOutcome {
    int      trackId{0};
    uint64_t sourceId{0};
    uint64_t placementId{0};
    ContentKey contentKey;
};

// ============================================================================
// PlayHeadPresentationProjection — single-writer seqlock projection anchor
// ============================================================================
//
// Audio thread is the sole regular writer. UI thread reads snapshots.
// sequence=0 means no anchor published yet; odd means writer inside; even means valid.
//
// projectAt(nowClockSeconds) = min(horizon, anchorPosition + max(0, nowClock - anchorClock))
// The horizon is the end of the committed audio block; UI never paints beyond it.
//
// All anchor fields are std::atomic to avoid UB from concurrent reads under seqlock.
// Field accesses use memory_order_relaxed; ordering between field stores/loads
// and the seqlock sequence is provided by the two acquire loads.
struct PlayHeadPresentationProjection
{
    // seqlock sequence (even=valid snapshot, odd=writer active, 0=never published)
    std::atomic<uint64_t> sequence{0};

    std::atomic<double>   anchorPositionSeconds{0.0};
    std::atomic<double>   anchorClockSeconds{0.0};
    std::atomic<double>   horizonPositionSeconds{0.0};
    std::atomic<uint64_t> anchorEpoch{0};

    struct Snapshot
    {
        double anchorPosition = 0.0;
        double anchorClock    = 0.0;
        double horizon        = 0.0;
        uint64_t epoch        = 0;
        bool     valid        = false;

        double projectAt(double nowClockSeconds) const
        {
            if (!valid) return 0.0;
            const double elapsed = nowClockSeconds - anchorClock;
            if (elapsed <= 0.0) return anchorPosition;
            const double projected = anchorPosition + elapsed;
            return (projected > horizon) ? horizon : projected;
        }
    };

    // Seqlock reader with two acquire sequence loads. sequence==0 (never published)
    // returns invalid Snapshot immediately; odd sequence (writer inside) retries.
    Snapshot load() const
    {
        for (;;)
        {
            const uint64_t seq0 = sequence.load(std::memory_order_acquire);
            if (seq0 == 0)
                return {};
            if (seq0 & 1)
                continue;

            Snapshot snap;
            snap.anchorPosition = anchorPositionSeconds.load(std::memory_order_relaxed);
            snap.anchorClock    = anchorClockSeconds.load(std::memory_order_relaxed);
            snap.horizon        = horizonPositionSeconds.load(std::memory_order_relaxed);
            snap.epoch          = anchorEpoch.load(std::memory_order_relaxed);

            const uint64_t seq1 = sequence.load(std::memory_order_acquire);
            if (seq0 == seq1)
            {
                snap.valid = true;
                return snap;
            }
        }
    }

    // Single-writer publish: fetch_add acq_rel enters odd phase; fetch_add release
    // commits even phase and makes field stores visible to readers.
    void publish(double positionSec, double nowClockSec, double horizonSec, uint64_t epoch)
    {
        sequence.fetch_add(1, std::memory_order_acq_rel);  // enter write (odd)
        anchorPositionSeconds.store(positionSec,  std::memory_order_relaxed);
        anchorClockSeconds.store(nowClockSec,      std::memory_order_relaxed);
        horizonPositionSeconds.store(horizonSec,    std::memory_order_relaxed);
        anchorEpoch.store(epoch,                    std::memory_order_relaxed);
        sequence.fetch_add(1, std::memory_order_release);  // commit (even)
    }
};

// ============================================================================
// PlayHeadState — processor-owned canonical transport truth
// ============================================================================
//
// Each OpenTuneAudioProcessor owns exactly one PlayHeadState. It is updated
// only from the processor's own processBlock() by consuming the host
// AudioPlayHead::PositionInfo exactly once per block. UI binds to a const,
// non-owning reference and reads atomics directly. ARA transport requests
// are single-direction HostPlaybackController requests and never write back
// here. DocumentController does NOT own transport state.
//
// presentationEpoch is incremented on every discrete control change
// (play/stop/seek/reset); the audio thread publishes projection anchors
// tagged with the current epoch. UI projection is only valid when
// anchorEpoch == presentationEpoch; otherwise fall back to canonical time.
//
// isPlaying is the release/acquire publication point between writer
// (audio/control thread) and reader (UI thread). getPresentedPositionAt
// acquires isPlaying; when it sees false, timeInSeconds is guaranteed
// visible. When it sees true, the projection path handles ordering via
// the presentation epoch.
//
// update() contract (per docs/plans/2026-07-15-ara-playhead-official-state-hard-cut.md §3):
//  1. nullopt: no-op. Do not clear fields, do not fake stopped, do not bump revision.
//  2. valid PositionInfo but no timeInSeconds: keep last valid time, do not bump
//     revision; still write isPlaying/isLooping from this PositionInfo and loop
//     points if present.
//  3. valid PositionInfo with timeInSeconds: write time BEFORE isPlaying, bump
//     hostPositionRevision; write isLooping and loop points if present.
//
// reset() contract (only prepareToPlay/releaseResources call it):
//  - clear isPlaying/isLooping; keep last time/loop range; bump presentationEpoch.
//
// sampleCursor — standalone canonical integer cursor in device sample space.
// Audio thread is the sole canonical writer (Playing advance, fade completion,
// direct cursor commit for non-fading commands). Control thread publishes cursor
// intent via seqlock (pendingMainCursor_) but NEVER writes sampleCursor directly.
// The one exception is prepareToPlay rate-change projection, which runs on the
// audio thread under host serialisation.
// VST3/ARA host path never reads or writes this.
// UI timeInSeconds is derived from sampleCursor / device rate for Standalone.
struct PlayHeadState
{
    std::atomic<bool>    isPlaying { false };
    std::atomic<bool>    isLooping { false };
    std::atomic<double>  timeInSeconds { 0.0 };
    std::atomic<double>  loopPpqStart { 0.0 };
    std::atomic<double>  loopPpqEnd { 0.0 };
    std::atomic<uint64_t> hostPositionRevision { 0 };
    std::atomic<uint64_t> presentationEpoch { 0 };
    std::atomic<int64_t> sampleCursor { 0 };

    PlayHeadPresentationProjection presentationProjection;

    // ---- presented-position API (UI entry point) ----

    double getPresentedPositionAt(double nowClockSeconds) const
    {
        if (!isPlaying.load(std::memory_order_acquire))
            return timeInSeconds.load(std::memory_order_relaxed);

        const auto snap = presentationProjection.load();
        if (!snap.valid || snap.epoch != presentationEpoch.load(std::memory_order_acquire))
            return timeInSeconds.load(std::memory_order_relaxed);

        return snap.projectAt(nowClockSeconds);
    }

    double getPresentedPositionSeconds() const
    {
        return getPresentedPositionAt(juce::Time::getMillisecondCounterHiRes() * 0.001);
    }

    // ---- canonical-state update (audio thread only) ----

    void update(const juce::Optional<juce::AudioPlayHead::PositionInfo>& info)
    {
        if (!info.hasValue())
            return;

        const auto& positionInfo = *info;

        // Write time-in-seconds before isPlaying so a reader that acquires
        // isPlaying==false sees this block's canonical time.
        if (const auto timeSeconds = positionInfo.getTimeInSeconds())
        {
            timeInSeconds.store(*timeSeconds, std::memory_order_relaxed);
            hostPositionRevision.fetch_add(1, std::memory_order_acq_rel);
        }

        if (const auto loopPoints = positionInfo.getLoopPoints())
        {
            loopPpqStart.store(loopPoints->ppqStart, std::memory_order_relaxed);
            loopPpqEnd.store(loopPoints->ppqEnd, std::memory_order_relaxed);
        }

        isLooping.store(positionInfo.getIsLooping(), std::memory_order_relaxed);

        // isPlaying release publishes timeInSeconds/loop written above.
        // Pairs with getPresentedPositionAt's isPlaying acquire: when
        // the UI sees false, the canonical pause position is visible.
        isPlaying.store(positionInfo.getIsPlaying(), std::memory_order_release);
    }

    // reset() is only called from prepareToPlay/releaseResources on the audio thread.
    // Bumps presentationEpoch so that any stale projection anchor is invalidated.
    void reset()
    {
        isPlaying.store(false, std::memory_order_release);
        isLooping.store(false, std::memory_order_relaxed);
        presentationEpoch.fetch_add(1, std::memory_order_release);
    }
};

#if JucePlugin_Enable_ARA
class OpenTuneDocumentController;
#endif

namespace Capture {
    class CaptureSession;  // forward decl; full type in Source/Plugin/Capture/CaptureSession.h
}

void fillF0GapsForVocoder(std::vector<float>& f0,
                          const std::shared_ptr<const PitchCurveSnapshot>& snap,
                          double frameStartTimeSec,
                          double frameEndTimeSec,
                          double hopDuration,
                          double f0FrameRate,
                          bool allowTrailingExtension);

/**
 * OpenTuneAudioProcessor - 核心音频处理器类
 * 
 * 继承自 juce::AudioProcessor，实现 JUCE 音频插件接口。
 * 管理多轨道、Clip、音高曲线、渲染缓存等核心数据。
 */
class OpenTuneAudioProcessor : public juce::AudioProcessor,
                               public juce::AsyncUpdater,
                               private ReferenceAnalysisService::Listener
#if JucePlugin_Enable_ARA
                           , public juce::AudioProcessorARAExtension
#endif
{
public:
    struct HostTransportSnapshot {
        double bpm{120.0};
        double ppqPosition{0.0};
        bool isRecording{false};
        int timeSignatureNumerator{4};
        int timeSignatureDenominator{4};
    };

    struct ReferenceAlignmentResult {
        enum class Status : uint8_t {
            Succeeded = 0,
            TargetPlacementNotFound,
            NoReferenceBinding,
            ReferencePlacementNotFound,
            SelfReference,
            NoOverlap,
            TargetAnalysisNotReady,
            ReferenceAnalysisNotReady,
            InsufficientFeatures,
            InvalidTimeGrid,
            NoMutation,
            CommitFailed
        };

    Status status{Status::CommitFailed};
    juce::String message;
    ContentKey targetContentKey;
    int affectedStartFrame{0};
    int affectedEndFrame{0};

    bool succeeded() const noexcept { return status == Status::Succeeded; }
    };

    struct AutoRefAvailability {
        enum class Status : uint8_t {
            InvalidSelection = 0,
            NoReference,
            GameUnavailable,
            Ready
        };

        Status status{Status::InvalidSelection};
        juce::String message;
        uint64_t targetPlacementId{0};
        uint64_t referencePlacementId{0};

        bool hasReferenceBinding() const noexcept { return referencePlacementId != 0; }
        bool canRunAutoRef() const noexcept { return status == Status::Ready; }
    };

    enum class ReferenceAnalysisPreheatStatus : uint8_t {
        AlreadyReady = 0,
        Queued,
        InvalidContent,
        AnalysisFailed
    };

    static constexpr int MAX_TRACKS = MaxTracks;
public:
    OpenTuneAudioProcessor();
    ~OpenTuneAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock(juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    bool supportsDoublePrecisionProcessing() const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    void handleAsyncUpdate() override;

    double getSampleRate() const { return currentSampleRate_.load(std::memory_order_relaxed); }
    
    // 音频以固定 44.1kHz 存储，用于存储音频数据的采样-时间转换
    // 注意：此采样率用于音频数据存储，与设备采样率（currentSampleRate_）可能不同
    static constexpr double getStoredAudioSampleRate() { return TimeCoordinate::kRenderSampleRate; }

    // ============================================================================
    // Import API (Two-phase: prepare in worker thread, commit in main thread)
    // ============================================================================
    
    /**
     * PreparedImport - 导入预处理结果（在后台线程完成）
     */
    struct PreparedImport {
        juce::String displayName;
        juce::String sourceFilePath;  // 原始导入文件路径（空字符串表示无文件来源，如 ARA）
        juce::AudioBuffer<float> storedAudioBuffer;
        std::vector<SilentGap> silentGaps;
        SourceWindow sourceWindow;
    };

    struct ImportPlacement {
        int trackId{-1};
        double timelineStartSeconds{0.0};

        bool isValid() const noexcept
        {
            return trackId >= 0 && trackId < MAX_TRACKS && timelineStartSeconds >= 0.0;
        }
    };

    struct CommittedPlacement {
        uint64_t sourceId{0};
        ContentKey contentKey;
        uint64_t placementId{0};

        bool isValid() const noexcept
        {
            return sourceId != 0 && contentKey.isValid() && placementId != 0;
        }
    };

    struct ContentRefreshRequest {
        ContentKey contentKey;
        bool preserveCorrectionsOutsideChangedRange{false};
        double changedStartSeconds{0.0};
        double changedEndSeconds{0.0};
    };

    bool requestContentRefresh(const ContentRefreshRequest& request);

    bool prepareImport(juce::AudioBuffer<float>&& inBuffer,
                       double inSampleRate,
                       const juce::String& displayName,
                       const juce::String& sourceFilePath,
                       PreparedImport& out,
                       const char* entrySourceTag = "standalone-import");

    CommittedPlacement commitPreparedImportAsPlacement(PreparedImport&& prepared,
                                                       const ImportPlacement& placement,
                                                       uint64_t sourceId = 0);
    uint64_t commitPreparedImportAsContent(PreparedImport&& prepared,
                                                    uint64_t sourceId = 0);

    bool ensureSourceById(uint64_t sourceId,
                          const juce::String& displayName,
                          std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
                          double sampleRate);

    bool movePlacementToTrack(int sourceTrackId,
                              int targetTrackId,
                              uint64_t placementId,
                              double newTimelineStartSeconds);

    // Clipboard for arrangement clip copy/paste
    PlacementClipboard& getClipClipboard() { return clipClipboard_; }

    ContentKey cloneContent(ContentKey sourceContentKey,
                            const juce::String& newName = {});

    // Deep copy content audio data for paste/duplicate operations.
    // Creates a new content from a range of an existing one.
    ContentKey copyContentRange(ContentKey sourceContentKey,
                                double offsetSeconds,
                                double durationSeconds,
                                const juce::String& newName = {});

private:
    std::atomic<double> currentSampleRate_{44100.0};
    int currentBlockSize_ = 512;

    juce::AudioBuffer<float> doublePrecisionScratch_;
    juce::AudioBuffer<float> trackMixScratch_;
    juce::AudioBuffer<float> clipReadScratch_;

    juce::AudioParameterInt* editVersionParam_{nullptr};
    std::atomic<juce::int64> lastControlTimestamp_{0};
    std::atomic<int> lastControlType_{static_cast<int>(DiagnosticControlCall::None)};
    std::atomic<bool> noteGenReady_{false};
    std::atomic<bool> noteGenInitAttempted_{false};
    mutable std::mutex noteGenInitMutex_;

public:
    // ========================================================================
    // Playback Read API Types (Unified read path for Standalone/VST3)

    enum class DiagnosticControlCall : uint8_t {
        None = 0,
        Play,
        Pause,
        Stop,
        Seek
    };

    // Control-thread → audio-thread transport command. Control thread writes
    // the latest command + target cursor; audio thread consumes at block start.
    enum class TransportCommand : uint8_t {
        None = 0,
        Play,
        Pause,
        Stop,
        Seek,
        PauseAtPosition
    };

    // Audio-thread runtime phase. Only Stopped/Paused/Playing/Fading exist.
    // Fading is a transitional phase between Playing and Paused/Stopped.
    enum class RuntimePhase : uint8_t {
        Stopped = 0,
        Paused,
        Playing,
        Fading
    };

    struct DiagnosticInfo {
        int editVersion{0};
        ContentKey contentKey;
        uint64_t placementId{0};
        uint64_t publishedRevision{0};
        uint64_t desiredRevision{0};
        juce::String lastControlCall{"none"};
        juce::int64 lastControlTimestamp{0};
        RenderCache::ChunkStats chunkStats;
    };

    struct AnalysisAudioProvider {
        const float* samples = nullptr;
        int numSamples = 0;
        double sampleRate = 0.0;
        bool valid = false;
    };

    /**
     * 统一播放读取 API — 参见 Utils/PlaybackAudioReader.h 自由函数。
     */
    DiagnosticInfo getDiagnosticInfo(int trackId = 0, uint64_t placementId = 0) const;
    void recordControlCall(DiagnosticControlCall controlCall);

private:
    std::shared_ptr<SourceStore> sourceStore_;
    std::shared_ptr<ContentRenderService> contentRenderService_;
    std::unique_ptr<StandaloneContentRepository> standaloneContentRepository_;
    std::shared_ptr<ContentEditCommands> contentCommands_;
    std::unique_ptr<StandaloneArrangement> standaloneArrangement_;
    PlacementClipboard clipClipboard_;
    ReferenceAnalysisService referenceAnalysisService_;

    // Regular VST3 capture state. ARA-capable builds still create this for
    // unbound insert instances; access is suppressed after the instance binds to ARA.
    // nullptr in Standalone instances and in VST3 instances bound to ARA.
    std::unique_ptr<Capture::CaptureSession> captureSession_;



    // Transport control (Standalone-only helpers; canonical truth is playHeadState_)
    std::atomic<double> playStartPosition_{0.0};  // 播放起始位置（按下 Play 时的位置）

    // Standalone canonical BPM and time signature. These are the only owner-truth
    // for Standalone transport metadata. VST3/ARA reads host snapshot via
    // getHostTransportSnapshot(); the host atomics are write-once per processBlock
    // by updateHostTransportSnapshot() and never touched by any setter.
    double bpm_{120.0};
    int    timeSigNumerator_{4};
    int    timeSigDenominator_{4};

    // Processor-owned canonical transport truth. Updated only from this
    // processor's processBlock(); ARA/UI read it via getPlayHeadState().
    PlayHeadState playHeadState_;

    // Fade state for smooth pause/stop (audio-thread only, except fadeTotalSamples_ set in prepareToPlay)
    int fadeTotalSamples_{0};  // Absolute fade duration in device samples (0.2s * sampleRate)
    int fadeElapsed_{0};
    int64_t fadeReadCursor_{0};
    RuntimePhase fadeCompletionPhase_{RuntimePhase::Paused};
    int64_t fadeCompletionCursor_{0};
    bool fadeActive_{false};

    // Seqlock command dispatch — single control-thread writer.
    // controlSequence_: even = stable snapshot ready, odd = writer inside.
    // Each control API: fetch_add to odd → write all fields → fetch_add to even.
    // Audio thread reads snapshot at block start when seq is even && seq != applied.
    // No mutex. No CAS-clear. Latest complete snapshot always preserved.
    //
    // Four seqlock-protected fields carry control-thread intent:
    //   pendingCommand_       — the transport operation to apply
    //   pendingMainCursor_    — main freeze cursor (presented position at command time)
    //   pendingTargetCursor_  — completion target cursor (0 for Stop, mainCursor for Pause, target for PauseAtPosition/Seek)
    //   pendingIsPlaying_     — desired isPlaying after command (true for Play, preserved for Seek, false otherwise)
    // Control thread NEVER writes sampleCursor; audio thread is the sole canonical writer.
    std::atomic<uint64_t> controlSequence_{0};
    std::atomic<TransportCommand> pendingCommand_{TransportCommand::None};
    std::atomic<int64_t> pendingMainCursor_{0};
    std::atomic<int64_t> pendingTargetCursor_{0};
    std::atomic<bool> pendingIsPlaying_{false};
    uint64_t appliedControlSequence_{0};  // audio-thread only

    // Audio-thread runtime phase (Standalone only). Fading is transitional.
    RuntimePhase phase_{RuntimePhase::Stopped};

    // Independent host metadata snapshot (no loop fields; loop truth lives in
    // playHeadState_). BPM/PPQ/recording/time-signature presentation only.
    std::atomic<double> hostTransportBpm_{120.0};
    std::atomic<double> hostTransportPpqPosition_{0.0};
    std::atomic<bool> hostTransportIsRecording_{false};
    std::atomic<int> hostTransportTimeSignatureNumerator_{4};
    std::atomic<int> hostTransportTimeSignatureDenominator_{4};
    HostTransportSnapshot updateHostTransportSnapshot(const juce::AudioPlayHead::PositionInfo& positionInfo);

    std::shared_ptr<ResamplingManager> resamplingManager_;

    // Note generator (GAME-small by default; LegacyNoteGenerator
    // when env OPENTUNE_NOTE_BACKEND=legacy or models missing). Lazily
    // initialised by ensureNoteGeneratorReady().
    std::unique_ptr<INoteGenerator> noteGenerator_;
    std::mutex                      noteGeneratorInferenceMutex_; // serialise inference calls
    juce::ThreadPool                noteGeneratorPool_{1};         // single-threaded ORT-safe

    ExperimentalReferenceAlignMode experimentalReferenceAlignMode_ = ExperimentalReferenceAlignMode::Off;

    // Set of ContentKeys with a note-generation job pending or running
    // on noteGeneratorPool_. Editors poll `isNoteGenInFlightForContent`
    // to drive the shared "正在处理音频" overlay (covers F0 + note-gen).
    mutable std::mutex                  noteGenInFlightMutex_;
    std::unordered_set<ContentKey>      noteGenInFlightContentKeys_;

public:
    bool isNoteGenInFlightForContent(ContentKey contentKey) const;
private:
    F0ExtractionService f0ExtractionService_{1, 64};

    std::shared_ptr<std::atomic<bool>> contentRefreshAliveFlag_{std::make_shared<std::atomic<bool>>(true)};

    // UI state
    bool showWaveform_{true};
    
    bool showLanes_{true};
    double zoomLevel_{1.0};
    int trackHeight_{120};
    
    // 导出错误信息
    juce::String lastExportError_;

    bool ensureF0Ready();
    bool ensureNoteGeneratorReady();

    bool ensureServiceReady(std::atomic<bool>& readyFlag,
                            std::atomic<bool>& attemptedFlag,
                            std::mutex& initMutex,
                            const char* serviceName,
                            std::function<bool(const std::string&)> initFunc);

    void detectContentKeyIfUnset(ContentKey key);
    ContentKey ensureSourceAndCreateStandaloneClip(PreparedImport&& prepared, uint64_t& sourceId, bool& createdSource);
    void configureReferenceAnalysisService();

    ContentRenderService* resolveMutableLocalContentRenderService(ContentKey key) const noexcept;
    const ContentRenderService* resolveReadableContentRenderService(ContentKey key) const noexcept;
    AnalysisAudioProvider resolveAnalysisAudioProvider(ContentKey key);

    void analysisCompleted(ContentKey key,
                           const ReferenceFeatureSet& result) override;
    void analysisFailed(ContentKey key,
                        const juce::String& reason) override;

public:
    // Track State Management
    // Track height (shared state)
    void setTrackHeight(int height);
    int getTrackHeight() const { return trackHeight_; }

    void setShowWaveform(bool shouldShow) { showWaveform_ = shouldShow; }
    bool getShowWaveform() const { return showWaveform_; }
    void setShowLanes(bool shouldShow) { showLanes_ = shouldShow; }
    bool getShowLanes() const { return showLanes_; }

    /**
     * 重置推理后端（切换 GPU/CPU 时调用，UI 线程）
     * 停止 render worker → 释放推理服务 → 重新检测 → worker 惰性重启
     */
    void resetInferenceBackend(bool forceCpu);

    /** @brief 切换声码器模型权重。停worker→清cache→懒重建vocoder。调用方负责持久化偏好。 */
    void setVocoderModelWeight(VocoderModelWeight weight);

    bool isInferenceReady() const { return ProcessF0Runtime::getInstance().isReady(); }

    F0InferenceService* getF0Service() const { return ProcessF0Runtime::getInstance().getF0Service().get(); }
    VocoderDomain* getVocoderDomain() const { return ProcessRenderRuntime::getInstance().getVocoderDomain(); }
    SourceStore* getSourceStore() noexcept { return sourceStore_.get(); }
    const SourceStore* getSourceStore() const noexcept { return sourceStore_.get(); }
    ContentRenderService* getContentRenderService() noexcept { return contentRenderService_.get(); }
    const ContentRenderService* getContentRenderService() const noexcept { return contentRenderService_.get(); }
    RenderCache::ChunkStats getReadableContentChunkStats(ContentKey key) const noexcept;

    /** Returns the regular VST3 capture session, or nullptr outside regular VST3 mode. */
    Capture::CaptureSession* getCaptureSession() noexcept;
    const Capture::CaptureSession* getCaptureSession() const noexcept;
    StandaloneArrangement* getStandaloneArrangement() noexcept { return standaloneArrangement_.get(); }
    const StandaloneArrangement* getStandaloneArrangement() const noexcept { return standaloneArrangement_.get(); }
    StandaloneContentRepository* getStandaloneContentRepository() noexcept { return standaloneContentRepository_.get(); }
    const StandaloneContentRepository* getStandaloneContentRepository() const noexcept { return standaloneContentRepository_.get(); }

#if JucePlugin_Enable_ARA
    OpenTuneDocumentController* getDocumentController() const;
    void didBindToARA() noexcept override;
#endif

    // Content and placement access
    uint64_t getPlacementId(int trackId, int placementIndex) const;
    int findPlacementIndexById(int trackId, uint64_t placementId) const;
    bool getPlacementByIndex(int trackId, int placementIndex, StandaloneArrangement::Placement& out) const;
    bool getPlacementById(int trackId, uint64_t placementId, StandaloneArrangement::Placement& out) const;
    PitchShiftSettings getPitchShiftSettings(ContentKey key) const;
    ReferenceFeatureSet getReferenceFeatures(ContentKey key) const;

    // ⚡️ vocal-time-stretch §3.6 — TimeGrid accessors per content
    bool ensureTimeToolAnchorSeed(ContentKey key);
    AutoRefAvailability queryAutoRefAvailability(uint64_t targetPlacementId) const;

    /** AUTO(REF) 正式特征生产入口。产品合同固定使用 GAME producer。 */
    ReferenceFeatureSet buildReferenceFeatureSet(
        ContentKey key, const EditableContentSnapshot& snapshot);

    /** 设置当前实验性参考对齐模式。由 UI 首选项变更驱动。 */
    void setExperimentalReferenceAlignMode(ExperimentalReferenceAlignMode mode)
    {
        experimentalReferenceAlignMode_ = mode;
    }

private:
    ReferenceFeatureSet buildGameReferenceFeatureSet(
        ContentKey key, const EditableContentSnapshot& snapshot);
public:

    std::shared_ptr<ContentEditCommands> getContentCommands() const { return contentCommands_; }

    // ── Mutation notification scope ────────────────────────────────────────────
    enum class MutationScope : uint8_t {
        TimeGridChanged,      // setContentTimeGrid
        PitchShiftChanged,    // setContentPitchShiftSettings
        PitchCurveChanged,    // setContentPitchCurve
        NotesChanged,         // replaceContentNotesForFullMutation
    };

    // Hard-cut render mutation sinks: local edits carry their true affected
    // range; whole-content rebuilds carry only the reason. The two paths never
    // mix — local edits never widen into a full rebuild, full rebuilds never
    // pretend to know a precise range.
    void onContentLocalMutationCompleted(ContentKey key,
                                         MutationScope scope,
                                         ContentEditRangeFrames affectedRange);
    void onContentFullMutationCompleted(ContentKey key,
                                        MutationScope scope,
                                        FullRenderReason reason);

    // Full-content render request for import/restore/global operations.
    // Local edits must go through onContentLocalMutationCompleted instead.
    void requestFullContentRender(ContentKey key, FullRenderReason reason);

    void handleStage1ChunkPublished(ContentKey key, uint64_t publishedRevision);

    void refreshCRSMetadata(ContentKey key);

    // ── ContentKey-based mutation and snapshot APIs (Phase 4.3) ─────────────────
    std::shared_ptr<const EditableContentSnapshot> getContentSnapshot(ContentKey key) const;

    bool setContentReferenceFeatures(ContentKey key, const ReferenceFeatureSet& features);

    bool replaceContentNotesForFullMutation(ContentKey key, std::vector<Note> notes);
    ContentCommitSnapshot commitContentNotesAndSegments(ContentKey key,
                                        std::vector<Note> notes,
                                        std::vector<PitchCorrectionSegment> segments,
                                        ContentEditRangeFrames affectedRange);
    ContentCommitSnapshot commitContentNotePatch(ContentKey key, ContentNoteRangePatch patch);
    bool setContentPitchCurve(ContentKey key,
                              std::shared_ptr<PitchCurve> curve,
                              ContentEditRangeFrames affectedRange);
    bool setContentTimeGrid(ContentKey key,
                            std::shared_ptr<const TimeGridSnapshot> grid);
    bool setContentDetectedKey(ContentKey key, const DetectedKey& detectedKey);
    bool setContentOriginalF0State(ContentKey key, OriginalF0State state);
    bool setContentPitchShiftSettings(ContentKey key, const PitchShiftSettings& settings);
    bool commitAutoTuneGeneratedNotesByContentKey(ContentKey key,
                                                   std::vector<Note> generatedNotes,
                                                   int startFrame,
                                                   int endFrameExclusive,
                                                   float retuneSpeed,
                                                   float vibratoDepth,
                                                   float vibratoRate);
public:

#if defined(OPENTUNE_TEST_BUILD)
    void setReferenceAnalysisNotificationDispatcherForTests(
        ReferenceAnalysisService::NotificationDispatcher dispatcher)
    {
        referenceAnalysisService_.setNotificationDispatcher(std::move(dispatcher));
    }
#endif
    ReferenceAlignmentResult executeReferenceAlignmentForPlacement(uint64_t targetPlacementId);

    std::optional<SplitOutcome> splitPlacementAtSeconds(int trackId, int placementIndex, double splitSeconds);
    std::optional<MergeOutcome> mergePlacements(int trackId, uint64_t leadingPlacementId, uint64_t trailingPlacementId, int targetPlacementIndex);
    std::optional<DeleteOutcome> deletePlacement(int trackId, int placementIndex);
    void runReclaimSweepOnMessageThread();   // public for test synchronous invocation
    void scheduleReclaimSweep();

    // ── Internal: these APIs exist to serve remaining PluginProcessor.cpp callers
    // ── (import, split, merge, clone, state save/load). Not for new code.
    bool getSourceSnapshotById(uint64_t sourceId, SourceStore::SourceSnapshot& out) const;
    bool extractImportedClipOriginalF0(const EditableContentSnapshot& snap,
                                       F0ExtractionService::Result& out,
                                        std::string& errorMessage);
    ReferenceAnalysisPreheatStatus preheatReferenceAlignmentFeatures(ContentKey key);

    bool exportPlacementAudio(int trackId, int placementIndex, const juce::File& file);
    // 导出整个轨道的音频（时长以最晚Clip结束为准）
    bool exportTrackAudio(int trackId, const juce::File& file);
    // 导出总线混音（所有轨道）
    bool exportMasterMixAudio(const juce::File& file);
    
    // 导出错误信息
    juce::String getLastExportError() const { return lastExportError_; }

    // Rendering & Buffering

    // Transport control API — thin accessors over processor-owned PlayHeadState.
    // VST3/ARA uses getPlayHeadState() const reference for UI. Writes only via
    // Standalone setter methods (play/pause/stop/setPosition/setLoopEnabled).
    void play();
    void pause();
    void stop();
    void pauseAtPosition(double targetSeconds);
    void setPosition(double seconds);
    void setLoopEnabled(bool enabled);
    bool isPlaying() const noexcept { return playHeadState_.isPlaying.load(std::memory_order_relaxed); }
    bool isLoopEnabled() const noexcept { return playHeadState_.isLooping.load(std::memory_order_relaxed); }
    double getPosition() const { return playHeadState_.getPresentedPositionSeconds(); }
    HostTransportSnapshot getHostTransportSnapshot() const;

    /** Canonical processor-owned transport truth; UI binds a const non-owning reference. */
    const PlayHeadState& getPlayHeadState() const noexcept { return playHeadState_; }

    double getPlayStartPosition() const { return playStartPosition_.load(); }

    // Standalone canonical BPM setter. Validates 1..999 range and writes only
    // the processor-owned canonical bpm_; never touches host transport atomics.
    void setBpm(double bpm);

    // Standalone canonical time signature setter. Validates numerator 1..64
    // and denominator in {1,2,4,8,16,32,64}; writes processor-owned canonical
    // state only. Never touches host transport atomics.
    void setTimeSignature(int numerator, int denominator);

    // Shared code dispatches by runtime wrapperType, NOT by the
    // JucePlugin_Build_Standalone macro: the OpenTune_SharedCode target is
    // compiled with both JucePlugin_Build_Standalone=1 and
    // JucePlugin_Build_VST3=1 simultaneously, so a compile-time branch would
    // wrongly excise the VST3 host-snapshot read path. Only the VST3 runtime
    // wrapper reads host transport; Standalone uses the processor-owned local
    // canonical BPM and time signature.
    double getBpm() const
    {
        if (wrapperType == juce::AudioProcessor::wrapperType_VST3)
            return getHostTransportSnapshot().bpm;
        return bpm_;
    }

    int getTimeSigNumerator() const
    {
        if (wrapperType == juce::AudioProcessor::wrapperType_VST3)
            return getHostTransportSnapshot().timeSignatureNumerator;
        return timeSigNumerator_;
    }

    int getTimeSigDenominator() const
    {
        if (wrapperType == juce::AudioProcessor::wrapperType_VST3)
            return getHostTransportSnapshot().timeSignatureDenominator;
        return timeSigDenominator_;
    }

    void setZoomLevel(double zoom);
    double getZoomLevel() const { return zoomLevel_; }

    SnapSettings getSnapSettings() const;
    void setSnapSettings(const SnapSettings& snap);

    /** Wire AppPreferences pointer so getSnapSettings() returns live data. */
    void setAppPreferences(AppPreferences* prefs) { appPreferences_ = prefs; }

    UndoManager& getUndoManager() { return undoManager_; }
    PianoKeyAudition& getPianoKeyAudition() { return pianoKeyAudition_; }

private:
    UndoManager undoManager_;
    PianoKeyAudition pianoKeyAudition_;

    AppPreferences* appPreferences_{nullptr};

    // Hard-cut render mutation request primitive. This is the only path
    // that builds RenderJob and enqueues into ContentRenderService; every other
    // mutation entry point funnels through the two sinks above.
    void requestRenderForLocalMutationRange(ContentKey key, double startSeconds, double endSeconds);

    // Owner-truth-only write helpers. These update the underlying content stores without triggering
    // immediate render. They are designed for write-back mutation paths (Correction/Final F0) and are
    // paired with onContentFullMutationCompleted() to trigger batch rendering.
    bool writePitchCurveToOwner(ContentKey key, std::shared_ptr<PitchCurve> curve);
    // For OriginalF0 analysis data only — does NOT trigger audio rendering, only updates analysis state.
    bool writeOriginalF0ToOwner(ContentKey key, std::shared_ptr<PitchCurve> curve);

#if JucePlugin_Enable_ARA
    // Cached project state for pre-bind restore.
    // When setStateInformation arrives before didBindToARA, we cache the raw
    // block and replay it into the final shared stores after attach.
    juce::MemoryBlock pendingAraState_;

#endif

public:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneAudioProcessor)
};

} // namespace OpenTune
