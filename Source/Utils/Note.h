#pragma once

/**
 * 音符和音符序列数据结构
 * 
 * 定义音频处理中使用的核心数据结构：
 * - Note：单个音符的时间、音高、颤音参数等
 * - NoteSequence：音符序列管理，支持插入、删除、查询等操作
 * - LineAnchor：音高线锚点，用于手绘F0曲线
 */

#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>
#include <optional>

#include "NoteEqSettings.h"

namespace OpenTune {

struct Note {
    double startTime = 0.0;         // 起始时间（秒）
    double endTime = 0.0;           // 结束时间（秒）
    float pitch = 0.0f;             // 基准音高 (Hz)
    float originalPitch = 0.0f;     // 当前 Effective F0 域 correction anchor (Hz)，已含全局 pitchRatio
    float pitchOffset = 0.0f;       // 音高偏移（半音），用于拖拽调整
    float retuneSpeed = -1.0f;      // 重调速度（-1表示使用默认值）
    float vibratoDepth = -1.0f;     // 颤音深度（-1表示使用默认值）
    float vibratoRate = -1.0f;      // 颤音速率（-1表示使用默认值）
    float outputGainDb = 0.0f;     // 旧工程兼容字段；由 VolumeEnvelope 在 note.startTime 处派生
    float pitchDriftScale = 1.0f;    // 漂移修正比例（1.0=原始漂移，0.0=消除漂移，负值=反转，可超出±100%）
    bool isVoiced = true;           // 是否为有声段
    bool dirty = false;             // 脏标记，用于增量渲染
    std::optional<EqSettings> eq;   // Per-note EQ：nullopt = 无 EQ；active=false = 保留设置但全局旁通

    double getDuration() const {
        return endTime - startTime;
    }

    float getAdjustedPitch() const {
        if (pitch <= 0.0f) return 0.0f;
        return pitch * std::pow(2.0f, pitchOffset / 12.0f);
    }
};

class NoteSequence {
public:
    NoteSequence() = default;
    ~NoteSequence() = default;

    void insertNoteSorted(const Note& note) {
        if (note.endTime <= note.startTime) {
            return;
        }
        notes_.push_back(note);
        normalizeNonOverlapping(notes_);
    }

    void setNotesSorted(const std::vector<Note>& notes) {
        notes_ = notes;
        normalizeNonOverlapping(notes_);
    }

    void clear() {
        notes_.clear();
    }

    const std::vector<Note>& getNotes() const {
        return notes_;
    }

    std::vector<Note>& getNotes() {
        return notes_;
    }

    size_t size() const {
        return notes_.size();
    }

    bool isEmpty() const {
        return notes_.empty();
    }

    void eraseRange(double startTime, double endTime) {
        if (endTime < startTime) {
            std::swap(startTime, endTime);
        }
        if (endTime <= startTime) {
            return;
        }

        std::vector<Note> updated;
        updated.reserve(notes_.size() + 2);

        for (const auto& note : notes_) {
            bool overlap = (note.endTime > startTime && note.startTime < endTime);
            if (!overlap) {
                updated.push_back(note);
                continue;
            }

            if (note.startTime < startTime) {
                Note left = note;
                left.endTime = startTime;
                left.dirty = true;
                if (left.endTime > left.startTime) {
                    updated.push_back(left);
                }
            }

            if (note.endTime > endTime) {
                Note right = note;
                right.startTime = endTime;
                right.dirty = true;
                if (right.endTime > right.startTime) {
                    updated.push_back(right);
                }
            }
        }

        notes_ = std::move(updated);
    }

private:
    static void normalizeNonOverlapping(std::vector<Note>& notes) {
        notes.erase(
            std::remove_if(notes.begin(), notes.end(), [](const Note& n) {
                return n.endTime <= n.startTime;
            }),
            notes.end());

        std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
            return (a.startTime < b.startTime) || (a.startTime == b.startTime && a.endTime < b.endTime);
        });

        for (size_t i = 1; i < notes.size(); ++i) {
            if (notes[i - 1].endTime > notes[i].startTime) {
                notes[i - 1].endTime = notes[i].startTime;
            }
        }

        notes.erase(
            std::remove_if(notes.begin(), notes.end(), [](const Note& n) {
                return n.endTime <= n.startTime;
            }),
            notes.end());
    }

    std::vector<Note> notes_;
};

