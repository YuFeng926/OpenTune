/**
 * Timeline Rendering Pipeline tests for the new cache objects.
 * Tests: TimelineViewportState, ArrangementRenderModelCache.
 */

#include "TestSupport.h"
#include "../Source/Standalone/UI/TimelineViewportState.h"
#include "../Source/Standalone/UI/ArrangementRenderModelCache.h"
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

WaveformMipmap makeWindowedTestMipmap(double totalSeconds,
                                      double signalStartSeconds,
                                      double signalEndSeconds)
{
    const int numSamples = juce::roundToInt(totalSeconds * WaveformMipmap::kBaseSampleRate);
    WaveformMipmap mipmap;
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, numSamples);
    audio->clear();

    const int startSample = juce::jlimit(0, numSamples, juce::roundToInt(signalStartSeconds * WaveformMipmap::kBaseSampleRate));
    const int endSample = juce::jlimit(startSample, numSamples, juce::roundToInt(signalEndSeconds * WaveformMipmap::kBaseSampleRate));
    for (int i = startSample; i < endSample; ++i) {
        const float polarity = (i % 2 == 0) ? 1.0f : -1.0f;
        audio->setSample(0, i, polarity * 0.8f);
    }

    mipmap.setAudioSource(audio);
    int guard = 0;
    while (!mipmap.isComplete() && guard < 1000) {
        mipmap.buildIncremental(1.0);
        ++guard;
    }

    return mipmap;
}

juce::Rectangle<int> makePlacementBounds(const TimelineViewportState& viewport,
                                         double timelineStartSeconds,
                                         double durationSeconds,
                                         int y = 40,
                                         int height = 48)
{
    const int x1 = viewport.timeToViewportX(timelineStartSeconds);
    const int x2 = viewport.timeToViewportX(timelineStartSeconds + durationSeconds);
    return { x1, y, juce::jmax(8, x2 - x1), height };
}

