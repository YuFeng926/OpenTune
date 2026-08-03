#include "OutputGainEditActions.h"

namespace OpenTune {

OutputGainEditAction::OutputGainEditAction(std::shared_ptr<ContentEditCommands> commands,
                                           ContentKey key,
                                           juce::String description,
                                           ContentNoteRangePatch beforePatch,
                                           ContentNoteRangePatch afterPatch)
    : commands_(std::move(commands))
    , contentKey_(key)
    , description_(std::move(description))
    , beforePatch_(std::move(beforePatch))
    , afterPatch_(std::move(afterPatch))
{
}

void OutputGainEditAction::undo()
{
    commands_->commitNoteOutputGainPatch(contentKey_, beforePatch_);
}

void OutputGainEditAction::redo()
{
    commands_->commitNoteOutputGainPatch(contentKey_, afterPatch_);
}

SibilantEnvelopeEditAction::SibilantEnvelopeEditAction(std::shared_ptr<ContentEditCommands> commands,
                                                       ContentKey key,
                                                       juce::String description,
                                                       SibilantGainEnvelope beforeEnvelope,
                                                       SibilantGainEnvelope afterEnvelope)
    : commands_(std::move(commands))
    , contentKey_(key)
    , description_(std::move(description))
    , beforeEnvelope_(std::move(beforeEnvelope))
    , afterEnvelope_(std::move(afterEnvelope))
{
}

void SibilantEnvelopeEditAction::undo()
{
    commands_->commitSibilantGainEnvelope(contentKey_, beforeEnvelope_);
}

void SibilantEnvelopeEditAction::redo()
{
    commands_->commitSibilantGainEnvelope(contentKey_, afterEnvelope_);
}

} // namespace OpenTune
