#include "PianoRollUndoSupport.h"
#include "../../../Utils/AppLogger.h"
#include <cmath>

namespace OpenTune {

// ============================================================================
// PianoRollUndoSupport - 钢琴卷帘撤销支持实现
// ============================================================================

namespace {
    static bool nearlyEqualFloat(float a, float b, float epsilon = 1e-6f)
    // 浮点数近似相等判断
    {
        if (std::abs(a - b) < epsilon) return true;
        if (std::abs(a) < epsilon && std::abs(b) < epsilon) return true;
        return false;
    }
}

bool SegmentSnapshotCompare::equivalent(const CorrectedSegment& a, const CorrectedSegment& b)
// 比较两个修正段是否等价（帧范围、数据源、颤音参数、F0数据）
{
    if (a.startFrame != b.startFrame) return false;
    if (a.endFrame != b.endFrame) return false;
    if (a.source != b.source) return false;
    if (!nearlyEqualFloat(a.retuneSpeed, b.retuneSpeed)) return false;
    if (!nearlyEqualFloat(a.vibratoDepth, b.vibratoDepth)) return false;
    if (!nearlyEqualFloat(a.vibratoRate, b.vibratoRate)) return false;
    if (a.f0Data.size() != b.f0Data.size()) return false;
    for (size_t i = 0; i < a.f0Data.size(); ++i) {
        if (!nearlyEqualFloat(a.f0Data[i], b.f0Data[i])) return false;
    }
    return true;
}

bool SegmentSnapshotCompare::listsEquivalent(const std::vector<CorrectedSegment>& a, const std::vector<CorrectedSegment>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!equivalent(a[i], b[i])) return false;
    }
    return true;
}

bool SegmentSnapshotCompare::equivalent(const CorrectedSegmentsChangeAction::SegmentSnapshot& a,
                                        const CorrectedSegmentsChangeAction::SegmentSnapshot& b)
{
    if (a.startFrame != b.startFrame) return false;
    if (a.endFrame != b.endFrame) return false;
    if (a.source != b.source) return false;
    if (!nearlyEqualFloat(a.retuneSpeed, b.retuneSpeed)) return false;
    if (!nearlyEqualFloat(a.vibratoDepth, b.vibratoDepth)) return false;
    if (!nearlyEqualFloat(a.vibratoRate, b.vibratoRate)) return false;
    if (a.f0Data.size() != b.f0Data.size()) return false;
    for (size_t i = 0; i < a.f0Data.size(); ++i) {
        if (!nearlyEqualFloat(a.f0Data[i], b.f0Data[i])) return false;
    }
    return true;
}

bool SegmentSnapshotCompare::listsEquivalent(const std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot>& a,
                                             const std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!equivalent(a[i], b[i])) return false;
    }
    return true;
}

PianoRollUndoSupport::PianoRollUndoSupport(Context context)
    : ctx_(std::move(context))
{
    AppLogger::debug("[PianoRollUndoSupport] Created");
}

UndoManager* PianoRollUndoSupport::getCurrentUndoManager() noexcept
{
    return ctx_.getUndoManager ? ctx_.getUndoManager() : nullptr;
}

const UndoManager* PianoRollUndoSupport::getCurrentUndoManager() const noexcept
{
    return ctx_.getUndoManager ? ctx_.getUndoManager() : nullptr;
}

bool PianoRollUndoSupport::canUndo() const noexcept
{
    auto* um = getCurrentUndoManager();
    return um != nullptr && um->canUndo();
}

bool PianoRollUndoSupport::canRedo() const noexcept
{
    auto* um = getCurrentUndoManager();
    return um != nullptr && um->canRedo();
}

bool PianoRollUndoSupport::nearlyEqualFloat(float a, float b, float epsilon)
{
    if (std::abs(a - b) < epsilon) return true;
    if (std::abs(a) < epsilon && std::abs(b) < epsilon) return true;
    return false;
}