bool pathTimeEnvelopeCovers(const juce::Path& path,
                            const TimelineViewportState& viewport,
                            double expectedStartSeconds,
                            double expectedEndSeconds,
                            double toleranceSeconds)
{
    if (path.isEmpty())
        return false;

    const auto bounds = path.getBounds();
    const double observedStartSeconds = viewport.viewportXToTime(juce::roundToInt(bounds.getX()));
    const double observedEndSeconds = viewport.viewportXToTime(juce::roundToInt(bounds.getRight()));

    return observedStartSeconds <= expectedStartSeconds + toleranceSeconds
        && observedStartSeconds >= expectedStartSeconds - toleranceSeconds
        && observedEndSeconds >= expectedEndSeconds - toleranceSeconds
        && observedEndSeconds <= expectedEndSeconds + toleranceSeconds;
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

void runArrangementWaveformPathAnchorsToTimelineAcrossZoomAndScrollTest()
{
    constexpr const char* testName = "ArrangementWaveformPath_AnchorsToTimelineAcrossZoomAndScroll";

    auto mipmap = makeWindowedTestMipmap(1.0, 0.30, 0.36);
    if (!mipmap.isComplete()) {
        logFail(testName, "failed to build complete synthetic waveform mipmap");
        return;
    }

    TimelineViewportState normalViewport;
    normalViewport.zoomLevel = 1.0;
    normalViewport.scrollOffsetPx = 0;
    normalViewport.viewportWidthPx = 800;
    normalViewport.viewportHeightPx = 160;
    normalViewport.contentStartX = 8;

    const double timelineStartSeconds = 2.0;
    const double durationSeconds = 1.0;
    const auto normalBounds = makePlacementBounds(normalViewport, timelineStartSeconds, durationSeconds);
    const auto normalPath = ArrangementRenderModelCache::buildWaveformPathForPlacement(mipmap,
                                                                                       normalViewport,
                                                                                       normalBounds,
                                                                                       timelineStartSeconds,
                                                                                       durationSeconds,
                                                                                       0.0,
                                                                                       1.0f);

    TimelineViewportState zoomedViewport = normalViewport;
    zoomedViewport.zoomLevel = 2.0;
    zoomedViewport.scrollOffsetPx = 140;
    const auto zoomedBounds = makePlacementBounds(zoomedViewport, timelineStartSeconds, durationSeconds);
    const auto zoomedPath = ArrangementRenderModelCache::buildWaveformPathForPlacement(mipmap,
                                                                                       zoomedViewport,
                                                                                       zoomedBounds,
                                                                                       timelineStartSeconds,
                                                                                       durationSeconds,
                                                                                       0.0,
                                                                                       1.0f);

    const double expectedStartSeconds = timelineStartSeconds + 0.30;
    const double expectedEndSeconds = timelineStartSeconds + 0.36;
    const double toleranceSeconds = 0.02;

    if (!pathTimeEnvelopeCovers(normalPath, normalViewport, expectedStartSeconds, expectedEndSeconds, toleranceSeconds)) {
        logFail(testName, "normal zoom waveform path is not anchored to the expected timeline time range");
        return;
    }

    if (!pathTimeEnvelopeCovers(zoomedPath, zoomedViewport, expectedStartSeconds, expectedEndSeconds, toleranceSeconds)) {
        logFail(testName, "zoomed/scrolled waveform path drifted away from the expected timeline time range");
        return;
    }

    logPass(testName);
}

void runArrangementWaveformPathHonorsClipInSecondsTest()
{
    constexpr const char* testName = "ArrangementWaveformPath_HonorsClipInSeconds";

    auto mipmap = makeWindowedTestMipmap(1.2, 0.50, 0.56);
    if (!mipmap.isComplete()) {
        logFail(testName, "failed to build complete synthetic waveform mipmap");
        return;
    }

    TimelineViewportState viewport;
    viewport.zoomLevel = 2.0;
    viewport.scrollOffsetPx = 0;
    viewport.viewportWidthPx = 800;
    viewport.viewportHeightPx = 160;
    viewport.contentStartX = 8;

    const double timelineStartSeconds = 1.0;
    const double durationSeconds = 0.6;
    const double clipInSeconds = 0.25;
    const auto placementBounds = makePlacementBounds(viewport, timelineStartSeconds, durationSeconds);
    const auto path = ArrangementRenderModelCache::buildWaveformPathForPlacement(mipmap,
                                                                                 viewport,
                                                                                 placementBounds,
                                                                                 timelineStartSeconds,
                                                                                 durationSeconds,
                                                                                 clipInSeconds,
                                                                                 1.0f);

    const double expectedStartSeconds = timelineStartSeconds + (0.50 - clipInSeconds);
    const double expectedEndSeconds = timelineStartSeconds + (0.56 - clipInSeconds);
    const double toleranceSeconds = 0.02;

    if (!pathTimeEnvelopeCovers(path, viewport, expectedStartSeconds, expectedEndSeconds, toleranceSeconds)) {
        logFail(testName, "trimmed waveform path did not project clipInSeconds into the timeline position");
        return;
    }

    logPass(testName);
}

void runArrangementWaveformPathUsesHalfOpenClipIntervalTest()
{
    constexpr const char* testName = "ArrangementWaveformPath_UsesHalfOpenClipInterval";

    auto mipmap = makeWindowedTestMipmap(1.0, 0.0, 1.0);
    if (!mipmap.isComplete()) {
        logFail(testName, "failed to build complete synthetic waveform mipmap");
        return;
    }

    TimelineViewportState viewport;
    viewport.zoomLevel = 1.0;
    viewport.scrollOffsetPx = 100;
    viewport.viewportWidthPx = 800;
    viewport.viewportHeightPx = 160;
    viewport.contentStartX = 0;

    const double timelineStartSeconds = 0.0;
    const double durationSeconds = 1.0;
    const juce::Rectangle<int> placementBounds(viewport.timeToViewportX(timelineStartSeconds),
                                               40,
                                               juce::jmax(8, viewport.timeToViewportX(timelineStartSeconds + durationSeconds)
                                                           - viewport.timeToViewportX(timelineStartSeconds)),
                                               48);

    const auto path = ArrangementRenderModelCache::buildWaveformPathForPlacement(mipmap,
                                                                                 viewport,
                                                                                 placementBounds,
                                                                                 timelineStartSeconds,
                                                                                 durationSeconds,
                                                                                 0.0,
                                                                                 1.0f);

    if (!path.isEmpty()) {
        logFail(testName, "clip ending exactly at the viewport start must not draw an inclusive-end waveform pixel");
        return;
    }

    logPass(testName);
}

void runArrangementWaveformRenderModelBuildsPreparedPathNotTileLookupTest()
{
    constexpr const char* testName = "ArrangementWaveformRenderModel_BuildsPreparedPathNotTileLookup";

    const auto header = readTimelineRenderingWorkspaceFile("Source/Standalone/UI/ArrangementRenderModelCache.h");
    const auto source = readTimelineRenderingWorkspaceFile("Source/Standalone/UI/ArrangementRenderModelCache.cpp");
    const auto viewHeader = readTimelineRenderingWorkspaceFile("Source/Standalone/UI/ArrangementViewComponent.h");
    const auto viewSource = readTimelineRenderingWorkspaceFile("Source/Standalone/UI/ArrangementViewComponent.cpp");

    if (header.isEmpty() || source.isEmpty() || viewHeader.isEmpty() || viewSource.isEmpty()) {
        logFail(testName, "failed to read Arrangement waveform rendering files");
        return;
    }

    if (!header.contains("juce::Path waveformPath")
        || !source.contains("viewport.viewportXToTime(x)")
        || !source.contains("placement.clipInSeconds")
        || !source.contains("timelineTime - timelineStartSeconds")
        || !source.contains("timelineTime >= timelineEndSeconds")
        || !source.contains("materializationTime >= sourceEndSeconds")) {
        logFail(testName, "Arrangement render model does not build waveform paths from viewport-time reverse projection");
        return;
    }

    if (header.contains("WaveformTileCache")
        || viewHeader.contains("WaveformTileCache")
        || viewHeader.contains("waveformTileCache_")
        || viewSource.contains("waveformTileCache_.get(")
        || viewSource.contains("waveformTileCache_.getOrCreate(")) {
        logFail(testName, "Arrangement production path still depends on WaveformTileCache for clip waveform drawing");
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
    runArrangementWaveformMinZoomNarrowClipKeepsPositiveDrawableBoundsTest();
    runArrangementWaveformPathAnchorsToTimelineAcrossZoomAndScrollTest();
    runArrangementWaveformPathHonorsClipInSecondsTest();
    runArrangementWaveformPathUsesHalfOpenClipIntervalTest();
    runArrangementWaveformRenderModelBuildsPreparedPathNotTileLookupTest();
    runArrangementDragPreviewMouseDragDoesNotCommitMoveTest();
    runArrangementDragPreviewTargetTrackVisibleBeforeMouseUpTest();
    runArrangementDragPreviewMouseUpCommitsOnceAndClearsPreviewTest();
}

} // namespace OpenTune
