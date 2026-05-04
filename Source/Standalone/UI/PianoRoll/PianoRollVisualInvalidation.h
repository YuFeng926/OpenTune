#pragma once

#include <cstdint>

#include <juce_graphics/juce_graphics.h>

namespace OpenTune {

enum class PianoRollVisualInvalidationReason : uint32_t
{
    Interaction = 1u << 0,
    Viewport = 1u << 1,
    Content = 1u << 2,
    Playhead = 1u << 3,
    Decoration = 1u << 4
};

enum class PianoRollVisualInvalidationPriority : int
{
    Background = 0,
    Normal = 1,
    Interactive = 2
};

struct PianoRollVisualInvalidationRequest
{
    uint32_t reasonsMask = 0;
    bool fullRepaint = false;
    bool hasDirtyArea = false;
    juce::Rectangle<int> dirtyArea;
    PianoRollVisualInvalidationPriority priority = PianoRollVisualInvalidationPriority::Background;
};

struct PianoRollVisualInvalidationState
{
    uint32_t reasonsMask = 0;
    bool fullRepaint = false;
    bool hasDirtyArea = false;
    juce::Rectangle<int> dirtyArea;
    PianoRollVisualInvalidationPriority priority = PianoRollVisualInvalidationPriority::Background;

    void merge(const PianoRollVisualInvalidationRequest& request);
    bool hasWork() const;
    void clear();
};

struct PianoRollVisualFlushDecision
{
    bool shouldRepaint = false;
    bool fullRepaint = false;
    bool hasDirtyArea = false;
    juce::Rectangle<int> dirtyArea;
    PianoRollVisualInvalidationPriority priority = PianoRollVisualInvalidationPriority::Background;
};

PianoRollVisualFlushDecision makeVisualFlushDecision(const PianoRollVisualInvalidationState& state,
                                                     const juce::Rectangle<int>& localBounds);

} // namespace OpenTune
