#pragma once

/**
 * OpenTune 核心音频处理器
 * 
 * OpenTuneAudioProcessor 是 JUCE runtime 外壳，每个最终 wrapper 各编译一份
 * （Standalone / VST3 单格式 SharedCode）：
 * - VST3 变体组合 VST3 ARA session 与 regular VST3 capture
 * - Standalone 变体组合 SourceStore、content owner 与 StandaloneArrangement
 * - 公共部分：实时音频播放/混音、AI 推理调度、项目状态序列化
 * 
 * 线程安全说明：
 * - editable truth 由 content owner 管理（Standalone 由 StandaloneContentRepository，
 *   VST3 由 ARA Session / CaptureSession 管理）
 * - Standalone placement/mix truth 由 StandaloneArrangement 管理
 * - 音频线程只读取 immutable playback snapshot 与 clip core 读取源
 */

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <atomic>
#include <cstdint>
#include <vector>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#if JucePlugin_Build_Standalone
#include "SourceStore.h"
#include "StandaloneArrangement.h"
#include "Utils/PlacementClipboard.h"
#include "Content/StandaloneContentRepository.h"
#endif
#include "DSP/ResamplingManager.h"
#include "DSP/OutputSpectrumAnalyzer.h"
#include "Utils/SpectrumDisplayData.h"
#include "Utils/PitchCurve.h"
#include "Utils/ContentTimelineProjection.h"
#include "Utils/DetectedKey.h"
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
#include "Utils/AppPreferences.h"
#include "Utils/PlayHeadState.h"
#include "Utils/TrackConstants.h"
#include "Utils/PitchShiftSettings.h"
#include "Utils/PlaybackAudioReader.h"
#include "Content/ContentKey.h"
#include "Content/ContentEditCommands.h"
#include "Render/ContentRenderService.h"
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

// PlayHeadPresentationProjection and PlayHeadState are defined in Utils/PlayHeadState.h

#if JucePlugin_Enable_ARA
class OpenTuneDocumentController;
#endif

// ============================================================================
// PluginPianoRollSessionState — runtime piano roll camera memory (message thread)
// ============================================================================
//
// Processor-owned, editor-visible session memory: the VST3 editor remembers the
// full piano roll camera per placement and restores it when the editor closes
// and reopens within one processor lifetime. Never serialized into host state,
// never locked — all access happens on the message thread.
//
// Placement identity = ContentKey + the four ContentTimelineProjection time
// fields, so different PlaybackRegions of the same AudioModification never share
// an absolute-timeline camera. Only neutral primitives live here — this header
// includes no PianoRoll UI types.

struct PianoRollPlacementIdentity
{
    ContentKey contentKey;
    ContentTimelineProjection projection;

    bool operator==(const PianoRollPlacementIdentity& rhs) const noexcept
    {
        return contentKey == rhs.contentKey
            && projection.timelineStartSeconds == rhs.projection.timelineStartSeconds
            && projection.timelineDurationSeconds == rhs.projection.timelineDurationSeconds
            && projection.contentStartSeconds == rhs.projection.contentStartSeconds
            && projection.contentDurationSeconds == rhs.projection.contentDurationSeconds;
    }
};

/** 完整镜头的四个 primitive：横向 camera 2 项 + 纵向缩放 + 纵向偏移。 */
struct PianoRollViewportPrimitive
{
    double cameraStartSeconds = 0.0;
    double cameraPixelsPerSecond = 100.0;
    float pixelsPerSemitone = 25.0f;
    float verticalScrollOffset = 0.0f;
};

struct PluginPianoRollSessionState
{
    std::vector<std::pair<PianoRollPlacementIdentity, PianoRollViewportPrimitive>> remembered;
};

struct PluginProcessorTransportTestAccessor;  // forward decl for test access to transport fields

#if JucePlugin_Build_VST3
namespace Capture {
    class CaptureSession;  // forward decl; full type in Source/Plugin/Capture/CaptureSession.h
}
#endif

/**
 * OpenTuneAudioProcessor - 核心音频处理器类
 * 
 * 继承自 juce::AudioProcessor，实现 JUCE 音频插件接口。
 * 管理多轨道、Clip、音高曲线、渲染缓存等核心数据。
 */
