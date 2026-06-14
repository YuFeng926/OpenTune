#pragma once

#include <cstdint>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Content/ContentKey.h"

namespace OpenTune {

struct PlacementClipEntry {
    int sourceTrackId{0};
    ContentKey sourceContentKey;
    double clipInSeconds{0.0};
    double durationSeconds{0.0};
    float gain{1.0f};
    double fadeInDuration{0.0};
    double fadeOutDuration{0.0};
    juce::String name;
};

class PlacementClipboard {
public:
    bool hasEntries() const { return !entries_.empty(); }

    void store(std::vector<PlacementClipEntry> entries) { entries_ = std::move(entries); }

    const std::vector<PlacementClipEntry>& entries() const { return entries_; }

    void clear() { entries_.clear(); }

private:
    std::vector<PlacementClipEntry> entries_;
};

} // namespace OpenTune
