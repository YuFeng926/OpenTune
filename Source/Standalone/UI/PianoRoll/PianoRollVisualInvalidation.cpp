#include "PianoRollVisualInvalidation.h"

#include "Standalone/UI/FrameScheduler.h"

namespace OpenTune {

static_assert(static_cast<int>(PianoRollVisualInvalidationPriority::Background)
                  == static_cast<int>(FrameScheduler::Priority::Background));
static_assert(static_cast<int>(PianoRollVisualInvalidationPriority::Normal)
                  == static_cast<int>(FrameScheduler::Priority::Normal));
static_assert(static_cast<int>(PianoRollVisualInvalidationPriority::Interactive)
                  == static_cast<int>(FrameScheduler::Priority::Interactive));

namespace {

PianoRollVisualInvalidationPriority maxInvalidationPriority(PianoRollVisualInvalidationPriority lhs,
                                                            PianoRollVisualInvalidationPriority rhs)
{
    if (static_cast<int>(lhs) >= static_cast<int>(rhs))
        return lhs;

    return rhs;
}

void promoteToFullRepaint(PianoRollVisualInvalidationState& state)
{
    state.fullRepaint = true;
    state.hasDirtyArea = false;
    state.dirtyArea = {};
}

} // namespace

void PianoRollVisualInvalidationState::merge(const PianoRollVisualInvalidationRequest& request)
{
    reasonsMask |= request.reasonsMask;

    priority = maxInvalidationPriority(priority, request.priority);

    if (request.fullRepaint || (!request.hasDirtyArea && hasWork()))
    {
        promoteToFullRepaint(*this);
        return;
    }

    if (fullRepaint || !request.hasDirtyArea)
        return;

    if (hasDirtyArea)
    {
        dirtyArea = dirtyArea.getUnion(request.dirtyArea);
        return;
    }

    hasDirtyArea = true;
    dirtyArea = request.dirtyArea;
}

bool PianoRollVisualInvalidationState::hasWork() const
{
    return reasonsMask != 0;
}

void PianoRollVisualInvalidationState::clear()
{
    reasonsMask = 0;
    fullRepaint = false;
    hasDirtyArea = false;
    dirtyArea = {};
    priority = PianoRollVisualInvalidationPriority::Background;
}

PianoRollVisualFlushDecision makeVisualFlushDecision(const PianoRollVisualInvalidationState& state,
                                                     const juce::Rectangle<int>& localBounds)
{
    PianoRollVisualFlushDecision decision;
    decision.priority = state.priority;

    if (!state.hasWork())
        return decision;

    decision.shouldRepaint = true;

    if (state.fullRepaint)
    {
        decision.fullRepaint = true;
        return decision;
    }

    if (state.hasDirtyArea)
    {
        const auto clippedDirtyArea = state.dirtyArea.getIntersection(localBounds);
        if (!clippedDirtyArea.isEmpty())
        {
            decision.hasDirtyArea = true;
            decision.dirtyArea = clippedDirtyArea;
            return decision;
        }

        decision.fullRepaint = true;
        return decision;
    }

    decision.fullRepaint = true;
    return decision;
}

} // namespace OpenTune
