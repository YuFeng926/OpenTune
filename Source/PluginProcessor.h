#pragma once

/**
 * OpenTune 核心音频处理器
 * 
 * OpenTuneAudioProcessor 是整个应用的核心，负责：
 * - 多轨道音频数据管理（最多 MAX_TRACKS 个轨道）
 * - 实时音频播放和混音（processBlock）
 * - AI 推理调度（通过 RenderingManager）
 * - 全局 Undo/Redo 管理
 * - 项目状态序列化/反序列化
 * 
 * 线程安全说明：
 * - tracks_ 数组使用 tracksLock_（ReadWriteLock）保护
 * - 音频线程使用 ScopedReadLock 读取
 * - UI 线程使用 ScopedWriteLock 写入
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
#include "DSP/ResamplingManager.h"
#include "Utils/PitchCurve.h"
#include "DSP/ScaleInference.h"
#include "Inference/RenderingManager.h"
#include "Inference/RenderCache.h"
#include "Utils/UndoAction.h"
#include "Utils/SilentGapDetector.h"
#include "Utils/TimeCoordinate.h"

namespace OpenTune {

// ============================================================================
// Audio Processing Constants
// ============================================================================
namespace AudioConstants {
    constexpr double DefaultSampleRate = 44100.0;
    constexpr double StoredAudioSampleRate = TimeCoordinate::kRenderSampleRate;
    constexpr int MaxTracks = 8;
    constexpr int64_t RenderLookaheadSeconds = 5;
    constexpr int RenderTimeoutMs = 30000;
    constexpr int RenderPollIntervalMs = 20;
}

class HostIntegration;
class HostIntegrationPlugin;
class HostIntegrationStandalone;
std::unique_ptr<HostIntegration> createHostIntegration();

enum class OriginalF0State : uint8_t {
    NotRequested = 0,
    Extracting,
    Ready,
    Failed
};

/**
 * OpenTuneAudioProcessor - 核心音频处理器类
 * 
 * 继承自 juce::AudioProcessor，实现 JUCE 音频插件接口。
 * 管理多轨道、Clip、音高曲线、渲染缓存等核心数据。
 */
class OpenTuneAudioProcessor : public juce::AudioProcessor
#if JucePlugin_Enable_ARA
                           , public juce::AudioProcessorARAExtension
