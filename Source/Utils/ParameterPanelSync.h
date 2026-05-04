#pragma once

#include "AudioEditingScheme.h"

namespace OpenTune {

struct ParameterPanelSyncContext {
    bool hasSelectedNoteParameters = false;
    float selectedNoteRetuneSpeedPercent = 0.0f;
    float selectedNoteVibratoDepth = 0.0f;
    float selectedNoteVibratoRate = 0.0f;

    bool hasSelectedSegmentRetuneSpeed = false;
    float selectedSegmentRetuneSpeedPercent = 0.0f;

    float clipRetuneSpeedPercent = 0.0f;
    float clipVibratoDepth = 0.0f;
    float clipVibratoRate = 0.0f;

    bool wasShowingSelectionParameters = false;
};

struct ParameterPanelSyncDecision {
    bool shouldSetRetuneSpeed = false;
    float retuneSpeedPercent = 0.0f;

    bool shouldSetVibratoDepth = false;
    float vibratoDepth = 0.0f;

    bool shouldSetVibratoRate = false;
    float vibratoRate = 0.0f;

    bool nextShowingSelectionParameters = false;
};

inline ParameterPanelSyncDecision resolveParameterPanelSyncDecision(
    AudioEditingScheme::Scheme scheme,
    const ParameterPanelSyncContext& context) noexcept
{
    ParameterPanelSyncDecision decision;
    decision.nextShowingSelectionParameters = context.wasShowingSelectionParameters;

    AudioEditingScheme::ParameterTargetContext targetContext;
    targetContext.hasSelectedNotes = context.hasSelectedNoteParameters;
    targetContext.hasSelectedLineAnchorSegments = context.hasSelectedSegmentRetuneSpeed;
    targetContext.hasFrameSelection = false;
    targetContext.allowWholeClipFallback = false;

    const auto parameterTarget = AudioEditingScheme::resolveParameterTarget(
        scheme,
        AudioEditingScheme::ParameterKind::RetuneSpeed,
        targetContext);

    if (parameterTarget == AudioEditingScheme::ParameterTarget::SelectedNotes
        && context.hasSelectedNoteParameters) {
        decision.shouldSetRetuneSpeed = true;
        decision.retuneSpeedPercent = context.selectedNoteRetuneSpeedPercent;
        decision.shouldSetVibratoDepth = true;
        decision.vibratoDepth = context.selectedNoteVibratoDepth;
        decision.shouldSetVibratoRate = true;
        decision.vibratoRate = context.selectedNoteVibratoRate;
        decision.nextShowingSelectionParameters = true;
        return decision;
    }

    if (parameterTarget == AudioEditingScheme::ParameterTarget::SelectedLineAnchorSegments
        && context.hasSelectedSegmentRetuneSpeed) {
        decision.shouldSetRetuneSpeed = true;
        decision.retuneSpeedPercent = context.selectedSegmentRetuneSpeedPercent;
        decision.nextShowingSelectionParameters = true;
        return decision;
    }

    if (!context.wasShowingSelectionParameters) {
        return decision;
    }

    decision.shouldSetRetuneSpeed = true;
    decision.retuneSpeedPercent = context.clipRetuneSpeedPercent;
    decision.shouldSetVibratoDepth = true;
    decision.vibratoDepth = context.clipVibratoDepth;
    decision.shouldSetVibratoRate = true;
    decision.vibratoRate = context.clipVibratoRate;
    decision.nextShowingSelectionParameters = false;
    return decision;
}

} // namespace OpenTune
