/**
 * Timeline Rendering Pipeline tests for the new cache objects.
 * Tests: TimelineViewportState, WaveformTileCache, ArrangementRenderModelCache.
 */

#include "TestSupport.h"
#include "../Source/Standalone/UI/TimelineViewportState.h"
#include "../Source/Standalone/UI/ArrangementRenderModelCache.h"
#include "../Source/Standalone/UI/WaveformTileCache.h"
#include "../Source/Standalone/UI/WaveformMipmap.h"

namespace OpenTune {

namespace {

juce::File locateTimelineRenderingWorkspaceRoot()
{
    auto current = juce::File::getCurrentWorkingDirectory();
    for (int depth = 0; depth < 8 && current.isDirectory(); ++depth) {
        if (current.getChildFile("CMakeLists.txt").existsAsFile())
            return current;

        const auto parent = current.getParentDirectory();
        if (parent == current)
            break;

        current = parent;
    }

    return {};
}

juce::String readTimelineRenderingWorkspaceFile(const juce::String& relativePath)
{
    const auto root = locateTimelineRenderingWorkspaceRoot();
    if (!root.isDirectory())
        return {};

    const auto file = root.getChildFile(relativePath);
    return file.existsAsFile() ? file.loadFileAsString() : juce::String{};
}

juce::String extractTimelineRenderingWorkspaceSection(const juce::String& relativePath,
                                                      const juce::String& startNeedle,
                                                      const juce::String& endNeedle)
{
    const auto source = readTimelineRenderingWorkspaceFile(relativePath);
    const int start = source.indexOf(startNeedle);
    if (start < 0)
        return {};

    const int end = source.indexOf(start + startNeedle.length(), endNeedle);
    if (end < 0 || end <= start)
        return {};

    return source.substring(start, end);
}

WaveformMipmap makeCompleteTestMipmap(int numSamples)
{
    WaveformMipmap mipmap;
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, numSamples);
    audio->clear();
    for (int i = 0; i < audio->getNumSamples(); ++i)
        audio->setSample(0, i, std::sin(static_cast<float>(i) * 0.18f) * 0.75f);

    mipmap.setAudioSource(audio);
    int guard = 0;
    while (!mipmap.isComplete() && guard < 1000) {
        mipmap.buildIncremental(1.0);
        ++guard;
    }

    return mipmap;
}

} // namespace

// ============================================================================
// TimelineViewportState tests
// ============================================================================

void runTimelineViewportStateTimeMathTest()
{
    constexpr const char* testName = "TimelineViewportState_TimeMath";

    TimelineViewportState vp;
    vp.zoomLevel = 1.0;
    vp.scrollOffsetPx = 0;
    vp.viewportWidthPx = 800;
    vp.contentStartX = 8;

    // At zoom=1.0, 100 px/s, t=0 → contentX=0, viewportX=8
    if (vp.timeToContentX(0.0) != 0) {
        logFail(testName, "timeToContentX(0) should be 0 at zoom=1");
        return;
    }
    if (vp.timeToViewportX(0.0) != 8) {
        logFail(testName, "timeToViewportX(0) should be contentStartX (8)");
        return;
    }

    // At zoom=1.0, 100 px/s, t=1.0 → contentX=100, viewportX=108
    if (vp.timeToContentX(1.0) != 100) {
        logFail(testName, "timeToContentX(1) should be 100 at zoom=1");
        return;
    }
    if (vp.timeToViewportX(1.0) != 108) {
        logFail(testName, "timeToViewportX(1) should be 108");
        return;
    }

    // Reverse: viewportX=108 → time=1.0
    double time = vp.viewportXToTime(108);
    if (std::abs(time - 1.0) > 0.001) {
        logFail(testName, "viewportXToTime(108) should be ~1.0");
        return;
    }

    // Pixels per second
    if (std::abs(vp.pixelsPerSecond() - 100.0) > 0.001) {
        logFail(testName, "pixelsPerSecond should be 100 at zoom=1");
        return;
    }

    // Zoom=2.0 doubles everything
    vp.zoomLevel = 2.0;
    if (std::abs(vp.pixelsPerSecond() - 200.0) > 0.001) {
        logFail(testName, "pixelsPerSecond should be 200 at zoom=2");
        return;
    }
    if (vp.timeToContentX(1.0) != 200) {
        logFail(testName, "timeToContentX(1) should be 200 at zoom=2");
        return;
    }

    logPass(testName);
}