struct LineAnchor {
    int id = 0;             // 锚点ID
    double time = 0.0;      // 时间位置（秒）
    float freq = 0.0f;      // 频率 (Hz)
    bool selected = false;  // 选中状态
};

// 标准化存储的 notes：去重叠、排序、去零时长 note
inline std::vector<Note> normalizeStoredNotes(const std::vector<Note>& notes)
{
    NoteSequence sequence;
    sequence.setNotesSorted(notes);
    return sequence.getNotes();
}

// ============================================================================
// 粘贴拓扑计划
// ============================================================================

struct NoteRangeSeconds {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
};

// 粘贴事务的拓扑计划：合并原音符与已定位的粘贴音符并归一化，计算 affected range
// 与 before/after patch 内容（与 ContentNoteRangePatch 同构的纯领域数据，不依赖 JUCE）。
//
// affected range 以粘贴包络（pastedNotes.front() 初始化）为种子，再纳入所有与包络
// 严格相交的原音符完整 start/end：normalizeStoredNotes 会把相交原音符截短到包络边界
// （如 A=[0,10] 遇粘贴 B=[5,8] → A'=[0,5]），range 必须覆盖截短前后的完整区间，
// before/after patch 才对称，Undo/Redo 才可逆。原音符已标准化且互不重叠，
// 单遍折叠即可，无需迭代扩展。
struct PasteTopologyPlan {
    NoteRangeSeconds affectedRange;
    std::vector<Note> beforeNotesInRange;   // 原音符 ∩ affectedRange
    std::vector<Note> afterNotesInRange;    // 归一化合并结果 ∩ affectedRange
};

// 前置条件：pastedNotes 非空（调用方 pasteNotes 已在过滤空剪贴板后调用）。
inline PasteTopologyPlan planPasteTopology(const std::vector<Note>& originalNotes,
                                           const std::vector<Note>& pastedNotes)
{
    std::vector<Note> merged = originalNotes;
    merged.insert(merged.end(), pastedNotes.begin(), pastedNotes.end());
    merged = normalizeStoredNotes(merged);

    PasteTopologyPlan plan;
    plan.affectedRange.startSeconds = pastedNotes.front().startTime;
    plan.affectedRange.endSeconds = pastedNotes.front().endTime;
    for (const auto& n : pastedNotes) {
        plan.affectedRange.startSeconds = std::min(plan.affectedRange.startSeconds, n.startTime);
        plan.affectedRange.endSeconds = std::max(plan.affectedRange.endSeconds, n.endTime);
    }
    for (const auto& n : originalNotes) {
        if (n.endTime > plan.affectedRange.startSeconds && n.startTime < plan.affectedRange.endSeconds) {
            plan.affectedRange.startSeconds = std::min(plan.affectedRange.startSeconds, n.startTime);
            plan.affectedRange.endSeconds = std::max(plan.affectedRange.endSeconds, n.endTime);
        }
    }
    for (const auto& n : originalNotes) {
        if (n.endTime > plan.affectedRange.startSeconds && n.startTime < plan.affectedRange.endSeconds)
            plan.beforeNotesInRange.push_back(n);
    }
    for (const auto& n : merged) {
        if (n.endTime > plan.affectedRange.startSeconds && n.startTime < plan.affectedRange.endSeconds)
            plan.afterNotesInRange.push_back(n);
    }
    return plan;
}

// 秒域 range merge：keptBefore + afterNotesInRange + keptAfter，标准化后返回。
// Note 拓扑 patch 的唯一 range merge 语义（时间有序，range 边界处无重叠）。
inline std::vector<Note> mergeNotesRange(const std::vector<Note>& existing,
                                         const NoteRangeSeconds& affectedRange,
                                         const std::vector<Note>& afterNotesInRange)
{
    std::vector<Note> mergedNotes;
    mergedNotes.reserve(existing.size() + afterNotesInRange.size());

    for (const auto& note : existing) {
        if (note.endTime <= affectedRange.startSeconds)
            mergedNotes.push_back(note);
    }

    mergedNotes.insert(mergedNotes.end(),
                       afterNotesInRange.begin(),
                       afterNotesInRange.end());

    for (const auto& note : existing) {
        if (note.startTime >= affectedRange.endSeconds)
            mergedNotes.push_back(note);
    }

    return normalizeStoredNotes(std::move(mergedNotes));
}

} // namespace OpenTune
