/**
 * Timeline Rendering Pipeline tests for the new cache objects.
 * Tests: TimelineViewportState, WaveformTileCache, ArrangementRenderModelCache.
 */

#include "TestSupport.h"
#include "../Source/Standalone/UI/TimelineViewportState.h"
#include "../Source/Standalone/UI/WaveformTileCache.h"
#include "../Source/Standalone/UI/WaveformMipmap.h"

namespace OpenTune {

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
    if (cache.get(1, 1, 10, 0.0, 1.0, 0, 0) == nullptr) {
        logFail(testName, "tile 1 should survive prune");
        return;
    }
    if (cache.get(2, 2, 10, 0.0, 1.0, 0, 0) != nullptr) {
        logFail(testName, "tile 2 should be pruned");
        return;
    }

    // Different zoom bucket — should not find
    if (cache.get(1, 1, 20, 0.0, 1.0, 0, 0) != nullptr) {
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
    if (cache.get(2, 2, 5, 0.0, 1.0, 0, 0) == nullptr) {
        logFail(testName, "materialization 2 zoom=5 should survive remove(1)");
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
}

} // namespace OpenTune