void runTimelineViewportStateVisibleRangeTest()
{
    constexpr const char* testName = "TimelineViewportState_VisibleRange";

    TimelineViewportState vp;
    vp.zoomLevel = 1.0;
    vp.scrollOffsetPx = 0;
    vp.viewportWidthPx = 800;
    vp.contentStartX = 8;

    // With scroll=0, visible time ≈ 0s to 8s (800px at 100px/s = 8s, minus offset)
    // At x=0, time = (0 - 8 + 0) / 100 = -0.08
    // At x=800, time = (800 - 8 + 0) / 100 = 7.92
    double startTime = vp.visibleTimeStart();
    double endTime = vp.visibleTimeEnd();

    if (std::abs(startTime - (-0.08)) > 0.001) {
        logFail(testName, "visibleTimeStart should be ~-0.08 at scroll=0");
        return;
    }
    if (std::abs(endTime - 7.92) > 0.001) {
        logFail(testName, "visibleTimeEnd should be ~7.92 at scroll=0");
        return;
    }

    logPass(testName);
}

void runTimelineViewportStateExposedStripTest()
{
    constexpr const char* testName = "TimelineViewportState_ExposedStrip";

    TimelineViewportState vp;
    vp.zoomLevel = 1.0;
    vp.viewportWidthPx = 800;
    vp.viewportHeightPx = 600;

    // Small scroll right: exposed strip on the right
    auto strip = vp.exposedStripForScrollDelta(0, 50);
    if (strip.isEmpty()) {
        logFail(testName, "50px right scroll should have non-empty exposed strip");
        return;
    }
    if (strip.getWidth() != 50 || strip.getX() != 750) {
        logFail(testName, "50px right scroll should expose 50px strip on right (x=750, w=50)");
        return;
    }

    // Small scroll left: exposed strip on the left
    strip = vp.exposedStripForScrollDelta(100, 50);
    if (strip.isEmpty()) {
        logFail(testName, "50px left scroll should have non-empty exposed strip");
        return;
    }
    if (strip.getWidth() != 50 || strip.getX() != 0) {
        logFail(testName, "50px left scroll should expose 50px strip on left (x=0, w=50)");
        return;
    }

    // Large scroll (>= viewport width): empty (full redraw required)
    strip = vp.exposedStripForScrollDelta(0, 800);
    if (!strip.isEmpty()) {
        logFail(testName, "800px scroll (== viewport width) should return empty (full redraw)");
        return;
    }
    strip = vp.exposedStripForScrollDelta(0, 900);
    if (!strip.isEmpty()) {
        logFail(testName, "900px scroll (> viewport width) should return empty (full redraw)");
        return;
    }

    // Zero scroll: empty
    strip = vp.exposedStripForScrollDelta(100, 100);
    if (!strip.isEmpty()) {
        logFail(testName, "zero delta should return empty");
        return;
    }

    // requiresFullRedrawForDelta
    if (vp.requiresFullRedrawForDelta(0, 0)) {
        logFail(testName, "zero delta should not require full redraw");
        return;
    }
    if (vp.requiresFullRedrawForDelta(0, 50)) {
        logFail(testName, "50px delta should not require full redraw");
        return;
    }
    if (!vp.requiresFullRedrawForDelta(0, 800)) {
        logFail(testName, "800px delta should require full redraw");
        return;
    }

    logPass(testName);
}

// ============================================================================
// WaveformTileCache tests
// ============================================================================

