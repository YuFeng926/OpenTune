#pragma once

#include "UndoManager.h"
#include "Content/ContentKey.h"
#include "Content/ContentEditCommands.h"
#include "AutomationLane.h"
#include <memory>

namespace OpenTune {

class VolumeEnvelopeEditAction : public UndoAction {
public:
    VolumeEnvelopeEditAction(std::shared_ptr<ContentEditCommands> commands,
                             ContentKey key,
                             juce::String description,
                             AutomationLane beforeEnvelope,
                             AutomationLane afterEnvelope);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

private:
    std::shared_ptr<ContentEditCommands> commands_;
    ContentKey contentKey_;
    juce::String description_;
    AutomationLane beforeEnvelope_, afterEnvelope_;
};

} // namespace OpenTune
