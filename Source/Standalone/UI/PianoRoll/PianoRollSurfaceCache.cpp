#include "PianoRollSurfaceCache.h"
#include "PianoRollRenderer.h"

namespace OpenTune {

PianoRollSurfaceCache::PianoRollSurfaceCache()
{
}

void PianoRollSurfaceCache::markDirty(Slot s)
{
    dirtyMask_ |= (1u << static_cast<int>(s));
}

void PianoRollSurfaceCache::invalidateAll()
{
    dirtyMask_ = 0xFFFFFFFF;
}

void PianoRollSurfaceCache::publishGeneration(GeometryKey geometry, std::array<juce::Image, kSlotCount> images)
{
    published_.geometry = geometry;
    published_.images = std::move(images);
}

void PianoRollSurfaceCache::paint(juce::Graphics& g, int offsetX, int offsetY, bool timeView) const
{
    for (int i = 0; i < kSlotCount; ++i)
    {
        const auto s = static_cast<Slot>(i);
        if (timeView)
        {
            // Time view: only Background and TimeAnchors
            if (s != Slot::Background && s != Slot::TimeAnchors)
                continue;
        }
        else
        {
            // Pitch view: skip TimeAnchors
            if (s == Slot::TimeAnchors)
                continue;
        }
        if (published_.images[i].isValid())
            g.drawImageAt(published_.images[i], offsetX, offsetY);
    }
}

} // namespace OpenTune