bool PianoRollUndoSupport::notesEquivalent(const std::vector<Note>& a, const std::vector<Note>& b)
// 比较两个音符列表是否等价（时间、音高、偏移、颤音参数、力度）
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto& x = a[i];
        const auto& y = b[i];
        if (x.startTime != y.startTime) return false;
        if (x.endTime != y.endTime) return false;
        if (!nearlyEqualFloat(x.pitch, y.pitch)) return false;
        if (!nearlyEqualFloat(x.originalPitch, y.originalPitch)) return false;
        if (!nearlyEqualFloat(x.pitchOffset, y.pitchOffset)) return false;
        if (!nearlyEqualFloat(x.retuneSpeed, y.retuneSpeed)) return false;
        if (!nearlyEqualFloat(x.vibratoDepth, y.vibratoDepth)) return false;
        if (!nearlyEqualFloat(x.vibratoRate, y.vibratoRate)) return false;
        if (!nearlyEqualFloat(x.velocity, y.velocity)) return false;
        if (x.isVoiced != y.isVoiced) return false;
    }
    return true;
}

void PianoRollUndoSupport::beginNotesEditUndo()
// 开始音符编辑撤销事务：保存当前音符和F0修正状态
{
    if (notesEditUndoActive_) {
        AppLogger::debug("[PianoRollUndoSupport] beginNotesEditUndo: already active, skipping");
        return;
    }

    if (ctx_.getNotesCopy) {
        notesEditUndoBefore_ = ctx_.getNotesCopy();
    }

    notesEditF0Before_.clear();
    auto curve = ctx_.getPitchCurve ? ctx_.getPitchCurve() : nullptr;
    if (curve) {
        auto snapshot = curve->getSnapshot();
        const auto& segments = snapshot->getCorrectedSegments();
        notesEditF0Before_.reserve(segments.size());
        for (const auto& seg : segments) {
            notesEditF0Before_.emplace_back(seg);
        }
        AppLogger::debug("[PianoRollUndoSupport] beginNotesEditUndo: captured " + 
            juce::String(notesEditUndoBefore_.size()) + " notes and " +
            juce::String(notesEditF0Before_.size()) + " f0 segments");
    } else {
        AppLogger::debug("[PianoRollUndoSupport] beginNotesEditUndo: no pitch curve available");
    }

    notesEditUndoActive_ = true;
}

void PianoRollUndoSupport::setNotesEditUndoState(std::vector<Note> notesBefore,
                                                   std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot> f0Before)
{
    AppLogger::debug("[PianoRollUndoSupport] setNotesEditUndoState: notes=" + juce::String(static_cast<int>(notesBefore.size()))
        + ", f0Segments=" + juce::String(static_cast<int>(f0Before.size())));
    notesEditUndoBefore_ = std::move(notesBefore);
    notesEditF0Before_ = std::move(f0Before);
    notesEditUndoActive_ = true;
}

bool PianoRollUndoSupport::isNotesEditUndoActive() const noexcept
{
    return notesEditUndoActive_;
}

