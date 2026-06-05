#include "PianoRollEditAction.h"
#include "../ARA/MaterializationContentProvider.h"
#include <limits>

namespace OpenTune {

PianoRollEditAction::PianoRollEditAction(std::shared_ptr<MaterializationContentCommands> commands,
                                         uint64_t materializationId,
                                         juce::String description,
                                         std::vector<Note> oldNotes,
                                         std::vector<Note> newNotes,
                                         std::vector<CorrectedSegment> oldSegments,
                                         std::vector<CorrectedSegment> newSegments,
                                         int affectedStartFrame,
                                         int affectedEndFrame)
    : commands_(commands)
    , materializationId_(materializationId)
    , description_(std::move(description))
    , oldNotes_(std::move(oldNotes))
    , newNotes_(std::move(newNotes))
    , oldSegments_(std::move(oldSegments))
    , newSegments_(std::move(newSegments))
    , affectedStartFrame_(affectedStartFrame)
    , affectedEndFrame_(affectedEndFrame)
{
    // affected range 由 ToolHandler 计算时直接传入，不从 segments 反推。
    // 反推（union of all segments min/max）会被 PitchCurve 上无关分布的早段/晚段
    // 漂移成 [0, 全长]，让 undo/redo 退化为全长 vocoder 渲染（regression of c5c6c29
    // optimization, lost in v2.0 a122bca rewrite）。
    jassert(affectedStartFrame_ >= 0);
    jassert(affectedEndFrame_ >= affectedStartFrame_);
}

void PianoRollEditAction::undo()
{
    if (commands_ != nullptr)
        commands_->commitNotesAndSegments(materializationId_, oldNotes_, oldSegments_);
}

void PianoRollEditAction::redo()
{
    if (commands_ != nullptr)
        commands_->commitNotesAndSegments(materializationId_, newNotes_, newSegments_);
}

} // namespace OpenTune
