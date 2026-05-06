#pragma once

/**
 * OpenTune 核心音频处理器
 * 
 * OpenTuneAudioProcessor 是 JUCE runtime 外壳，负责：
 * - 组合 SourceStore、MaterializationStore、StandaloneArrangement 与 VST3 ARA session
 * - 实时音频播放和混音（processBlock）
 * - AI 推理调度（通过独立的 F0InferenceService 与 VocoderRenderScheduler）
 * - 项目状态序列化/反序列化
 * 
 * 线程安全说明：
 * - source truth 由 SourceStore 管理，editable truth 由 MaterializationStore 管理
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
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <optional>
#include "SourceStore.h"
#include "MaterializationStore.h"
#include "StandaloneArrangement.h"
#include "DSP/ResamplingManager.h"
#include "Utils/PitchCurve.h"
#include "DSP/ChromaKeyDetector.h"
#include "Inference/RenderCache.h"
#include "Inference/F0InferenceService.h"
#include "Inference/GameInferenceService.h"
#include "Utils/InferenceGate.h"
#include "Inference/VocoderDomain.h"
#include "Services/F0ExtractionService.h"
#include "Utils/MaterializationState.h"
#include "Utils/SourceWindow.h"
#include "Utils/SilentGapDetector.h"
#include "Utils/TimeCoordinate.h"
#include "Utils/UndoManager.h"
#include "Utils/PianoKeyAudition.h"
#include <functional>

namespace OpenTune {

// ============================================================================
// Placement Operation Outcome Structures
// ============================================================================

struct SplitOutcome {
    int      trackId{0};
    uint64_t sourceId{0};
    uint64_t originalPlacementId{0};
    uint64_t originalMaterializationId{0};
    uint64_t leadingPlacementId{0};
    uint64_t trailingPlacementId{0};
    uint64_t leadingMaterializationId{0};
    uint64_t trailingMaterializationId{0};
};

struct MergeOutcome {
    int      trackId{0};
    uint64_t sourceId{0};
    uint64_t leadingPlacementId{0};
    uint64_t trailingPlacementId{0};
    uint64_t leadingMaterializationId{0};
    uint64_t trailingMaterializationId{0};
    uint64_t mergedPlacementId{0};
    uint64_t mergedMaterializationId{0};
};

struct DeleteOutcome {
    int      trackId{0};
    uint64_t sourceId{0};
    uint64_t placementId{0};
    uint64_t materializationId{0};
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
                               public juce::AsyncUpdater
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

    static constexpr int MAX_TRACKS = 12;
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

    void handleAsyncUpdate() override { runReclaimSweepOnMessageThread(); }

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
        uint64_t materializationId{0};
        uint64_t placementId{0};

        bool isValid() const noexcept
        {
            return sourceId != 0 && materializationId != 0 && placementId != 0;
        }
    };

    struct MaterializationRefreshRequest {
        uint64_t materializationId{};
        bool preserveCorrectionsOutsideChangedRange{false};
        double changedStartSeconds{0.0};
        double changedEndSeconds{0.0};
    };
    
    /**
     * 预处理导入内容（后台线程调用，不持有排列状态锁）
     * @param inBuffer 原始音频 buffer
     * @param inSampleRate 原始采样率
     * @param displayName 显示名称
     * @param out 输出预处理结果
     * @return 成功返回 true
     */
    bool prepareImport(juce::AudioBuffer<float>&& inBuffer,
                       double inSampleRate,
                       const juce::String& displayName,
                       PreparedImport& out,
                       const char* entrySourceTag = "standalone-import");
    
    /**
     * 提交预处理内容并创建 placement（主线程调用，写锁内只做轻量挂接）
     * @param prepared 预处理结果（移动语义）
     * @param placement 显式 placement 语义
     * @return 成功返回 source/materialization/placement 三层 id，失败返回空结果
     */
    CommittedPlacement commitPreparedImportAsPlacement(PreparedImport&& prepared,
                                                       const ImportPlacement& placement,
                                                       uint64_t sourceId = 0);
    uint64_t commitPreparedImportAsMaterialization(PreparedImport&& prepared,
                                                   uint64_t sourceId = 0);
    bool requestMaterializationRefresh(const MaterializationRefreshRequest& request);
    bool ensureSourceById(uint64_t sourceId,
                          const juce::String& displayName,
                          std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
                          double sampleRate);

    struct AraRegionMaterializationBirthResult {
        uint64_t sourceId{0};
        uint64_t materializationId{0};
        uint64_t materializationRevision{0};
        double materializationDurationSeconds{0.0};
    };

    std::optional<AraRegionMaterializationBirthResult> ensureAraRegionMaterialization(
        juce::ARAAudioSource* audioSource,
        uint64_t sourceId,
        std::shared_ptr<const juce::AudioBuffer<float>> copiedAudio,
        double copiedAudioSampleRate,
        const SourceWindow& sourceWindow,
        double playbackStartSeconds);

    bool movePlacementToTrack(int sourceTrackId,
                              int targetTrackId,
                              uint64_t placementId,
                              double newTimelineStartSeconds);

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
    std::atomic<bool> f0Ready_{false};
    std::atomic<bool> f0InitAttempted_{false};
    std::mutex f0InitMutex_;

    std::atomic<bool> vocoderReady_{false};
    std::atomic<bool> vocoderInitAttempted_{false};
    std::mutex vocoderInitMutex_;

