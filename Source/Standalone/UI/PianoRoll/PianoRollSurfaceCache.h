#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PianoRollRenderer.h"
#include <array>
#include <cstdint>

namespace OpenTune {

class PianoRollSurfaceCache
{
public:
    enum class Slot : int
    {
        Waveform = 0,     // waveform per content item
        Notes,            // notes only (committed notes, no chunk boundaries, no highlights)
        F0,               // F0 curves per content item
        TimeAnchors,      // published TimeGrid anchors (not hover/selected handles)
        SlotCount         // 现在是4个slot
    };

    static constexpr int kSlotCount = static_cast<int>(Slot::SlotCount);

    struct GeometryKey {
        double surfaceStartSec = 0.0;
        double surfaceEndSec = 0.0;
        double pixelsPerSecond = 0.0;
        int surfaceWidth = 0;
        int surfaceHeight = 0;

        bool operator==(const GeometryKey& other) const
        {
            return surfaceStartSec == other.surfaceStartSec
                && surfaceEndSec == other.surfaceEndSec
                && pixelsPerSecond == other.pixelsPerSecond
                && surfaceWidth == other.surfaceWidth
                && surfaceHeight == other.surfaceHeight;
        }
    };

    struct PublishedGeneration {
        GeometryKey geometry;
        std::array<juce::Image, kSlotCount> images;

        bool isValid() const { return images[0].isValid(); }
    };

    PianoRollSurfaceCache();

    // 只读 published generation
    const PublishedGeneration& getPublishedGeneration() const { return published_; }
    bool hasPublishedGeneration() const { return published_.isValid(); }

    // 发布新 generation（由 builder 调用）
    void publishGeneration(GeometryKey geometry, std::array<juce::Image, kSlotCount> images);

    // 只设置 dirty mask，不 build
    void markDirty(Slot s);
    void invalidateAll();
    uint32_t getDirtyMask() const { return dirtyMask_; }
    void clearDirtyMask() { dirtyMask_ = 0; }

    // 只绘制，不 build
    void paint(juce::Graphics& g, int offsetX, int offsetY, bool timeView = false) const;

private:
    PublishedGeneration published_;
    uint32_t dirtyMask_ = 0;  // 初始为 0，不 dirty
};

} // namespace OpenTune