class OpenTuneAudioProcessor : public juce::AudioProcessor
#if JucePlugin_Build_Standalone
                             , public juce::AsyncUpdater
#else
                             , public juce::Timer
#endif
#if JucePlugin_Enable_ARA
                           , public juce::AudioProcessorARAExtension
#endif
{
public:
#if JucePlugin_Build_VST3
    struct HostTransportSnapshot {
        double bpm{120.0};
        double ppqPosition{0.0};
        bool isRecording{false};
        int timeSignatureNumerator{4};
        int timeSignatureDenominator{4};
    };
#endif

#if JucePlugin_Build_Standalone
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
            Ready
        };

        Status status{Status::InvalidSelection};
        juce::String message;
        uint64_t targetPlacementId{0};
        uint64_t referencePlacementId{0};

        bool hasReferenceBinding() const noexcept { return referencePlacementId != 0; }
        bool canRunAutoRef() const noexcept { return status == Status::Ready; }
    };
#endif

    enum class ReferenceAnalysisPreheatStatus : uint8_t {
        AlreadyReady = 0,
        Queued,
        WaitingForSource,
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

#if JucePlugin_Build_Standalone
    // Standalone arrangement reclaim sweep completes on the message thread.
    void handleAsyncUpdate() override;
#endif
#if JucePlugin_Build_VST3
    // Regular VST3 capture session tick.
    void timerCallback() override;
#endif

    double getSampleRate() const { return currentSampleRate_.load(std::memory_order_relaxed); }
    
    // 音频以固定 44.1kHz 存储，用于存储音频数据的采样-时间转换
    // 注意：此采样率用于音频数据存储，与设备采样率（currentSampleRate_）可能不同
    static constexpr double getStoredAudioSampleRate() { return TimeCoordinate::kRenderSampleRate; }

    // ============================================================================
    // Import API (Two-phase: prepare in worker thread, commit in main thread)
    // Standalone-only: VST3 content enters through CaptureSession / ARA.
    // ============================================================================
#if JucePlugin_Build_Standalone
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

    bool prepareImport(juce::AudioBuffer<float>&& inBuffer,
                       double inSampleRate,
                       const juce::String& displayName,
                       const juce::String& sourceFilePath,
                       PreparedImport& out,
                       const char* entrySourceTag = "standalone-import");

    CommittedPlacement commitPreparedImportAsPlacement(PreparedImport&& prepared,
                                                       const ImportPlacement& placement,
                                                       uint64_t sourceId = 0);

    bool movePlacementToTrack(int sourceTrackId,
                              int targetTrackId,
                              uint64_t placementId,
                              double newTimelineStartSeconds);

    // Clipboard for arrangement clip copy/paste
    PlacementClipboard& getClipClipboard() { return clipClipboard_; }

    // Deep copy content audio data for paste/duplicate operations.
    // Creates a new content from a range of an existing one.
    ContentKey cloneContent(ContentKey sourceContentKey,
                            const juce::String& newName = {});

    ContentKey copyContentRange(ContentKey sourceContentKey,
                                double offsetSeconds,
                                double durationSeconds,
                                const juce::String& newName = {});
#endif // JucePlugin_Build_Standalone

    struct ContentRefreshRequest {
        ContentKey contentKey;
        bool preserveCorrectionsOutsideChangedRange{false};
        double changedStartSeconds{0.0};
        double changedEndSeconds{0.0};
        // OpenDyne standalone import: when true, requestContentRefresh also runs
        // a one-shot whole-content note generation once F0 extraction completes.
        // 仅生成音符（不写修正曲线、不吸附），不请求 render。
        bool generateNotesWholeContentOnReady{false};
        NoteGeneratorParams noteGenerationParams;
        // 音符生成成功后的回调（message-thread，导入派生事务的 dirty 推进）。
        std::function<void()> onNotesGenerated;
    };

    bool requestContentRefresh(const ContentRefreshRequest& request);

private:
    static BusesProperties makeBuses();

    std::atomic<double> currentSampleRate_{44100.0};
    int currentBlockSize_ = 512;

    friend struct PluginProcessorTransportTestAccessor;

    juce::AudioBuffer<float> doublePrecisionScratch_;
    juce::AudioBuffer<float> trackMixScratch_;
    juce::AudioBuffer<float> clipReadScratch_;

    juce::AudioParameterInt* editVersionParam_{nullptr};
#if JucePlugin_Build_Standalone
    std::atomic<juce::int64> lastControlTimestamp_{0};
    std::atomic<int> lastControlType_{static_cast<int>(DiagnosticControlCall::None)};
#endif

public:
    // ========================================================================
    // Playback Read API Types (Unified read path for Standalone/VST3)

#if JucePlugin_Build_Standalone
    enum class DiagnosticControlCall : uint8_t {
        None = 0,
        Play,
        Pause,
        Stop,
        Seek
    };
#endif

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

    // Audio-thread runtime phase. Only Stopped/Paused/Playing exist.
    enum class RuntimePhase : uint8_t {
        Stopped = 0,
        Paused,
        Playing
    };

#if JucePlugin_Build_Standalone
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

    /**
     * 统一播放读取 API — 参见 Utils/PlaybackAudioReader.h 自由函数。
     */
    DiagnosticInfo getDiagnosticInfo(int trackId = 0, uint64_t placementId = 0) const;
    void recordControlCall(DiagnosticControlCall controlCall);
#endif

    struct AnalysisAudioProvider {
        const float* samples = nullptr;
        int numSamples = 0;
        double sampleRate = 0.0;
        bool valid = false;

        // Owns the source audio for the lifetime of samples.
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    };

private:
#if JucePlugin_Build_Standalone
    std::shared_ptr<SourceStore> sourceStore_;
#endif
    std::shared_ptr<ContentRenderService> contentRenderService_;
#if JucePlugin_Build_Standalone
    std::unique_ptr<StandaloneContentRepository> standaloneContentRepository_;
#endif
    std::shared_ptr<ContentEditCommands> contentCommands_;
#if JucePlugin_Build_Standalone
    std::unique_ptr<StandaloneArrangement> standaloneArrangement_;
    PlacementClipboard clipClipboard_;
#endif
    std::unique_ptr<ReferenceAnalysisService> referenceAnalysisService_;

#if JucePlugin_Build_VST3
    // Regular VST3 capture state. ARA-capable builds still create this for
    // unbound insert instances; access is suppressed after the instance binds to ARA.
    // nullptr in Standalone instances and in VST3 instances bound to ARA.
    std::unique_ptr<Capture::CaptureSession> captureSession_;
#endif



    // Transport control (Standalone-only helpers; canonical truth is playHeadState_)
    std::atomic<double> playStartPosition_{0.0};  // 播放起始位置（按下 Play 时的绝对秒位置）

#if JucePlugin_Build_Standalone
    // Standalone canonical BPM and time signature. These are the only owner-truth
    // for Standalone transport metadata. VST3/ARA reads host snapshot via
    // getHostTransportSnapshot().
    double bpm_{120.0};
    int    timeSigNumerator_{4};
    int    timeSigDenominator_{4};
#endif

    // Processor-owned canonical transport truth. Updated only from this
    // processor's processBlock(); ARA/UI read it via getPlayHeadState().
    PlayHeadState playHeadState_;

    // Plugin piano roll camera session memory (message thread only, no locks).
    PluginPianoRollSessionState pianoRollSession_;

    // ---- Transport ramp (audio-thread only writes) ----
    static constexpr double kTransportRampDurationSeconds = 0.2;
    int64_t audioReadCursor_{0};                                 // 下一段尚未生成的设备样本位置
    int64_t transportCursor_{0};                                 // UI 冻结/恢复/最终落点
    float   currentOutputGain_{0.0f};                            // 块首样本增益
    float   targetOutputGain_{0.0f};                             // ramp 目标增益（0 淡出，1 淡入）
    int     rampSamplesRemaining_{0};                            // ramp 剩余样本数
    int64_t transitionCompletionCursor_{0};                      // transition 完成落点
    RuntimePhase transitionCompletionPhase_{RuntimePhase::Stopped}; // transition 完成后的 phase
    bool    transitionActive_{false};                            // 是否处于 transition
    double  preparedPlaybackSampleRate_{0.0};                    // 首次 prepare 或真实采样率变化触发 CRS prepare

    // ---- Seqlock command dispatch — single control-thread writer ----
    // controlSequence_: even = stable snapshot ready, odd = writer inside.
    // Each control API: fetch_add to odd → write all fields → fetch_add to even.
    // Audio thread reads snapshot at block start when seq is even && seq != applied.
    // No mutex. No CAS-clear. Latest complete snapshot always preserved.
    //
    // Seqlock-protected fields carry control-thread intent in seconds:
    //   pendingCommand_            — the transport operation to apply
    //   pendingPresentationTime_   — UI freeze position (seconds)
    //   pendingCompletionTime_     — transition landing position (seconds)
    //   pendingTerminalPhase_      — target phase after transition completes
    std::atomic<uint64_t> controlSequence_{0};
    std::atomic<TransportCommand> pendingCommand_{TransportCommand::None};
    std::atomic<double> pendingPresentationTime_{0.0};
    std::atomic<double> pendingCompletionTime_{0.0};
    std::atomic<RuntimePhase> pendingTerminalPhase_{RuntimePhase::Stopped};
    uint64_t appliedControlSequence_{0};  // audio-thread only

    // Audio-thread runtime phase (Standalone only).
    RuntimePhase phase_{RuntimePhase::Stopped};

#if JucePlugin_Build_VST3
    // Independent host metadata snapshot (no loop fields; loop truth lives in
    // playHeadState_). BPM/PPQ/recording/time-signature presentation only.
    // Write-once per processBlock by updateHostTransportSnapshot(); never touched
    // by any setter.
    std::atomic<double> hostTransportBpm_{120.0};
    std::atomic<double> hostTransportPpqPosition_{0.0};
    std::atomic<bool> hostTransportIsRecording_{false};
    std::atomic<int> hostTransportTimeSignatureNumerator_{4};
    std::atomic<int> hostTransportTimeSignatureDenominator_{4};
    HostTransportSnapshot updateHostTransportSnapshot(const juce::AudioPlayHead::PositionInfo& positionInfo);
#endif

    std::shared_ptr<ResamplingManager> resamplingManager_;

    ExperimentalReferenceAlignMode experimentalReferenceAlignMode_ = ExperimentalReferenceAlignMode::StandardAuto;
    std::unordered_set<ContentKey> pendingTimeToolSeedKeys_; // message-thread only

    // 运行时惰性解析进程级 F0 服务，消除冷启动空快照
    // 仅在 initializeRuntimeState() 中构造，避免 scanner 在构造期间启动线程
    std::unique_ptr<F0ExtractionService> f0ExtractionService_;

    // Completion gate：F0 commit / Reference 分析 completion / 模型切换 completion
    // 回调（均捕获裸 this/processor）与析构互斥的唯一生命周期闸门。析构最先持锁置
    // closed=true：已进入的回调完成后才置位，此后进入者持锁见 closed 即返回，
    // 不再访问 owner。
    std::shared_ptr<ProcessRenderRuntime::CompletionGate> completionGate_{
        std::make_shared<ProcessRenderRuntime::CompletionGate>()};

    // UI state
    bool showWaveform_{true};
    // Discrete UI zoom percentage (75/90/100/110/125/150), per-processor-instance
    // single source of truth. Written via setUiZoomPercent (relaxed store); Editor
    // observes via relaxed load. Persisted in OTST v11 / OTSS v3.
    std::atomic<int> uiZoomPercent_{100};
    int trackHeight_{120};
    
#if JucePlugin_Build_Standalone
    // 导出错误信息
    juce::String lastExportError_;
#endif

    // 由宿主生命周期入口（prepareToPlay / createEditor / didBindToARA）调用，
    // 绝不从 processBlock 调用；并发由 call_once 协调。
    // 幂等；抛异常时 once_flag 复位，后续入口可重试。
    bool initializeRuntimeState() noexcept;
    // call_once 事务体：局部构造 → noexcept 发布成员。可抛。
    void initializeRuntimeStateOnce();
    std::atomic<bool> runtimeStateInitialized_{false};
    std::once_flag runtimeInitOnce_;

    // 状态恢复核心：解析并应用完整 raw payload。调用方保证 runtime 已初始化
    // （runtimeStateInitialized_ == true），因此不得触碰 initializeRuntimeState()。
    // 用于 setStateInformation 的立即恢复路径和 deferred replay 路径。
    bool restoreStatePayload(const void* data, int sizeInBytes);
    // 初始化成功后统一 replay 缓存的 deferred state（若有）。
    void replayDeferredState() noexcept;

#if JucePlugin_Build_Standalone
    ContentKey ensureSourceAndCreateStandaloneClip(PreparedImport&& prepared, uint64_t& sourceId, bool& createdSource);
#endif

    ContentRenderService* resolveMutableLocalContentRenderService(ContentKey key) const noexcept;
    const ContentRenderService* resolveReadableContentRenderService(ContentKey key) const noexcept;
    AnalysisAudioProvider resolveAnalysisAudioProvider(ContentKey key);

    void analysisFinished(ContentKey key,
                          const ReferenceFeatureSet& result);

public:
    // Track State Management
    // Track height (shared state)
    void setTrackHeight(int height);
    int getTrackHeight() const { return trackHeight_; }

    void setShowWaveform(bool shouldShow) { showWaveform_ = shouldShow; }
    bool getShowWaveform() const { return showWaveform_; }

    /**
     * 重置推理后端（切换 GPU/CPU 时调用，UI 线程）：
     * 暂停 render worker → 向进程寿命 control worker 投递命令（Session 销毁、
     * AccelerationDetector reset/detect、重建全部在其上执行）→ 立即返回。
     * 完成后经消息线程回调：先执行 beforeResume，再恢复 render worker。
     * gate 关闭后回调直接丢弃。
     */
    void resetInferenceBackend(bool forceCpu, std::function<void()> beforeResume = {});

    /**
     * 切换声码器模型权重（UI 线程）：暂停 render worker → 向进程寿命 control
     * worker 投递命令（严格先销毁旧 Session 再按当前配置重建）→ 立即返回。
     * 模型切换完成后才清 RenderCache/TimeStretchCache 并恢复 render worker。
     * gate 关闭后回调直接丢弃。调用方负责持久化偏好。
     */
    void setVocoderModelWeight(const VocoderModelWeight& weight);

    bool setF0ModelType(F0ModelType type);

    /** 清除缓存并重新渲染所有 clip（UI 线程调用）。 */
    void invalidateAllContentCaches();

    bool isVocoderReady() const { return ProcessRenderRuntime::getInstance().isVocoderReady(); }
#if JucePlugin_Build_Standalone
    SourceStore* getSourceStore() noexcept { return sourceStore_.get(); }
    const SourceStore* getSourceStore() const noexcept { return sourceStore_.get(); }
#endif
    ContentRenderService* getContentRenderService() noexcept { return contentRenderService_.get(); }
    const ContentRenderService* getContentRenderService() const noexcept { return contentRenderService_.get(); }
    RenderCache::ChunkStats getReadableContentChunkStats(ContentKey key) const noexcept;

#if JucePlugin_Build_VST3
    /** Returns the regular VST3 capture session, or nullptr outside regular VST3 mode. */
    Capture::CaptureSession* getCaptureSession() noexcept;
    const Capture::CaptureSession* getCaptureSession() const noexcept;
#endif
#if JucePlugin_Build_Standalone
    StandaloneArrangement* getStandaloneArrangement() noexcept { return standaloneArrangement_.get(); }
    const StandaloneArrangement* getStandaloneArrangement() const noexcept { return standaloneArrangement_.get(); }
    StandaloneContentRepository* getStandaloneContentRepository() noexcept { return standaloneContentRepository_.get(); }
    const StandaloneContentRepository* getStandaloneContentRepository() const noexcept { return standaloneContentRepository_.get(); }
#endif

#if JucePlugin_Enable_ARA
    OpenTuneDocumentController* getDocumentController() const;
    void didBindToARA() noexcept override;
#endif

#if JucePlugin_Build_Standalone
    // Content and placement access (Standalone arrangement truth)
    uint64_t getPlacementId(int trackId, int placementIndex) const;
    int findPlacementIndexById(int trackId, uint64_t placementId) const;
    bool getPlacementByIndex(int trackId, int placementIndex, StandaloneArrangement::Placement& out) const;
    bool getPlacementById(int trackId, uint64_t placementId, StandaloneArrangement::Placement& out) const;
#endif
    PitchShiftSettings getPitchShiftSettings(ContentKey key) const;
    ReferenceFeatureSet getReferenceFeatures(ContentKey key) const;

    // ========================================================================
    // Plugin piano roll session memory (message thread only; never serialized)
    // ========================================================================
    /** 记住 placement 的最后完整镜头。 */
    void rememberPianoRollViewport(PianoRollPlacementIdentity placement, PianoRollViewportPrimitive viewport);

    /** 读取 placement 记住的镜头；无记录返回 nullopt。 */
    std::optional<PianoRollViewportPrimitive> readPianoRollViewport(const PianoRollPlacementIdentity& placement) const;

    // ⚡️ vocal-time-stretch §3.6 — TimeGrid accessors per content
    bool ensureTimeToolAnchorSeed(ContentKey key);
#if JucePlugin_Build_Standalone
    AutoRefAvailability queryAutoRefAvailability(uint64_t targetPlacementId) const;
#endif

    /** 设置当前实验性参考对齐模式。由 UI 首选项变更驱动。 */
    void setExperimentalReferenceAlignMode(ExperimentalReferenceAlignMode mode)
    {
        experimentalReferenceAlignMode_ = mode;
    }

private:
#if JucePlugin_Build_Standalone
    void enqueueStandaloneStage2WhenCanonicalSettled(
        ContentKey key,
        std::shared_ptr<const EditableContentSnapshot> snapshot,
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
        double audioSampleRate);
#endif
    ReferenceFeatureProducer resolveReferenceFeatureProducer() const;
    // 纯数据 StandardAuto 特征生产（static：不访问 processor 状态，analysisRevision
    // 由提交方在消息线程固定，worker 只读提交时捕获的不可变 snapshot）。
    static ReferenceFeatureSet buildStandardAutoReferenceFeatureSet(
        const EditableContentSnapshot& snapshot, int analysisRevision);
public:

    std::shared_ptr<ContentEditCommands> getContentCommands() const { return contentCommands_; }

    // Local edits carry their precise range; full mutations rebuild all content.
    // 秒域 local-mutation helper：保持现有 ARA/非 ARA 单一 Stage1 调度；
    // 帧域入口只负责按 pitchCurve hop 换算后调用它。
    void onContentLocalMutationCompletedSeconds(ContentKey key,
                                               double startSeconds,
                                               double endSeconds);
    void onContentLocalMutationCompleted(ContentKey key,
                                         ContentEditRangeFrames affectedRange);
    void onContentFullMutationCompleted(ContentKey key);

    void requestFullContentRender(ContentKey key);

    void handleStage1ChunkSettled(
        ContentKey key,
        std::shared_ptr<const EditableContentSnapshot> snapshot,
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
        double audioSampleRate);

    // ── ContentKey-based mutation and snapshot APIs (Phase 4.3) ─────────────────
    std::shared_ptr<const EditableContentSnapshot> getContentSnapshot(ContentKey key) const;

    bool setContentReferenceFeatures(ContentKey key, const ReferenceFeatureSet& features);

    bool replaceContentNotesForFullMutation(ContentKey key, std::vector<Note> notes);
    ContentCommitSnapshot commitContentNotesAndSegments(ContentKey key,
                                        std::vector<Note> notes,
                                        std::vector<PitchCorrectionSegment> segments,
                                        ContentEditRangeFrames affectedRange);
    ContentCommitSnapshot commitContentNoteTopologyPatch(ContentKey key, ContentNoteRangePatch patch);
    // 一次替换整个 VolumeEnvelope，推进
    // outputGain/content revision，只走 republishPlaybackSource()（零 render enqueue）。
    ContentCommitSnapshot commitVolumeEnvelope(ContentKey key, AutomationLane envelope);
    // 无渲染发布入口：按 content domain 调现有装配函数，只读最新 snapshot、
    // 发布携带该 snapshot（AutomationLane/TimeGrid/revision）的不可变播放源。
    void republishPlaybackSource(ContentKey key);
    bool setContentTimeGrid(ContentKey key,
                            std::shared_ptr<const TimeGridSnapshot> grid);
    bool setContentDetectedKey(ContentKey key, const DetectedKey& detectedKey);
    bool setContentOriginalF0State(ContentKey key, OriginalF0State state);
    bool applyContentPitchShiftState(ContentKey key, const PitchShiftEditState& state);
    std::unique_ptr<PitchShiftEditAction> commitPitchShiftEdit(
        ContentKey key,
        const PitchShiftSettings& newSettings);
private:
    // 调式检测唯一入口（F0 提交成功链调用）：origin==Manual 的内容永不自动覆盖
    void updateContentKeyFromOriginalF0(ContentKey key);
    // AUTO 提交底层：合并/吸附后的音符 + 派生曲线一次性写回。
    // 唯一核心调用方是 autoTuneContentRangeByContentKey；不创建 undo、不 mark dirty。
    bool commitAutoTuneGeneratedNotesByContentKey(ContentKey key,
                                                   std::vector<Note> generatedNotes,
                                                   int startFrame,
                                                   int endFrameExclusive,
                                                   float retuneSpeed,
                                                   float vibratoDepth,
                                                   float vibratoRate);

    // 从不可变 PitchCurveSnapshot 的 OriginalF0 生成音符（generate + validate）。
    // nullopt = 输入无效/生成失败；空 vector = 生成成功但无音符（全静音是合法结果）。
    // 不写 correction segments、不吸附、不请求 render。
    std::optional<std::vector<Note>> generateNotesFromOriginalF0(
        const std::shared_ptr<const PitchCurveSnapshot>& curveSnapshot,
        int startFrame, int endFrameExclusive, const NoteGeneratorParams& params);
public:
    // 普通 AUTO 唯一核心：读 snapshot OriginalF0 → generateNotesFromOriginalF0
    // (energy=nullptr，与现有手动 AUTO 行为一致；nullopt/空 → 返回 false) →
    // scaleSnap apply → commitAutoTuneGeneratedNotesByContentKey。
    // 不创建 undo、不 mark dirty——事务与 dirty 归属调用方。scaleSnap 为空表示不做音阶吸附。
    bool autoTuneContentRangeByContentKey(
        ContentKey key,
        int startFrame,
        int endFrameExclusive,
        const NoteGeneratorParams& params,
        const std::optional<ScaleSnapConfig>& scaleSnap);

    // 仅生成音符唯一入口（单一快照）：读一次 getContentSnapshot →
    // generateNotesFromOriginalF0(0..f0Count) → 拓扑提交（commitContentNoteTopologyPatch）。
    // 不写 correction segments、不吸附、不请求 render。不创建 undo、不 mark dirty。
    // 空音符（全静音）是合法结果：仍提交拓扑 patch 清除该 range 音符。
    bool generateNotesOnlyByContentKey(ContentKey key, const NoteGeneratorParams& params);
public:

#if defined(OPENTUNE_TEST_BUILD)
    void setReferenceAnalysisNotificationDispatcherForTests(
        ReferenceAnalysisService::NotificationDispatcher dispatcher)
    {
        referenceAnalysisService_->setNotificationDispatcher(std::move(dispatcher));
    }
#endif
    ReferenceAnalysisPreheatStatus preheatReferenceAlignmentFeatures(ContentKey key);

#if JucePlugin_Build_Standalone
    ReferenceAlignmentResult executeReferenceAlignmentForPlacement(uint64_t targetPlacementId);

    std::optional<SplitOutcome> splitPlacementAtSeconds(int trackId, int placementIndex, double splitSeconds);
    std::optional<MergeOutcome> mergePlacements(int trackId, uint64_t leadingPlacementId, uint64_t trailingPlacementId, int targetPlacementIndex);
    std::optional<DeleteOutcome> deletePlacement(int trackId, int placementIndex);
    void runReclaimSweepOnMessageThread();   // public for test synchronous invocation
    void scheduleReclaimSweep();

    bool exportPlacementAudio(int trackId, int placementIndex, const juce::File& file);
    // 导出整个轨道的音频（时长以最晚Clip结束为准）
    bool exportTrackAudio(int trackId, const juce::File& file);
    // 导出总线混音（所有轨道）
    bool exportMasterMixAudio(const juce::File& file);
    
    // 导出错误信息
    juce::String getLastExportError() const { return lastExportError_; }
#endif

    // Rendering & Buffering

    // Standalone transport control API — thin accessors over processor-owned
    // PlayHeadState. VST3/ARA uses getPlayHeadState() const reference for UI.
    // Writes only via Standalone setter methods (play/pause/stop/setPosition/setLoopEnabled).
#if JucePlugin_Build_Standalone
    void play();
    void pause();
    void stop();
    void pauseAtPosition(double targetSeconds);
    void setPosition(double seconds);
    void setLoopEnabled(bool enabled);
#endif
    bool isPlaying() const noexcept { return getPlayHeadState().isPlaying.load(std::memory_order_relaxed); }

    /// 从 OutputSpectrumAnalyzer 复制最新 684 个对数频段：spectrum = 主频谱线，peaks = 峰值线（UI 线程调用）。
    void copyOutputSpectrum(SpectrumArray& spectrum,
                            SpectrumArray& peaks) const noexcept;
    bool isLoopEnabled() const noexcept { return getPlayHeadState().isLooping.load(std::memory_order_relaxed); }
    double getPosition() const { return getPlayHeadState().getPresentedPositionSeconds(); }
#if JucePlugin_Build_VST3
    HostTransportSnapshot getHostTransportSnapshot() const;
#endif

    /** Canonical transport truth; UI binds a const non-owning reference.
     *  In ARA mode, returns the document-level shared state so all roles
     *  (including ones that never receive processBlock) see the same truth.
     *  In Standalone/non-ARA VST3, returns this processor's local state.
     *  Defined in PluginProcessor.cpp (requires complete OpenTuneDocumentController type). */
    const PlayHeadState& getPlayHeadState() const noexcept;

    double getPlayStartPosition() const { return playStartPosition_.load(); }

#if JucePlugin_Build_Standalone
    // Standalone canonical BPM setter. Validates 1..999 range and writes only
    // the processor-owned canonical bpm_.
    void setBpm(double bpm);

    // Standalone canonical time signature setter. Validates numerator 1..64
    // and denominator in {1,2,4,8,16,32,64}; writes processor-owned canonical
    // state only.
    void setTimeSignature(int numerator, int denominator);
#endif

    // Each SharedCode target is single-format: the host-snapshot read path only
    // exists in the VST3 variant; Standalone reads its own canonical state.
#if JucePlugin_Build_VST3
    double getBpm() const { return getHostTransportSnapshot().bpm; }
    int getTimeSigNumerator() const { return getHostTransportSnapshot().timeSignatureNumerator; }
    int getTimeSigDenominator() const { return getHostTransportSnapshot().timeSignatureDenominator; }
#else
    double getBpm() const { return bpm_; }
    int getTimeSigNumerator() const { return timeSigNumerator_; }
    int getTimeSigDenominator() const { return timeSigDenominator_; }
#endif

    /** Discrete UI zoom percentage; invalid values are rejected (state unchanged). */
    void setUiZoomPercent(int percent) noexcept;
    int getUiZoomPercent() const noexcept;

    SnapSettings getSnapSettings() const;
    void setSnapSettings(const SnapSettings& snap);

    /** Wire AppPreferences pointer so getSnapSettings() returns live data. */
    void setAppPreferences(AppPreferences* prefs) { appPreferences_ = prefs; }

    UndoManager& getUndoManager() { return undoManager_; }
    PianoKeyAudition& getPianoKeyAudition() { return pianoKeyAudition_; }

private:
    UndoManager undoManager_;
    PianoKeyAudition pianoKeyAudition_;
    OutputSpectrumAnalyzer outputSpectrumAnalyzer_;

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

    // Cached pre-init state payload for deferred restore.
    // Scanner may perform a state round-trip (getStateInformation ->
    // setStateInformation) before prepareToPlay / createEditor / didBindToARA
    // complete runtime init. setStateInformation never triggers
    // initializeRuntimeState(); instead it caches the complete raw payload here
    // and replayDeferredState() restores it once init has succeeded.
    // Message-thread only: setStateInformation and the replay entry points all
    // run on the message thread, so no locking is required.
    juce::MemoryBlock pendingState_;

public:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneAudioProcessor)
};

} // namespace OpenTune