void PianoRollUndoSupport::commitNotesEditUndo(const juce::String& description)
// 提交音符编辑撤销事务：比较前后状态，创建撤销动作
{
    if (!notesEditUndoActive_) {
        AppLogger::debug("[PianoRollUndoSupport] commitNotesEditUndo: not active, skipping");
        return;
    }

    auto after = ctx_.getNotesCopy ? ctx_.getNotesCopy() : std::vector<Note>();
    bool notesChanged = !notesEquivalent(notesEditUndoBefore_, after);

    bool f0Changed = false;
    std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot> f0After;
    auto curve = ctx_.getPitchCurve ? ctx_.getPitchCurve() : nullptr;
    if (curve) {
        auto snapshot = curve->getSnapshot();
        const auto& segments = snapshot->getCorrectedSegments();
        f0After.reserve(segments.size());
        for (const auto& seg : segments) {
            f0After.emplace_back(seg);
        }

        std::vector<CorrectedSegment> beforeSegs, afterSegs;
        beforeSegs.reserve(notesEditF0Before_.size());
        for (const auto& snap : notesEditF0Before_) {
            CorrectedSegment seg;
            seg.startFrame = snap.startFrame;
            seg.endFrame = snap.endFrame;
            seg.f0Data = snap.f0Data;
            seg.source = snap.source;
            seg.retuneSpeed = snap.retuneSpeed;
            seg.vibratoDepth = snap.vibratoDepth;
            seg.vibratoRate = snap.vibratoRate;
            beforeSegs.push_back(std::move(seg));
        }
        afterSegs.reserve(f0After.size());
        for (const auto& snap : f0After) {
            CorrectedSegment seg;
            seg.startFrame = snap.startFrame;
            seg.endFrame = snap.endFrame;
            seg.f0Data = snap.f0Data;
            seg.source = snap.source;
            seg.retuneSpeed = snap.retuneSpeed;
            seg.vibratoDepth = snap.vibratoDepth;
            seg.vibratoRate = snap.vibratoRate;
            afterSegs.push_back(std::move(seg));
        }
        f0Changed = !SegmentSnapshotCompare::listsEquivalent(beforeSegs, afterSegs);
    }

    AppLogger::debug("[PianoRollUndoSupport] commitNotesEditUndo: \"" + description + 
        "\", notesChanged=" + (notesChanged ? "true" : "false") +
        ", f0Changed=" + (f0Changed ? "true" : "false"));

    if (notesChanged || f0Changed) {
        auto notesApplier = [this](const std::vector<Note>& notes) {
            if (ctx_.getNotes) {
                ctx_.getNotes() = notes;
            }
        };

        if (notesChanged && f0Changed && curve) {
            auto compound = std::make_unique<CompoundUndoAction>(description);
            compound->addAction(
                std::make_unique<NotesChangeAction>(
                    notesEditUndoBefore_, after, notesApplier, description)
            );

            int affectedStart = 0;
            int affectedEnd = static_cast<int>(curve->size()) - 1;
            CorrectedSegmentsChangeAction::ChangeInfo info;
            info.affectedStartFrame = affectedStart;
            info.affectedEndFrame = affectedEnd;
            info.description = description;

            auto f0Applier = [this](const std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot>& snapshots) {
                auto c = ctx_.getPitchCurve ? ctx_.getPitchCurve() : nullptr;
                if (!c) return;
                c->clearAllCorrections();
                for (const auto& snap : snapshots) {
                    CorrectedSegment seg;
                    seg.startFrame = snap.startFrame;
                    seg.endFrame = snap.endFrame;
                    seg.f0Data = snap.f0Data;
                    seg.source = snap.source;
                    seg.retuneSpeed = snap.retuneSpeed;
                    seg.vibratoDepth = snap.vibratoDepth;
                    seg.vibratoRate = snap.vibratoRate;
                    c->restoreCorrectedSegment(seg);
                }
            };

            compound->addAction(
                std::make_unique<CorrectedSegmentsChangeAction>(
                    notesEditF0Before_, f0After, std::move(f0Applier), info)
            );
            if (auto* um = getCurrentUndoManager())
                um->addAction(std::move(compound));
        } else if (notesChanged) {
            if (auto* um = getCurrentUndoManager())
                um->addAction(
                    std::make_unique<NotesChangeAction>(
                        notesEditUndoBefore_, after, notesApplier, description)
                );
        } else if (f0Changed && curve) {
            int affectedStart = 0;
            int affectedEnd = static_cast<int>(curve->size()) - 1;
            CorrectedSegmentsChangeAction::ChangeInfo info;
            info.affectedStartFrame = affectedStart;
            info.affectedEndFrame = affectedEnd;
            info.description = description;

            auto f0Applier = [this](const std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot>& snapshots) {
                auto c = ctx_.getPitchCurve ? ctx_.getPitchCurve() : nullptr;
                if (!c) return;
                c->clearAllCorrections();
                for (const auto& snap : snapshots) {
                    CorrectedSegment seg;
                    seg.startFrame = snap.startFrame;
                    seg.endFrame = snap.endFrame;
                    seg.f0Data = snap.f0Data;
                    seg.source = snap.source;
                    seg.retuneSpeed = snap.retuneSpeed;
                    seg.vibratoDepth = snap.vibratoDepth;
                    seg.vibratoRate = snap.vibratoRate;
                    c->restoreCorrectedSegment(seg);
                }
            };

            if (auto* um = getCurrentUndoManager())
                um->addAction(
                    std::make_unique<CorrectedSegmentsChangeAction>(
                        notesEditF0Before_, f0After, std::move(f0Applier), info)
                );
        }
    }

    notesEditUndoBefore_.clear();
    notesEditF0Before_.clear();
    notesEditUndoActive_ = false;

    if (notesChanged && ctx_.notifyNotesChanged) {
        ctx_.notifyNotesChanged(after);
    }
}

