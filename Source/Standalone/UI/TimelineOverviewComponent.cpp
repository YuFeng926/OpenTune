#include "TimelineOverviewComponent.h"

#include "UIColors.h"
#include "PianoRoll/PianoRollRenderer.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace OpenTune {

namespace {

constexpr int kOverviewVisualHeight = 60;
constexpr float kOverviewContentInsetX = 4.0f;
constexpr float kOverviewContentInsetY = 5.0f;
constexpr float kWaveformAlpha = 0.42f;

void hashCombine(uint64_t& seed, uint64_t value) noexcept
{
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

void paintOverviewClipWaveform(juce::Graphics& g,
                               ContentKey contentKey,
                               const ContentTimelineProjection& projection,
                               juce::Rectangle<float> contentBounds,
                               double pixelsPerSecond,
                               double overviewTimelineStart,
                               const WaveformMipmapCache& waveformMipmapCache)
{
    if (!projection.isValid() || pixelsPerSecond <= 0.0)
        return;

    const auto* mipmap = waveformMipmapCache.get(contentKey);
    if (mipmap == nullptr || !mipmap->hasSource())
        return;

    const int levelIndex = mipmap->selectBestLevelIndex(pixelsPerSecond);
    if (levelIndex < 0)
        return;
    const auto& level = mipmap->getLevel(levelIndex);

    const int64_t numPeaks = static_cast<int64_t>(level.peaks.size());
    const int samplesPerPeak = WaveformMipmap::kSamplesPerPeak[levelIndex];
    const double timePerPeak = static_cast<double>(samplesPerPeak)
        / WaveformMipmap::kBaseSampleRate;
    const double clipStartSeconds = projection.timelineStartSeconds;
    const double clipEndSeconds = projection.timelineEndSeconds();

    juce::Path wavePath;
    const int firstX = static_cast<int>(std::floor(contentBounds.getX()));
    const int lastX = static_cast<int>(std::ceil(contentBounds.getRight()));
    const float midY = contentBounds.getCentreY();
    const float halfH = contentBounds.getHeight() * 0.40f;

    for (int x = firstX; x < lastX; ++x)
    {
        const double timelineTime = overviewTimelineStart
            + (static_cast<double>(x) + 0.5 - static_cast<double>(contentBounds.getX()))
                / pixelsPerSecond;
        if (timelineTime < clipStartSeconds || timelineTime >= clipEndSeconds)
            continue;

        const double contentTime = projection.projectTimelineTimeToContent(timelineTime);
        const int64_t peakIndex = static_cast<int64_t>(std::floor(contentTime / timePerPeak));

        const double nextTimelineTime = std::min(
            clipEndSeconds,
            overviewTimelineStart
                + (static_cast<double>(x) + 1.5 - static_cast<double>(contentBounds.getX()))
                    / pixelsPerSecond);
        const double nextContentTime = projection.projectTimelineTimeToContent(nextTimelineTime);

        int64_t indexStart = peakIndex;
        int64_t indexEnd = static_cast<int64_t>(std::floor(nextContentTime / timePerPeak));
        if (indexEnd <= indexStart)
            indexEnd = indexStart + 1;

        indexStart = std::max<int64_t>(0, indexStart);
        indexEnd = std::min<int64_t>(numPeaks, indexEnd);
        if (indexStart >= indexEnd)
            continue;

        float aggregateMin = 0.0f;
        float aggregateMax = 0.0f;
        bool hasData = false;

        for (int64_t i = indexStart; i < indexEnd; ++i)
        {
            const auto& peak = level.peaks[static_cast<std::size_t>(i)];
            if (peak.isZero())
                continue;

            if (!hasData)
            {
                aggregateMin = peak.getMin();
                aggregateMax = peak.getMax();
                hasData = true;
            }
            else
            {
                aggregateMin = std::min(aggregateMin, peak.getMin());
                aggregateMax = std::max(aggregateMax, peak.getMax());
            }
        }

        if (!hasData)
            continue;

        const float displayTop = aggregateMax * halfH;
        const float displayBottom = aggregateMin * halfH;
        float y1 = midY - displayTop;
        float y2 = midY - displayBottom;
        if ((y2 - y1) < 1.0f)
        {
            const float expand = (1.0f - (y2 - y1)) * 0.5f;
            y1 -= expand;
            y2 += expand;
        }

        const float lineX = static_cast<float>(x) + 0.5f;
        wavePath.startNewSubPath(lineX, y1);
        wavePath.lineTo(lineX, y2);
    }

    if (wavePath.isEmpty())
        return;

    juce::Graphics::ScopedSaveState scoped(g);
    g.reduceClipRegion(contentBounds.getSmallestIntegerContainer());
    g.setColour(UIColors::pianoRollWaveform.withAlpha(kWaveformAlpha));
    g.strokePath(wavePath,
                 juce::PathStrokeType(1.0f, juce::PathStrokeType::curved));
}

} // namespace

TimelineOverviewComponent::TimelineOverviewComponent(
    WaveformMipmapCache& waveformMipmapCache)
    : waveformMipmapCache_(waveformMipmapCache)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void TimelineOverviewComponent::addListener(Listener* listener)
{
    listeners_.add(listener);
}

void TimelineOverviewComponent::removeListener(Listener* listener)
{
    listeners_.remove(listener);
}

juce::Rectangle<float> TimelineOverviewComponent::getContentBounds() const noexcept
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop(juce::jmax(0, bounds.getHeight() - kOverviewVisualHeight));
    return bounds.toFloat().reduced(kOverviewContentInsetX, kOverviewContentInsetY);
}