void runWaveformTileCacheBoundedLruTest()
{
    constexpr const char* testName = "WaveformTileCache_BoundedLru";

    WaveformTileCache cache;
    if (cache.size() != 0) {
        logFail(testName, "fresh cache should be empty");
        return;
    }

    // Create a minimal mipmap with one level
    WaveformMipmap mipmap;
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, 1024);
    audio->clear();
    // Fill with some signal
    for (int i = 0; i < 1024; ++i)
        audio->setSample(0, i, std::sin(static_cast<float>(i) * 0.1f) * 0.5f);
    mipmap.setAudioSource(audio);

    // Build mipmap completely
    int guard = 0;
    while (!mipmap.isComplete() && guard < 1000) {
        mipmap.buildIncremental(1.0);
        ++guard;
    }
    if (!mipmap.isComplete()) {
        logFail(testName, "failed to build test mipmap");
        return;
    }

    // Add tiles
    juce::Rectangle<int> bounds(10, 20, 100, 50);
    for (int i = 0; i < 10; ++i) {
        cache.getOrCreate(static_cast<uint64_t>(i + 1),
                          static_cast<uint64_t>(i + 1),
                          10,
                          mipmap,
                          1.0f,
                          bounds,
                          0.0,
                          1.0,
                          0,
                          0);
    }

    if (cache.size() != 10) {
        logFail(testName, "cache should have 10 entries after adding 10 tiles");
        return;
    }

    // Verify all tiles are accessible
    for (int i = 0; i < 10; ++i) {
        const auto* tile = cache.get(static_cast<uint64_t>(i + 1),
                                     static_cast<uint64_t>(i + 1),
                                     10,
                                     bounds,
                                     0.0,
                                     1.0,
                                     0,
                                     0);
        if (tile == nullptr) {
            logFail(testName, "tile should be findable after insertion");
            return;
        }
    }

    // Prune: keep only some
    std::unordered_set<uint64_t> alive;
    alive.insert(1);
    alive.insert(3);
    alive.insert(5);
    cache.prune(alive);

    if (cache.size() != 3) {
        logFail(testName, "cache should have 3 entries after pruning to {1,3,5}");
        return;
    }

    // Verify kept vs pruned
    if (cache.get(1, 1, 10, bounds, 0.0, 1.0, 0, 0) == nullptr) {
        logFail(testName, "tile 1 should survive prune");
        return;
    }
    if (cache.get(2, 2, 10, bounds, 0.0, 1.0, 0, 0) != nullptr) {
        logFail(testName, "tile 2 should be pruned");
        return;
    }

    // Different zoom bucket — should not find
    if (cache.get(1, 1, 20, bounds, 0.0, 1.0, 0, 0) != nullptr) {
        logFail(testName, "tile 1 zoom=20 should not be found (inserted as zoom=10)");
        return;
    }

    cache.clear();
    if (cache.size() != 0) {
        logFail(testName, "cache should be empty after clear");
        return;
    }

    logPass(testName);
}

void runWaveformTileCachePruneByMaterializationTest()
{
    constexpr const char* testName = "WaveformTileCache_PruneByMaterialization";

    WaveformTileCache cache;
    WaveformMipmap mipmap;
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, 256);
    audio->clear();
    mipmap.setAudioSource(audio);
    int guard = 0;
    while (!mipmap.isComplete() && guard < 100) { mipmap.buildIncremental(1.0); ++guard; }

    juce::Rectangle<int> bounds(0, 0, 100, 50);

    // Two materializations, two zoom buckets each
    cache.getOrCreate(1, 1, 5, mipmap, 1.0f, bounds, 0.0, 1.0, 0, 0);
    cache.getOrCreate(1, 1, 10, mipmap, 1.0f, bounds, 0.0, 1.0, 0, 0);
    cache.getOrCreate(2, 2, 5, mipmap, 1.0f, bounds, 0.0, 1.0, 0, 0);

    if (cache.size() != 3) {
        logFail(testName, "should have 3 tiles after insertion");
        return;
    }

    // Remove materialization 1
    cache.remove(1);
    if (cache.size() != 1) {
        logFail(testName, "should have 1 tile after removing materialization 1");
        return;
    }
    if (cache.get(2, 2, 5, bounds, 0.0, 1.0, 0, 0) == nullptr) {
        logFail(testName, "materialization 2 zoom=5 should survive remove(1)");
        return;
    }

    logPass(testName);
}