void PianoRollUndoSupport::beginF0EditUndo(const juce::String& description)
// 开始F0编辑撤销事务：保存当前修正段状态
{
    if (f0EditUndoActive_) {
        AppLogger::debug("[PianoRollUndoSupport] beginF0EditUndo: already active, skipping");
        return;
    }

    auto curve = ctx_.getPitchCurve ? ctx_.getPitchCurve() : nullptr;
    if (!curve) {
        AppLogger::debug("[PianoRollUndoSupport] beginF0EditUndo: no pitch curve available");
        return;
    }

    {
        auto snapshot = curve->getSnapshot();
        f0EditUndoBefore_.clear();
        const auto& segments = snapshot->getCorrectedSegments();
        f0EditUndoBefore_.reserve(segments.size());
        for (const auto& seg : segments) {
            f0EditUndoBefore_.emplace_back(seg);
        }
        AppLogger::debug("[PianoRollUndoSupport] beginF0EditUndo: \"" + description + 
            "\", captured " + juce::String(f0EditUndoBefore_.size()) + " segments");
    }

    f0EditUndoDescription_ = description;
    f0EditUndoActive_ = true;
    f0EditStartFrame_ = -1;
    f0EditEndFrame_ = -1;
}

void PianoRollUndoSupport::commitF0EditUndo()
// 提交F0编辑撤销事务：比较前后修正段状态，创建撤销动作
{
    if (!f0EditUndoActive_) return;

    auto curve = ctx_.getPitchCurve ? ctx_.getPitchCurve() : nullptr;
    if (!curve) {
        f0EditUndoBefore_.clear();
        f0EditUndoActive_ = false;
        f0EditUndoDescription_.clear();
        return;
    }

    std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot> f0EditUndoAfter;
    {
        auto snapshot = curve->getSnapshot();
        const auto& segments = snapshot->getCorrectedSegments();
        f0EditUndoAfter.reserve(segments.size());
        for (const auto& seg : segments) {
            f0EditUndoAfter.emplace_back(seg);
        }
    }

    std::vector<CorrectedSegment> beforeSegs, afterSegs;
    beforeSegs.reserve(f0EditUndoBefore_.size());
    for (const auto& snap : f0EditUndoBefore_) {
        CorrectedSegment seg;
        seg.startFrame = snap.startFrame;
        seg.endFrame = snap.endFrame;
        seg.f0Data = snap.f0Data;
        seg.source = snap.source;
        seg.retuneSpeed = snap.retuneSpeed;
        seg.vibratoDepth = snap.vibratoDepth;
        seg.vibratoRate = snap.vibratoRate;
        beforeSegs.push_back(std::move(seg));
    }
    afterSegs.reserve(f0EditUndoAfter.size());
    for (const auto& snap : f0EditUndoAfter) {
        CorrectedSegment seg;
        seg.startFrame = snap.startFrame;
        seg.endFrame = snap.endFrame;
        seg.f0Data = snap.f0Data;
        seg.source = snap.source;
        seg.retuneSpeed = snap.retuneSpeed;
        seg.vibratoDepth = snap.vibratoDepth;
        seg.vibratoRate = snap.vibratoRate;
        afterSegs.push_back(std::move(seg));
    }
    bool hasChange = !SegmentSnapshotCompare::listsEquivalent(beforeSegs, afterSegs);

    AppLogger::debug("[PianoRollUndoSupport] commitF0EditUndo: \"" + f0EditUndoDescription_ + 
        "\", hasChange=" + (hasChange ? "true" : "false"));

    if (hasChange) {
        int affectedStartFrame = 0;
        int affectedEndFrame = static_cast<int>(curve->size()) - 1;
        if (f0EditStartFrame_ >= 0 && f0EditEndFrame_ >= 0) {
            affectedStartFrame = f0EditStartFrame_;
            affectedEndFrame = f0EditEndFrame_;
        }

        CorrectedSegmentsChangeAction::ChangeInfo info;
        info.affectedStartFrame = affectedStartFrame;
        info.affectedEndFrame = affectedEndFrame;
        info.description = f0EditUndoDescription_;

        auto f0Applier = [this](const std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot>& snapshots) {
            auto c = ctx_.getPitchCurve ? ctx_.getPitchCurve() : nullptr;
            if (!c) return;
            c->clearAllCorrections();
            for (const auto& snap : snapshots) {
                CorrectedSegment seg;
                seg.startFrame = snap.startFrame;
                seg.endFrame = snap.endFrame;
                seg.f0Data = snap.f0Data;
                seg.source = snap.source;
                seg.retuneSpeed = snap.retuneSpeed;
                seg.vibratoDepth = snap.vibratoDepth;
                seg.vibratoRate = snap.vibratoRate;
                c->restoreCorrectedSegment(seg);
            }
        };

        if (auto* um = getCurrentUndoManager())
            um->addAction(
                std::make_unique<CorrectedSegmentsChangeAction>(
                    f0EditUndoBefore_, f0EditUndoAfter, std::move(f0Applier), info)
            );
    }

    f0EditUndoBefore_.clear();
    f0EditUndoActive_ = false;
    f0EditUndoDescription_.clear();
    f0EditStartFrame_ = -1;
    f0EditEndFrame_ = -1;
}

