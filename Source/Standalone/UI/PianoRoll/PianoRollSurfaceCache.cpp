#include "PianoRollSurfaceCache.h"
#include "PianoRollRenderer.h"
#include "PianoRollVisualInvalidation.h"
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

void PianoRollSurfaceCache::markDirtyFromReasonsMask(uint32_t reasonsMask)
{
    // Map invalidation reasons to slots:
    // Interaction (1<<0) → Notes
    // Viewport (1<<1) → No slot (scroll/zoom handled by Graphics translate in paint)
    // Content (1<<2) → Background, Waveform, Notes, F0 (all content layers)
    // Playhead (1<<3) → No slot (overlay)
    // Decoration (1<<4) → Background
    // TimeGrid (1<<5) → Background
    
    const uint32_t interaction = static_cast<uint32_t>(PianoRollVisualInvalidationReason::Interaction);
    const uint32_t content = static_cast<uint32_t>(PianoRollVisualInvalidationReason::Content);
    const uint32_t decoration = static_cast<uint32_t>(PianoRollVisualInvalidationReason::Decoration);
    const uint32_t timeGrid = static_cast<uint32_t>(PianoRollVisualInvalidationReason::TimeGrid);
    
    // Interaction → Notes
    if (reasonsMask & interaction)
        markDirty(Slot::Notes);
    
    // Viewport → Nothing (scroll/zoom handled by Graphics translate)
    // No need to rebuild cache layers for viewport changes
    
    // Content → Background, Waveform, Notes, F0 (all content layers)
    if (reasonsMask & content)
    {
        markDirty(Slot::Background);
        markDirty(Slot::Waveform);
        markDirty(Slot::Notes);
        markDirty(Slot::F0);
    }
    
    // Playhead → No slot (overlay)
    
    // Decoration → Background
    if (reasonsMask & decoration)
        markDirty(Slot::Background);
    
    // TimeGrid → Background
    if (reasonsMask & timeGrid)
        markDirty(Slot::Background);
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

void PianoRollSurfaceCache::buildSlot(Slot s, const PianoRollRenderer::RenderContext& ctx, PianoRollRenderer& renderer)
{
    const int slotIndex = static_cast<int>(s);
    
    // Ensure image is valid and correctly sized
    if (!images_[slotIndex].isValid() || images_[slotIndex].getWidth() != surfaceWidth_ || images_[slotIndex].getHeight() != surfaceHeight_)
    {
        images_[slotIndex] = juce::Image(juce::Image::ARGB, surfaceWidth_, surfaceHeight_, true);
    }
    
    // Create graphics context and clear with transparent
    juce::Graphics g(images_[slotIndex]);
    g.fillAll(juce::Colours::transparentBlack);
    
    // Render based on slot type
    switch (s)
    {
        case Slot::Background:
            renderer.drawLanes(g, ctx);
            renderer.drawGridLines(g, ctx);
            renderer.drawTimeRuler(g, ctx);
            break;
            
        case Slot::Waveform:
            for (const auto& item : ctx.contents)
                renderer.drawWaveform(g, ctx, item);
            break;
            
        case Slot::Notes:
            for (const auto& item : ctx.contents)
            {
                renderer.drawNotes(g, ctx, item);
                renderer.drawChunkBoundaries(g, ctx, item);
                // Pass empty indices — interactive highlights are in overlay
                renderer.drawSelectedNoteHighlights(g, ctx, item.displayNotes, {}, item);
            }
            break;
            
        case Slot::F0:
            for (const auto& item : ctx.contents)
            {
                renderer.drawUnvoicedFrameBands(g, ctx, item);
                renderer.drawF0Curve(g, ctx, item);
            }
            break;
            
        default:
            break;
    }
    
    // Clear dirty flag for this slot
    clearDirty(s);
}

void PianoRollSurfaceCache::buildAllDirty(const PianoRollRenderer::RenderContext& ctx, PianoRollRenderer& renderer)
{
    for (int i = 0; i < kSlotCount; ++i)
    {
        if (isDirty(static_cast<Slot>(i)))
            buildSlot(static_cast<Slot>(i), ctx, renderer);
    }
}

void PianoRollSurfaceCache::paint(juce::Graphics& g, int offsetX, int offsetY) const
{
    for (int i = 0; i < kSlotCount; ++i)
    {
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
