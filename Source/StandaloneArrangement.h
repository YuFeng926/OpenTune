/**
 * Standalone 编曲视图数据模型（StandaloneArrangement）
 *
 * 管理 Standalone 模式下的多轨时间轴：Track、Placement 与播放快照。
 * 每个 Placement 引用一个 Materialization，并在时间轴上确定起始位置和时长。
 * 本类不持有音频数据本身——音频编辑内容由 MaterializationStore 管理。
 *
 * 线程安全：stateLock_ (ReadWriteLock) 保护所有 Track/Placement 状态；
 *          PlaybackSnapshot 通过 std::atomic_load/exchange(shared_ptr) 无锁发布。
 * 设计：音频线程只通过 loadPlaybackSnapshot() atomic_load 读取不可变快照，零锁竞争。
 *         旧快照推入 retiredSnapshots_ 列表，写入方按 use_count()==1 sweep 回收。
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Utils/TrackConstants.h"

namespace OpenTune {

class StandaloneArrangement {
public:
    static constexpr int kTrackCount = MaxTracks;

    // 时间轴上的一个片段摆放，引用 MaterializationStore 中的编辑结果
    struct Placement {
        uint64_t placementId{0};
        uint64_t materializationId{0};
        uint64_t mappingRevision{0};
        double timelineStartSeconds{0.0};
        double durationSeconds{0.0};
        float gain{1.0f};
        double fadeInDuration{0.0};
        double fadeOutDuration{0.0};
        double clipInSeconds{0.0};          // Trim start offset in source materialization (0 = start from beginning)
        juce::String name;
        bool isRetired{false};

        uint64_t referencePlacementId{0};        // 0 = 无参考
        int64_t  referenceBindingRevision{0};     // 每次 binding 变更 +1

        bool isValid() const noexcept
        {
            return placementId != 0 && materializationId != 0 && durationSeconds > 0.0;
        }

        double timelineEndSeconds() const noexcept
        {
            return timelineStartSeconds + durationSeconds;
        }

    };

    // 单条轨道：包含 Placement 列表、选择状态、混音参数
    struct Track {
        std::vector<Placement> placements;
        uint64_t selectedPlacementId{0};
        bool isMuted{false};
        bool isSolo{false};
        float volume{1.0f};
        juce::String name;
        juce::Colour colour;
        std::atomic<float> currentRmsDb{-100.0f};
    };

    // 音频线程消费的轻量放置投影 — 仅含播放必需的数字字段，不含 String/UI/业务字段
    struct PlaybackPlacement {
        uint64_t materializationId{0};
        double timelineStartSeconds{0.0};
        double durationSeconds{0.0};
        double clipInSeconds{0.0};
        float gain{1.0f};
        double fadeInDuration{0.0};
        double fadeOutDuration{0.0};
    };

    // 音频线程消费的轨道快照（不可变）
    struct PlaybackTrack {
        bool isMuted{false};
        bool isSolo{false};
        float volume{1.0f};
        std::vector<PlaybackPlacement> placements;
    };

    // 完整的播放快照，由音频线程通过 loadPlaybackSnapshot() 获取
    struct PlaybackSnapshot {
        uint64_t epoch{0};
        bool anySoloed{false};
        std::array<PlaybackTrack, kTrackCount> tracks;
    };

    using PlaybackSnapshotHandle = std::shared_ptr<const PlaybackSnapshot>;

    StandaloneArrangement();
    ~StandaloneArrangement();

    StandaloneArrangement(const StandaloneArrangement&) = delete;
    StandaloneArrangement& operator=(const StandaloneArrangement&) = delete;

    PlaybackSnapshotHandle loadPlaybackSnapshot() const;

    int getNumTracks() const noexcept { return kTrackCount; }

    int getActiveTrackId() const;
    bool setActiveTrack(int trackId);

    bool setTrackMuted(int trackId, bool muted);
    bool isTrackMuted(int trackId) const;
    bool setTrackSolo(int trackId, bool solo);
    bool isTrackSolo(int trackId) const;
    bool setTrackVolume(int trackId, float volume);
    float getTrackVolume(int trackId) const;
    // 删除轨道并将后续轨道上移填补空位，清空最后一个可见槽位
    // visibleCount: 当前可见轨道数（调用方负责之后减少 visibleTrackCount）
    bool removeTrackAndShift(int trackId, int visibleCount);

    bool setTrackColour(int trackId, juce::Colour colour);
    juce::Colour getTrackColour(int trackId) const;
    float getTrackRmsDb(int trackId) const;
    bool setTrackRmsDb(int trackId, float rmsDb);

    int getNumPlacements(int trackId) const;
    uint64_t getPlacementId(int trackId, int placementIndex) const;
    int findPlacementIndexById(int trackId, uint64_t placementId) const;
    bool getPlacementByIndex(int trackId, int placementIndex, Placement& out) const;
    bool getPlacementById(int trackId, uint64_t placementId, Placement& out) const;
    uint64_t getSelectedPlacementId(int trackId) const;
    int getSelectedPlacementIndex(int trackId) const;
    bool selectPlacement(int trackId, uint64_t placementId);
    bool setSelectedPlacementIndex(int trackId, int placementIndex);
    void clear();

    bool insertPlacement(int trackId, Placement& placement);
    bool insertPlacement(int trackId, int insertIndex, Placement& placement);
    bool deletePlacementById(int trackId, uint64_t placementId, Placement* deletedOut = nullptr, int* deletedIndexOut = nullptr);
    bool movePlacementToTrack(int sourceTrackId, int targetTrackId, uint64_t placementId, double newTimelineStartSeconds);
    bool setPlacementTimelineStartSeconds(int trackId, uint64_t placementId, double timelineStartSeconds);
    bool setPlacementGain(int trackId, uint64_t placementId, float gain);
    bool setPlacementTrim(int trackId, uint64_t placementId, double clipInSeconds, double durationSeconds);
    bool setPlacementFade(int trackId, uint64_t placementId, double fadeInDuration, double fadeOutDuration);
    // 一次写锁内完成 trim + timelineStart 更新，只发布一次快照
    bool setPlacementTrimAndTimelineStart(int trackId, uint64_t placementId,
                                          double clipInSeconds, double durationSeconds,
                                          double timelineStartSeconds);

    // 软删除/恢复接口，供 UndoAction 和垃圾回收使用
    bool retirePlacement(int trackId, uint64_t placementId);
    bool revivePlacement(int trackId, uint64_t placementId);
    bool referencesMaterializationAnyState(uint64_t materializationId) const;

    // 已 retire 的 Placement 信息，用于垃圾回收判断
    struct RetiredPlacementEntry { int trackId; uint64_t placementId; uint64_t materializationId; };
    std::vector<RetiredPlacementEntry> getRetiredPlacements() const;

    // ============================================================================
    // Reference binding API
    // ============================================================================
    bool setPlacementReferencePlacement(int trackId, uint64_t targetPlacementId, uint64_t referencePlacementId);
    bool clearPlacementReferencePlacement(int trackId, uint64_t targetPlacementId);
    uint64_t getPlacementReferencePlacement(int trackId, uint64_t targetPlacementId) const;
    bool isCyclicReference(int trackId, uint64_t targetPlacementId, uint64_t candidateReferenceId) const;

private:
    static bool isValidTrackId(int trackId) noexcept;
    static int selectIndexAfterErase(int currentSelectedIndex, int erasedIndex, int remainingCount);
    bool placementIdExistsUnlocked(uint64_t placementId) const;
    int findPlacementIndexUnlocked(int trackId, uint64_t placementId) const;
    void refreshSelectedPlacementUnlocked(int trackId, int preferredIndex);
    void publishPlaybackSnapshotLocked();

    // Reference binding helpers (要求调用方已持有 stateLock_ 写锁)
    bool findPlacementByIdGlobalUnlocked(uint64_t placementId, int& outTrackId, size_t& outIndex) const;
    bool isCyclicReferenceUnlocked(int trackId, uint64_t targetPlacementId, uint64_t candidateReferenceId) const;
    static bool placementsOverlap(const Placement& target, const Placement& reference) noexcept;
    static bool clearReferenceBindingUnlocked(Placement& target) noexcept;
    void checkOverlapAndClearReferenceUnlocked(int trackId, uint64_t targetPlacementId);
    void clearInboundReferencesToPlacementUnlocked(uint64_t referencePlacementId);
    void clearInvalidInboundReferencesToPlacementUnlocked(uint64_t referencePlacementId);

    mutable juce::ReadWriteLock stateLock_;
    std::array<Track, kTrackCount> tracks_;
    int activeTrackId_{0};
    uint64_t nextPlaybackEpoch_{1};
    uint64_t nextPlacementId_{1};

    PlaybackSnapshotHandle playbackSnapshot_;

    // Writer-side delayed destruction: retired snapshots are held until
    // use_count()==1 (no audio-thread references remain), then swept on the
    // writer thread. This guarantees free/malloc never hits the RT path.
    mutable std::vector<PlaybackSnapshotHandle> retiredSnapshots_;
};

} // namespace OpenTune