uint64_t TimelineOverviewComponent::calculateContentSignature() const
{
    uint64_t signature = 0xcbf29ce484222325ull;
    hashCombine(signature, static_cast<uint64_t>(contentKey_.domainKind));
    hashCombine(signature, contentKey_.objectId);
    hashCombine(signature, contentKey_.sourceWindowDiscriminator);
    hashCombine(signature,
                static_cast<uint64_t>(std::hash<double>{}(projection_.timelineStartSeconds)));
    hashCombine(signature,
                static_cast<uint64_t>(std::hash<double>{}(projection_.timelineDurationSeconds)));
    hashCombine(signature,
                static_cast<uint64_t>(std::hash<double>{}(projection_.contentStartSeconds)));
    hashCombine(signature,
                static_cast<uint64_t>(std::hash<double>{}(projection_.contentDurationSeconds)));
    hashCombine(signature, static_cast<uint64_t>(viewportWidthPx_));
    return signature;
}

uint64_t TimelineOverviewComponent::calculateRegularSignature() const
{
    uint64_t signature = 0xcbf29ce484222325ull;
    hashCombine(signature, static_cast<uint64_t>(regularPlacements_.size()));
    hashCombine(signature, static_cast<uint64_t>(std::hash<double>{}(regularTimelineStart_)));
    hashCombine(signature, static_cast<uint64_t>(std::hash<double>{}(regularTimelineEnd_)));
    for (const auto& p : regularPlacements_)
    {
        hashCombine(signature, static_cast<uint64_t>(p.contentKey.domainKind));
        hashCombine(signature, p.contentKey.objectId);
        hashCombine(signature, p.contentKey.sourceWindowDiscriminator);
        hashCombine(signature,
                    static_cast<uint64_t>(std::hash<double>{}(p.projection.timelineStartSeconds)));
        hashCombine(signature,
                    static_cast<uint64_t>(std::hash<double>{}(p.projection.timelineDurationSeconds)));
        hashCombine(signature,
                    static_cast<uint64_t>(std::hash<double>{}(p.projection.contentStartSeconds)));
        hashCombine(signature,
                    static_cast<uint64_t>(std::hash<double>{}(p.projection.contentDurationSeconds)));
        // Each mipmap's build progress participates independently so any single
        // placement's progress change triggers repaint.
        if (const auto* mipmap = waveformMipmapCache_.get(p.contentKey))
            hashCombine(signature, static_cast<uint64_t>(std::hash<float>{}(mipmap->getBuildProgress())));
        else
            hashCombine(signature, 0ull);
    }
    hashCombine(signature, static_cast<uint64_t>(viewportWidthPx_));
    // Camera state is part of the signature so camera changes trigger repaint.
    hashCombine(signature, static_cast<uint64_t>(std::hash<double>{}(camera_.pixelsPerSecond)));
    hashCombine(signature, static_cast<uint64_t>(std::hash<double>{}(camera_.visibleStartSeconds)));
    return signature;
}

