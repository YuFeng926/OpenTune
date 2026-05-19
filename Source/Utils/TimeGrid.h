/**
 * TimeGrid — 单 clip 多 handle 时间映射 COW 数据结构。
 *
 * 用于支持 vocal time-stretch 功能。设计冻结于:
 *   research/p0_time_stretch/DESIGN.md v7
 *   openspec/changes/vocal-time-stretch/specs/time-grid/spec.md
 *
 * 不变量:
 *   1. handles[0].kind == ClipStart, locked == true, output_seconds == source_seconds == 0
 *   2. handles[N-1].kind == ClipEnd, locked == true
 *   3. source_seconds 严格递增 (i < j ⇒ handles[i].source_seconds < handles[j].source_seconds)
 *   4. output_seconds 严格递增
 *   5. 总时长守恒: handles[N-1].output_seconds == handles[N-1].source_seconds
 *      (端点锁定 → ARA region 长度恒等于 source 长度)
 *
 * 线程模型: COW snapshot 模式
 *   - 写者: clone snapshot → 修改 → atomic_store
 *   - 读者: atomic_load 拿 shared_ptr<const TimeGridSnapshot>，无锁读
 *
 * 与现有 PitchCurve 的协作: 平级在 MaterializationStore 内,
 * 编辑 TimeGrid 不让 PitchCache 失效 (Stage 2 全重建,但 NSF 不重跑)。
 */
#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace OpenTune {

enum class HandleKind : uint8_t {
    ClipStart = 0,      // 永远 locked, output==source==0
    ClipEnd = 1,        // 永远 locked
    OnsetVoiced = 2,    // WordSegmenter Tier 1: 进入 Voiced 段的边界
    OnsetSibilant = 3,  // WordSegmenter Tier 1: 进入 Sibilant 段的边界
    OnsetSilence = 4,   // WordSegmenter Tier 1: 进入 Silence 段的边界
    InternalOnset = 5,  // WordSegmenter Tier 2: 同类内瞬态(OnsetDetector)
    UserAdded = 6,      // 用户手动双击插入
    NoteOnly = 7        // HandleNoteMerger Pass 2: note 边界失配补位 (单源信号)
};

// Confidence 标识 handle 的播种置信度。
// Default: 单源播种 (仅 WordSegmenter / 仅 GameNoteGenerator) 或用户手动添加。
// High: 双源命中 — WordSegmenter handle 与 GameNoteGenerator note 边界 ±50ms 对齐。
// Once-confirmed-always: 一旦标记为 High，永远不变 (与 source_seconds 同 immutable)。
enum class Confidence : uint8_t {
    Default = 0,
    High = 1
};

struct TimeHandle {
    uint64_t   id{0};                  // 稳定 ID, undo/redo 跨 snapshot 跟踪
    double     source_seconds{0.0};    // 源时间锚, 一旦插入即不可变
    double     output_seconds{0.0};    // 输出时间位置, 用户编辑唯一目标
    HandleKind kind{HandleKind::UserAdded};
    bool       locked{false};          // ClipStart/ClipEnd 强制 true
    Confidence confidence{Confidence::Default}; // 播种置信度, 与 source_seconds 同 immutable
};

/**
 * TimeGridSnapshot — 不可变 COW 快照
 *
 * 通过工厂方法创建以保证不变量。直接修改字段是 undefined behavior。
 */
class TimeGridSnapshot {
public:
    TimeGridSnapshot() = default;

    /**
     * 构造一个恒等映射 snapshot (output==source 全部 handle, τ=identity)。
     * 仅含 ClipStart + ClipEnd 两个端点.
     *
     * @param totalDurationSeconds clip 源时间总长 (== output 总长)
     */
    static std::shared_ptr<const TimeGridSnapshot> makeIdentity(double totalDurationSeconds);

    /**
     * 构造一个 snapshot 从 handles 列表(必须已经按 source_seconds 升序、含 ClipStart/End)。
     * 失败返回 nullptr,日志记录失败原因。
     */
    static std::shared_ptr<const TimeGridSnapshot> makeFromHandles(std::vector<TimeHandle> handles,
                                                                   uint64_t revision = 1);

    /**
     * 验证不变量。
     * 失败时通过 outError 返回详细信息(空字符串表示成功)。
     */
    static bool validate(const std::vector<TimeHandle>& handles, juce::String& outError);

    const std::vector<TimeHandle>& handles() const noexcept { return handles_; }
    uint64_t revision() const noexcept { return revision_; }
    bool empty() const noexcept { return handles_.size() < 2; }

    /**
     * 总输出时长 = 总源时长 (端点锁定的不变量).
     */
    double totalDurationSeconds() const noexcept;

    /**
     * 段内时间映射查询 (TimeWarpCurve API 提供).
     * 这两个查询在 handle 锚点上是位精确的 (不是 epsilon 比较):
     *   tauForward(handles[i].source_seconds) == handles[i].output_seconds
     *
     * 对超出端点范围的查询: clamp 到 [0, totalDurationSeconds].
     */
    double tauForward(double sourceSeconds) const noexcept;
    double tauInverse(double outputSeconds) const noexcept;

    /**
     * 检查 [t_src_a, t_src_b] 区间内是否所有 handle 的 output_seconds == source_seconds.
     * 用于 Stage 2 bypass 判定。
     * 时间复杂度: O(log H + K)，K = 区间内 handle 数 (典型 <10)。
     */
    bool isIdentityInRange(double t_src_a, double t_src_b) const noexcept;

    /**
     * 检查整个 snapshot 是否为恒等映射 (所有 handle output==source).
     * O(N).
     */
    bool isIdentity() const noexcept;

private:
    explicit TimeGridSnapshot(std::vector<TimeHandle> h, uint64_t rev)
        : handles_(std::move(h)), revision_(rev) {}

    std::vector<TimeHandle> handles_;
    uint64_t                revision_{0};
};

/**
 * TimeGrid — 持有当前 snapshot 的薄包装器.
 *
 * 支持原子 publish/load. 与 PitchCurve 相同模式.
 */
class TimeGrid {
public:
    TimeGrid();
    explicit TimeGrid(std::shared_ptr<const TimeGridSnapshot> initial);

    std::shared_ptr<const TimeGridSnapshot> getSnapshot() const noexcept;
    void setSnapshot(std::shared_ptr<const TimeGridSnapshot> newSnapshot) noexcept;

private:
    std::shared_ptr<const TimeGridSnapshot> snapshot_;
};

} // namespace OpenTune