void PianoRollUndoSupport::undo()
// 执行撤销：恢复上一状态并刷新界面
{
    auto* um = getCurrentUndoManager();
    if (um && um->canUndo()) {
        AppLogger::debug("[PianoRollUndoSupport] undo: performing undo");
        um->undo();
        if (ctx_.getNotes && ctx_.getNotesCopy) {
            ctx_.getNotes() = ctx_.getNotesCopy();
        }
        if (ctx_.updateScrollBars) {
            ctx_.updateScrollBars();
        }
        auto curve = ctx_.getPitchCurve ? ctx_.getPitchCurve() : nullptr;
        if (curve) {
            int affectedStartFrame = 0;
            int affectedEndFrame = static_cast<int>(curve->size()) - 1;
            if (ctx_.notifyPitchCurveEdited) {
                ctx_.notifyPitchCurveEdited(affectedStartFrame, affectedEndFrame);
            }
        }
        if (ctx_.requestRepaint) {
            ctx_.requestRepaint();
        }
    } else {
        AppLogger::debug("[PianoRollUndoSupport] undo: nothing to undo");
    }
}

void PianoRollUndoSupport::redo()
// 执行重做：恢复下一状态并刷新界面
{
    auto* um = getCurrentUndoManager();
    if (um && um->canRedo()) {
        AppLogger::debug("[PianoRollUndoSupport] redo: performing redo");
        um->redo();
        if (ctx_.getNotes && ctx_.getNotesCopy) {
            ctx_.getNotes() = ctx_.getNotesCopy();
        }
        if (ctx_.updateScrollBars) {
            ctx_.updateScrollBars();
        }
        auto curve = ctx_.getPitchCurve ? ctx_.getPitchCurve() : nullptr;
        if (curve) {
            int affectedStartFrame = 0;
            int affectedEndFrame = static_cast<int>(curve->size()) - 1;
            if (ctx_.notifyPitchCurveEdited) {
                ctx_.notifyPitchCurveEdited(affectedStartFrame, affectedEndFrame);
            }
        }
        if (ctx_.requestRepaint) {
            ctx_.requestRepaint();
        }
    } else {
        AppLogger::debug("[PianoRollUndoSupport] redo: nothing to redo");
    }
}

} // namespace OpenTune