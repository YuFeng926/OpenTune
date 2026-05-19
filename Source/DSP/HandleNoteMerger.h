/**
 * HandleNoteMerger — TimeGrid handle + Note merge pass
 *
 * 设计冻结于:
 *   openspec/changes/add-note-confirmed-handles/specs/time-grid-note-confirmation/spec.md
 *   openspec/changes/add-note-confirmed-handles/design.md
 *
 * 用途: 在 import 阶段 WordSegmenter (handles) + GameNoteGenerator (notes) 都到达后
 *       做一次 barrier merge，按 50ms 阈值做"双重确认 + 失配补位":
 *   - Pass 1: WordSegmenter handle 与 note 边界 ±50ms 内重合 → confidence=High
 *   - Pass 2: 失配 note 边界（无附近 handle）→ 插入 NoteOnly handle (Default conf)
 *
 * Barrier 触发: 顺序无关——先到的 producer 缓存等待，两者都到 → 触发 merge。
 *
 * Manual re-seed: 用户右键"Re-seed handles from notes"调 reSeed() 重新计算。
 *
 * 算法纯函数: 同输入产同输出（除 stable id 生成外），无随机性、无外部状态。
 */
#pragma once

#include "../Utils/TimeGrid.h"
#include "../Utils/Note.h"
#include <functional>
#include <optional>
#include <vector>

namespace OpenTune {

class HandleNoteMerger {
public:
    // Match threshold (50 ms). Compile-time constant per spec — not user-configurable.
    static constexpr double kMatchThresholdSeconds = 0.050;

    // Min segment between handles after merge (matches TimeGrid 30ms physical lower bound).
    static constexpr double kMinSegmentSeconds = 0.030;

    HandleNoteMerger() = default;

    // ============================================================================
    // Barrier API: producers 各自异步完成时 deliver；两者都到 → onMergeComplete 触发
    // ============================================================================

    /// WordSegmenter 完成时调用，发送 phoneme-onset handles 列表。
    /// 必须含 ClipStart + ClipEnd（valid TimeGridSnapshot 入口要求）。
    void deliverHandles(std::vector<TimeHandle> handles);

    /// GameNoteGenerator 完成时调用，发送 notes 列表。
    void deliverNotes(std::vector<Note> notes);

    /// 当 handles + notes 都到达时被回调一次，传入 merge 结果。
    /// 调用方负责将结果包装成 TimeGridSnapshot 并 publish。
    std::function<void(std::vector<TimeHandle>)> onMergeComplete;

    /// Reset barrier state (e.g. when clip 卸载或重新 import)。
    void reset();

    /// 当前是否已收到 handles（debug helpers）。
    bool hasHandles() const noexcept { return cachedHandles_.has_value(); }
    bool hasNotes() const noexcept { return cachedNotes_.has_value(); }

    // ============================================================================
    // 算法 API: 纯函数，可独立测试
    // ============================================================================

    /// Initial merge pass: WordSegmenter handles + GameNoteGenerator notes → merged handles.
    ///
    /// 算法:
    ///   Pass 1: 对每个非 endpoint handle h，找最近 note nb |nb.startTime - h.source| <= T_match
    ///           最近者 → h.confidence = High，note 标记为 matched
    ///   Pass 2: 对未 matched 的 note，在 [ClipStart+T_match, ClipEnd-T_match] 内
    ///           且与已有最近 handle 距离 >= 30ms 时插入 NoteOnly handle (Default)
    ///
    /// 输入 handles 必须满足 TimeGridSnapshot invariants（ClipStart 在前 + ClipEnd 在后 + 严格递增）。
    /// 返回值仍满足 invariants。
    static std::vector<TimeHandle> merge(const std::vector<TimeHandle>& handlesPre,
                                          const std::vector<Note>& notes);

    /// Manual re-seed: 基于当前 TimeGridSnapshot 重新计算。保留 UserAdded 与
    /// 用户编辑过的 output_seconds，丢弃旧 NoteOnly handle 与所有 confidence 标签。
    ///
    /// 实现:
    ///   - 取 currentSnapshot 中的 UserAdded handles → 完整保留
    ///   - 对每个非 UserAdded auto handle，如 source_seconds 仍在 handlesPre 中 → 保留 output_seconds
    ///   - 否则丢弃
    ///   - 然后跑 merge 算法 on (剩余 auto handles + UserAdded) ∪ notes
    static std::vector<TimeHandle> reSeed(const TimeGridSnapshot& currentSnapshot,
                                           const std::vector<TimeHandle>& handlesPre,
                                           const std::vector<Note>& notes);

private:
    std::optional<std::vector<TimeHandle>> cachedHandles_;
    std::optional<std::vector<Note>>       cachedNotes_;

    void tryFireMerge();
};

} // namespace OpenTune
