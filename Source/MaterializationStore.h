/**
 * 物料化存储（MaterializationStore）
 *
 * 管理所有可编辑音频载荷的真相（notes、corrected F0、调式、RenderCache 等）。
 * 每个 Materialization 由一个 Source 派生，持有该 Source 的 provenance window
 * 以及所有编辑后的结果数据。Placement 只负责时间轴摆放，不持有编辑内容。
 *
 * 线程安全：内部使用 ReadWriteLock，读写均可跨线程调用。
 * 生命周期：支持 retire/revive 软删除，用于 Undo 系统的延迟回收。
 * 渲染调度：提供 enqueuePartialRender 队列接口，委托 ContentRenderService。
 *
 * Phase 0.7: 改用 extracted runtime services（PlaybackSourcePublisher、
 * RenderCacheRegistry、StretcherPool、RenderWorker），删除 entry-owned runtime。
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "Content/ContentKey.h"
#include "DSP/ChromaKeyDetector.h"
#include "DSP/ReferenceFeatures.h"
#include "Inference/RenderCache.h"
#include "Inference/TimeStretchCache.h"
#include "Render/ContentRenderService.h"
#include "Render/PlaybackReadSource.h"
#include "Render/RenderChunkPlanner.h"
#include "Render/RenderJob.h"
#include "Utils/MaterializationState.h"
#include "Utils/Note.h"
#include "Utils/PitchCurve.h"
#include "Utils/PitchShiftSettings.h"
#include "Utils/SilentGapDetector.h"
#include "Utils/SourceWindow.h"
#include "Utils/TimeGrid.h"

namespace OpenTune {
class SoundTouchStretcher;   // forward-decl
}

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
        std::shared_ptr<const TimeGridSnapshot> timeGrid;
    };

    // Phase 0.7: PlaybackReadSource 使用统一定义
    using PlaybackReadSource = OpenTune::PlaybackReadSource;
    using PendingRenderJob = OpenTune::RenderJob;

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
        std::shared_ptr<const TimeGridSnapshot> timeGrid;
        uint64_t timeGridRevision{0};
        PitchShiftSettings pitchShiftSettings;
        uint64_t pitchShiftRevision{0};
    };

    // 仅 notes 部分的轻量快照
    struct MaterializationNotesSnapshot {
        std::vector<Note> notes;
        uint64_t notesRevision{0};
    };

    // Reference feature cache
    MaterializationStore();
    ~MaterializationStore();

    MaterializationStore(const MaterializationStore&) = delete;
    MaterializationStore& operator=(const MaterializationStore&) = delete;

    uint64_t createMaterialization(CreateMaterializationRequest request,
                                   uint64_t forcedMaterializationId = 0);
    void clear();
    bool deleteMaterialization(uint64_t materializationId);
    bool containsMaterialization(uint64_t materializationId) const;
    bool hasMaterializationForSource(uint64_t sourceId) const;
    bool hasMaterializationForSourceAnyState(uint64_t sourceId) const;

    std::vector<uint64_t> getAllActiveMaterializationIds() const;
    std::vector<MaterializationSnapshot> getAllActiveMaterializationSnapshots() const;
    int getTotalCount() const;

    // 软删除/恢复接口
    bool retireMaterialization(uint64_t id);
    bool reviveMaterialization(uint64_t id);
    bool isRetired(uint64_t id) const;
    bool physicallyDeleteIfReclaimable(uint64_t id);
    std::vector<uint64_t> getRetiredIds() const;
    uint64_t getSourceIdAnyState(uint64_t id) const;

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
    bool commitReferenceAlignmentPatch(uint64_t materializationId,
                                       std::vector<Note> notesAfter,
                                       std::shared_ptr<PitchCurve> pitchCurveAfter,
                                       std::shared_ptr<const TimeGridSnapshot> timeGridAfter);

    // TimeGrid accessors
    bool getTimeGrid(uint64_t materializationId,
                     std::shared_ptr<const TimeGridSnapshot>& outSnapshot) const;
    uint64_t getTimeGridRevision(uint64_t materializationId) const;
    bool setTimeGrid(uint64_t materializationId,
                     std::shared_ptr<const TimeGridSnapshot> snapshot,
                     int64_t affectedSrcStartFrame = 0,
                     int64_t affectedSrcEndFrame = 0);

    // Pitch Shift
    PitchShiftSettings getPitchShiftSettings(uint64_t materializationId) const;
    uint64_t getPitchShiftRevision(uint64_t materializationId) const;
    bool setPitchShiftSettings(uint64_t materializationId, const PitchShiftSettings& settings);

    // Stretcher（委托到 StretcherPool）
    SoundTouchStretcher* getOpenTuneStretcher(uint64_t materializationId,
                                                double sampleRate,
                                                int channels);

    // TimeStretchCache accessor
    TimeStretchCache& getTimeStretchCache() noexcept;
    const TimeStretchCache& getTimeStretchCache() const noexcept;

    OriginalF0State getOriginalF0State(uint64_t materializationId) const;
    bool setOriginalF0State(uint64_t materializationId, OriginalF0State state);

    DetectedKey getDetectedKey(uint64_t materializationId) const;
    bool setDetectedKey(uint64_t materializationId, const DetectedKey& key);

    std::vector<Note> getNotes(uint64_t materializationId) const;
    bool getNotesSnapshot(uint64_t materializationId, MaterializationNotesSnapshot& out) const;
    bool setNotes(uint64_t materializationId, std::vector<Note> notes);

    bool replaceAudio(uint64_t materializationId,
                       std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
                       std::vector<SilentGap> silentGaps);

    uint64_t replaceMaterializationWithNewLineage(uint64_t oldId,
                                                   CreateMaterializationRequest request);

    bool enqueuePartialRender(uint64_t materializationId,
                              double relStartSeconds,
                              double relEndSeconds,
                              int hopSize);

    double getMaterializationAudioDurationById(uint64_t materializationId) const noexcept;
    uint64_t findMaterializationBySourceWindow(uint64_t sourceId, const SourceWindow& window) const;

    static std::vector<int64_t> buildChunkBoundariesFromSilentGaps(int64_t materializationSampleCount,
                                                                      const std::vector<SilentGap>& silentGaps,
                                                                      int hopSize);

    // Reference feature cache API
    bool setReferenceFeatures(uint64_t materializationId, const ReferenceFeatureSet& features);
    bool getReferenceFeatures(uint64_t materializationId, ReferenceFeatureSet& out) const;

    void pauseRenderWorker();
    void resumeRenderWorker();

    void attachContentRenderService(ContentRenderService* crs) { contentRenderService_ = crs; }
    ContentRenderService* getContentRenderService() const noexcept { return contentRenderService_; }

private:
    // Phase 0.7: 不再持有 runtime 基础设施（RenderCache/Stretcher 由 CRS 管理）
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
        std::vector<Note> notes;
        std::vector<SilentGap> silentGaps;
        std::shared_ptr<const TimeGridSnapshot> timeGrid;
        uint64_t timeGridRevision{0};
        PitchShiftSettings pitchShiftSettings;
        uint64_t pitchShiftRevision{0};
        ReferenceFeatureSet referenceFeatures;
        bool isRetired_{false};
    };

    juce::ReadWriteLock lock_;
    std::map<uint64_t, MaterializationEntry> materializations_;
    std::atomic<uint64_t> nextMaterializationId_{1};
    ContentRenderService* contentRenderService_{nullptr};

    // Helpers
    static ContentKey contentKeyForMaterializationId(uint64_t id) noexcept;
    bool hasRuntimeServices() const noexcept;
    void publishPlaybackSourceForEntry(uint64_t id, const MaterializationEntry& entry);
    PlaybackReadSource makePlaybackReadSourceForEntry(ContentKey key, const MaterializationEntry& entry) const;
    void removeRuntimeForMaterialization(uint64_t id);
};

} // namespace OpenTune