public:
    // ========================================================================
    // Playback Read API Types (Unified read path for Standalone/VST3)
    // ========================================================================

    /**
     * PlaybackReadSource - 统一读取源描述
     *
     * 读取核只依赖当前播放采样率的 dry signal 与已发布 render cache，
     * 不依赖 clip 或 ARA source 的宿主容器。
     */
    struct PlaybackReadSource {
        std::shared_ptr<RenderCache> renderCache;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;

        bool hasAudio() const
        {
            return audioBuffer != nullptr && audioBuffer->getNumSamples() > 0;
        }

        bool canRead() const
        {
            return hasAudio();
        }
    };

    /**
     * PlaybackReadRequest - 统一读取请求结构
     *
     * readStartSeconds 必须与 source 中 renderCache / dry buffer 的时间基保持一致。
     */
    struct PlaybackReadRequest {
        PlaybackReadSource source;
        double readStartSeconds{0.0};
        double targetSampleRate{44100.0};
        int numSamples{0};

        PlaybackReadRequest() = default;
        PlaybackReadRequest(PlaybackReadSource src, double start, double rate, int samples)
            : source(src), readStartSeconds(start), targetSampleRate(rate), numSamples(samples) {}
    };

    enum class DiagnosticControlCall : uint8_t {
        None = 0,
        Play,
        Pause,
        Stop,
        Seek
    };

    struct DiagnosticInfo {
        int editVersion{0};
        uint64_t materializationId{0};
        uint64_t placementId{0};
        uint64_t publishedRevision{0};
        uint64_t desiredRevision{0};
        juce::String lastControlCall{"none"};
        juce::int64 lastControlTimestamp{0};
        RenderCache::ChunkStats chunkStats;
    };

    struct MaterializationSampleRange {
        int64_t startSample{0};
        int64_t endSampleExclusive{0};

        int64_t sampleCount() const
        {
            return endSampleExclusive - startSample;
        }

        bool isValid() const
        {
            return endSampleExclusive > startSample;
        }
    };

    struct FrozenRenderBoundaries {
        int64_t trueStartSample{0};
        int64_t trueEndSample{0};
        int64_t synthEndSample{0};
        int64_t publishSampleCount{0};
        int64_t synthSampleCount{0};
        int frameCount{0};
        int hopSize{0};
    };

    /**
     * 统一播放读取 API
     *
     * 二元播放模型：先读当前播放采样率 dry signal，再用当前播放采样率的
     * 已发布 render cache 覆盖同一段目标 buffer。
     */
    int readPlaybackAudio(const PlaybackReadRequest& request,
                          juce::AudioBuffer<float>& destination,
                          int destinationStartSample,
                          CrossoverMixer* mixer = nullptr) const;
    DiagnosticInfo getDiagnosticInfo(int trackId = 0, uint64_t placementId = 0) const;
    void recordControlCall(DiagnosticControlCall controlCall);