void runArrangementWaveformMinZoomNarrowClipKeepsPositiveDrawableBoundsTest()
{
    constexpr const char* testName = "ArrangementWaveform_MinZoomNarrowClipKeepsPositiveDrawableBounds";

    const juce::Rectangle<int> minZoomPlacementBounds(100, 40, 8, 48);
    const auto waveformBounds = ArrangementRenderModelCache::computeWaveformDrawableBounds(minZoomPlacementBounds);

    if (waveformBounds.isEmpty() || waveformBounds.getWidth() <= 0 || waveformBounds.getHeight() <= 0) {
        logFail(testName, "8px visible placement collapsed to an empty waveform drawable bounds");
        return;
    }

    if (!minZoomPlacementBounds.contains(waveformBounds)) {
        logFail(testName, "waveform drawable bounds must stay inside the visible placement bounds");
        return;
    }

    logPass(testName);
}

void runWaveformTileCacheNarrowDrawableBoundsBuildsNonEmptyPathTest()
{
    constexpr const char* testName = "WaveformTileCache_NarrowDrawableBoundsBuildsNonEmptyPath";

    auto mipmap = makeCompleteTestMipmap(4096);
    if (!mipmap.isComplete()) {
        logFail(testName, "failed to build complete synthetic waveform mipmap");
        return;
    }

    WaveformTileCache cache;
    const juce::Rectangle<int> narrowBounds(12, 8, 1, 32);
    const auto& tile = cache.getOrCreate(11, 11, 1, mipmap, 1.0f, narrowBounds, 0.0, 0.08, 0, 0);

    if (tile.widthPx != 1 || tile.path.isEmpty()) {
        logFail(testName, "positive narrow drawable bounds should build a non-empty waveform path");
        return;
    }

    logPass(testName);
}

void runWaveformTileCacheDifferentBoundsDoNotReuseAbsolutePathTest()
{
    constexpr const char* testName = "WaveformTileCache_DifferentBoundsDoNotReuseAbsolutePath";

    auto mipmap = makeCompleteTestMipmap(4096);
    if (!mipmap.isComplete()) {
        logFail(testName, "failed to build complete synthetic waveform mipmap");
        return;
    }

    WaveformTileCache cache;
    const juce::Rectangle<int> originalBounds(12, 24, 48, 28);
    const juce::Rectangle<int> previewBounds(12, 104, 48, 28);

    const auto& originalTile = cache.getOrCreate(21, 21, 1, mipmap, 1.0f, originalBounds, 0.0, 0.48, 0, 0);
    const auto originalPathBounds = originalTile.path.getBounds();

    if (originalTile.path.isEmpty()) {
        logFail(testName, "original waveform tile should produce a non-empty path");
        return;
    }

    const auto& previewTile = cache.getOrCreate(21, 21, 1, mipmap, 1.0f, previewBounds, 0.0, 0.48, 0, 0);
    const auto previewPathBounds = previewTile.path.getBounds();

    if (previewTile.path.isEmpty()) {
        logFail(testName, "preview waveform tile should produce a non-empty path");
        return;
    }

    if (cache.size() != 2) {
        logFail(testName, "different preview bounds should build a distinct tile when cached paths embed absolute coordinates");
        return;
    }

    const float expectedDeltaY = static_cast<float>(previewBounds.getY() - originalBounds.getY());
    const float actualDeltaY = previewPathBounds.getCentreY() - originalPathBounds.getCentreY();
    const float actualDeltaX = previewPathBounds.getX() - originalPathBounds.getX();

    if (std::abs(actualDeltaY - expectedDeltaY) > 1.0f || std::abs(actualDeltaX) > 1.0f) {
        logFail(testName, "waveform tile path should translate with preview bounds instead of reusing stale track geometry");
        return;
    }

    logPass(testName);
}

