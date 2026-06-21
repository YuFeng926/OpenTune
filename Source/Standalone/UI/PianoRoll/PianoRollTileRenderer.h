#pragma once

#include "PianoRollRenderSnapshot.h"
#include "PianoRollTimelineSurfaceCache.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace OpenTune {

class PianoRollTileRenderer {
public:
    static juce::Image renderTile(const PianoRollRenderSnapshot& snapshot,
                                  const PianoRollTileKey& key);
};

} // namespace OpenTune
