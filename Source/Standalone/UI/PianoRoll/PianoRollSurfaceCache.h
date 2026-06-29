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
        Background = 0,   // lanes + grid lines + time ruler
        Waveform,         // waveform per content item
        Notes,            // notes only (committed notes, no chunk boundaries, no highlights)
        F0,               // F0 curves per content item
        TimeAnchors,      // published TimeGrid anchors (not hover/selected handles)
        SlotCount
    };

    static constexpr int kSlotCount = static_cast<int>(Slot::SlotCount);

    PianoRollSurfaceCache();

    void markDirty(Slot s);
    bool isDirty(Slot s) const;
    void clearDirty(Slot s);
    void invalidateAll();

    juce::Image& image(Slot s);
    const juce::Image& image(Slot s) const;

    /// Build a single slot. Creates/resizes image, renders into it, clears dirty.
    void buildSlot(Slot s, const PianoRollRenderer::SurfaceRenderContext& sctx, PianoRollRenderer& renderer);

    /// Build all dirty slots.
    void buildAllDirty(const PianoRollRenderer::SurfaceRenderContext& sctx, PianoRollRenderer& renderer);

    /// Paint all slot images at the given offset. When timeView is true, skips Waveform/Notes/F0.
    void paint(juce::Graphics& g, int offsetX, int offsetY, bool timeView = false) const;

    /// Configure full-clip domain geometry. Dirties all slots on change.
    bool configureGeometry(double startSec, double endSec, double pps, int height);
    double surfaceStartSec() const { return surfaceStartSec_; }
    double surfaceEndSec() const { return surfaceEndSec_; }
    double cachePixelsPerSecond() const { return pixelsPerSecond_; }
    int surfaceWidthPx() const { return surfaceWidth_; }
    int surfaceHeightPx() const { return surfaceHeight_; }

private:
    std::array<juce::Image, kSlotCount> images_;
    uint32_t dirtyMask_ = 0xFFFFFFFF;
    double surfaceStartSec_ = 0.0;
    double surfaceEndSec_ = 0.0;
    double pixelsPerSecond_ = 0.0;
    int surfaceWidth_ = 0;
    int surfaceHeight_ = 0;
};

} // namespace OpenTune
