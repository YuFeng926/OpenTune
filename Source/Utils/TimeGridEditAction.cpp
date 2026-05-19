#include "TimeGridEditAction.h"
// NOTE: We forward-declare OpenTuneAudioProcessor in the header to break the
// utils → core-processor circular include risk; the actual `setMaterializationTimeGridById`
// API is implemented in PluginProcessor.{h,cpp}. This .cpp can include the full
// processor header because it is itself a leaf compilation unit.
#include "../PluginProcessor.h"
#include "AppLogger.h"

namespace OpenTune {

TimeGridEditAction::TimeGridEditAction(OpenTuneAudioProcessor& processor,
                                       uint64_t materializationId,
                                       juce::String description,
                                       std::shared_ptr<const TimeGridSnapshot> oldSnapshot,
                                       std::shared_ptr<const TimeGridSnapshot> newSnapshot,
                                       int64_t affectedSrcStartFrame,
                                       int64_t affectedSrcEndFrame)
    : processor_(processor)
    , materializationId_(materializationId)
    , description_(std::move(description))
    , oldSnapshot_(std::move(oldSnapshot))
    , newSnapshot_(std::move(newSnapshot))
    , affectedSrcStartFrame_(affectedSrcStartFrame)
    , affectedSrcEndFrame_(affectedSrcEndFrame)
{
    // Strict invariant: range is provided by the UI layer at edit time, NOT
    // computed from snapshot diff in the constructor. Snapshot diff (e.g.,
    // union of all handle source positions) would dilute the range to the
    // entire clip in the worst case, defeating the purpose of partial Stage 2
    // re-render.
    //
    // See knowledge/current/cross-cutting/undo-affected-range-invariant.md
    // for the rationale and the v2.0 a122bca regression history that this
    // discipline prevents.
    jassert(affectedSrcStartFrame_ >= 0);
    jassert(affectedSrcEndFrame_   >= affectedSrcStartFrame_);
    jassert(materializationId_     != 0);
}

void TimeGridEditAction::undo()
{
    if (oldSnapshot_ == nullptr) {
        AppLogger::warn("[TimeGridEditAction] undo skipped: oldSnapshot_ is null");
        return;
    }
    processor_.setMaterializationTimeGridById(materializationId_,
                                              oldSnapshot_,
                                              affectedSrcStartFrame_,
                                              affectedSrcEndFrame_);
}

void TimeGridEditAction::redo()
{
    if (newSnapshot_ == nullptr) {
        AppLogger::warn("[TimeGridEditAction] redo skipped: newSnapshot_ is null");
        return;
    }
    processor_.setMaterializationTimeGridById(materializationId_,
                                              newSnapshot_,
                                              affectedSrcStartFrame_,
                                              affectedSrcEndFrame_);
}

} // namespace OpenTune