private:
    std::shared_ptr<SourceStore> sourceStore_;
    std::shared_ptr<MaterializationStore> materializationStore_;
    std::unique_ptr<StandaloneArrangement> standaloneArrangement_;

    // VST3 non-ARA capture session (runtime-isolated to wrapperType_VST3, see spec REQ 14).
    // nullptr in Standalone instances and in VST3 instances bound to ARA — both paths
    // continue to use the original processBlock fallback.
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

    mutable juce::SpinLock hostTransportSnapshotLock_;
    HostTransportSnapshot hostTransportSnapshot_;
    void updateHostTransportSnapshot(const juce::AudioPlayHead::PositionInfo& positionInfo);
    
    std::shared_ptr<Ort::Env> ortEnv_;
    std::shared_ptr<ResamplingManager> resamplingManager_;
    std::unique_ptr<F0InferenceService> f0Service_;
    std::unique_ptr<VocoderDomain> vocoderDomain_;
    std::unique_ptr<GameInferenceService> gameService_;
    InferenceGate inferenceGate_;
    F0ExtractionService materializationRefreshService_{1, 64};
    std::shared_ptr<std::atomic<bool>> materializationRefreshAliveFlag_{std::make_shared<std::atomic<bool>>(true)};

    // UI state
    bool showWaveform_{true};
    
    bool showLanes_{true};
    double zoomLevel_{1.0};
    int trackHeight_{120};
    
    // 导出错误信息
    juce::String lastExportError_;

    bool ensureF0Ready();
    bool ensureVocoderReady();
    bool ensureGameServiceReady();

    bool ensureServiceReady(std::atomic<bool>& readyFlag,
                            std::atomic<bool>& attemptedFlag,
                            std::mutex& initMutex,
                            const char* serviceName,
                            std::function<bool(const std::string&)> initFunc);

    uint64_t ensureSourceAndCreateMaterialization(PreparedImport&& prepared, uint64_t& sourceId, bool& createdSource);

    // ========================================================================
    // Chunk-Level Render Queue (重构：状态驱动，无任务快照队列)
    // ========================================================================

    void ensureChunkRenderWorkerStarted();
    void chunkRenderWorkerLoop();

    std::thread chunkRenderWorkerThread_;
    mutable std::mutex schedulerMutex_;
    std::condition_variable schedulerCv_;
    std::atomic<bool> chunkRenderWorkerRunning_{false};
    std::atomic<int> chunkRenderJobsInFlight_{0};

public:
    // Clip Chunk 状态查询（替代原 RenderQueueStatus）
    RenderCache::ChunkStats getMaterializationChunkStatsById(uint64_t materializationId) const;
    bool getMaterializationChunkBoundariesById(uint64_t materializationId, std::vector<double>& outSeconds) const;

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

    bool isInferenceReady() const { return f0Ready_.load(); }

    F0InferenceService* getF0Service() const { return f0Service_.get(); }
    VocoderDomain* getVocoderDomain() const { return vocoderDomain_.get(); }
    GameInferenceService* getGameService() const { return gameService_.get(); }
    InferenceGate& getInferenceGate() { return inferenceGate_; }
    SourceStore* getSourceStore() noexcept { return sourceStore_.get(); }
    const SourceStore* getSourceStore() const noexcept { return sourceStore_.get(); }
    MaterializationStore* getMaterializationStore() noexcept { return materializationStore_.get(); }
    const MaterializationStore* getMaterializationStore() const noexcept { return materializationStore_.get(); }

    /** Returns the VST3 non-ARA capture session, or nullptr in Standalone / pre-VST3 contexts. */
    Capture::CaptureSession* getCaptureSession() noexcept { return captureSession_.get(); }
    const Capture::CaptureSession* getCaptureSession() const noexcept { return captureSession_.get(); }
    StandaloneArrangement* getStandaloneArrangement() noexcept { return standaloneArrangement_.get(); }
    const StandaloneArrangement* getStandaloneArrangement() const noexcept { return standaloneArrangement_.get(); }

#if JucePlugin_Enable_ARA
    OpenTuneDocumentController* getDocumentController() const;
    void didBindToARA() noexcept override;
