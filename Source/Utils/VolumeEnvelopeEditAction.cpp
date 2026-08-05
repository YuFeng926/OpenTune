#include "VolumeEnvelopeEditAction.h"

namespace OpenTune {

VolumeEnvelopeEditAction::VolumeEnvelopeEditAction(
    std::shared_ptr<ContentEditCommands> commands,
    ContentKey key,
    juce::String description,
    AutomationLane beforeEnvelope,
    AutomationLane afterEnvelope)
    : commands_(std::move(commands))
    , contentKey_(key)
    , description_(std::move(description))
    , beforeEnvelope_(std::move(beforeEnvelope))
    , afterEnvelope_(std::move(afterEnvelope))
{
}

void VolumeEnvelopeEditAction::undo()
{
    commands_->commitVolumeEnvelope(contentKey_, beforeEnvelope_);
}

void VolumeEnvelopeEditAction::redo()
{
    commands_->commitVolumeEnvelope(contentKey_, afterEnvelope_);
}

} // namespace OpenTune
