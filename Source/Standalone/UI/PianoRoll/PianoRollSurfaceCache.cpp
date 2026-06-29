#include "PianoRollSurfaceCache.h"
#include "PianoRollRenderer.h"
#include <cmath>

namespace OpenTune {

PianoRollSurfaceCache::PianoRollSurfaceCache()
{
    // All slots start dirty (dirtyMask_ initialized to 0xFFFFFFFF)
}

void PianoRollSurfaceCache::markDirty(Slot s)
{
    dirtyMask_ |= (1u << static_cast<int>(s));
}

bool PianoRollSurfaceCache::isDirty(Slot s) const
{
    return (dirtyMask_ & (1u << static_cast<int>(s))) != 0;
}

void PianoRollSurfaceCache::clearDirty(Slot s)
{
    dirtyMask_ &= ~(1u << static_cast<int>(s));
}

void PianoRollSurfaceCache::invalidateAll()
{
    dirtyMask_ = 0xFFFFFFFF;
}

juce::Image& PianoRollSurfaceCache::image(Slot s)
{
    return images_[static_cast<int>(s)];
}

const juce::Image& PianoRollSurfaceCache::image(Slot s) const
{
    return images_[static_cast<int>(s)];
}

void PianoRollSurfaceCache::buildSlot(Slot s, const PianoRollRenderer::SurfaceRenderContext& sctx, PianoRollRenderer& renderer)
{
    const int slotIndex = static_cast<int>(s);
    
    if (!images_[slotIndex].isValid() || images_[slotIndex].getWidth() != surfaceWidth_ || images_[slotIndex].getHeight() != surfaceHeight_)
    {
        images_[slotIndex] = juce::Image(juce::Image::ARGB, surfaceWidth_, surfaceHeight_, true);
    }
    
    juce::Graphics g(images_[slotIndex]);
    g.fillAll(juce::Colours::transparentBlack);
    
    switch (s)
    {
        case Slot::Background:
            renderer.drawLanes(g, sctx);
            renderer.drawGridLines(g, sctx);
            renderer.drawTimeRuler(g, sctx);
            break;
            
        case Slot::Waveform:
            for (const auto& item : sctx.contents)
                renderer.drawWaveform(g, sctx, item);
            break;
            
        case Slot::Notes:
            for (const auto& item : sctx.contents)
                renderer.drawNotes(g, sctx, item);
            break;
            
        case Slot::F0:
            for (const auto& item : sctx.contents)
            {
                renderer.drawUnvoicedFrameBands(g, sctx, item);
                renderer.drawF0Curve(g, sctx, item);
            }
            break;
            
        case Slot::TimeAnchors:
            renderer.drawTimeGridAnchors(g, sctx);
            break;
            
        default:
            break;
    }
    
    clearDirty(s);
}

void PianoRollSurfaceCache::buildAllDirty(const PianoRollRenderer::SurfaceRenderContext& sctx, PianoRollRenderer& renderer)
{
    for (int i = 0; i < kSlotCount; ++i)
    {
        if (isDirty(static_cast<Slot>(i)))
            buildSlot(static_cast<Slot>(i), sctx, renderer);
    }
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
        if (images_[i].isValid())
            g.drawImageAt(images_[i], offsetX, offsetY);
    }
}

bool PianoRollSurfaceCache::configureGeometry(double startSec, double endSec, double pps, int height)
{
    const int newWidth = static_cast<int>(std::ceil((endSec - startSec) * pps));
    const bool changed = (newWidth != surfaceWidth_ || height != surfaceHeight_ || pps != pixelsPerSecond_
                          || startSec != surfaceStartSec_ || endSec != surfaceEndSec_);
    
    surfaceStartSec_ = startSec;
    surfaceEndSec_ = endSec;
    pixelsPerSecond_ = pps;
    surfaceWidth_ = newWidth;
    surfaceHeight_ = (newWidth > 0) ? height : 0;
    
    if (changed) {
        for (int i = 0; i < kSlotCount; ++i) {
            if (surfaceWidth_ > 0 && surfaceHeight_ > 0)
                images_[i] = juce::Image(juce::Image::ARGB, surfaceWidth_, surfaceHeight_, true);
            else
                images_[i] = juce::Image();
        }
        invalidateAll();
    }
    
    return changed;
}

} // namespace OpenTune