TimelineOverviewComponent::Geometry TimelineOverviewComponent::calculateGeometry() const
{
    Geometry geometry;
    geometry.contentBounds = getContentBounds();
    if (geometry.contentBounds.isEmpty() || !projection_.isValid())
        return geometry;

    geometry.spanSeconds = projection_.timelineDurationSeconds;
    if (geometry.spanSeconds <= 0.0)
        return geometry;

    geometry.previewPixelsPerSecond = geometry.contentBounds.getWidth()
        / geometry.spanSeconds;
    if (camera_.pixelsPerSecond > 0.0 && viewportWidthPx_ > 0)
    {
        geometry.visibleDurationSeconds = static_cast<double>(viewportWidthPx_)
            / camera_.pixelsPerSecond;
    }

    const double clipStartSeconds = projection_.timelineStartSeconds;
    const double clipEndSeconds = projection_.timelineEndSeconds();
    geometry.maxVisibleStartSeconds = std::max(
        clipStartSeconds, clipEndSeconds - geometry.visibleDurationSeconds);
    geometry.visibleStartSeconds = juce::jlimit(
        clipStartSeconds, geometry.maxVisibleStartSeconds, camera_.visibleStartSeconds);
    return geometry;
}

TimelineOverviewComponent::Geometry TimelineOverviewComponent::calculateRegularGeometry() const
{
    Geometry geometry;
    geometry.contentBounds = getContentBounds();
    if (geometry.contentBounds.isEmpty())
        return geometry;

    geometry.spanSeconds = regularTimelineEnd_ - regularTimelineStart_;
    if (geometry.spanSeconds <= 0.0)
        return geometry;

    geometry.previewPixelsPerSecond = geometry.contentBounds.getWidth()
        / geometry.spanSeconds;
    if (camera_.pixelsPerSecond > 0.0 && viewportWidthPx_ > 0)
    {
        geometry.visibleDurationSeconds = static_cast<double>(viewportWidthPx_)
            / camera_.pixelsPerSecond;
    }

    geometry.maxVisibleStartSeconds = std::max(
        regularTimelineStart_, regularTimelineEnd_ - geometry.visibleDurationSeconds);
    geometry.visibleStartSeconds = juce::jlimit(
        regularTimelineStart_, geometry.maxVisibleStartSeconds, camera_.visibleStartSeconds);
    return geometry;
}

void TimelineOverviewComponent::onHeartbeatTick(ContentKey contentKey,
                                                const ContentTimelineProjection& projection,
                                                TimelineViewportCamera camera,
                                                int viewportWidthPx)
{
    // Exit regular mode and clear regular state to avoid stale data lingering
    regularCaptureMode_ = false;
    regularPlacements_.clear();
    regularTimelineStart_ = 0.0;
    regularTimelineEnd_ = 0.0;
    lastRegularSignature_ = 0;

    contentKey_ = contentKey;
    projection_ = projection;
    camera_ = camera;
    viewportWidthPx_ = viewportWidthPx;

    const uint64_t signature = calculateContentSignature();

    // mipmap 构建进度变化 → 重绘：任一 complete 非空 level 出现即显示，不等待全量完成
    float buildProgress = 0.0f;
    if (const auto* mipmap = waveformMipmapCache_.get(contentKey_))
        buildProgress = mipmap->getBuildProgress();

    const bool dirty = camera_ != lastCamera_
        || signature != lastSignature_
        || buildProgress != lastBuildProgress_;

    if (!dirty)
        return;

    lastCamera_ = camera_;
    lastSignature_ = signature;
    lastBuildProgress_ = buildProgress;
    repaint();
}