#endif

    static bool freezeRenderBoundaries(const MaterializationSampleRange& materializationRange,
                                       int64_t startSample,
                                       int64_t endSampleExclusive,
                                       int hopSize,
                                       FrozenRenderBoundaries& out);
    static bool preparePublishedAudioFromSynthesis(const FrozenRenderBoundaries& boundaries,
                                                   const std::vector<float>& synthesizedAudio,
                                                   std::vector<float>& publishedAudio);
    
    // Materialization and placement access
    std::shared_ptr<const juce::AudioBuffer<float>> getMaterializationAudioBufferById(uint64_t materializationId) const;
    bool getPlaybackReadSourceByMaterializationId(uint64_t materializationId, PlaybackReadSource& out) const;
    uint64_t getPlacementId(int trackId, int placementIndex) const;
    int findPlacementIndexById(int trackId, uint64_t placementId) const;
    bool getPlacementByIndex(int trackId, int placementIndex, StandaloneArrangement::Placement& out) const;
    bool getPlacementById(int trackId, uint64_t placementId, StandaloneArrangement::Placement& out) const;
    std::shared_ptr<PitchCurve> getMaterializationPitchCurveById(uint64_t materializationId) const;
    bool setMaterializationPitchCurveById(uint64_t materializationId, std::shared_ptr<PitchCurve> curve);
    OriginalF0State getMaterializationOriginalF0StateById(uint64_t materializationId) const;
    bool setMaterializationOriginalF0StateById(uint64_t materializationId, OriginalF0State state);
    DetectedKey getMaterializationDetectedKeyById(uint64_t materializationId) const;
    bool setMaterializationDetectedKeyById(uint64_t materializationId, const DetectedKey& key);

    std::shared_ptr<RenderCache> getMaterializationRenderCacheById(uint64_t materializationId) const;
    
    // Note and Anchor management per materialization
    using MaterializationNotesSnapshot = MaterializationStore::MaterializationNotesSnapshot;

    std::vector<Note> getMaterializationNotesById(uint64_t materializationId) const;
    MaterializationNotesSnapshot getMaterializationNotesSnapshotById(uint64_t materializationId) const;
    bool setMaterializationNotesById(uint64_t materializationId, const std::vector<Note>& notes);
    bool setMaterializationCorrectedSegmentsById(uint64_t materializationId, const std::vector<CorrectedSegment>& segments);
    bool commitMaterializationNotesAndSegmentsById(uint64_t materializationId,
                                          const std::vector<Note>& notes,
                                          const std::vector<CorrectedSegment>& segments);
    bool commitAutoTuneGeneratedNotesByMaterializationId(uint64_t materializationId,
                                                  const std::vector<Note>& generatedNotes,
                                                  int startFrame,
                                                  int endFrameExclusive,
                                                 float retuneSpeed,
                                                 float vibratoDepth,
                                                 float vibratoRate,
                                                 double audioSampleRate);
    
    std::optional<SplitOutcome> splitPlacementAtSeconds(int trackId, int placementIndex, double splitSeconds);
    std::optional<MergeOutcome> mergePlacements(int trackId, uint64_t leadingPlacementId, uint64_t trailingPlacementId, int targetPlacementIndex);
    std::optional<DeleteOutcome> deletePlacement(int trackId, int placementIndex);
    void runReclaimSweepOnMessageThread();   // public for test synchronous invocation
    void scheduleReclaimSweep();

    using MaterializationSnapshot = MaterializationStore::MaterializationSnapshot;
    bool getSourceSnapshotById(uint64_t sourceId, SourceStore::SourceSnapshot& out) const;

    bool extractImportedClipOriginalF0(const MaterializationSnapshot& snap,
                                       F0ExtractionService::Result& out,
                                       std::string& errorMessage);
    bool getMaterializationSnapshotById(uint64_t materializationId, MaterializationSnapshot& out) const;
    double getMaterializationAudioDurationById(uint64_t materializationId) const noexcept;
    bool replaceMaterializationAudioById(uint64_t materializationId,
                                 std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
                                 std::vector<SilentGap> silentGaps);
    uint64_t replaceMaterializationWithNewLineage(uint64_t oldId,
                                 MaterializationStore::CreateMaterializationRequest request);
    
    // 导出单个 placement 的音频
    bool exportPlacementAudio(int trackId, int placementIndex, const juce::File& file);
    // 导出整个轨道的音频（时长以最晚Clip结束为准）
    bool exportTrackAudio(int trackId, const juce::File& file);
    // 导出总线混音（所有轨道）
    bool exportMasterMixAudio(const juce::File& file);
    
    // 导出错误信息
    juce::String getLastExportError() const { return lastExportError_; }

    // Rendering & Buffering
    bool enqueueMaterializationPartialRenderById(uint64_t materializationId, double relStartSeconds, double relEndSeconds);

    // Transport control API
    void setPlaying(bool playing);
    void setPlayingStateOnly(bool playing) { isPlaying_.store(playing, std::memory_order_relaxed); }
    bool isPlaying() const { return isPlaying_; }
    void setLoopEnabled(bool enabled);
    bool isLoopEnabled() const { return loopEnabled_; }
    void setPosition(double seconds);
    double getPosition() const;
    HostTransportSnapshot getHostTransportSnapshot() const;
    
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

    UndoManager& getUndoManager() { return undoManager_; }
    PianoKeyAudition& getPianoKeyAudition() { return pianoKeyAudition_; }

private:
    UndoManager undoManager_;
    PianoKeyAudition pianoKeyAudition_;

public:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneAudioProcessor)
};

} // namespace OpenTune
