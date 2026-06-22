#include "PianoRollTileRenderer.h"
#include "PianoRollRenderer.h"
#include "UI/UIColors.h"

namespace OpenTune {

juce::Image PianoRollTileRenderer::renderTile(const PianoRollRenderSnapshot& snapshot,
                                               const PianoRollTileKey& key) {
    const int w = juce::jmax(1, key.tileWidthPx);
    const int h = juce::jmax(1, key.contentHeightPx);
    juce::Image image(juce::Image::ARGB, w, h, true);

    // Build RenderContext from snapshot — use tile-local coordinates.
    // Tile content is rendered relative to tileStartContentX, then drawVisibleTiles()
    // places the tile at the correct screen position.
    PianoRollRenderer::RenderContext ctx;
    ctx.width = w;
    ctx.height = h;
    ctx.pianoKeyWidth = 0;  // No piano keys inside tile
    ctx.pixelsPerSecond = snapshot.pixelsPerSecond;
    ctx.pixelsPerSemitone = snapshot.pixelsPerSemitone;
    ctx.minMidi = 0.0f;
    ctx.maxMidi = snapshot.maxMidi;
    ctx.bpm = snapshot.bpm;
    ctx.scaleRootNote = snapshot.scaleRootNote;
    ctx.scaleType = snapshot.scaleType;
    ctx.noteNameMode = snapshot.noteNameMode;
    ctx.showLanes = snapshot.showLanes;
    ctx.showChunkBoundaries = snapshot.showChunkBoundaries;
    ctx.showUnvoicedFrames = snapshot.showUnvoicedFrames;
    ctx.showOriginalF0 = snapshot.showOriginalF0;
    ctx.showCorrectedF0 = snapshot.showCorrectedF0;
    ctx.currentTool = snapshot.currentTool;

    // Tile-local coordinate mapper: scrollOffset = tileStartContentX, contentStartX = 0
    ctx.coords.pixelsPerSecond = snapshot.pixelsPerSecond;
    ctx.coords.scrollOffsetPx = key.tileStartContentX;  // Tile-local scroll
    ctx.coords.contentStartX = 0;  // No piano key offset inside tile
    ctx.coords.pixelsPerSemitone = snapshot.pixelsPerSemitone;
    ctx.coords.verticalScrollOffset = snapshot.verticalScrollOffset;
    ctx.coords.maxMidi = snapshot.maxMidi;

    ctx.activeProjection = snapshot.activeProjection;
    ctx.timeGridSnapshot = snapshot.timeGridSnapshot;

    // Build ContentRenderItem from snapshot
    PianoRollRenderer::ContentRenderItem item;
    item.contentKey = snapshot.contentKey;
    item.projection = snapshot.activeProjection;
    item.active = true;
    item.displayNotes = snapshot.notes;
    item.pitchSnapshot = snapshot.pitchSnapshot;
    item.f0Timeline = snapshot.f0Timeline;
    item.chunkBoundaries = snapshot.chunkBoundaries;
    // waveformMipmap and audioBuffer are NOT available in snapshot (live pointers).
    // Waveform is rendered in the base layer (paint-time, immediate).

    ctx.contents.push_back(item);

    // Render detail layer
    juce::Graphics g(image);

    PianoRollRenderer renderer;

    if (!ctx.isTimeView()) {
        renderer.drawLanes(g, ctx);
        renderer.drawUnvoicedFrameBands(g, ctx, item);
        renderer.drawNotes(g, ctx, item);
        renderer.drawF0Curve(g, ctx, item);
    }

    renderer.drawChunkBoundaries(g, ctx, item);

    return image;
}

} // namespace OpenTune
