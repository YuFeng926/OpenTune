#include "PianoRollUndoSupport.h"
#include "../../../Utils/AppLogger.h"
#include <cmath>

namespace OpenTune {

// ============================================================================
// PianoRollUndoSupport - 钢琴卷帘撤销支持实现
// ============================================================================

PianoRollUndoSupport::PianoRollUndoSupport(Context context)
    : ctx_(std::move(context))
{
    AppLogger::debug("[PianoRollUndoSupport] Created");
}

UndoManager* PianoRollUndoSupport::getCurrentUndoManager() noexcept
{
    return ctx_.getUndoManager ? ctx_.getUndoManager() : nullptr;
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

void PianoRollUndoSupport::beginTransaction(const juce::String& description)
{
    jassert(!transactionActive_);

    transactionDescription_ = description;
    transactionBeforeNotes_ = ctx_.getNotesCopy ? ctx_.getNotesCopy() : std::vector<Note>();

    auto curve = ctx_.getPitchCurve ? ctx_.getPitchCurve() : nullptr;
    transactionBeforeSegments_ = CorrectedSegmentsChangeAction::captureSegments(curve);

    transactionActive_ = true;
}

bool PianoRollUndoSupport::isTransactionActive() const noexcept
{
    return transactionActive_;
}

void PianoRollUndoSupport::clearTransaction()
{
    transactionDescription_.clear();
    transactionBeforeNotes_.clear();
    transactionBeforeSegments_.clear();
    transactionActive_ = false;
}

void PianoRollUndoSupport::commitTransaction()
{
    if (!transactionActive_) {
        jassertfalse;
        return;
    }

    auto afterNotes = ctx_.getNotesCopy ? ctx_.getNotesCopy() : std::vector<Note>();
    const bool notesChanged = !notesEquivalent(transactionBeforeNotes_, afterNotes);

    auto curve = ctx_.getPitchCurve ? ctx_.getPitchCurve() : nullptr;
    auto afterSegments = CorrectedSegmentsChangeAction::captureSegments(curve);
    const bool segmentsChanged = !CorrectedSegmentsChangeAction::snapshotsEquivalent(
        transactionBeforeSegments_, afterSegments);

    if (notesChanged || segmentsChanged) {
        const uint64_t clipId = ctx_.getCurrentClipId ? ctx_.getCurrentClipId() : 0;
        const int trackId = ctx_.getCurrentTrackId ? ctx_.getCurrentTrackId() : 0;
        auto* processor = ctx_.getProcessor ? ctx_.getProcessor() : nullptr;

        if (notesChanged && segmentsChanged && processor && curve) {
            auto action = std::make_unique<CompoundUndoAction>(transactionDescription_);
            action->addAction(std::make_unique<NotesChangeAction>(
                *processor,
                trackId,
                clipId,
                transactionBeforeNotes_,
                afterNotes,
                transactionDescription_));

            auto curveAction = CorrectedSegmentsChangeAction::createForCurve(
                clipId,
                transactionBeforeSegments_,
                afterSegments,
                curve,
                0,
                static_cast<int>(curve->size()) - 1,
                transactionDescription_);
            if (curveAction) {
                action->addAction(std::move(curveAction));
            }

            if (auto* um = getCurrentUndoManager()) {
                um->addAction(std::move(action));
            }
        } else if (notesChanged && processor) {
            if (auto* um = getCurrentUndoManager()) {
                um->addAction(std::make_unique<NotesChangeAction>(
                    *processor,
                    trackId,
                    clipId,
                    transactionBeforeNotes_,
                    afterNotes,
                    transactionDescription_));
            }
        } else if (segmentsChanged && curve) {
            if (auto* um = getCurrentUndoManager()) {
                um->addAction(CorrectedSegmentsChangeAction::createForCurve(
                    clipId,
                    transactionBeforeSegments_,
                    afterSegments,
                    curve,
                    0,
                    static_cast<int>(curve->size()) - 1,
                    transactionDescription_));
            }
        }
    }

    clearTransaction();
}

} // namespace OpenTune
