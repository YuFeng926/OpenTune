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
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "DSP/ChromaKeyDetector.h"
#include "DSP/ReferenceFeatures.h"
#include "Inference/RenderCache.h"
#include "Inference/TimeStretchCache.h"  // ⚡️ vocal-time-stretch §6.2 — store-wide Stage 2 cache
#include "Utils/MaterializationState.h"
#include "Utils/Note.h"
#include "Utils/PitchCurve.h"
#include "Utils/SilentGapDetector.h"
#include "Utils/SourceWindow.h"
#include "Utils/TimeGrid.h"   // ⚡️ vocal-time-stretch §3.6 — per-materialization TimeGrid
#include "Utils/PitchShiftSettings.h"  // clip-level pitch shift render modifier
#include "Render/ContentRenderService.h"

namespace OpenTune {
class SoundTouchStretcher;   // forward-decl — §5.5 (lazy time-stretch accessor)
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

        // vocal-time-stretch §3.7: optional initial TimeGrid.
        // If null, createMaterialization auto-seeds an identity grid spanning
        // [0, audioDurationSeconds] with locked endpoints (output==source).
        std::shared_ptr<const TimeGridSnapshot> timeGrid;
    };

    // 播放时的只读音频来源（优先 RenderCache，降级为原始音频）
    struct PlaybackReadSource {
        std::shared_ptr<RenderCache> renderCache;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;

        // vocal-time-stretch §7 (Phase D MVP):
        //   When timeGridIsIdentity == false AND timeStretchCache != nullptr,
        //   readPlaybackAudio fast-paths from the TimeStretchCache (Stage 2 output),
        //   bypassing the dry-then-overlay piecewise path.  When the cache misses
        //   (Stage 2 not yet rendered), readPlaybackAudio falls back to that path
        //   until the worker finishes.  When timeGridIsIdentity == true (default
        //   after import), behavior is unchanged.
        TimeStretchCache* timeStretchCache{nullptr};
        uint64_t materializationId{0};
        uint32_t pitchRevision{0};
        uint32_t timeGridRevision{0};
        PitchShiftSettings pitchShiftSettings;
        bool timeGridIsIdentity{true};

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

    // ============================================================
    // Reference feature cache
    //
    // Each materialization holds one ReferenceFeatureSet that caches
    // source-local pitch/timing facts for AUTO(REF) and TimeTool seed.
    // Invalidated when source audio or original F0 changes.
    // setNotes does NOT invalidate — user note edits are independent.
    // ============================================================
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

    /** 获取所有 active materialization 的 ID 列表（用于工程保存等需要枚举的场景） */
    std::vector<uint64_t> getAllActiveMaterializationIds() const;

    /** 获取所有 active materialization 的完整快照列表（批量获取，用于工程保存） */
    std::vector<MaterializationSnapshot> getAllActiveMaterializationSnapshots() const;

    /** 获取 materialization 总数（含 retired），用于容量估计 */
    int getTotalCount() const;

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
    bool commitReferenceAlignmentPatch(uint64_t materializationId,
                                       std::vector<Note> notesAfter,
                                       std::shared_ptr<PitchCurve> pitchCurveAfter,
                                       std::shared_ptr<const TimeGridSnapshot> timeGridAfter);

    // ============================================================
    // vocal-time-stretch §3.6 — TimeGrid accessors (per-materialization)
    //
    // The TimeGrid lives alongside PitchCurve: COW snapshot, atomic publish.
    // setTimeGridById bumps timeGridRevision so cache layers can detect change.
    // ============================================================

    bool getTimeGrid(uint64_t materializationId,
                     std::shared_ptr<const TimeGridSnapshot>& outSnapshot) const;
    uint64_t getTimeGridRevision(uint64_t materializationId) const;
    bool setTimeGrid(uint64_t materializationId,
                     std::shared_ptr<const TimeGridSnapshot> snapshot,
                     int64_t affectedSrcStartFrame = 0,
                     int64_t affectedSrcEndFrame = 0);

    // ============================================================
    // Pitch Shift — clip-level render modifier (per-materialization)
    //
    // PitchShiftSettings lives alongside PitchCurve/TimeGrid as a
    // materialization-level render modifier. setPitchShiftSettings
    // bumps pitchShiftRevision and invalidates RenderCache + TimeStretchCache.
    // ============================================================
    PitchShiftSettings getPitchShiftSettings(uint64_t materializationId) const;
    uint64_t getPitchShiftRevision(uint64_t materializationId) const;
    bool setPitchShiftSettings(uint64_t materializationId, const PitchShiftSettings& settings);

    // ============================================================
    // vocal-time-stretch §5.5 — per-materialization SoundTouch stretcher
    //
    // The stretcher is constructed lazily when first needed (Stage 2 render
    // worker) and destroyed when the materialization is destroyed.  Returns
    // a non-owning pointer; caller must NOT delete.
    // ============================================================
    SoundTouchStretcher* getOpenTuneStretcher(uint64_t materializationId,
                                                double sampleRate,
                                                int channels);

    // ============================================================
    // TimeStretchCache accessor
    //
    // Delegates to ContentRenderService when attached; otherwise
    // returns a static local fallback.  No cache is owned directly
    // by MaterializationStore.
    // ============================================================
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

    // 用新 lineage 原子替换旧 materialization：在一把 write lock 内完成 erase oldId + emplace 新 entry + 返回 newId。
    // 调用方负责把所有指向 oldId 的 placement 重指向 newId。
    // oldId 必须存在，否则返回 0 并 jassert。
    uint64_t replaceMaterializationWithNewLineage(uint64_t oldId,
                                                   CreateMaterializationRequest request);

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

    // ============================================================
    // Reference feature cache API
    // ============================================================
    bool setReferenceFeatures(uint64_t materializationId, const ReferenceFeatureSet& features);
    bool getReferenceFeatures(uint64_t materializationId, ReferenceFeatureSet& out) const;

    /** Register the callback that processes each render job.
     *  Must be set before the first job is enqueued (typically
     *  set by the owning processor or document controller).
     *  Delegates to ContentRenderService::attachExecutionLease
     *  when contentRenderService_ is non-null; otherwise no-op. */
    void setRenderJobCallback(std::function<void(PendingRenderJob&)> cb);

    /** Wake the render worker (e.g. from vocoder completion callback). */
    void notifyRenderWorker();

    /** Pause/resume the render worker.
     *  Delegates to ContentRenderService when attached. */
    void pauseRenderWorker();
    void resumeRenderWorker();

    /** Drain any in-flight render job and pause the render worker.
     *  Blocks until all in-flight jobs complete.
     *  Delegates to ContentRenderService when attached. */
    void drainRenderWorker();

    // ============================================================
    // Delegation to ContentRenderService
    //
    // When contentRenderService_ is non-null, render worker and
    // TimeStretchCache methods forward to it. No local cache or
    // worker infrastructure remains in MaterializationStore.
    // ============================================================
    void attachContentRenderService(ContentRenderService* crs) { contentRenderService_ = crs; }
    ContentRenderService* getContentRenderService() const noexcept { return contentRenderService_; }

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
        std::shared_ptr<const TimeGridSnapshot> timeGrid;   // §3.6
        uint64_t timeGridRevision{0};                        // §3.6
        PitchShiftSettings pitchShiftSettings;              // clip-level pitch shift modifier
        uint64_t pitchShiftRevision{0};                     // independent revision for cache invalidation
        std::unique_ptr<SoundTouchStretcher> stretcher;     // §5.5 — lazy-constructed
        ReferenceFeatureSet referenceFeatures;            // reference auto-align cache
        bool isRetired_{false};
    };

    juce::ReadWriteLock lock_;

    // Lock-free snapshot cache for audio-thread getPlaybackReadSource().
    // Rebuilt under write lock after every mutation that affects playback data.
    // Pattern: std::atomic_load/store(shared_ptr<const Map>) — same as CaptureSession.
    mutable std::shared_ptr<const std::map<uint64_t, PlaybackReadSource>> playbackSourceCache_;

    void rebuildPlaybackSourceCache();

    std::map<uint64_t, MaterializationEntry> materializations_;
    std::atomic<uint64_t> nextMaterializationId_{1};
    ContentRenderService* contentRenderService_{nullptr};
};

} // namespace OpenTune