void TimelineOverviewComponent::onHeartbeatTickRegular(std::vector<TimelineContentPlacement> placements,
                                                        double timelineStart,
                                                        double timelineEnd,
                                                        TimelineViewportCamera camera,
                                                        int viewportWidthPx)
{
    regularCaptureMode_ = true;
    regularPlacements_ = std::move(placements);
    regularTimelineStart_ = timelineStart;
    regularTimelineEnd_ = timelineEnd;
    lastSignature_ = 0;
    camera_ = camera;
    viewportWidthPx_ = viewportWidthPx;

    const uint64_t signature = calculateRegularSignature();

    const bool dirty = camera_ != lastCamera_
        || signature != lastRegularSignature_;

    if (!dirty)
        return;

    lastCamera_ = camera_;
    lastRegularSignature_ = signature;
    repaint();
}

void TimelineOverviewComponent::requestNavigation(double visibleStartSeconds)
{
    const auto geometry = regularCaptureMode_ ? calculateRegularGeometry() : calculateGeometry();
    if (geometry.previewPixelsPerSecond <= 0.0 || camera_.pixelsPerSecond <= 0.0)
        return;

    const double rangeStart = regularCaptureMode_
                                  ? regularTimelineStart_
                                  : projection_.timelineStartSeconds;
    const double targetStart = juce::jlimit(
        rangeStart,
        geometry.maxVisibleStartSeconds,
        visibleStartSeconds);
    camera_.visibleStartSeconds = targetStart;

    listeners_.call([targetStart, pixelsPerSecond = camera_.pixelsPerSecond](Listener& listener) {
        listener.overviewNavigateRequested(targetStart, pixelsPerSecond);
    });
    repaint();
}

void TimelineOverviewComponent::updateMouseCursor(juce::Point<float> position)
{
    const auto geometry = regularCaptureMode_ ? calculateRegularGeometry() : calculateGeometry();
    if (geometry.previewPixelsPerSecond <= 0.0
        || !geometry.contentBounds.contains(position))
    {
        setMouseCursor(juce::MouseCursor::NormalCursor);
        return;
    }

    if (isDragging_)
    {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }

    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void TimelineOverviewComponent::mouseDown(const juce::MouseEvent& e)
{
    const auto geometry = regularCaptureMode_ ? calculateRegularGeometry() : calculateGeometry();
    if (geometry.previewPixelsPerSecond <= 0.0
        || !geometry.contentBounds.contains(e.position))
        return;

    const double rangeStart = regularCaptureMode_
                                  ? regularTimelineStart_
                                  : projection_.timelineStartSeconds;
    const double pointerTime = rangeStart
        + (static_cast<double>(e.position.x) - geometry.contentBounds.getX())
            / geometry.previewPixelsPerSecond;
    const float viewX = geometry.contentBounds.getX()
        + static_cast<float>((geometry.visibleStartSeconds - rangeStart)
                             * geometry.previewPixelsPerSecond);
    const float viewRight = viewX + static_cast<float>(geometry.visibleDurationSeconds
                                                        * geometry.previewPixelsPerSecond);
    const bool insideViewport = e.position.x >= viewX && e.position.x <= viewRight;

    dragPointerOffsetSeconds_ = insideViewport
        ? pointerTime - geometry.visibleStartSeconds
        : geometry.visibleDurationSeconds * 0.5;
    isDragging_ = true;
    requestNavigation(pointerTime - dragPointerOffsetSeconds_);
    updateMouseCursor(e.position);
}

void TimelineOverviewComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (!isDragging_)
        return;

    const auto geometry = regularCaptureMode_ ? calculateRegularGeometry() : calculateGeometry();
    if (geometry.previewPixelsPerSecond <= 0.0)
        return;

    const double rangeStart = regularCaptureMode_
                                  ? regularTimelineStart_
                                  : projection_.timelineStartSeconds;
    const double pointerTime = rangeStart
        + (static_cast<double>(e.position.x) - geometry.contentBounds.getX())
            / geometry.previewPixelsPerSecond;
    requestNavigation(pointerTime - dragPointerOffsetSeconds_);
    updateMouseCursor(e.position);
}

