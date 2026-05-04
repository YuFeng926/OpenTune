/**
 * 物料化存储（MaterializationStore）
 *
 * 管理所有可编辑音频载荷的真相（notes、corrected F0、调式、RenderCache 等）。
 * 每个 Materialization 由一个 Source 派生，持有该 Source 的 provenance window
 * 以及所有编辑后的结果数据。Placement 只负责时间轴摆放，不持有编辑内容。
 *
 * 线程安全：内部使用 ReadWriteLock，读写均可跨线程调用。
 * 生命周期：支持 retire/revive 软删除，用于 Undo 系统的延迟回收。
 * 渲染调度：提供 enqueuePartialRender / pullNextPendingRenderJob 队列接口。
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "DSP/ChromaKeyDetector.h"
#include "Inference/RenderCache.h"
#include "Utils/MaterializationState.h"
#include "Utils/Note.h"
#include "Utils/PitchCurve.h"
#include "Utils/SilentGapDetector.h"
#include "Utils/SourceWindow.h"

namespace OpenTune {

class MaterializationStore {
public:
    // 创建 Materialization 的请求参数
    struct CreateMaterializationRequest {
        uint64_t sourceId{0};
        uint64_t lineageParentMaterializationId{0};
        SourceWindow sourceWindow;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        std::shared_ptr<PitchCurve> pitchCurve;
        OriginalF0State originalF0State{OriginalF0State::NotRequested};
        DetectedKey detectedKey;
        std::shared_ptr<RenderCache> renderCache;
        std::vector<Note> notes;
        std::vector<SilentGap> silentGaps;
        uint64_t renderRevision{0};
    };

    // 播放时的只读音频来源（优先 RenderCache，降级为原始音频）
    struct PlaybackReadSource {
        std::shared_ptr<RenderCache> renderCache;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;

        bool hasAudio() const
        {
            return audioBuffer != nullptr && audioBuffer->getNumSamples() > 0;
        }

        bool canRead() const
        {
            return renderCache != nullptr || hasAudio();
        }
    };

    // Materialization 完整只读快照
    struct MaterializationSnapshot {
        uint64_t materializationId{0};
        uint64_t sourceId{0};
        uint64_t lineageParentMaterializationId{0};
        SourceWindow sourceWindow;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        std::shared_ptr<PitchCurve> pitchCurve;
        OriginalF0State originalF0State{OriginalF0State::NotRequested};
        DetectedKey detectedKey;
        std::shared_ptr<RenderCache> renderCache;
        std::vector<Note> notes;
        uint64_t notesRevision{0};
        std::vector<SilentGap> silentGaps;
        uint64_t renderRevision{0};
    };

    // 仅 notes 部分的轻量快照
    struct MaterializationNotesSnapshot {
        std::vector<Note> notes;
        uint64_t notesRevision{0};
    };

    // 待执行的局部重渲染任务
    struct PendingRenderJob {
        uint64_t materializationId{0};
        std::shared_ptr<RenderCache> renderCache;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        std::shared_ptr<PitchCurve> pitchCurve;
        std::vector<SilentGap> silentGaps;
        double startSeconds{0.0};
        double endSeconds{0.0};
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
        uint64_t targetRevision{0};
    };

    MaterializationStore();
    ~MaterializationStore();

    MaterializationStore(const MaterializationStore&) = delete;
    MaterializationStore& operator=(const MaterializationStore&) = delete;

    uint64_t createMaterialization(CreateMaterializationRequest request,
                                   uint64_t forcedMaterializationId = 0);
    void clear();
    bool deleteMaterialization(uint64_t materializationId);
    bool containsMaterialization(uint64_t materializationId) const;   // active only
    bool hasMaterializationForSource(uint64_t sourceId) const;         // active only
    bool hasMaterializationForSourceAnyState(uint64_t sourceId) const; // active + retired

    // 软删除/恢复接口，供 UndoAction 和垃圾回收使用
    bool retireMaterialization(uint64_t id);
    bool reviveMaterialization(uint64_t id);
    bool isRetired(uint64_t id) const;
    bool physicallyDeleteIfReclaimable(uint64_t id);
    std::vector<uint64_t> getRetiredIds() const;
    uint64_t getSourceIdAnyState(uint64_t id) const;  // returns sourceId even if retired, 0 if not found

    bool getAudioBuffer(uint64_t materializationId,
                        std::shared_ptr<const juce::AudioBuffer<float>>& out) const;
    bool getPlaybackReadSource(uint64_t materializationId, PlaybackReadSource& out) const;
    bool getSnapshot(uint64_t materializationId, MaterializationSnapshot& out) const;
    bool getRenderCache(uint64_t materializationId, std::shared_ptr<RenderCache>& out) const;

    bool getPitchCurve(uint64_t materializationId, std::shared_ptr<PitchCurve>& out) const;
    bool setPitchCurve(uint64_t materializationId, std::shared_ptr<PitchCurve> curve);
    bool commitNotesAndPitchCurve(uint64_t materializationId,
                                  std::vector<Note> notes,
                                  std::shared_ptr<PitchCurve> curve);

    OriginalF0State getOriginalF0State(uint64_t materializationId) const;
    bool setOriginalF0State(uint64_t materializationId, OriginalF0State state);

    DetectedKey getDetectedKey(uint64_t materializationId) const;
    bool setDetectedKey(uint64_t materializationId, const DetectedKey& key);

    std::vector<Note> getNotes(uint64_t materializationId) const;
    bool getNotesSnapshot(uint64_t materializationId, MaterializationNotesSnapshot& out) const;
    bool setNotes(uint64_t materializationId, std::vector<Note> notes);

    bool setSilentGaps(uint64_t materializationId, std::vector<SilentGap> silentGaps);
    bool replaceAudio(uint64_t materializationId,
                       std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
                       std::vector<SilentGap> silentGaps);

    // 用新 lineage 原子替换旧 materialization：在一把 write lock 内完成 erase oldId + emplace 新 entry + 返回 newId。
    // 调用方负责把所有指向 oldId 的 placement 重指向 newId。
    // oldId 必须存在，否则返回 0 并 jassert。
    uint64_t replaceMaterializationWithNewLineage(uint64_t oldId,
                                                   CreateMaterializationRequest request);

    void prepareAllCrossoverMixers(double sampleRate, int maxBlockSize);
    bool enqueuePartialRender(uint64_t materializationId,
                              double relStartSeconds,
                              double relEndSeconds,
                              int hopSize);
    bool hasPendingRenderJobs() const;
    bool pullNextPendingRenderJob(PendingRenderJob& out);

    double getMaterializationAudioDurationById(uint64_t materializationId) const noexcept;

    uint64_t findMaterializationBySourceWindow(uint64_t sourceId, const SourceWindow& window) const;

    static std::vector<int64_t> buildChunkBoundariesFromSilentGaps(int64_t materializationSampleCount,
                                                                    const std::vector<SilentGap>& silentGaps,
                                                                    int hopSize);

private:
    // 内部存储条目
    struct MaterializationEntry {
        uint64_t materializationId{0};
        uint64_t sourceId{0};
        uint64_t lineageParentMaterializationId{0};
        SourceWindow sourceWindow;
        uint64_t renderRevision{0};
        uint64_t notesRevision{0};
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        std::shared_ptr<PitchCurve> pitchCurve;
        OriginalF0State originalF0State{OriginalF0State::NotRequested};
        DetectedKey detectedKey;
        std::shared_ptr<RenderCache> renderCache;
        std::vector<Note> notes;
        std::vector<SilentGap> silentGaps;
        bool isRetired_{false};
    };

    // 独立渲染任务队列，pull 时不再遍历 materializations_ map
    struct PendingRenderEntry {
        uint64_t materializationId{0};
        double startSeconds{0.0};
        double endSeconds{0.0};
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
    };
    mutable std::mutex renderQueueMutex_;
    std::deque<PendingRenderEntry> pendingRenderQueue_;

    juce::ReadWriteLock lock_;
    std::map<uint64_t, MaterializationEntry> materializations_;
    std::atomic<uint64_t> nextMaterializationId_{1};
};

} // namespace OpenTune
