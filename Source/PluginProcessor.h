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
// Audio Processing Constants
// ============================================================================
namespace AudioConstants {
    constexpr double DefaultSampleRate = 44100.0;
    constexpr double StoredAudioSampleRate = TimeCoordinate::kRenderSampleRate;
    constexpr int64_t RenderLookaheadSeconds = 5;
    constexpr int RenderTimeoutMs = 30000;
    constexpr int RenderPollIntervalMs = 20;
}

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
        bool isPlaying{false};
        double timeSeconds{0.0};
        double bpm{120.0};
        double ppqPosition{0.0};
        bool loopEnabled{false};
        double loopPpqStart{0.0};
        double loopPpqEnd{0.0};
        bool isRecording{false};
        int timeSignatureNumerator{4};
        int timeSignatureDenominator{4};
    };

    // Audio-thread → message-thread log event dispatch.
    // Audio thread writes to logEventData_ then bumps logEventGeneration_;
    // message thread reads via consumeAudioThreadLogs().
    struct AudioThreadLogEvent {
        enum class Type : uint8_t { None, FadeOutComplete, CaptureDiag };
        Type type = Type::None;
        int diagNumChannels = 0;
        int diagNumSamples = 0;
        float diagMag = 0.0f;
        float diagS0 = 0.0f;
        float diagS1 = 0.0f;
        float diagS2 = 0.0f;
        float diagS3 = 0.0f;
        float diagS64 = 0.0f;
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
    static constexpr double getStoredAudioSampleRate() { return AudioConstants::StoredAudioSampleRate; }

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

    std::shared_ptr<std::atomic<double>> positionAtomic_{std::make_shared<std::atomic<double>>(0.0)};

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
     * 统一播放读取 API
     *
     * 二元播放模型：先读当前播放采样率 dry signal，再用当前播放采样率的
     * 已发布 render cache 覆盖同一段目标 buffer。
     */
    int readPlaybackAudio(const ::OpenTune::PlaybackReadRequest& request,
                          juce::AudioBuffer<float>& destination,
                          int destinationStartSample) const;
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



    // Transport control
    std::atomic<bool> isPlaying_{false};
    std::atomic<bool> loopEnabled_{false};
    double bpm_{120.0};  // Standalone 模式下的默认 BPM（插件模式下从主机同步）
    std::atomic<double> playStartPosition_{0.0};  // 播放起始位置（按下 Play 时的位置）

    // Fade-out state for smooth stop/pause
    std::atomic<bool> isFadingOut_{false};
    std::atomic<int> fadeOutSampleCount_{0};
    int fadeOutTotalSamples_{0};  // Set in prepareToPlay based on sample rate

    std::atomic<bool> hostTransportIsPlaying_{false};
    std::atomic<double> hostTransportTimeSeconds_{0.0};
    std::atomic<double> hostTransportBpm_{120.0};
    std::atomic<double> hostTransportPpqPosition_{0.0};
    std::atomic<bool> hostTransportLoopEnabled_{false};
    std::atomic<double> hostTransportLoopPpqStart_{0.0};
    std::atomic<double> hostTransportLoopPpqEnd_{0.0};
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
    // ⚡️ vocal-time-stretch §7 — Stage 2 worker progress query for UI feedback.


private:

public:
    // Clip Chunk 状态查询（替代原 RenderQueueStatus）

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

    // Transport control API
    void setPlaying(bool playing);
    bool isPlaying() const { return isPlaying_; }
    void setLoopEnabled(bool enabled);
    bool isLoopEnabled() const
    {
       #if !JucePlugin_Build_Standalone
        return getHostTransportSnapshot().loopEnabled;
       #else
        return loopEnabled_.load(std::memory_order_relaxed);
       #endif
    }
    void setPosition(double seconds);
    double getPosition() const;
    HostTransportSnapshot getHostTransportSnapshot() const;

    /// Consume audio-thread log events on message thread. Called from PluginEditor::timerCallback().
    void consumeAudioThreadLogs();
    
    double getPlayStartPosition() const { return playStartPosition_.load(); }
    void setPlayStartPosition(double seconds) { playStartPosition_.store(seconds); }
    
    std::shared_ptr<std::atomic<double>> getPositionAtomic() { return positionAtomic_; }

    void setBpm(double bpm);
    double getBpm() const
    {
       #if !JucePlugin_Build_Standalone
        return getHostTransportSnapshot().bpm;
       #else
        return bpm_;
       #endif
    }

    int getTimeSigNumerator() const
    {
       #if !JucePlugin_Build_Standalone
        return getHostTransportSnapshot().timeSignatureNumerator;
       #else
        return 4;
       #endif
    }

    int getTimeSigDenominator() const
    {
       #if !JucePlugin_Build_Standalone
        return getHostTransportSnapshot().timeSignatureDenominator;
       #else
        return 4;
       #endif
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

    // Audio-thread → message-thread log event dispatch (SPSC).
    // Audio thread writes logEventData_ and bumps logEventGeneration_;
    // message thread reads via consumeAudioThreadLogs(), using logEventReadGeneration_ to dedup.
    AudioThreadLogEvent              logEventData_;
    std::atomic<uint64_t>           logEventGeneration_{0};
    uint64_t                        logEventReadGeneration_{0}; // message-thread only

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