void TimelineOverviewComponent::mouseMove(const juce::MouseEvent& e)
{
    updateMouseCursor(e.position);
}

void TimelineOverviewComponent::mouseUp(const juce::MouseEvent& e)
{
    isDragging_ = false;
    dragPointerOffsetSeconds_ = 0.0;
    updateMouseCursor(e.position);
}

void TimelineOverviewComponent::paint(juce::Graphics& g)
{
    auto panelBounds = getLocalBounds();
    panelBounds.removeFromTop(juce::jmax(0, panelBounds.getHeight() - kOverviewVisualHeight));
    const auto panel = panelBounds.toFloat().reduced(0.5f);
    if (panel.isEmpty())
        return;

    const float radius = juce::jmin(8.0f, UIColors::currentThemeStyle().controlRadius);

    // Melodyne style: neutral dark background, waveform as full-width background
    g.setColour(UIColors::darkControlFace.withAlpha(0.90f));
    g.fillRoundedRectangle(panel, radius);

    const auto contentBounds = getContentBounds();
    if (!contentBounds.isEmpty())
    {
        if (regularCaptureMode_)
        {
            const auto geometry = calculateRegularGeometry();
            if (geometry.previewPixelsPerSecond > 0.0)
            {
                for (const auto& placement : regularPlacements_)
                {
                    paintOverviewClipWaveform(g,
                                              placement.contentKey,
                                              placement.projection,
                                              contentBounds,
                                              geometry.previewPixelsPerSecond,
                                              regularTimelineStart_,
                                              waveformMipmapCache_);
                }

                const auto viewBounds = juce::Rectangle<float>(
                    contentBounds.getX()
                        + static_cast<float>((geometry.visibleStartSeconds
                                              - regularTimelineStart_)
                                             * geometry.previewPixelsPerSecond),
                    contentBounds.getY(),
                    static_cast<float>(geometry.visibleDurationSeconds
                                       * geometry.previewPixelsPerSecond),
                    contentBounds.getHeight()).getIntersection(contentBounds);

                if (!viewBounds.isEmpty())
                {
                    const float vr = juce::jmin(4.0f, radius);
                    g.setColour(juce::Colour(0xff2a2a2a).withAlpha(0.55f));
                    g.fillRoundedRectangle(viewBounds, vr);
                    g.setColour(juce::Colour(0xff888888).withAlpha(0.60f));
                    g.drawRoundedRectangle(viewBounds, vr, 1.0f);
                }
            }
        }
        else
        {
            const auto geometry = calculateGeometry();
            if (geometry.previewPixelsPerSecond > 0.0)
            {
                paintOverviewClipWaveform(g,
                                          contentKey_,
                                          projection_,
                                          contentBounds,
                                          geometry.previewPixelsPerSecond,
                                          projection_.timelineStartSeconds,
                                          waveformMipmapCache_);

                const auto viewBounds = juce::Rectangle<float>(
                    contentBounds.getX()
                        + static_cast<float>((geometry.visibleStartSeconds
                                              - projection_.timelineStartSeconds)
                                             * geometry.previewPixelsPerSecond),
                    contentBounds.getY(),
                    static_cast<float>(geometry.visibleDurationSeconds
                                       * geometry.previewPixelsPerSecond),
                    contentBounds.getHeight()).getIntersection(contentBounds);

                if (!viewBounds.isEmpty())
                {
                    const float vr = juce::jmin(4.0f, radius);
                    g.setColour(juce::Colour(0xff2a2a2a).withAlpha(0.55f));
                    g.fillRoundedRectangle(viewBounds, vr);
                    g.setColour(juce::Colour(0xff888888).withAlpha(0.60f));
                    g.drawRoundedRectangle(viewBounds, vr, 1.0f);
                }
            }
        }
    }

    UIColors::drawPanelFrame(g, panel, radius);
}

} // namespace OpenTune
