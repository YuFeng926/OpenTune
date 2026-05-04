/**
 * Standalone 编曲视图数据模型（StandaloneArrangement）
 *
 * 管理 Standalone 模式下的多轨时间轴：Track、Placement 与播放快照。
 * 每个 Placement 引用一个 Materialization，并在时间轴上确定起始位置和时长。
 * 本类不持有音频数据本身——音频编辑内容由 MaterializationStore 管理。
 *
 * 线程安全：stateLock_ (ReadWriteLock) 保护所有 Track/Placement 状态；
 *          snapshotLock_ (SpinLock) 保护音频线程消费的 PlaybackSnapshot。
 * 设计：音频线程只通过 loadPlaybackSnapshot() 读取不可变快照，零锁竞争。
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace OpenTune {

class StandaloneArrangement {
public:
    static constexpr int kTrackCount = 12;

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
        juce::String name;
        juce::Colour colour;
        bool isRetired{false};

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

    // 音频线程消费的轨道快照（不可变）
    struct PlaybackTrack {
        bool isMuted{false};
        bool isSolo{false};
        float volume{1.0f};
        std::vector<Placement> placements;
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

    // 软删除/恢复接口，供 UndoAction 和垃圾回收使用
    bool retirePlacement(int trackId, uint64_t placementId);
    bool revivePlacement(int trackId, uint64_t placementId);
    bool referencesMaterializationAnyState(uint64_t materializationId) const;

    // 已 retire 的 Placement 信息，用于垃圾回收判断
    struct RetiredPlacementEntry { int trackId; uint64_t placementId; uint64_t materializationId; };
    std::vector<RetiredPlacementEntry> getRetiredPlacements() const;

private:
    static bool isValidTrackId(int trackId) noexcept;
    static int selectIndexAfterErase(int currentSelectedIndex, int erasedIndex, int remainingCount);
    bool placementIdExistsUnlocked(uint64_t placementId) const;
    int findPlacementIndexUnlocked(int trackId, uint64_t placementId) const;
    void refreshSelectedPlacementUnlocked(int trackId, int preferredIndex);
    void publishPlaybackSnapshotLocked();

    mutable juce::ReadWriteLock stateLock_;
    std::array<Track, kTrackCount> tracks_;
    int activeTrackId_{0};
    uint64_t nextPlaybackEpoch_{1};
    uint64_t nextPlacementId_{1};

    mutable juce::SpinLock snapshotLock_;
    PlaybackSnapshotHandle playbackSnapshot_;
};

} // namespace OpenTune