void runArrangementDragPreviewMouseDragDoesNotCommitMoveTest()
{
    constexpr const char* testName = "ArrangementDragPreview_MouseDragDoesNotCommitMove";

    const auto mouseDragSection = extractTimelineRenderingWorkspaceSection(
        "Source/Standalone/UI/ArrangementViewComponent.cpp",
        "void ArrangementViewComponent::mouseDrag",
        "void ArrangementViewComponent::mouseUp");

    const auto updatePreviewSection = extractTimelineRenderingWorkspaceSection(
        "Source/Standalone/UI/ArrangementViewComponent.cpp",
        "void ArrangementViewComponent::updateMoveDragPreview",
        "void ArrangementViewComponent::paint");

    if (mouseDragSection.isEmpty() || updatePreviewSection.isEmpty()) {
        logFail(testName, "failed to locate ArrangementViewComponent move drag sections");
        return;
    }

    if (mouseDragSection.contains("moveStandalonePlacement(")) {
        logFail(testName, "mouseDrag must not call the real cross-track move commit helper");
        return;
    }

    if (updatePreviewSection.contains("setStandalonePlacementStartSeconds(")
        || updatePreviewSection.contains("moveStandalonePlacement(")) {
        logFail(testName, "move preview update must not mutate placement truth");
        return;
    }

    logPass(testName);
}

void runArrangementDragPreviewTargetTrackVisibleBeforeMouseUpTest()
{
    constexpr const char* testName = "ArrangementDragPreview_TargetTrackVisibleBeforeMouseUp";

    const auto header = readTimelineRenderingWorkspaceFile("Source/Standalone/UI/ArrangementRenderModelCache.h");
    const auto source = readTimelineRenderingWorkspaceFile("Source/Standalone/UI/ArrangementRenderModelCache.cpp");
    const auto viewSource = readTimelineRenderingWorkspaceFile("Source/Standalone/UI/ArrangementViewComponent.cpp");

    if (!header.contains("MoveDragPreviewState")
        || !header.contains("isPreview")
        || !source.contains("previewTrackId")
        || !source.contains("appendVisiblePlacement(trackId, preview->previewTrackId")
        || !viewSource.contains("updateMoveDragPreview(e)")) {
        logFail(testName, "render model does not project active move preview onto the target track");
        return;
    }

    logPass(testName);
}

void runArrangementDragPreviewMouseUpCommitsOnceAndClearsPreviewTest()
{
    constexpr const char* testName = "ArrangementDragPreview_MouseUpCommitsOnceAndClearsPreview";

    const auto mouseUpSection = extractTimelineRenderingWorkspaceSection(
        "Source/Standalone/UI/ArrangementViewComponent.cpp",
        "void ArrangementViewComponent::mouseUp",
        "void ArrangementViewComponent::mouseDoubleClick");

    if (mouseUpSection.isEmpty()) {
        logFail(testName, "failed to locate ArrangementViewComponent::mouseUp");
        return;
    }

    if (!mouseUpSection.contains("moveStandalonePlacement(")
        || !mouseUpSection.contains("setStandalonePlacementStartSeconds(")
        || !mouseUpSection.contains("clearMoveDragPreview();")
        || !mouseUpSection.contains("finalTrack != dragStartTrackId_")) {
        logFail(testName, "mouseUp must commit final move/time once and clear the UI-only preview");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Suite runner
// ============================================================================

void runTimelineRenderingPipelineCacheTests()
{
    logSection("Timeline Rendering Pipeline — Cache Objects");

    runTimelineViewportStateTimeMathTest();
    runTimelineViewportStateVisibleRangeTest();
    runTimelineViewportStateExposedStripTest();
    runTimelineViewportStateTimeMathTest(); // (already called above — kept for suite completeness)
    runWaveformTileCacheBoundedLruTest();
    runWaveformTileCachePruneByMaterializationTest();
    runArrangementWaveformMinZoomNarrowClipKeepsPositiveDrawableBoundsTest();
    runWaveformTileCacheNarrowDrawableBoundsBuildsNonEmptyPathTest();
    runWaveformTileCacheDifferentBoundsDoNotReuseAbsolutePathTest();
    runArrangementDragPreviewMouseDragDoesNotCommitMoveTest();
    runArrangementDragPreviewTargetTrackVisibleBeforeMouseUpTest();
    runArrangementDragPreviewMouseUpCommitsOnceAndClearsPreviewTest();
}

} // namespace OpenTune