#endif
{
public:
    struct PerfProbeSnapshot {
        double audioCallbackP99Ms{0.0};
        double cacheMissRate{0.0};
        int renderQueueDepth{0};
        uint64_t cacheChecks{0};
        uint64_t cacheMisses{0};
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

    double getSampleRate() const { return currentSampleRate_.load(std::memory_order_relaxed); }
    
    // 音频以固定 44.1kHz 存储，用于存储音频数据的采样-时间转换
    // 注意：此采样率用于音频数据存储，与设备采样率（currentSampleRate_）可能不同
    static constexpr double getStoredAudioSampleRate() { return AudioConstants::StoredAudioSampleRate; }

    // ============================================================================
    // Import API (Two-phase: prepare in worker thread, commit in main thread)
    // ============================================================================
    
    /**
     * PreparedImportClip - 导入预处理结果（在后台线程完成）
     */
    struct PreparedImportClip {
        int trackId{};
        juce::String clipName;
        juce::AudioBuffer<float> hostRateBuffer;
        std::vector<SilentGap> silentGaps;
    };

    struct PreparedClipPostProcess {
        int trackId{};
        uint64_t clipId{};
        std::vector<SilentGap> silentGaps;
    };
    
    /**
     * 预处理导入 clip（后台线程调用，不持有 tracksLock_）
     * @param trackId 目标轨道
     * @param inBuffer 原始音频 buffer
     * @param inSampleRate 原始采样率
     * @param clipName clip 名称
     * @param out 输出预处理结果
     * @return 成功返回 true
     */
    bool prepareImportClip(int trackId,
                           juce::AudioBuffer<float>&& inBuffer,
                           double inSampleRate,
                           const juce::String& clipName,
                           PreparedImportClip& out);
    
    /**
     * 提交预处理的 clip（主线程调用，写锁内只做轻量挂接）
     * @param prepared 预处理结果（移动语义）
     * @return 成功返回 true
     */
    bool commitPreparedImportClip(PreparedImportClip&& prepared);

    bool prepareDeferredClipPostProcess(int trackId,
                                        uint64_t clipId,
                                        PreparedClipPostProcess& out) const;

    bool commitDeferredClipPostProcess(int trackId,
                                       uint64_t clipId,
                                       PreparedClipPostProcess&& prepared);
    
    // 跨轨道移动 Clip
    bool moveClipToTrack(int sourceTrackId, int targetTrackId, uint64_t clipId, double newStartSeconds);

    void bumpEditVersion();
    void showAudioSettingsDialog(juce::AudioProcessorEditor& editor);

private:
    friend class HostIntegrationPlugin;
    friend class HostIntegrationStandalone;

    std::atomic<double> currentSampleRate_{44100.0};
    int currentBlockSize_ = 512;

    std::shared_ptr<std::atomic<double>> positionAtomic_{std::make_shared<std::atomic<double>>(0.0)};

    std::unique_ptr<HostIntegration> hostIntegration_;

    std::atomic<double> lastHostPlayheadSeconds_{-1.0};

    juce::AudioBuffer<float> doublePrecisionScratch_;

    juce::AudioParameterInt* editVersionParam_{nullptr};
    std::atomic<bool> inferenceReady_{false};
    std::atomic<bool> inferenceInitAttempted_{false};
    std::mutex inferenceInitMutex_;

    // 缓冲状态（用于推理计算期间的状态管理）
    // Buffering State
    std::atomic<bool> isBuffering_{false};
    std::atomic<bool> useDrySignalFallback_{false};
    int64_t bufferingStartCheckTime_{0}; // Milliseconds

    // Multi-track support
    struct TrackState {
        struct AudioClip {
            uint64_t clipId{0};
            juce::AudioBuffer<float> audioBuffer;         // Fixed 44100Hz, never resampled on device rate change
            juce::AudioBuffer<float> drySignalBuffer_;   // Pre-resampled to device rate for dry signal playback
            double startSeconds{0.0};
            float gain{1.0f};
            double fadeInDuration{0.0};
            double fadeOutDuration{0.0};
            juce::String name;
            juce::Colour colour;
            std::shared_ptr<PitchCurve> pitchCurve;
            OriginalF0State originalF0State{OriginalF0State::NotRequested};
            DetectedKey detectedKey;
            std::shared_ptr<RenderCache> renderCache;
            // Per-clip editing data
            std::vector<Note> notes;
            
            // Silent gap detection results (computed on import)
            std::vector<SilentGap> silentGaps;

            AudioClip() = default;
            AudioClip(const AudioClip& other);
            AudioClip& operator=(const AudioClip& other);
            AudioClip(AudioClip&& other) noexcept;
            AudioClip& operator=(AudioClip&& other) noexcept;
        };

        std::vector<AudioClip> clips;
        int selectedClipIndex{0};

        bool isMuted{false};
        bool isSolo{false};
        float volume{1.0f};
        juce::String name;
        juce::Colour colour;
        std::atomic<float> currentRMS{-100.0f}; // Current RMS level in dB
    };

    std::array<TrackState, MAX_TRACKS> tracks_;
    
    // 轨道数据读写锁：保护 tracks_ 数组的线程安全访问
    // 音频线程（processBlock）使用 ScopedReadLock 读取
    // UI线程（导入/移动/删除Clip等）使用 ScopedWriteLock 写入
    mutable juce::ReadWriteLock tracksLock_;
    
    int activeTrackId_{0};
    bool anyTrackSoloed_{false};
    std::atomic<uint64_t> nextClipId_{1};

    // Transport control
    std::atomic<bool> isPlaying_{false};
    std::atomic<bool> loopEnabled_{false};
    double bpm_{120.0};  // Standalone 模式下的默认 BPM（插件模式下从主机同步）

    // Fade-out state for smooth stop/pause
    std::atomic<bool> isFadingOut_{false};
    std::atomic<int> fadeOutSampleCount_{0};
    int fadeOutTotalSamples_{0};  // Set in prepareToPlay based on sample rate

#if !JucePlugin_Build_Standalone
    std::atomic<bool> hostIsRecording_{false};
    std::atomic<bool> hostIsLooping_{false};
    std::atomic<double> hostBpm_{120.0};
    std::atomic<int> hostTimeSigNum_{4};
    std::atomic<int> hostTimeSigDenom_{4};
    std::atomic<double> hostPpqPosition_{0.0};
    std::atomic<double> hostPpqLoopStart_{0.0};
    std::atomic<double> hostPpqLoopEnd_{0.0};
#endif
    
    std::unique_ptr<ResamplingManager> resamplingManager_;
    std::unique_ptr<RenderingManager> renderingManager_;

    // UI state
    bool showWaveform_{true};
    
    bool showLanes_{true};
    double zoomLevel_{1.0};
    int trackHeight_{120};
    
    // 导出错误信息
    juce::String lastExportError_;

    // 全局 Undo/Redo 管理器（统一所有视图的操作历史）
    UndoManager globalUndoManager_{500};

    bool ensureInferenceReady();
    void recordAudioCallbackDurationMs(double durationMs);
    void recordCacheCheck(bool cacheHit);
    double computeAudioCallbackPercentileMs(double percentile) const;
    void resampleDrySignal(TrackState::AudioClip& clip, double deviceSampleRate);

    // ========================================================================
    // Chunk-Level Render Queue
    // ========================================================================
    
    struct ChunkTaskKey {
        int trackId{0};
        uint64_t clipId{0};
        int64_t chunkIndex{0};
        
        bool operator==(const ChunkTaskKey& other) const noexcept {
            return trackId == other.trackId &&
                   clipId == other.clipId &&
                   chunkIndex == other.chunkIndex;
        }
        
        bool operator<(const ChunkTaskKey& other) const noexcept {
            if (trackId != other.trackId) return trackId < other.trackId;
            if (clipId != other.clipId) return clipId < other.clipId;
            return chunkIndex < other.chunkIndex;
        }
    };

    struct ChunkTaskKeyHash {
        size_t operator()(const ChunkTaskKey& k) const noexcept {
            size_t h1 = std::hash<int>{}(k.trackId);
            size_t h2 = std::hash<uint64_t>{}(k.clipId);
            size_t h3 = std::hash<int64_t>{}(k.chunkIndex);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    struct RenderTxn {
        uint64_t id{0};
        int trackId{-1};
        uint64_t clipId{0};
        std::atomic<int> planned{0};
        std::atomic<int> converged{0};
        std::atomic<int> errors{0};
        
        void reset() {
            id = 0;
            trackId = -1;
            clipId = 0;
            planned.store(0);
            converged.store(0);
            errors.store(0);
        }
    };

    struct ChunkRenderTask {
        ChunkTaskKey key;
        double relStartSeconds{0.0};
        double relEndSeconds{0.0};
        uint64_t targetRevision{0};
        uint64_t txnId{0};
    };

    void triggerChunkRender(const ChunkRenderTask& task);
    void chunkRenderWorkerLoop();

    std::thread chunkRenderWorkerThread_;
    mutable std::mutex chunkQueueMutex_;
    std::condition_variable chunkQueueCv_;
    bool chunkRenderWorkerRunning_{false};
    
    static constexpr size_t kMaxChunkQueueSize = 1000;
    std::deque<ChunkTaskKey> chunkQueueOrder_;
    using ChunkQueueMap = std::unordered_map<ChunkTaskKey, ChunkRenderTask, ChunkTaskKeyHash>;
    ChunkQueueMap chunkPending_;
    
    bool enqueueChunkTask(const ChunkTaskKey& key, const ChunkRenderTask& task);
    
    RenderTxn currentTxn_;
    std::atomic<uint64_t> nextTxnId_{1};
    
    void finishChunk(const ChunkRenderTask& task, std::shared_ptr<RenderCache> renderCache, bool isError);

    // ========================================================================
    // Performance Probes
    // ========================================================================
    std::atomic<uint64_t> perfCacheChecks_{0};
    std::atomic<uint64_t> perfCacheMisses_{0};
    std::atomic<uint64_t> perfAudioCallbackCount_{0};
    static constexpr int PerfHistogramBins = 201;      // 0.0ms~20.0ms，每 0.1ms 一档 + 溢出档
    static constexpr double PerfHistogramStepMs = 0.1;
    std::array<std::atomic<uint32_t>, PerfHistogramBins> perfAudioDurationHistogram_{};

public:
    // Multi-track API
    void setActiveTrack(int trackId);
    int getActiveTrackId() const { return activeTrackId_; }
    
    const RenderTxn& getCurrentTxn() const { return currentTxn_; }
    
    // Track State Management
    int getNumTracks() const { return MAX_TRACKS; }
    
    // Track control (Solo/Mute)
    void setTrackMuted(int trackId, bool muted);
    bool isTrackMuted(int trackId) const;
    void setTrackSolo(int trackId, bool solo);
    bool isTrackSolo(int trackId) const;
    void setTrackVolume(int trackId, float volume);
    float getTrackVolume(int trackId) const;
    float getTrackRMS(int trackId) const;

    // Track height (shared state)
    void setTrackHeight(int height);
    int getTrackHeight() const { return trackHeight_; }

    void setShowWaveform(bool shouldShow) { showWaveform_ = shouldShow; }
    bool getShowWaveform() const { return showWaveform_; }
    void setShowLanes(bool shouldShow) { showLanes_ = shouldShow; }
    bool getShowLanes() const { return showLanes_; }

    bool isInferenceReady() const { return inferenceReady_.load(); }
    bool initializeInferenceIfNeeded();

    int getNumClips(int trackId) const;
    int getSelectedClip(int trackId) const;
    void setSelectedClip(int trackId, int clipIndex);
    
    // Clip access
    const juce::AudioBuffer<float>* getClipAudioBuffer(int trackId, int clipIndex) const;
    uint64_t getClipId(int trackId, int clipIndex) const;
    int findClipIndexById(int trackId, uint64_t clipId) const;
    double getClipStartSeconds(int trackId, int clipIndex) const;
    juce::String getClipName(int trackId, int clipIndex) const;
    float getClipGain(int trackId, int clipIndex) const;
    void setClipStartSeconds(int trackId, int clipIndex, double startSeconds);
    void setClipGain(int trackId, int clipIndex, float gain);
    void setClipGainById(int trackId, uint64_t clipId, float gain);
    std::shared_ptr<PitchCurve> getClipPitchCurve(int trackId, int clipIndex) const;
    void setClipPitchCurve(int trackId, int clipIndex, std::shared_ptr<PitchCurve> curve);
    OriginalF0State getClipOriginalF0State(int trackId, int clipIndex) const;
    void setClipOriginalF0State(int trackId, int clipIndex, OriginalF0State state);
    bool setClipOriginalF0StateById(int trackId, uint64_t clipId, OriginalF0State state);
    DetectedKey getClipDetectedKey(int trackId, int clipIndex) const;
    void setClipDetectedKey(int trackId, int clipIndex, const DetectedKey& key);
    
    // Note and Anchor management per clip
    std::vector<Note> getClipNotes(int trackId, int clipIndex) const;
    void setClipNotes(int trackId, int clipIndex, const std::vector<Note>& notes);
    
    // Silent gap and chunk boundary access
    const std::vector<SilentGap>& getClipSilentGaps(int trackId, int clipIndex) const;
    int getClipChunkCountInRange(int trackId, int clipIndex, double relStartSeconds, double relEndSeconds) const;
    SilentGapDetector::DetectionConfig getSilentGapDetectionConfig() const;
    void setSilentGapDetectionConfig(const SilentGapDetector::DetectionConfig& config);
    
    bool splitClipAtSeconds(int trackId, int clipIndex, double splitSeconds);
    bool mergeSplitClips(int trackId, uint64_t originalClipId, uint64_t newClipId, int targetClipIndex);
    bool deleteClip(int trackId, int clipIndex);

    struct ClipSnapshot {
        juce::AudioBuffer<float> audioBuffer;
        double startSeconds{0.0};
        float gain{1.0f};
        double fadeInDuration{0.0};
        double fadeOutDuration{0.0};
        juce::String name;
        juce::Colour colour;
        std::shared_ptr<PitchCurve> pitchCurve;
        OriginalF0State originalF0State{OriginalF0State::NotRequested};
        DetectedKey detectedKey;
        std::shared_ptr<RenderCache> renderCache;
    };

    bool getClipSnapshot(int trackId, uint64_t clipId, ClipSnapshot& out) const;
    double getClipStartSecondsById(int trackId, uint64_t clipId) const;
    bool setClipStartSecondsById(int trackId, uint64_t clipId, double startSeconds);
    bool deleteClipById(int trackId, uint64_t clipId, ClipSnapshot* deletedOut, int* deletedIndexOut);
    bool insertClipSnapshot(int trackId, int insertIndex, const ClipSnapshot& snap, uint64_t forcedClipId);
    
    bool hasTrackAudio(int trackId) const;
    // 导出单个Clip的音频
    bool exportClipAudio(int trackId, int clipIndex, const juce::File& file);
    // 导出整个轨道的音频（时长以最晚Clip结束为准）
    bool exportTrackAudio(int trackId, const juce::File& file);
    // 导出总线混音（所有轨道）
    bool exportMasterMixAudio(const juce::File& file);
    
    // 导出错误信息
    juce::String getLastExportError() const { return lastExportError_; }
    void clearLastExportError() { lastExportError_.clear(); }

    // Rendering & Buffering
    void enqueuePartialRender(int trackId, int clipIndex, double relStartSeconds, double relEndSeconds);
    // 清除指定范围的渲染缓存（当 correctedF0 被清除时调用）
    void clearClipCacheRange(int trackId, int clipIndex, double relStartSeconds, double relEndSeconds);

    // Playback Buffering State
    bool isBuffering() const { return isBuffering_; }
    bool isDrySignalFallback() const { return useDrySignalFallback_; }

    // Transport control API
    void setPlaying(bool playing);
    bool isPlaying() const { return isPlaying_; }
    void setLoopEnabled(bool enabled);
    bool isLoopEnabled() const { return loopEnabled_; }
    void setPosition(double seconds);
    double getPosition() const;
    
    std::shared_ptr<std::atomic<double>> getPositionAtomic() { return positionAtomic_; }

    void setBpm(double bpm);
    double getBpm() const
    {
       #if !JucePlugin_Build_Standalone
        return hostBpm_.load();
       #else
        return bpm_;
       #endif
    }

    int getTimeSigNumerator() const
    {
       #if !JucePlugin_Build_Standalone
        return hostTimeSigNum_.load();
       #else
        return 4;
       #endif
    }

    int getTimeSigDenominator() const
    {
       #if !JucePlugin_Build_Standalone
        return hostTimeSigDenom_.load();
       #else
        return 4;
       #endif
    }

    double getPpqPosition() const
    {
       #if !JucePlugin_Build_Standalone
        return hostPpqPosition_.load();
       #else
        return 0.0;
       #endif
    }

    bool isRecording() const
    {
       #if !JucePlugin_Build_Standalone
        return hostIsRecording_.load();
       #else
        return false;
       #endif
    }

    bool isHostLooping() const
    {
       #if !JucePlugin_Build_Standalone
        return hostIsLooping_.load();
       #else
        return false;
       #endif
    }

    double getLoopStartPpq() const
    {
       #if !JucePlugin_Build_Standalone
        return hostPpqLoopStart_.load();
       #else
        return 0.0;
       #endif
    }

    double getLoopEndPpq() const
    {
       #if !JucePlugin_Build_Standalone
        return hostPpqLoopEnd_.load();
       #else
        return 0.0;
       #endif
    }

    void setZoomLevel(double zoom);
    double getZoomLevel() const { return zoomLevel_; }

    RenderingManager* getRenderingManager() const { return renderingManager_.get(); }
    PerfProbeSnapshot getPerfProbeSnapshot() const;
    void resetPerfProbeCounters();

    // 全局 Undo/Redo API
    UndoManager& getUndoManager() { return globalUndoManager_; }
    const UndoManager& getUndoManager() const { return globalUndoManager_; }
    void performUndo();
    void performRedo();
    bool canUndo() const { return globalUndoManager_.canUndo(); }
    bool canRedo() const { return globalUndoManager_.canRedo(); }
    juce::String getUndoDescription() const { return globalUndoManager_.getUndoDescription(); }
    juce::String getRedoDescription() const { return globalUndoManager_.getRedoDescription(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneAudioProcessor)
};

} // namespace OpenTune
