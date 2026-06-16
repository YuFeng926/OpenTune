#include "ArrangementViewComponent.h"
#include "AuroraTheme.h"
#include "FrameScheduler.h"
#include "UiAssets.h"
#include "../Utils/ZoomSensitivityConfig.h"
#include "../../Utils/KeyShortcutConfig.h"
#include "../../Utils/PlacementActions.h"
#include "../../Utils/PlacementClipboard.h"
#include "../../Utils/LocalizationManager.h"
#include "../../Utils/SnapUtils.h"
#include "../../Utils/TrackConstants.h"
#include "../StandaloneArrangementHelpers.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace OpenTune {

namespace {

constexpr int kArrangementContentStartX = 8;

uint64_t hashCombine(uint64_t seed, uint64_t value) noexcept
{
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
}

bool setStandalonePlacementStartSeconds(OpenTuneAudioProcessor& processor,
                                        int trackId,
                                        uint64_t placementId,
                                        double startSeconds)
{
    auto* arrangement = processor.getStandaloneArrangement();
    return arrangement != nullptr
        && trackId >= 0
        && trackId < OpenTuneAudioProcessor::MAX_TRACKS
        && placementId != 0
        && arrangement->setPlacementTimelineStartSeconds(trackId, placementId, startSeconds);
}

bool setStandalonePlacementGain(OpenTuneAudioProcessor& processor,
                                int trackId,
                                uint64_t placementId,
                                float gain)
{
    auto* arrangement = processor.getStandaloneArrangement();
    return arrangement != nullptr
        && trackId >= 0
        && trackId < OpenTuneAudioProcessor::MAX_TRACKS
        && placementId != 0
        && arrangement->setPlacementGain(trackId, placementId, gain);
}

} // namespace

static double selectBeatInterval(double pixelsPerBeat) {
    if (pixelsPerBeat < 2.5) return 32.0;
    if (pixelsPerBeat < 5.0) return 16.0;
    if (pixelsPerBeat < 10.0) return 8.0;
    if (pixelsPerBeat < 40.0) return 4.0;
    return 1.0;
}

static double selectMarkerInterval(double pixelsPerSecond) {
    if (pixelsPerSecond < 1.33) return 60.0;
    if (pixelsPerSecond < 4.0) return 30.0;
    if (pixelsPerSecond < 8.0) return 10.0;
    if (pixelsPerSecond < 40.0) return 5.0;
    return 1.0;
}

constexpr double kArrangementDefaultSpanSeconds = 60.0 * 5.0;
constexpr double kArrangementTrailingPaddingSeconds = 10.0;
constexpr double kArrangementPinnedViewportRatio = 0.5;
constexpr double kArrangementRenderBandOverscanScreens = 1.0;

ArrangementViewComponent::ArrangementViewComponent(OpenTuneAudioProcessor& processor)
    : processor_(processor)
{
    viewportState_.contentStartX = kArrangementContentStartX;
    setWantsKeyboardFocus(true);

    addAndMakeVisible(horizontalScrollBar_);
    addAndMakeVisible(verticalScrollBar_);
    horizontalScrollBar_.addListener(this);
    verticalScrollBar_.addListener(this);
    horizontalScrollBar_.setAutoHide(false);
    verticalScrollBar_.setAutoHide(false);

    scrollModeToggleButton_.setButtonText(scrollMode_ == ScrollMode::Continuous ? "Cont" : "Page");
    scrollModeToggleButton_.setLookAndFeel(&smallButtonLookAndFeel_);
    scrollModeToggleButton_.setColour(juce::TextButton::buttonColourId, UIColors::backgroundLight);
    scrollModeToggleButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
    scrollModeToggleButton_.onClick = [this] {
        if (scrollMode_ == ScrollMode::Page) {
            scrollMode_ = ScrollMode::Continuous;
            scrollModeToggleButton_.setButtonText("Cont");
        } else {
            scrollMode_ = ScrollMode::Page;
            scrollModeToggleButton_.setButtonText("Page");
        }
        updateAutoScroll();
        listeners_.call([isCont = (scrollMode_ == ScrollMode::Continuous)](Listener& l) {
            l.scrollModeChanged(isCont);
        });
    };
    scrollModeToggleButton_.setColour(juce::TextButton::buttonColourId, UIColors::backgroundLight);
    scrollModeToggleButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
    addAndMakeVisible(scrollModeToggleButton_);
    scrollModeToggleButton_.setTooltip(LOC(kTooltipScrollMode));

    timeUnitToggleButton_.setButtonText("Time");
    timeUnitToggleButton_.setLookAndFeel(&smallButtonLookAndFeel_);
    timeUnitToggleButton_.onClick = [this] {
        if (timeUnit_ == TimeUnit::Seconds) {
            timeUnit_ = TimeUnit::Bars;
            timeUnitToggleButton_.setButtonText("BPM");
        } else {
            timeUnit_ = TimeUnit::Seconds;
            timeUnitToggleButton_.setButtonText("Time");
        }
        refreshRenderModel();
        FrameScheduler::instance().requestContentInvalidation(*this, {}, FrameScheduler::Priority::Normal);
    };
    timeUnitToggleButton_.setColour(juce::TextButton::buttonColourId, UIColors::backgroundLight);
    timeUnitToggleButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
    addAndMakeVisible(timeUnitToggleButton_);
    timeUnitToggleButton_.setTooltip(LOC(kTooltipTimeUnit));

    addAndMakeVisible(contentSurface_);
    addAndMakeVisible(rulerSurface_);
    addAndMakeVisible(playheadOverlay_);
    timeUnitToggleButton_.toFront(false);
    scrollModeToggleButton_.toFront(false);
    playheadOverlay_.setPianoKeyWidth(kArrangementContentStartX);
    scrollVBlankAttachment_ = std::make_unique<juce::VBlankAttachment>(
        this, [this](double timestampSec) { onScrollVBlankCallback(timestampSec); });
}

ArrangementViewComponent::~ArrangementViewComponent()
{
    scrollVBlankAttachment_.reset();
    scrollModeToggleButton_.setLookAndFeel(nullptr);
    timeUnitToggleButton_.setLookAndFeel(nullptr);
    stopTimer();
    horizontalScrollBar_.removeListener(this);
    verticalScrollBar_.removeListener(this);
}

void ArrangementViewComponent::addListener(Listener* listener)
{
    listeners_.add(listener);
}

void ArrangementViewComponent::removeListener(Listener* listener)
{
    listeners_.remove(listener);
}

void ArrangementViewComponent::setZoomLevel(double zoom)
{
    // 限制缩放范围：0.02~10.0（支持更长音频的完整显示）
    zoomLevel_ = juce::jlimit(0.02, 10.0, zoom);
    viewportState_.zoomLevel = zoomLevel_;
    playheadOverlay_.setZoomLevel(zoomLevel_);
    resetPresentationClock(readPlayheadSeconds());
    updateScrollBars();
    ensureRenderBandCoversCurrentViewport(true);
    updateOverlayPresentation(readPlayheadSeconds());
    FrameScheduler::instance().requestContentInvalidation(*this, {}, FrameScheduler::Priority::Normal);
    listeners_.call([zoom = zoomLevel_](Listener& l) { l.zoomLevelChanged(zoom); });
}

void ArrangementViewComponent::setScrollOffset(int pixels)
{
    const int visibleWidth = getVisibleViewportWidth();
    const int maxScrollOffset = juce::jmax(0, getTotalContentWidth() - visibleWidth);
    const int newOffset = juce::jlimit(0, maxScrollOffset, pixels);
    if (newOffset == scrollOffset_)
        return;

    const int oldOffset = scrollOffset_;
    scrollOffset_ = newOffset;
    viewportState_.scrollOffsetPx = scrollOffset_;

    horizontalScrollBar_.setCurrentRangeStart(scrollOffset_, juce::dontSendNotification);
    updateContentSurfaceBounds();
    updateRulerSurfaceBounds();

    const auto dirtyArea = viewportState_.exposedStripForScrollDelta(oldOffset, newOffset);
    const bool requiresFullRedraw = viewportState_.requiresFullRedrawForDelta(oldOffset, newOffset);
    const bool rebuiltRenderBand = ensureRenderBandCoversCurrentViewport(false);
    const bool requiresFullRepaint = requiresFullRedraw || rebuiltRenderBand;

    if (requiresFullRepaint) {
        FrameScheduler::instance().requestContentInvalidation(*this,
                                                              getLocalBounds(),
                                                              FrameScheduler::Priority::Interactive);
    } else {
        rulerSurface_.repaint();
        contentSurface_.repaint();
        FrameScheduler::instance().requestViewportShift(*this, dirtyArea);
    }

    listeners_.call([newOffset](Listener& l) { l.horizontalScrollChanged(newOffset); });
}

void ArrangementViewComponent::setVerticalScrollOffset(int offset)
{
    // 计算最大滚动偏移（可见轨道高度 + ruler高度 - 可见高度）
    const int totalContentHeight = rulerHeight_ + visibleTrackCount_ * processor_.getTrackHeight();
    const int visibleHeight = getHeight();
    const int maxScrollOffset = juce::jmax(0, totalContentHeight - visibleHeight);
    
    // 限制滚动范围 [0, maxScrollOffset]
    verticalScrollOffset_ = juce::jlimit(0, maxScrollOffset, offset);
    verticalScrollBar_.setCurrentRangeStart(verticalScrollOffset_);
    updateContentSurfaceBounds();
    updateRulerSurfaceBounds();
    ensureRenderBandCoversCurrentViewport(true);
    FrameScheduler::instance().requestContentInvalidation(*this, {}, FrameScheduler::Priority::Normal);
}

void ArrangementViewComponent::setVisibleTrackCount(int count)
{
    visibleTrackCount_ = juce::jlimit(1, OpenTuneAudioProcessor::MAX_TRACKS, count);
    // Re-clamp scroll offset for new track count
    setVerticalScrollOffset(verticalScrollOffset_);
    updateScrollBars();
}

void ArrangementViewComponent::fitToContent()
{
    // 如果用户已手动调整过缩放，不自动覆盖
    if (userHasManuallyZoomed_) {
        return;
    }

    double maxEndTime = 0.0;
    // 音频存储采样率为固定 44.1kHz，用于计算音频时长
    constexpr double storedSampleRate = 44100.0;

    for (int t = 0; t < OpenTuneAudioProcessor::MAX_TRACKS; ++t)
    {
        const int placementCount = getStandalonePlacementCount(processor_, t);
        for (int i = 0; i < placementCount; ++i)
        {
            StandaloneArrangement::Placement placement;
            if (!getStandalonePlacementByIndex(processor_, t, i, placement)) {
                continue;
            }

            juce::ignoreUnused(storedSampleRate);
            maxEndTime = juce::jmax(maxEndTime, placement.timelineEndSeconds());
        }
    }

    if (maxEndTime <= 0.0 || getWidth() <= 8) {
        return;
    }

    int viewWidth = getWidth() - kArrangementContentStartX;
    int paddingPx = 12;
    int drawableWidth = juce::jmax(1, viewWidth - paddingPx);
    double zoom = (static_cast<double>(drawableWidth) / maxEndTime) / 100.0;
    // 限制缩放范围：0.02~10.0（支持自动缩放到更长音频）
    zoom = juce::jlimit(0.02, 10.0, zoom);
    setZoomLevel(zoom);
    setScrollOffset(0);
}

void ArrangementViewComponent::setExperimentalReferenceControlsEnabled(bool enabled)
{
    if (experimentalReferenceControlsEnabled_ == enabled) {
        return;
    }

    experimentalReferenceControlsEnabled_ = enabled;
    if (!experimentalReferenceControlsEnabled_) {
        hoveredPlacementId_ = 0;
        mouseOverReferenceButton_ = false;
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
}

void ArrangementViewComponent::resized()
{
    auto bounds = getLocalBounds();
    horizontalScrollBar_.setBounds(bounds.removeFromBottom(UIColors::scrollBarThickness));
    verticalScrollBar_.setBounds(bounds.removeFromRight(UIColors::scrollBarThickness));

    // Position toggle buttons in top right of ruler
    int btnW = 50;
    int btnH = 20;
    int spacing = 5;
    int currentX = getWidth() - spacing - btnW;
    
    scrollModeToggleButton_.setBounds(currentX, 5, btnW, btnH);
    currentX -= (btnW + spacing);
    timeUnitToggleButton_.setBounds(currentX, 5, btnW, btnH);

    // Sync viewport state dimensions
    viewportState_.viewportWidthPx = getVisibleViewportWidth();
    viewportState_.viewportHeightPx = getContentViewportBounds().getHeight();

    updateScrollBars();
    updateContentSurfaceBounds();
    updateRulerSurfaceBounds();
    ensureRenderBandCoversCurrentViewport(true);

    // 播放头覆盖层覆盖整个组件区域
    playheadOverlay_.setBounds(getLocalBounds());
    timeUnitToggleButton_.toFront(false);
    scrollModeToggleButton_.toFront(false);
    updateOverlayPresentation(readPlayheadSeconds());
}

void ArrangementViewComponent::scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart)
{
    if (scrollBar == &horizontalScrollBar_)
    {
        setScrollOffset(static_cast<int>(newRangeStart));
    }
    else if (scrollBar == &verticalScrollBar_)
    {
        verticalScrollOffset_ = static_cast<int>(newRangeStart);
        // 通知监听器垂直滚动偏移变化（用于同步TrackPanel）
        listeners_.call([this](Listener& l) { l.verticalScrollChanged(verticalScrollOffset_); });
        updateContentSurfaceBounds();
        updateRulerSurfaceBounds();
        ensureRenderBandCoversCurrentViewport(true);
        FrameScheduler::instance().requestContentInvalidation(*this, {}, FrameScheduler::Priority::Normal);
    }
}

int ArrangementViewComponent::getTotalContentWidth() const
{
    return juce::jmax(contentMetrics_.totalContentWidthPx,
                      viewportState_.timeToContentX(kArrangementDefaultSpanSeconds));
}

void ArrangementViewComponent::updateScrollBars()
{
    const int totalContentWidth = getTotalContentWidth();
    const int visibleWidth = getVisibleViewportWidth();

    horizontalScrollBar_.setRangeLimits(0.0, totalContentWidth + visibleWidth, juce::dontSendNotification);
    horizontalScrollBar_.setCurrentRange(scrollOffset_, visibleWidth, juce::dontSendNotification);

    int totalTrackHeight = rulerHeight_ + visibleTrackCount_ * processor_.getTrackHeight();
    int visibleHeight = getHeight() - UIColors::scrollBarThickness;
    verticalScrollBar_.setRangeLimits(0.0, totalTrackHeight + visibleHeight);
    verticalScrollBar_.setCurrentRange(verticalScrollOffset_, visibleHeight);
}

int ArrangementViewComponent::getVisibleViewportWidth() const
{
    return juce::jmax(1,
                      getWidth() - UIColors::scrollBarThickness - viewportState_.contentStartX);
}

juce::Rectangle<int> ArrangementViewComponent::getContentViewportBounds() const
{
    return { viewportState_.contentStartX,
             rulerHeight_,
             getVisibleViewportWidth(),
             juce::jmax(0, getHeight() - rulerHeight_ - UIColors::scrollBarThickness) };
}

bool ArrangementViewComponent::isPinnedContinuousFollowActive() const
{
    return isPlaying_.load(std::memory_order_relaxed) && scrollMode_ == ScrollMode::Continuous;
}

double ArrangementViewComponent::getPinnedPlayheadViewportX() const
{
    const auto viewportBounds = getContentViewportBounds();
    return viewportBounds.getX() + (static_cast<double>(viewportBounds.getWidth()) * kArrangementPinnedViewportRatio);
}

double ArrangementViewComponent::getContinuousFollowTargetScroll(double displayPlayheadTime) const
{
    const double contentX = static_cast<double>(absoluteTimeToContentX(displayPlayheadTime));
    const double targetScroll = contentX - (getPinnedPlayheadViewportX() - viewportState_.contentStartX);
    return juce::jmax(0.0, targetScroll);
}

double ArrangementViewComponent::getDisplayPlayheadTime(double timestampSec) const
{
    if (!presentationClockPrimed_)
        return lastAuthoritativePlayheadTime_;

    const double elapsed = juce::jmax(0.0, timestampSec - presentationClockAnchorTimestampSec_);
    return juce::jmax(0.0, presentationClockAnchorTime_ + elapsed);
}

void ArrangementViewComponent::updatePresentationClock(double authoritativeTime, double timestampSec)
{
    if (!presentationClockPrimed_ || !isPlaying_.load(std::memory_order_relaxed)) {
        resetPresentationClock(authoritativeTime);
        presentationClockAnchorTimestampSec_ = timestampSec;
        presentationClockLastObservationTimestampSec_ = timestampSec;
        return;
    }

    const double predictedNow = getDisplayPlayheadTime(timestampSec);
    const double predictionError = authoritativeTime - predictedNow;
    const double authoritativeDelta = authoritativeTime - lastAuthoritativePlayheadTime_;
    const double observationGap = juce::jmax(0.0, timestampSec - presentationClockLastObservationTimestampSec_);

    const bool discontinuity = authoritativeDelta < -0.001
        || authoritativeDelta > observationGap + 0.050
        || std::abs(predictionError) > 0.050;

    if (discontinuity) {
        presentationClockAnchorTime_ = authoritativeTime;
        presentationClockAnchorTimestampSec_ = timestampSec;
    }

    lastAuthoritativePlayheadTime_ = authoritativeTime;
    presentationClockLastObservationTimestampSec_ = timestampSec;
}

void ArrangementViewComponent::resetPresentationClock(double authoritativeTime)
{
    presentationClockPrimed_ = true;
    presentationClockAnchorTime_ = authoritativeTime;
    presentationClockAnchorTimestampSec_ = juce::Time::getMillisecondCounterHiRes() * 0.001;
    presentationClockLastObservationTimestampSec_ = presentationClockAnchorTimestampSec_;
    lastAuthoritativePlayheadTime_ = authoritativeTime;
}

void ArrangementViewComponent::rebuildContentMetrics()
{
    double maxEndTime = kArrangementDefaultSpanSeconds;
    uint64_t revision = 1469598103934665603ull;

    for (int trackId = 0; trackId < OpenTuneAudioProcessor::MAX_TRACKS; ++trackId) {
        const int placementCount = getStandalonePlacementCount(processor_, trackId);
        revision = hashCombine(revision, static_cast<uint64_t>(trackId + 1));
        revision = hashCombine(revision, static_cast<uint64_t>(placementCount + 1));

        for (int placementIndex = 0; placementIndex < placementCount; ++placementIndex) {
            StandaloneArrangement::Placement placement;
            if (!getStandalonePlacementByIndex(processor_, trackId, placementIndex, placement))
                continue;

            maxEndTime = juce::jmax(maxEndTime,
                                    placement.timelineEndSeconds() + kArrangementTrailingPaddingSeconds);
            revision = hashCombine(revision, placement.placementId);
            revision = hashCombine(revision, placement.contentKey.objectId);
            revision = hashCombine(revision, static_cast<uint64_t>(std::llround(placement.timelineStartSeconds * 1000.0)));
            revision = hashCombine(revision, static_cast<uint64_t>(std::llround(placement.durationSeconds * 1000.0)));
        }
    }

    contentMetrics_.revision = revision;
    contentMetrics_.maxEndTimeSeconds = maxEndTime;
    contentMetrics_.totalContentWidthPx = viewportState_.timeToContentX(maxEndTime);
}

bool ArrangementViewComponent::renderBandNeedsRebuild() const
{
    if (!renderBand_.valid)
        return true;

    const auto viewportBounds = getContentViewportBounds();
    const int visibleStartContentX = scrollOffset_;
    const int visibleEndContentX = visibleStartContentX + viewportBounds.getWidth();
    const int bandEndContentX = renderBand_.startContentX + renderBand_.widthPx;
    return renderBand_.revision != contentMetrics_.revision
        || renderBand_.heightPx != viewportBounds.getHeight()
        || visibleStartContentX < renderBand_.startContentX
        || visibleEndContentX > bandEndContentX;
}

bool ArrangementViewComponent::ensureRenderBandCoversCurrentViewport(bool forceRebuild)
{
    rebuildContentMetrics();
    if (!forceRebuild && !renderBandNeedsRebuild()) {
        updateContentSurfaceBounds();
        updateRulerSurfaceBounds();
        return false;
    }

    const auto viewportBounds = getContentViewportBounds();
    const int overscanPx = static_cast<int>(std::llround(viewportBounds.getWidth() * kArrangementRenderBandOverscanScreens));
    const int totalContentWidth = getTotalContentWidth();
    int bandStartContentX = juce::jmax(0, scrollOffset_ - overscanPx);
    int bandWidthPx = viewportBounds.getWidth() + overscanPx * 2;
    if (bandStartContentX + bandWidthPx > totalContentWidth)
        bandStartContentX = juce::jmax(0, totalContentWidth - bandWidthPx);
    bandWidthPx = juce::jmax(viewportBounds.getWidth(), juce::jmin(totalContentWidth - bandStartContentX, bandWidthPx));

    renderBand_.startContentX = bandStartContentX;
    renderBand_.widthPx = bandWidthPx;
    renderBand_.heightPx = viewportBounds.getHeight();
    renderBand_.startSeconds = viewportState_.viewportXToTime(viewportState_.contentStartX, bandStartContentX);
    renderBand_.endSeconds = viewportState_.viewportXToTime(viewportState_.contentStartX + bandWidthPx, bandStartContentX);
    renderBand_.revision = contentMetrics_.revision;
    renderBand_.valid = true;

    requestRenderModelUpdate();
    rebuildContentSurface();
    rebuildRulerSurface();
    updateContentSurfaceBounds();
    updateRulerSurfaceBounds();
    return true;
}

void ArrangementViewComponent::rebuildContentSurface()
{
    const auto viewportBounds = getContentViewportBounds();
    if (!renderBand_.valid || renderBand_.widthPx <= 0 || viewportBounds.getHeight() <= 0) {
        contentSurfaceImage_ = {};
        contentSurface_.clearSurfaceImage();
        return;
    }

    contentSurfaceImage_ = juce::Image(juce::Image::ARGB,
                                       renderBand_.widthPx,
                                       viewportBounds.getHeight(),
                                       true);
    juce::Graphics g(contentSurfaceImage_);
    drawGridLines(g);
    drawPlacementClips(g, renderModelCache_.getModel(), viewportState_);
    contentSurface_.setSurfaceImage(contentSurfaceImage_);
}

void ArrangementViewComponent::updateContentSurfaceBounds()
{
    const auto viewportBounds = getContentViewportBounds();
    // Fixed bounds at viewport position — never moves during scroll
    contentSurfaceBounds_ = viewportBounds;
    contentSurface_.setBounds(contentSurfaceBounds_);
    // Offset the pre-rendered image to compensate for scroll within the render band
    contentSurface_.setImageOffsetX(renderBand_.valid ? renderBand_.startContentX - scrollOffset_ : 0);
}

void ArrangementViewComponent::drawTimeRulerBackdrop(juce::Graphics& g)
{
    const auto themeId = UIColors::currentThemeId();
    const auto bounds = getLocalBounds();
    const juce::Rectangle<int> rulerArea(0, 0, bounds.getWidth(), rulerHeight_);

    // Exclude button area so the backdrop doesn't paint over the toggle buttons
    const auto buttonCoverBounds = timeUnitToggleButton_.getBounds()
        .getUnion(scrollModeToggleButton_.getBounds())
        .expanded(6, 2);
    g.excludeClipRegion(buttonCoverBounds);

    if (themeId == ThemeId::Aurora)
    {
        UIColors::fillAuroraTimelineBackground(g, rulerArea.toFloat(), 0.0f);
    }
    else if (themeId == ThemeId::BlueBreeze)
    {
        UIColors::fillMistedTimelineField(g, rulerArea.toFloat(), 0.0f);
    }
    else if (themeId != ThemeId::Overdose)
    {
        g.setColour(UIColors::rollBackground);
        g.fillRect(rulerArea);
    }

    g.setColour(themeId == ThemeId::DarkBlueGrey
                    ? UIColors::panelBorder.withAlpha(0.18f)
                    : (themeId == ThemeId::Aurora
                           ? UIColors::gridLine.withAlpha(0.060f)
                           : ((themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose)
                                  ? UIColors::pianoRollGrid.withAlpha(0.040f)
                                  : UIColors::panelBorder)));
    g.drawLine(0.0f,
               static_cast<float>(rulerHeight_),
               static_cast<float>(getWidth()),
               static_cast<float>(rulerHeight_),
               (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) ? 0.7f : 1.0f);
}

void ArrangementViewComponent::rebuildRulerSurface()
{
    const auto viewportBounds = getContentViewportBounds();
    const auto themeId = UIColors::currentThemeId();
    const double bpm = lastContextBpm_ > 0.0 ? lastContextBpm_ : 120.0;
    const int timeSigNum = lastContextTimeSigNum_ > 0 ? lastContextTimeSigNum_ : 4;
    const int timeSigDenom = lastContextTimeSigDenom_ > 0 ? lastContextTimeSigDenom_ : 4;
    const int surfaceWidth = renderBand_.valid ? renderBand_.widthPx : viewportBounds.getWidth();
    const double startSeconds = renderBand_.valid ? renderBand_.startSeconds
                                                  : viewportState_.viewportXToTime(viewportState_.contentStartX, scrollOffset_);
    const double endSeconds = renderBand_.valid ? renderBand_.endSeconds
                                                : viewportState_.viewportXToTime(viewportState_.contentStartX + surfaceWidth, scrollOffset_);
    const int startContentX = renderBand_.valid ? renderBand_.startContentX : scrollOffset_;

    if (rulerSurfaceState_.valid
        && rulerSurfaceState_.startContentX == startContentX
        && rulerSurfaceState_.widthPx == surfaceWidth
        && rulerSurfaceState_.zoomLevel == zoomLevel_
        && rulerSurfaceState_.timeUnit == timeUnit_
        && rulerSurfaceState_.themeId == themeId
        && rulerSurfaceState_.bpm == bpm
        && rulerSurfaceState_.timeSigNum == timeSigNum
        && rulerSurfaceState_.timeSigDenom == timeSigDenom
        && rulerSurfaceState_.startSeconds == startSeconds
        && rulerSurfaceState_.endSeconds == endSeconds)
    {
        updateRulerSurfaceBounds();
        return;
    }

    if (surfaceWidth <= 0)
    {
        rulerSurfaceImage_ = {};
        rulerSurface_.clearSurfaceImage();
        rulerSurfaceState_ = {};
        return;
    }

    rulerSurfaceImage_ = juce::Image(juce::Image::ARGB, surfaceWidth, rulerHeight_, true);
    juce::Graphics g(rulerSurfaceImage_);
    drawTimeRuler(g);
    rulerSurface_.setSurfaceImage(rulerSurfaceImage_);

    rulerSurfaceState_.startSeconds = startSeconds;
    rulerSurfaceState_.endSeconds = endSeconds;
    rulerSurfaceState_.zoomLevel = zoomLevel_;
    rulerSurfaceState_.bpm = bpm;
    rulerSurfaceState_.startContentX = startContentX;
    rulerSurfaceState_.widthPx = surfaceWidth;
    rulerSurfaceState_.timeSigNum = timeSigNum;
    rulerSurfaceState_.timeSigDenom = timeSigDenom;
    rulerSurfaceState_.timeUnit = timeUnit_;
    rulerSurfaceState_.themeId = themeId;
    rulerSurfaceState_.valid = true;

    updateRulerSurfaceBounds();
}

void ArrangementViewComponent::updateRulerSurfaceBounds()
{
    const auto viewportBounds = getContentViewportBounds();
    // Fixed bounds at viewport position — never moves during scroll
    rulerSurfaceBounds_ = { viewportBounds.getX(), 0,
                            viewportBounds.getWidth(), rulerHeight_ };
    rulerSurface_.setBounds(rulerSurfaceBounds_);
    // Offset the pre-rendered image to compensate for scroll within the render band
    rulerSurface_.setImageOffsetX(renderBand_.valid ? renderBand_.startContentX - scrollOffset_ : 0);
}

void ArrangementViewComponent::updateOverlayPresentation(double displayPlayheadTime)
{
    if (isPinnedContinuousFollowActive()) {
        const double contentX = static_cast<double>(absoluteTimeToContentX(displayPlayheadTime));
        const double pinnedOffset = getPinnedPlayheadViewportX() - viewportState_.contentStartX;
        const double desiredScroll = contentX - pinnedOffset;
        const int maxScroll = juce::jmax(0, getTotalContentWidth() - getVisibleViewportWidth());

        if (desiredScroll >= 0.0 && desiredScroll <= static_cast<double>(maxScroll)) {
            playheadOverlay_.setPinnedViewportX(getPinnedPlayheadViewportX());
        } else {
            playheadOverlay_.clearPinnedViewportX();
            playheadOverlay_.setScrollOffset(static_cast<double>(scrollOffset_));
        }
    } else {
        playheadOverlay_.clearPinnedViewportX();
        playheadOverlay_.setScrollOffset(static_cast<double>(scrollOffset_));
    }

    playheadOverlay_.setPlayheadSeconds(displayPlayheadTime);
}

int ArrangementViewComponent::absoluteTimeToContentX(double seconds) const
{
    return viewportState_.timeToContentX(seconds);
}

int ArrangementViewComponent::absoluteTimeToViewportX(double seconds) const
{
    return viewportState_.timeToViewportX(seconds);
}

int ArrangementViewComponent::absoluteTimeToViewportX(double seconds, double projectedScrollOffset) const
{
    return viewportState_.timeToViewportX(seconds, static_cast<int>(std::llround(projectedScrollOffset)));
}

double ArrangementViewComponent::viewportXToAbsoluteTime(int x) const
{
    return viewportState_.viewportXToTime(x);
}

juce::Rectangle<int> ArrangementViewComponent::getTrackLaneBounds(int trackId) const
{
    auto bounds = getLocalBounds().withTrimmedTop(rulerHeight_);
    bounds.removeFromRight(UIColors::scrollBarThickness); // Reserve space for vertical scrollbar
    bounds.removeFromBottom(UIColors::scrollBarThickness); // Reserve space for horizontal scrollbar
    int h = processor_.getTrackHeight();
    return bounds.withY(rulerHeight_ + trackId * h - verticalScrollOffset_).withHeight(h);
}

juce::Rectangle<int> ArrangementViewComponent::buildProjectedPlacementBounds(int trackId, int placementIndex) const
{
    if (trackId < 0 || trackId >= OpenTuneAudioProcessor::MAX_TRACKS || placementIndex < 0) {
        return {};
    }

    StandaloneArrangement::Placement placement;
    if (!getStandalonePlacementByIndex(processor_, trackId, placementIndex, placement)) {
        return {};
    }

    if (placement.durationSeconds <= 0.0) {
        return {};
    }

    const double endSeconds = placement.timelineEndSeconds();

    auto lane = getTrackLaneBounds(trackId).reduced(kClipShellInsetX, kClipShellInsetY);
    const int x1 = absoluteTimeToViewportX(placement.timelineStartSeconds);
    const int x2 = absoluteTimeToViewportX(endSeconds);
    const int width = juce::jmax(8, x2 - x1);
    return {x1, lane.getY(), width, lane.getHeight()};
}

juce::Rectangle<int> ArrangementViewComponent::getPlacementBounds(int trackId, int placementIndex) const
{
    return buildProjectedPlacementBounds(trackId, placementIndex);
}

ArrangementViewComponent::HitTestResult ArrangementViewComponent::hitTestPlacement(juce::Point<int> p) const
{
    HitTestResult r;
    if (p.y < rulerHeight_)
        return r;

    // Adjust for vertical scroll
    int adjustedY = p.y + verticalScrollOffset_;

    // Determine track ID based on dynamic track height
    int h = processor_.getTrackHeight();
    int trackId = (h > 0) ? (adjustedY - rulerHeight_) / h : -1;
    if (trackId < 0 || trackId >= OpenTuneAudioProcessor::MAX_TRACKS) return r;

    const int placementCount = getStandalonePlacementCount(processor_, trackId);
    for (int i = 0; i < placementCount; ++i)
    {
        auto bounds = getPlacementBounds(trackId, i);
        if (bounds.isEmpty())
            continue;
        if (bounds.contains(p))
        {
            r.trackId = trackId;
            r.placementIndex = i;
            r.placementBounds = bounds;
            r.isTopEdge = (p.y - bounds.getY()) <= 6;
            r.isLeftEdge = (p.x - bounds.getX()) <= 8 && bounds.getWidth() > 30;
            r.isRightEdge = (bounds.getRight() - p.x) <= 8 && bounds.getWidth() > 30;

            // Fade handle hit-test (top corners, 16x16 areas)
            if (bounds.getWidth() > 40) {
                juce::Rectangle<int> fadeInRect(bounds.getX(), bounds.getY(), 16, 16);
                juce::Rectangle<int> fadeOutRect(bounds.getRight() - 16, bounds.getY(), 16, 16);
                r.isFadeInHandle = fadeInRect.contains(p);
                r.isFadeOutHandle = fadeOutRect.contains(p);
            }
            return r;
        }
    }
    return r;
}

bool ArrangementViewComponent::buildWaveformCaches(double timeBudgetMs)
{
    if (timeBudgetMs <= 0.0)
        return false;

    std::set<ContentKey> alive;
    
    for (int trackId = 0; trackId < OpenTuneAudioProcessor::MAX_TRACKS; ++trackId)
    {
        const int placementCount = getStandalonePlacementCount(processor_, trackId);
        for (int placementIndex = 0; placementIndex < placementCount; ++placementIndex)
        {
            StandaloneArrangement::Placement placement;
            if (!getStandalonePlacementByIndex(processor_, trackId, placementIndex, placement)) {
                continue;
            }

            const ContentKey key = placement.contentKey;
            alive.insert(key);
            
            auto snap = processor_.getContentSnapshot(placement.contentKey);
            auto audioBuffer = snap ? snap->audioBuffer : nullptr;
            if (audioBuffer)
            {
                auto& mipmap = waveformMipmapCache_.getOrCreate(key);
                mipmap.setAudioSource(audioBuffer);
            }
        }
    }

    waveformMipmapCache_.prune(alive);

    return waveformMipmapCache_.buildIncremental(timeBudgetMs);
}

void ArrangementViewComponent::requestRenderModelUpdate()
{
    const auto viewportBounds = getContentViewportBounds();
    viewportState_.viewportWidthPx = viewportBounds.getWidth();
    viewportState_.viewportHeightPx = viewportBounds.getHeight();

    // Invoke render model cache update with current state
    renderModelCache_.update(processor_,
                             viewportState_,
                             selectedTrack_,
                             selectedPlacementIndex_,
                             [this](int trackId, uint64_t placementId) -> bool {
                                 return isPlacementSelected(trackId, placementId);
                             },
                             [this](uint64_t placementId) -> bool {
                                 auto it = clipAnalysisStates_.find(placementId);
                                 return it != clipAnalysisStates_.end() && it->second.isAnalysisInProgress;
                             },
                             hoveredPlacementId_,
                             mouseOverReferenceButton_,
                             waveformMipmapCache_,
                             renderBand_.startSeconds,
                             renderBand_.endSeconds,
                             renderBand_.startContentX,
                             renderBand_.widthPx,
                             processor_.getTrackHeight(),
                             moveDragPreview_);

    const auto& renderModel = renderModelCache_.getModel();
    const bool rulerContextChanged = lastContextBpm_ != renderModel.bpm
        || lastContextTimeSigNum_ != renderModel.timeSigNumerator
        || lastContextTimeSigDenom_ != renderModel.timeSigDenominator;
    lastContextBpm_ = renderModel.bpm;
    lastContextTimeSigNum_ = renderModel.timeSigNumerator;
    lastContextTimeSigDenom_ = renderModel.timeSigDenominator;
    if (rulerContextChanged)
        rebuildRulerSurface();
}

void ArrangementViewComponent::refreshRenderModel()
{
    renderModelCache_.invalidate();
    ensureRenderBandCoversCurrentViewport(true);
}

int ArrangementViewComponent::trackIdForViewportY(int y) const noexcept
{
    const int trackHeight = processor_.getTrackHeight();
    if (trackHeight <= 0)
        return 0;

    const int adjustedY = y + verticalScrollOffset_;
    return juce::jlimit(0,
                        OpenTuneAudioProcessor::MAX_TRACKS - 1,
                        (adjustedY - rulerHeight_) / trackHeight);
}

void ArrangementViewComponent::clearMoveDragPreview()
{
    if (!moveDragPreview_.active && moveDragPreview_.placements.empty())
        return;

    moveDragPreview_ = {};
    moveDragPreview_.revision = nextMoveDragPreviewRevision_++;
    ensureRenderBandCoversCurrentViewport(true);
    FrameScheduler::instance().requestContentInvalidation(*this, {}, FrameScheduler::Priority::Interactive);
}

// ============================================================================
// Import Drop Preview
// ============================================================================

void ArrangementViewComponent::setImportDropPreview(const ImportDropPreview& preview)
{
    importDropPreview_ = preview;
    repaint();
}

void ArrangementViewComponent::clearImportDropPreview()
{
    if (!importDropPreview_.active && importDropPreview_.targetTrackId < 0 && !importDropPreview_.isNewTrack)
        return;

    importDropPreview_ = {};
    repaint();
}

void ArrangementViewComponent::updateMoveDragPreview(const juce::MouseEvent& e)
{
    if (!isDraggingPlacement_ || currentDragOp_ != DragOperation::Move)
        return;

    const double startT = viewportXToAbsoluteTime(dragStartPos_.x);
    const double currentT = viewportXToAbsoluteTime(e.x);
    const double deltaSeconds = currentT - startT;
    const double bpm = lastContextBpm_ > 0.0 ? lastContextBpm_ : 120.0;
    const SnapSettings snap = processor_.getSnapSettings();
    const int previewTrackId = trackIdForViewportY(e.y);

    ArrangementRenderModelCache::MoveDragPreviewState preview;
    preview.active = true;
    preview.revision = nextMoveDragPreviewRevision_++;

    auto appendPreview = [&](const DragStartState& state) {
        double newStart = state.startSeconds + deltaSeconds;
        if (newStart < 0.0)
            newStart = 0.0;
        newStart = SnapUtils::snapTime(newStart, bpm, snap);

        preview.placements.push_back({ state.trackId, previewTrackId, state.placementId, newStart });
    };

    if (selectedPlacements_.size() > 1 && !multiDragStartStates_.empty()) {
        for (const auto& state : multiDragStartStates_)
            appendPreview(state);
    } else if (selectedPlacementId_ != 0) {
        appendPreview({ dragStartTrackId_, selectedPlacementId_, dragStartPlacementSeconds_ });
    }

    moveDragPreview_ = std::move(preview);
    ensureRenderBandCoversCurrentViewport(true);
    FrameScheduler::instance().requestContentInvalidation(*this, {}, FrameScheduler::Priority::Interactive);
}

void ArrangementViewComponent::paint(juce::Graphics& g)
{
    const auto themeId = UIColors::currentThemeId();
    const auto& model = renderModelCache_.getModel();
    
    auto bounds = getLocalBounds().toFloat();
    
    if (themeId == ThemeId::Aurora)
    {
        UIColors::fillAuroraTimelineBackground(g, bounds, 0.0f);
    }
    else if (themeId == ThemeId::BlueBreeze)
    {
        UIColors::fillMistedTimelineField(g, bounds, 0.0f);
    }
    else if (themeId == ThemeId::Overdose)
    {
        UiAssets::drawAssetStretch(g, UiAssetId::PanelEditorMain, bounds);
    }
    else if (themeId == ThemeId::DarkBlueGrey) {
        // Soothe 2 Spectrum Background Style
        // It has a specific gradient and grid look
        UIColors::fillSoothe2SpectrumBackground(g, bounds, 0.0f);
    } else {
        g.fillAll(UIColors::rollBackground);
    }

    if ((themeId == ThemeId::Aurora || themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose)
        && !model.lanes.empty())
    {
        for (const auto& laneModel : model.lanes)
        {
            auto lane = laneModel.area;
            if (lane.getBottom() < static_cast<float>(rulerHeight_) || lane.getY() > bounds.getBottom())
                continue;

            const auto laneFill = themeId == ThemeId::Aurora
                ? ((laneModel.trackId % 2 == 0) ? UIColors::glassSurface.withAlpha(0.055f) : UIColors::pianoRollLane.withAlpha(0.030f))
                : ((laneModel.trackId % 2 == 0) ? UIColors::pianoRollLane.withAlpha(0.060f) : UIColors::glassSurface.withAlpha(0.022f));
            g.setColour(laneFill);
            g.fillRect(lane);

            if (laneModel.selected)
            {
                g.setColour(UIColors::panelGlow.withAlpha(themeId == ThemeId::Aurora ? 0.040f : 0.038f));
                g.fillRect(lane);
            }

            g.setColour((themeId == ThemeId::Aurora ? UIColors::gridLine : UIColors::pianoRollGrid).withAlpha(themeId == ThemeId::Aurora ? 0.026f : 0.036f));
            g.drawHorizontalLine(juce::roundToInt(lane.getBottom()), lane.getX(), lane.getRight());
        }
    }

    // ---- Import drop preview highlight (transient, UI-only) ----
    if (importDropPreview_.active)
    {
        if (importDropPreview_.isNewTrack)
        {
            // Draw a "new track" indicator below the last visible track
            const int visibleTracks = juce::jmax(1, importDropPreview_.visibleTrackCount);
            const int newTrackHeight = juce::jmax(1, importDropPreview_.trackHeight);
            const int newTrackY = rulerHeight_ + visibleTracks * newTrackHeight - verticalScrollOffset_;
            const int barWidth = juce::jmax(getTotalContentWidth(), getWidth() - UIColors::scrollBarThickness * 2);

            juce::Rectangle<int> newTrackRect(0, newTrackY, barWidth, newTrackHeight);
            if (newTrackRect.getBottom() > 0 && newTrackRect.getY() < getHeight())
            {
                g.setColour(UIColors::panelGlow.withAlpha(0.12f));
                g.fillRect(newTrackRect);
                g.setColour(UIColors::panelGlow.withAlpha(0.40f));
                g.drawHorizontalLine(newTrackRect.getY(), 0.0f, static_cast<float>(barWidth));
                g.setColour(UIColors::panelGlow.withAlpha(0.60f));
                g.setFont(16.0f);
                g.drawText(juce::String::fromUTF8(u8"+ 新建轨道"),
                           newTrackRect.toFloat(),
                           juce::Justification::centredLeft);
            }
        }
        else if (importDropPreview_.targetTrackId >= 0)
        {
            // Highlight the target existing track lane from the prepared lane model.
            for (const auto& laneModel : model.lanes) {
                if (laneModel.trackId != importDropPreview_.targetTrackId)
                    continue;

                const auto laneBounds = laneModel.area.getSmallestIntegerContainer();
                if (laneBounds.isEmpty())
                    break;

                const juce::Colour previewFill = themeId == ThemeId::Aurora
                    ? UIColors::panelGlow.withAlpha(0.10f)
                    : UIColors::panelGlow.withAlpha(0.12f);
                const juce::Colour previewBorder = themeId == ThemeId::Aurora
                    ? UIColors::panelGlow.withAlpha(0.30f)
                    : UIColors::panelGlow.withAlpha(0.35f);

                g.setColour(previewFill);
                g.fillRect(laneBounds);

                g.setColour(previewBorder);
                g.drawRect(laneBounds.toFloat(), 1.5f);
                break;
            }
        }
    }

    drawTimeRulerBackdrop(g);

}

#if JUCE_DEBUG
bool ArrangementViewComponent::runDebugSelfTest()
{
    juce::AudioBuffer<float> audio(2, 4096);
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        float* w = audio.getWritePointer(ch);
        const float v = (ch == 0) ? 0.5f : -0.25f;
        for (int i = 0; i < audio.getNumSamples(); ++i)
            w[i] = v;
    }

    WaveformMipmap mipmap;
    auto sharedAudio = std::make_shared<const juce::AudioBuffer<float>>(audio);
    mipmap.setAudioSource(sharedAudio);
    
    if (!mipmap.hasSource())
        return false;
    if (mipmap.getNumSamples() != 4096)
        return false;

    int guard = 0;
    while (!mipmap.isComplete() && guard < 10000)
    {
        if (!mipmap.buildIncremental(0.25))
            break;
        ++guard;
    }

    if (!mipmap.isComplete())
        return false;
    
    // 测试层级选择
    const auto& level = mipmap.selectBestLevel(100.0);
    if (level.peaks.empty())
        return false;

    // 测试WaveformMipmapCache
    WaveformMipmapCache cache;
    const ContentKey key1{DomainKind::StandaloneClip, 1, 0};
    const ContentKey key2{DomainKind::StandaloneClip, 2, 0};
    auto& m1 = cache.getOrCreate(key1);
    auto& m2 = cache.getOrCreate(key2);
    m1.setAudioSource(sharedAudio);
    m2.setAudioSource(sharedAudio);
    
    std::set<ContentKey> alive;
    alive.insert(key2);
    cache.prune(alive);
    
    if (cache.get(key1) != nullptr)
        return false;
    if (cache.get(key2) == nullptr)
        return false;

    return true;
}
#endif

void ArrangementViewComponent::drawPlacementClips(juce::Graphics& g,
                                                    const ArrangementRenderModelCache::RenderModel& model,
                                                    const TimelineViewportState& /*viewport*/)
{
    const auto themeId = UIColors::currentThemeId();

    for (const auto& vp : model.placements)
    {
        const auto& placementArea = vp.pixelArea;

        if (themeId == ThemeId::DarkBlueGrey && vp.isSelected)
        {
            juce::ColourGradient sel(juce::Colour { 0xFFF7F3EA }, placementArea.getX(), placementArea.getBottom(),
                                     juce::Colour { 0xFFBFE0EF }, placementArea.getX(), placementArea.getY(), false);
            g.setGradientFill(sel);
            g.fillRoundedRectangle(placementArea, 6.0f);
            g.setColour(UIColors::panelBorder.withAlpha(0.55f));
            g.drawRoundedRectangle(placementArea.reduced(0.5f), 6.0f, 1.0f);
        }
        else if (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose)
        {
            const auto topColor = vp.isSelected
                ? UIColors::buttonHover.interpolatedWith(UIColors::glassHighlight, 0.12f)
                : UIColors::buttonNormal.interpolatedWith(UIColors::glassHighlight, 0.075f);
            const auto bottomColor = vp.isSelected
                ? UIColors::buttonPressed.interpolatedWith(UIColors::pianoRollBackground, 0.22f)
                : UIColors::buttonNormal.interpolatedWith(UIColors::pianoRollBackground, 0.22f);

            juce::ColourGradient grad(topColor, placementArea.getX(), placementArea.getY(),
                                      bottomColor, placementArea.getRight(), placementArea.getBottom(), false);
            g.setGradientFill(grad);
            g.fillRoundedRectangle(placementArea, 6.0f);

            juce::ColourGradient source(UIColors::glassHighlight.withAlpha(vp.isSelected ? 0.15f : 0.085f),
                                        placementArea.getX() + placementArea.getWidth() * 0.18f,
                                        placementArea.getY() + placementArea.getHeight() * 0.12f,
                                        juce::Colours::transparentBlack,
                                        placementArea.getRight(),
                                        placementArea.getBottom(),
                                        true);
            g.setGradientFill(source);
            g.fillRoundedRectangle(placementArea.reduced(1.0f), 5.0f);

            g.setColour((vp.isSelected ? UIColors::accent : UIColors::panelBorder).withAlpha(vp.isSelected ? 0.72f : 0.42f));
            g.drawRoundedRectangle(placementArea.reduced(0.5f), 6.0f, vp.isSelected ? 1.2f : 0.9f);
        }
        else if (themeId == ThemeId::Aurora)
        {
            auto trackColor = vp.colour;

            if (vp.isSelected)
            {
                g.setColour(trackColor.withAlpha(0.45f));
                g.fillRoundedRectangle(placementArea, 6.0f);
                g.setColour(juce::Colour(Aurora::Colors::Cyan));
                g.drawRoundedRectangle(placementArea.reduced(0.5f), 6.0f, 2.0f);
            }
            else
            {
                g.setColour(trackColor.withAlpha(0.30f));
                g.fillRoundedRectangle(placementArea, 6.0f);
                g.setColour(trackColor.withAlpha(0.6f));
                g.drawRoundedRectangle(placementArea.reduced(0.5f), 6.0f, 1.0f);
            }
        }
        else
        {
            juce::Colour fill = vp.isSelected ? UIColors::primaryPurple : UIColors::buttonNormal;
            g.setColour(fill);
            g.fillRoundedRectangle(placementArea, 6.0f);
            g.setColour(UIColors::panelBorder);
            g.drawRoundedRectangle(placementArea.reduced(0.5f), 6.0f, 1.0f);
        }

        if (vp.isPreview)
        {
            g.setColour(UIColors::accent.withAlpha(0.18f));
            g.fillRoundedRectangle(placementArea.reduced(1.0f), 5.0f);
            g.setColour(UIColors::accent.withAlpha(0.82f));
            g.drawRoundedRectangle(placementArea.reduced(0.5f), 6.0f, 1.6f);
        }

        // Waveform prepared by ArrangementRenderModelCache.
        if (vp.hasAudioBuffer && !vp.waveformPath.isEmpty())
        {
            const juce::PathStrokeType mainStroke(1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
            const juce::PathStrokeType glowStroke(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

            if (themeId == ThemeId::Aurora)
            {
                g.setColour(juce::Colours::white.withAlpha(0.22f));
                g.strokePath(vp.waveformPath, glowStroke);
                g.setColour(juce::Colours::white.withAlpha(0.78f));
                g.strokePath(vp.waveformPath, mainStroke);
            }
            else if (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose)
            {
                g.setColour(UIColors::pianoRollWaveform.withAlpha(0.09f));
                g.strokePath(vp.waveformPath, glowStroke);
                g.setColour(UIColors::pianoRollWaveform.withAlpha(0.24f));
                g.strokePath(vp.waveformPath, mainStroke);
            }
            else
            {
                g.setColour(juce::Colour(0xFF3E4652).withAlpha(0.22f));
                g.strokePath(vp.waveformPath, glowStroke);
                g.setColour(juce::Colour(0xFF3E4652).withAlpha(0.78f));
                g.strokePath(vp.waveformPath, mainStroke);
            }
        }

        // Draw fade curves
        if (vp.fadeInDuration > 0.001 || vp.fadeOutDuration > 0.001) {
            const double pixelsPerSec = 100.0 * zoomLevel_;

            if (vp.fadeInDuration > 0.001) {
                const double fadePixels = vp.fadeInDuration * pixelsPerSec;
                // Fade-in triangle overlay
                juce::Path fadeInPath;
                fadeInPath.addTriangle(
                    placementArea.getX(), placementArea.getY(),
                    static_cast<float>(placementArea.getX() + fadePixels), placementArea.getY(),
                    placementArea.getX(), placementArea.getBottom());
                g.setColour(juce::Colours::white.withAlpha(0.12f));
                g.fillPath(fadeInPath);

                // Fade-in handle (small square at top-left)
                juce::Rectangle<float> handleInRect(
                    placementArea.getX(), placementArea.getY(), 10.0f, 10.0f);
                g.setColour(juce::Colours::white.withAlpha(0.35f));
                g.fillRect(handleInRect);
            }

            if (vp.fadeOutDuration > 0.001) {
                const double fadePixels = vp.fadeOutDuration * pixelsPerSec;
                // Fade-out triangle overlay
                juce::Path fadeOutPath;
                fadeOutPath.addTriangle(
                    placementArea.getRight(), placementArea.getY(),
                    static_cast<float>(placementArea.getRight() - fadePixels), placementArea.getY(),
                    placementArea.getRight(), placementArea.getBottom());
                g.setColour(juce::Colours::white.withAlpha(0.12f));
                g.fillPath(fadeOutPath);

                // Fade-out handle (small square at top-right)
                juce::Rectangle<float> handleOutRect(
                    placementArea.getRight() - 10.0f, placementArea.getY(), 10.0f, 10.0f);
                g.setColour(juce::Colours::white.withAlpha(0.35f));
                g.fillRect(handleOutRect);
            }
        }

        // Clip name — drawn in a second pass so overlap overlay does not occlude it
    }

    // ==========================================================================
    // Overlap detection: darken overlapping regions between placements on the same track
    // Drawn AFTER waveforms/fades but BEFORE text overlays, so clip name/gain/reference remain visible.
    // ==========================================================================
    if (model.placements.size() > 1)
    {
        for (size_t i = 0; i < model.placements.size(); ++i)
        {
            for (size_t j = i + 1; j < model.placements.size(); ++j)
            {
                const auto& vpA = model.placements[i];
                const auto& vpB = model.placements[j];

                if (vpA.trackId != vpB.trackId)
                    continue;

                const float aLeft = vpA.pixelArea.getX();
                const float aRight = vpA.pixelArea.getRight();
                const float bLeft = vpB.pixelArea.getX();
                const float bRight = vpB.pixelArea.getRight();

                // Check time overlap on X axis
                if (aRight > bLeft && bRight > aLeft)
                {
                    const float overlapLeft = std::max(aLeft, bLeft);
                    const float overlapRight = std::min(aRight, bRight);

                    if (overlapRight > overlapLeft)
                    {
                        // Same track → same Y/height, draw dark overlay once on the overlap region
                        juce::Rectangle<float> overlapRect(
                            overlapLeft, vpA.pixelArea.getY(),
                            overlapRight - overlapLeft, vpA.pixelArea.getHeight());
                        g.setColour(juce::Colours::black.withAlpha(0.30f));
                        g.fillRect(overlapRect);
                    }
                }
            }
        }
    }

    // Second pass: text overlays on top of overlap overlay
    for (const auto& vp : model.placements)
    {
        const auto& placementArea = vp.pixelArea;
        const auto& placementBounds = vp.pixelBounds;

        // Clip name
        if (vp.name.isNotEmpty())
        {
            juce::String displayName = vp.name;
            if (displayName.length() > 20)
                displayName = displayName.substring(0, 17) + "...";
            g.setColour(UIColors::textPrimary.withAlpha(0.85f));
            g.setFont(UIColors::getUIFont(10.0f));
            g.drawText(displayName, placementBounds.reduced(6, 4), juce::Justification::topLeft);
        }

        // Gain label
        {
            float db = (vp.gain > 0.0001f) ? 20.0f * std::log10(vp.gain) : -100.0f;
            juce::String gainStr;
            if (db > -90.0f)
                gainStr = (db >= 0 ? "+" : "") + juce::String(db, 1) + " dB";
            else
                gainStr = "-inf dB";
            g.setColour(UIColors::textSecondary.withAlpha(0.9f));
            g.setFont(UIColors::getUIFont(11.0f));
            g.drawText(gainStr, placementBounds.reduced(6, 4), juce::Justification::topRight);
        }

        // Analysis animation
        if (vp.analysisInProgress)
        {
            const double currentTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
            const float alpha = 0.3f + 0.7f * (1.0f + std::sin(static_cast<float>(currentTime) * 3.0f)) * 0.5f;

            juce::Path clipOutline;
            clipOutline.addRoundedRectangle(placementArea.reduced(2.0f), 6.0f);

            juce::Path dashedPath;
            float dashes[2] = { 5.0f, 5.0f };
            juce::PathStrokeType(1.5f).createDashedStroke(dashedPath, clipOutline, dashes, 2);

            g.setColour(juce::Colours::white.withAlpha(alpha));
            g.fillPath(dashedPath);
        }

        // Reference button icon
        if (experimentalReferenceControlsEnabled_ && placementBounds.getWidth() > 30)
        {
            const bool hasRef = (vp.referencePlacementId != 0);
            const bool isHovering = vp.isHovered && vp.mouseOverReferenceButton;
            const auto iconColor = isHovering
                ? UIColors::accent
                : (hasRef
                    ? UIColors::textSecondary.withAlpha(0.75f)
                    : UIColors::textSecondary.withAlpha(0.35f));

            auto refRect = juce::Rectangle<float>(
                static_cast<float>(placementBounds.getRight() - 19),
                static_cast<float>(placementBounds.getBottom() - 4 - 14),
                14.0f, 14.0f);

            if (hasRef)
            {
                juce::Path refOutline;
                refOutline.addRoundedRectangle(refRect, 3.0f);
                g.setColour(iconColor);
                g.strokePath(refOutline, juce::PathStrokeType(2.0f));

                const float spineX = refRect.getX() + refRect.getWidth() * 0.4f;
                g.drawLine(spineX, refRect.getY() + 2.5f,
                           spineX, refRect.getBottom() - 2.5f, 2.0f);
            }
            else
            {
                juce::Path refOutline;
                refOutline.addRoundedRectangle(refRect, 3.0f);
                g.setColour(iconColor);
                g.strokePath(refOutline, juce::PathStrokeType(1.5f));

                const float cx = refRect.getCentreX();
                const float cy = refRect.getCentreY();
                const float halfLen = 3.5f;
                g.drawLine(cx - halfLen, cy, cx + halfLen, cy, 1.5f);
                g.drawLine(cx, cy - halfLen, cx, cy + halfLen, 1.5f);
            }
        }
    }

    // Debug: draw revision/state info when available
    // (Intentionally blank — reserved for future diagnostic overlay)
}

void ArrangementViewComponent::drawTimeRuler(juce::Graphics& g)
{
    const auto themeId = UIColors::currentThemeId();
    const int rulerWidth = g.getClipBounds().getWidth();
    const double startTime = renderBand_.valid
        ? renderBand_.startSeconds
        : viewportState_.viewportXToTime(viewportState_.contentStartX, scrollOffset_);
    const double endTime = renderBand_.valid
        ? renderBand_.endSeconds
        : viewportState_.viewportXToTime(viewportState_.contentStartX + rulerWidth, scrollOffset_);
    const auto timeToRulerX = [this](double seconds) {
        return renderBand_.valid
            ? absoluteTimeToContentX(seconds) - renderBand_.startContentX
            : absoluteTimeToViewportX(seconds) - viewportState_.contentStartX;
    };

    // Switch between Seconds and Bars based on timeUnit_
    if (timeUnit_ == TimeUnit::Bars)
    {
        double bpm = lastContextBpm_;
        if (bpm <= 0.0) bpm = 120.0;
        
        // Calculate pixels per beat
        double pixelsPerSecond = 100.0 * zoomLevel_;
        double secondsPerBeat = 60.0 / bpm;
        double pixelsPerBeat = pixelsPerSecond * secondsPerBeat;
        
        // Determine interval (in beats) based on density
        // We want at least ~40 pixels between labels
        double beatInterval = selectBeatInterval(pixelsPerBeat);
        
        // Convert visible range to beats
        int64_t startBeat = static_cast<int64_t>(startTime / secondsPerBeat);
        if (startBeat < 0) startBeat = 0;
        // Align to interval
        startBeat = (startBeat / (int64_t)beatInterval) * (int64_t)beatInterval;
        
        int64_t endBeat = static_cast<int64_t>(endTime / secondsPerBeat) + 1;
        
        g.setFont(UIColors::getUIFont(13.0f));
        
        for (int64_t beat = startBeat; beat <= endBeat; beat += (int64_t)beatInterval)
        {
            double time = beat * secondsPerBeat;
            int pixelX = timeToRulerX(time);
            
            // Draw tick
            g.setColour(themeId == ThemeId::DarkBlueGrey
                            ? UIColors::gridLine.withAlpha(0.10f)
                            : (themeId == ThemeId::Aurora
                                   ? UIColors::gridLine.withAlpha(0.080f)
                                   : ((themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) ? UIColors::pianoRollGrid.withAlpha(0.052f) : UIColors::gridLine)));
            g.drawLine(static_cast<float>(pixelX), static_cast<float>(rulerHeight_ - 10),
                       static_cast<float>(pixelX), static_cast<float>(rulerHeight_), (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) ? 0.7f : 1.0f);
            
            // Draw label (Bar:Beat) -> actually just Bar number usually for overview
            // Let's show Bar number (1-based)
            int64_t bar = (beat / 4) + 1;
            int64_t beatInBar = (beat % 4) + 1;
            
            juce::String label;
            if (beatInterval >= 4.0)
                label = juce::String(bar);
            else
                label = juce::String::formatted("%lld.%lld", (long long) bar, (long long) beatInBar);
            
            g.setColour((themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) ? UIColors::textSecondary.withAlpha(0.58f) : UIColors::textSecondary);
            g.drawText(label, pixelX - 20, 2, 40, rulerHeight_ - 12, juce::Justification::centred);
        }
    }
    else
    {
        // Seconds mode
        double pixelsPerSecond = 100.0 * zoomLevel_;
        // double secondsPerPixel = 1.0 / pixelsPerSecond;
        
        double markerInterval = selectMarkerInterval(pixelsPerSecond);

        double firstMarkerTime = juce::jmax(0.0, startTime);
        firstMarkerTime = std::floor(firstMarkerTime / markerInterval) * markerInterval;

        g.setFont(UIColors::getUIFont(13.0f));
        for (double time = firstMarkerTime; time < endTime; time += markerInterval) {
            int pixelX = timeToRulerX(time);
            
            g.setColour(themeId == ThemeId::DarkBlueGrey
                            ? UIColors::gridLine.withAlpha(0.10f)
                            : (themeId == ThemeId::Aurora
                                   ? UIColors::gridLine.withAlpha(0.080f)
                                   : ((themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) ? UIColors::pianoRollGrid.withAlpha(0.052f) : UIColors::gridLine)));
            g.drawLine(static_cast<float>(pixelX), static_cast<float>(rulerHeight_ - 10),
                       static_cast<float>(pixelX), static_cast<float>(rulerHeight_), (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) ? 0.7f : 1.0f);

            int totalSecs = static_cast<int>(time);
            int mins = totalSecs / 60;
            int secs = totalSecs % 60;
            juce::String timeStr = juce::String::formatted("%d:%02d", mins, secs);

            g.setColour((themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) ? UIColors::textSecondary.withAlpha(0.58f) : UIColors::textSecondary);
            g.drawText(timeStr, pixelX - 20, 2, 40, rulerHeight_ - 12, juce::Justification::centred);
        }
    }
}

void ArrangementViewComponent::drawGridLines(juce::Graphics& g)
{
    const auto themeId = UIColors::currentThemeId();
    const int gridWidth = g.getClipBounds().getWidth();
    const int gridHeight = g.getClipBounds().getHeight();
    const auto bandTimeToLocalX = [this](double seconds) {
        return absoluteTimeToContentX(seconds) - renderBand_.startContentX;
    };
    const auto bandStartTime = renderBand_.valid
        ? renderBand_.startSeconds
        : viewportState_.viewportXToTime(viewportState_.contentStartX, scrollOffset_);
    const auto bandEndTime = renderBand_.valid
        ? renderBand_.endSeconds
        : viewportState_.viewportXToTime(viewportState_.contentStartX + gridWidth, scrollOffset_);

    // Switch between Seconds and Bars based on timeUnit_ (match drawTimeRuler logic)
    if (timeUnit_ == TimeUnit::Bars)
    {
        double bpm = lastContextBpm_;
        if (bpm <= 0.0) bpm = 120.0;
        
        double pixelsPerSecond = 100.0 * zoomLevel_;
        double secondsPerBeat = 60.0 / bpm;
        double pixelsPerBeat = pixelsPerSecond * secondsPerBeat;
        
        double beatInterval = selectBeatInterval(pixelsPerBeat);
        
        double startTime = bandStartTime;
        double endTime = bandEndTime;
        
        int64_t startBeat = static_cast<int64_t>(startTime / secondsPerBeat);
        if (startBeat < 0) startBeat = 0;
        startBeat = (startBeat / (int64_t)beatInterval) * (int64_t)beatInterval;
        
        int64_t endBeat = static_cast<int64_t>(endTime / secondsPerBeat) + 1;
        
        // Safety limit
        if (endBeat - startBeat > 2000) endBeat = startBeat + 2000;

        for (int64_t beat = startBeat; beat <= endBeat; beat += (int64_t)beatInterval)
        {
            double time = beat * secondsPerBeat;
            const int pixelX = bandTimeToLocalX(time);
            
            // Only draw lines that are visible and within bounds
            // timeToX already handles scroll offset
            if (pixelX < -2 || pixelX > gridWidth + 2) continue; // Allow slight margin
            
            // Determine if this is a major measure line
            // If beatInterval >= 4 (measures), all are measure lines
            // If beatInterval < 4, only multiples of 4 are measures
            bool isMeasure = false;
            if (beatInterval >= 4.0) {
                isMeasure = true; 
            } else {
                isMeasure = (beat % 4) == 0;
            }

            if (themeId == ThemeId::DarkBlueGrey)
            {
                g.setColour(UIColors::panelBorder.withAlpha(0.12f));
                g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(gridHeight));
            }
            else if (themeId == ThemeId::Aurora)
            {
                g.setColour(UIColors::gridLine.withAlpha(isMeasure ? 0.060f : 0.022f));
                g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(gridHeight));
            }
            else if (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose)
            {
                g.setColour(UIColors::pianoRollGrid.withAlpha(isMeasure ? 0.040f : 0.016f));
                g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(gridHeight));
            }
            else
            {
                if (isMeasure) {
                    g.setColour(UIColors::panelBorder.brighter(0.3f));
                    g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(gridHeight));
                } else {
                    g.setColour(UIColors::panelBorder.withAlpha(0.25f));
                    g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(gridHeight));
                }
            }
        }
    }
    else
    {
        // Seconds mode
        double pixelsPerSecond = 100.0 * zoomLevel_;
        // double secondsPerPixel = 1.0 / pixelsPerSecond;
        
        double markerInterval = selectMarkerInterval(pixelsPerSecond);

        double startTime = bandStartTime;
        if (startTime < 0.0) startTime = 0.0;
        startTime = std::floor(startTime / markerInterval) * markerInterval;

        double endTime = bandEndTime;

        // Safety limit: prevent infinite loop if markerInterval is somehow 0
        if (markerInterval < 0.001) markerInterval = 1.0;

        for (double time = startTime; time < endTime + markerInterval; time += markerInterval) {
            const int pixelX = bandTimeToLocalX(time);
            
            if (pixelX < -2 || pixelX > gridWidth + 2) continue;

            if (themeId == ThemeId::Aurora)
                g.setColour(UIColors::gridLine.withAlpha(0.022f));
            else if (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose)
                g.setColour(UIColors::pianoRollGrid.withAlpha(0.022f));
            else
                g.setColour(themeId == ThemeId::DarkBlueGrey ? UIColors::panelBorder.withAlpha(0.12f) : UIColors::panelBorder.withAlpha(0.25f));
            g.drawVerticalLine(pixelX, 0.0f, static_cast<float>(gridHeight));
        }
    }
}

void ArrangementViewComponent::timerCallback()
{
    onHeartbeatTick();
}

void ArrangementViewComponent::onHeartbeatTick()
{
    if (!isShowing())
        return;

    const bool playingNow = processor_.isPlaying();

    // 同步播放状态到高性能播放头覆盖层
    playheadOverlay_.setPlaying(playingNow);

    // 推理活跃时主动降频：波形后台构建改为低频小预算，减少消息线程竞争。
    bool progressed = false;
    if (inferenceActive_)
    {
        waveformBuildTickCounter_ = (waveformBuildTickCounter_ + 1) % 8;
        if (waveformBuildTickCounter_ == 0)
            progressed = buildWaveformCaches(0.15);
    }
    else if (playingNow)
    {
        waveformBuildTickCounter_ = (waveformBuildTickCounter_ + 1) % 6;
        if (waveformBuildTickCounter_ == 0)
            progressed = buildWaveformCaches(0.25);
    }
    else
    {
        waveformBuildTickCounter_ = 0;
        progressed = buildWaveformCaches(0.75);
    }

    if (progressed) {
        if (playingNow) {
            waveformVisualRefreshPending_ = true;
        } else {
            waveformVisualRefreshPending_ = false;
            renderModelCache_.invalidate();
            ensureRenderBandCoversCurrentViewport(true);
            FrameScheduler::instance().requestContentInvalidation(*this, {}, FrameScheduler::Priority::Background);
        }
    }

    if (!playingNow && waveformVisualRefreshPending_) {
        waveformVisualRefreshPending_ = false;
        renderModelCache_.invalidate();
        ensureRenderBandCoversCurrentViewport(true);
        FrameScheduler::instance().requestContentInvalidation(*this, {}, FrameScheduler::Priority::Background);
    }

    if (!playingNow)
        return;

    // Playback follow remains overlay-only unless waveform mipmaps advanced above.
}

void ArrangementViewComponent::performPageScroll(double playheadTime)
{
    const int visibleW = getVisibleViewportWidth();
    const int absX = absoluteTimeToContentX(playheadTime);
    const int pageIndex = visibleW > 0 ? juce::jmax(0, absX / visibleW) : 0;
    const int newScroll = pageIndex * visibleW;
    if (newScroll != scrollOffset_)
        setScrollOffset(newScroll);
}

void ArrangementViewComponent::updateAutoScroll()
{
    if (!isPlaying_.load(std::memory_order_relaxed))
        return;

    const double playheadTime = readPlayheadSeconds();

    if (scrollMode_ == ScrollMode::Page)
        performPageScroll(playheadTime);
}

void ArrangementViewComponent::onScrollVBlankCallback(double timestampSec)
{
    if (!isShowing() || !isPlaying_.load(std::memory_order_relaxed))
        return;

    const double authoritativeTime = readPlayheadSeconds();
    updatePresentationClock(authoritativeTime, timestampSec);
    const double displayPlayheadTime = getDisplayPlayheadTime(timestampSec);

    if (scrollMode_ == ScrollMode::Continuous)
    {
        setScrollOffset(static_cast<int>(std::llround(getContinuousFollowTargetScroll(displayPlayheadTime))));
        updateOverlayPresentation(displayPlayheadTime);
        return;
    }

    if (scrollMode_ == ScrollMode::Page)
        performPageScroll(displayPlayheadTime);

    updateOverlayPresentation(displayPlayheadTime);
}

double ArrangementViewComponent::readPlayheadSeconds() const
{
    if (auto source = positionSource_.lock())
        return source->load(std::memory_order_relaxed);

    return 0.0;
}

void ArrangementViewComponent::syncPlayheadOverlay()
{
    updateOverlayPresentation(readPlayheadSeconds());
}

void ArrangementViewComponent::syncPlayheadOverlayToAbsoluteTime(double absoluteSeconds, bool repaintOverlay)
{
    juce::ignoreUnused(repaintOverlay);
    updateOverlayPresentation(absoluteSeconds);
}

void ArrangementViewComponent::mouseMove(const juce::MouseEvent& e)
{
    // Track hover state before update to detect transitions
    const auto oldHoveredPlacementId = hoveredPlacementId_;
    const bool wasOverRefBtn = mouseOverReferenceButton_;

    hoveredPlacementId_ = 0;
    mouseOverReferenceButton_ = false;

    auto moveHit = hitTestPlacement(e.getPosition());
    if (moveHit.trackId >= 0 && moveHit.placementIndex >= 0)
    {
        const uint64_t movePlacementId = processor_.getPlacementId(moveHit.trackId, moveHit.placementIndex);
        hoveredPlacementId_ = movePlacementId;

        // Reference button area — match paint gate (width > 30)
        if (experimentalReferenceControlsEnabled_ && moveHit.placementBounds.getWidth() > 30)
        {
            juce::Rectangle<int> refBtnArea(moveHit.placementBounds.getRight() - 20,
                                             moveHit.placementBounds.getBottom() - 20, 20, 20);
            if (refBtnArea.contains(e.getPosition()))
            {
                mouseOverReferenceButton_ = true;
            }
        }
    }

    // Repaint reference button area (narrow dirty zone) if hover state changed
    if (oldHoveredPlacementId != hoveredPlacementId_
        || wasOverRefBtn != mouseOverReferenceButton_)
    {
        // Narrow dirty rect for reference button icon area (bottom-right corner)
        FrameScheduler::instance().requestInvalidate(*this,
            juce::Rectangle<int>(getWidth() - 60, getHeight() - 60, 60, 60),
            FrameScheduler::Priority::Interactive);
    }

    auto hit = hitTestPlacement(e.getPosition());

    // Ctrl+drag cursor preview — only on empty area, consistent with mouseDown
    if (e.mods.isCtrlDown() && hit.trackId < 0)
    {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }

    if (e.y <= rulerHeight_)
    {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        return;
    }

    // Reference button area: pointing hand cursor
    if (mouseOverReferenceButton_)
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        return;
    }

    if (hit.trackId >= 0)
    {
        if (hit.isLeftEdge || hit.isRightEdge) {
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            return;
        }
        if (hit.isFadeInHandle || hit.isFadeOutHandle) {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
            return;
        }
        if (hit.isTopEdge)
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        else
            setMouseCursor(juce::MouseCursor::NormalCursor);
    }
    else
    {
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }
}

void ArrangementViewComponent::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();

    // Ctrl+drag panning — only on empty area (not on a placement, to avoid
    // conflicting with Ctrl+click toggle placement selection).
    if (e.mods.isCtrlDown()) {
        auto hit = hitTestPlacement(e.getPosition());
        if (hit.trackId < 0) {
            isPanning_ = true;
            lastMousePos_ = e.getPosition();
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            return;
        }
    }

    // Hit-test placement before seek — reference button intercepts without seeking
    auto hit = hitTestPlacement(e.getPosition());

    // Check reference button click (bottom-right corner) — always active, before seek
    if (experimentalReferenceControlsEnabled_ && hit.trackId >= 0 && hit.placementBounds.getWidth() > 30)
    {
        juce::Rectangle<int> refBtnArea(hit.placementBounds.getRight() - 20,
                                         hit.placementBounds.getBottom() - 20, 20, 20);
        if (refBtnArea.contains(e.getPosition()))
        {
            // Select this placement so downstream context is correct
            const uint64_t hitPlacementId = processor_.getPlacementId(hit.trackId, hit.placementIndex);
            selectedTrack_ = hit.trackId;
            selectedPlacementIndex_ = hit.placementIndex;
            selectedPlacementId_ = hitPlacementId;
            listeners_.call([&](Listener& l) {
                l.placementSelectionChanged(selectedTrack_, selectedPlacementId_);
            });

            // Compute reference button screen area for popup menu positioning
            auto refBtnScreenArea = localAreaToGlobal(refBtnArea.toFloat()).toNearestInt();
            listeners_.call([&](Listener& l) {
                l.referenceButtonClicked(hit.trackId, hitPlacementId, refBtnScreenArea);
            });
            refreshRenderModel();
            FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
            return;
        }
    }

    // Seek playhead (for clicks outside reference button area)
    double newPosSeconds = juce::jmax(0.0, viewportXToAbsoluteTime(e.x));
    processor_.setPosition(newPosSeconds);
    syncPlayheadOverlayToAbsoluteTime(newPosSeconds, true);

    if (e.y <= rulerHeight_)
    {
        isDraggingPlayhead_ = true;
        dragStartPos_ = e.getPosition();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    // No placement hit — clear selection (unless mod key held)
    if (hit.trackId < 0)
    {
        if (!e.mods.isCtrlDown() && !e.mods.isShiftDown())
        {
            clearPlacementSelection();
        }
        refreshRenderModel();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    const uint64_t hitPlacementId = processor_.getPlacementId(hit.trackId, hit.placementIndex);

    if (e.mods.isCtrlDown() && !e.mods.isShiftDown())
    {
        togglePlacementSelection(hit.trackId, hitPlacementId);
        selectedTrack_ = hit.trackId;
        selectedPlacementIndex_ = hit.placementIndex;
        selectedPlacementId_ = hitPlacementId;
        listeners_.call([this](Listener& l) {
            l.placementSelectionChanged(selectedTrack_, selectedPlacementId_);
        });
        refreshRenderModel();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    if (e.mods.isShiftDown() && hasShiftAnchor_)
    {
        PlacementSelectionKey toKey{hit.trackId, hitPlacementId};
        selectPlacementsInRange(shiftAnchor_, toKey);
        selectedTrack_ = hit.trackId;
        selectedPlacementIndex_ = hit.placementIndex;
        selectedPlacementId_ = hitPlacementId;
        listeners_.call([this](Listener& l) {
            l.placementSelectionChanged(selectedTrack_, selectedPlacementId_);
        });
        refreshRenderModel();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    if (!e.mods.isCtrlDown() && !e.mods.isShiftDown())
    {
        if (!isPlacementSelected(hit.trackId, hitPlacementId))
        {
            clearPlacementSelection();
        }
        shiftAnchor_ = PlacementSelectionKey{hit.trackId, hitPlacementId};
        hasShiftAnchor_ = true;
    }

    selectedTrack_ = hit.trackId;
    selectedPlacementIndex_ = hit.placementIndex;
    selectedPlacementId_ = hitPlacementId;

    if (selectedPlacements_.empty())
    {
        selectedPlacements_.insert(PlacementSelectionKey{selectedTrack_, selectedPlacementId_});
    }
    else if (!isPlacementSelected(selectedTrack_, selectedPlacementId_))
    {
        if (!e.mods.isCtrlDown())
        {
            clearPlacementSelection();
            selectedPlacements_.insert(PlacementSelectionKey{selectedTrack_, selectedPlacementId_});
        }
    }

    listeners_.call([this](Listener& l) {
        l.placementSelectionChanged(selectedTrack_, selectedPlacementId_);
    });

    dragStartPos_ = e.getPosition();
    getStandalonePlacementStartSeconds(processor_, selectedTrack_, selectedPlacementId_, dragStartPlacementSeconds_);
    getStandalonePlacementGain(processor_, selectedTrack_, selectedPlacementId_, dragStartPlacementGain_);
    dragStartPlacementId_ = selectedPlacementId_;
    dragStartTrackId_ = selectedTrack_;

    // Check Fade handles first (before trim — to give priority to 16x16 fade handle areas
    // over 8px edge hit zones that would otherwise absorb clicks in the top corners)
    if (hit.isFadeInHandle) {
        currentDragOp_ = DragOperation::FadeIn;
        StandaloneArrangement::Placement placement;
        if (getStandalonePlacementById(processor_, hit.trackId, hitPlacementId, placement)) {
            fadeStartInDuration_ = placement.fadeInDuration;
            dragOperationPlacementId_ = hitPlacementId;
            dragStartPos_ = e.getPosition();
            dragStartTrackId_ = hit.trackId;
        }
        refreshRenderModel();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    if (hit.isFadeOutHandle) {
        currentDragOp_ = DragOperation::FadeOut;
        StandaloneArrangement::Placement placement;
        if (getStandalonePlacementById(processor_, hit.trackId, hitPlacementId, placement)) {
            fadeStartOutDuration_ = placement.fadeOutDuration;
            dragOperationPlacementId_ = hitPlacementId;
            dragStartPos_ = e.getPosition();
            dragStartTrackId_ = hit.trackId;
        }
        refreshRenderModel();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    // Check Trim edges (after fade — 8px edge zones should not steal hits from 16x16 fade handles)
    if (hit.isLeftEdge) {
        currentDragOp_ = DragOperation::TrimLeft;
        StandaloneArrangement::Placement placement;
        if (getStandalonePlacementById(processor_, hit.trackId, hitPlacementId, placement)) {
            trimStartClipInSeconds_ = placement.clipInSeconds;
            trimStartDurationSeconds_ = placement.durationSeconds;
            dragStartPlacementSeconds_ = placement.timelineStartSeconds;
            dragOperationPlacementId_ = hitPlacementId;
            dragStartPos_ = e.getPosition();
            dragStartTrackId_ = hit.trackId;
        }
        refreshRenderModel();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    if (hit.isRightEdge) {
        currentDragOp_ = DragOperation::TrimRight;
        StandaloneArrangement::Placement placement;
        if (getStandalonePlacementById(processor_, hit.trackId, hitPlacementId, placement)) {
            trimStartClipInSeconds_ = placement.clipInSeconds;
            trimStartDurationSeconds_ = placement.durationSeconds;
            dragOperationPlacementId_ = hitPlacementId;
            dragStartPos_ = e.getPosition();
            dragStartTrackId_ = hit.trackId;
        }
        refreshRenderModel();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    currentDragOp_ = hit.isTopEdge ? DragOperation::Gain : DragOperation::Move;
    isAdjustingGain_ = hit.isTopEdge;
    isDraggingPlacement_ = !isAdjustingGain_;
    clearMoveDragPreview();

    multiDragStartStates_.clear();
    if (isDraggingPlacement_ && selectedPlacements_.size() > 1)
    {
        for (const auto& sel : selectedPlacements_)
        {
            double startSec = 0.0;
            getStandalonePlacementStartSeconds(processor_, sel.trackId, sel.placementId, startSec);
            multiDragStartStates_.push_back({sel.trackId, sel.placementId, startSec});
        }
    }
    
    refreshRenderModel();
    FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
}

void ArrangementViewComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (isPanning_)
    {
        auto delta = e.getPosition() - lastMousePos_;
        
        // Horizontal Scroll
        setScrollOffset(scrollOffset_ - delta.x);
        
        // Vertical Scroll
        verticalScrollOffset_ -= delta.y;
        
        double maxScroll = verticalScrollBar_.getRangeLimit().getEnd() - verticalScrollBar_.getCurrentRangeSize();
        verticalScrollOffset_ = juce::jlimit(0, static_cast<int>(maxScroll), verticalScrollOffset_);
        verticalScrollBar_.setCurrentRangeStart(verticalScrollOffset_);
        
        // 通知监听器垂直滚动偏移变化（用于同步TrackPanel）
        listeners_.call([this](Listener& l) { l.verticalScrollChanged(verticalScrollOffset_); });
        
        lastMousePos_ = e.getPosition();
        return;
    }

    if (isDraggingPlayhead_)
    {
        double newPosSeconds = juce::jmax(0.0, viewportXToAbsoluteTime(e.x));
        processor_.setPosition(newPosSeconds);
        syncPlayheadOverlayToAbsoluteTime(newPosSeconds, true);
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    if (currentDragOp_ == DragOperation::TrimLeft || currentDragOp_ == DragOperation::TrimRight) {
        const double pixelsPerSec = 100.0 * zoomLevel_;
        const double deltaSeconds = static_cast<double>(e.x - dragStartPos_.x) / pixelsPerSec;
        const double bpm = lastContextBpm_ > 0.0 ? lastContextBpm_ : 120.0;
        const SnapSettings snap = processor_.getSnapSettings();
        const double snappedDelta = SnapUtils::snapDelta(deltaSeconds, bpm, snap);

        auto* arr = processor_.getStandaloneArrangement();
        if (!arr || dragOperationPlacementId_ == 0) return;

        StandaloneArrangement::Placement placement;
        if (!arr->getPlacementById(dragStartTrackId_, dragOperationPlacementId_, placement)) return;

        if (currentDragOp_ == DragOperation::TrimLeft) {
            double newClipIn = trimStartClipInSeconds_ + snappedDelta;
            if (newClipIn < 0.0) newClipIn = 0.0;
            double newDuration = trimStartDurationSeconds_ - snappedDelta;
            constexpr double minDur = 0.01;
            if (newDuration < minDur) { newDuration = minDur; newClipIn = trimStartClipInSeconds_ + trimStartDurationSeconds_ - minDur; }
            if (newClipIn < 0.0) newClipIn = 0.0;
            // Shift timelineStart to keep right edge static
            double newStart = dragStartPlacementSeconds_ + snappedDelta;
            if (newStart < 0.0) newStart = 0.0;
            arr->setPlacementTrimAndTimelineStart(dragStartTrackId_, dragOperationPlacementId_,
                                                   newClipIn, newDuration, newStart);
        } else { // TrimRight
            double newDuration = trimStartDurationSeconds_ + snappedDelta;
            constexpr double minDur = 0.01;
            if (newDuration < minDur) newDuration = minDur;
            arr->setPlacementTrim(dragStartTrackId_, dragOperationPlacementId_, trimStartClipInSeconds_, newDuration);
        }

        listeners_.call([this](Listener& l) {
            l.placementTimingChanged(dragStartTrackId_, selectedPlacementIndex_);
        });
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    if (currentDragOp_ == DragOperation::FadeIn || currentDragOp_ == DragOperation::FadeOut) {
        const double pixelsPerSec = 100.0 * zoomLevel_;
        const double deltaSeconds = static_cast<double>(e.x - dragStartPos_.x) / pixelsPerSec;

        auto* arr = processor_.getStandaloneArrangement();
        if (!arr || dragOperationPlacementId_ == 0) return;

        StandaloneArrangement::Placement placement;
        if (!arr->getPlacementById(dragStartTrackId_, dragOperationPlacementId_, placement)) return;

        const double maxFade = placement.durationSeconds * 0.9;
        if (currentDragOp_ == DragOperation::FadeIn) {
            double newFade = fadeStartInDuration_ + deltaSeconds;
            if (newFade < 0.0) newFade = 0.0;
            if (newFade > maxFade) newFade = maxFade;
            arr->setPlacementFade(dragStartTrackId_, dragOperationPlacementId_, newFade, placement.fadeOutDuration);
        } else {
            double newFade = fadeStartOutDuration_ - deltaSeconds; // opposite direction for right side
            if (newFade < 0.0) newFade = 0.0;
            if (newFade > maxFade) newFade = maxFade;
            arr->setPlacementFade(dragStartTrackId_, dragOperationPlacementId_, placement.fadeInDuration, newFade);
        }
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    if (selectedTrack_ < 0 || selectedTrack_ >= OpenTuneAudioProcessor::MAX_TRACKS)
        return;

    int placementIndex = selectedPlacementIndex_;
    if (selectedPlacementId_ != 0) {
        placementIndex = processor_.findPlacementIndexById(selectedTrack_, selectedPlacementId_);
    }
    if (placementIndex < 0 || placementIndex >= getStandalonePlacementCount(processor_, selectedTrack_))
        return;
    selectedPlacementIndex_ = placementIndex;

    if (isDraggingPlacement_)
    {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);

        double startT = viewportXToAbsoluteTime(dragStartPos_.x);
        double currentT = viewportXToAbsoluteTime(e.x);
        double deltaSeconds = currentT - startT;
        const double bpm = lastContextBpm_ > 0.0 ? lastContextBpm_ : 120.0;
        const SnapSettings snap = processor_.getSnapSettings();

        if (selectedPlacements_.size() > 1 && !multiDragStartStates_.empty())
        {
            juce::ignoreUnused(deltaSeconds, bpm, snap);
        }
        else
        {
            juce::ignoreUnused(deltaSeconds, bpm, snap);
        }

        updateMoveDragPreview(e);

        listeners_.call([this](Listener& l) {
            l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
        });

        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
    }
    else if (isAdjustingGain_)
    {
        auto delta = e.getPosition() - dragStartPos_;
        double factor = std::pow(10.0, (-static_cast<double>(delta.y)) / 200.0);
        setStandalonePlacementGain(processor_, selectedTrack_, selectedPlacementId_, static_cast<float>(dragStartPlacementGain_ * factor));

        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
    }
}

void ArrangementViewComponent::mouseUp(const juce::MouseEvent& e)
{
    // Record Trim undo
    if ((currentDragOp_ == DragOperation::TrimLeft || currentDragOp_ == DragOperation::TrimRight) && dragOperationPlacementId_ != 0) {
        auto* arr = processor_.getStandaloneArrangement();
        if (arr) {
            StandaloneArrangement::Placement placement;
            if (arr->getPlacementById(dragStartTrackId_, dragOperationPlacementId_, placement)) {
                if (placement.clipInSeconds != trimStartClipInSeconds_ || placement.durationSeconds != trimStartDurationSeconds_) {
                    processor_.getUndoManager().addAction(
                        std::make_unique<TrimPlacementAction>(processor_, dragStartTrackId_, dragOperationPlacementId_,
                                                               trimStartClipInSeconds_, trimStartDurationSeconds_,
                                                               placement.clipInSeconds, placement.durationSeconds,
                                                               dragStartPlacementSeconds_, placement.timelineStartSeconds));
                }
            }
        }
    }

    // Record Fade undo
    if ((currentDragOp_ == DragOperation::FadeIn || currentDragOp_ == DragOperation::FadeOut) && dragOperationPlacementId_ != 0) {
        auto* arr = processor_.getStandaloneArrangement();
        if (arr) {
            StandaloneArrangement::Placement placement;
            if (arr->getPlacementById(dragStartTrackId_, dragOperationPlacementId_, placement)) {
                if (placement.fadeInDuration != fadeStartInDuration_ || placement.fadeOutDuration != fadeStartOutDuration_) {
                    processor_.getUndoManager().addAction(
                        std::make_unique<FadeChangeAction>(processor_, dragStartTrackId_, dragOperationPlacementId_,
                                                           fadeStartInDuration_, fadeStartOutDuration_,
                                                           placement.fadeInDuration, placement.fadeOutDuration));
                }
            }
        }
    }

    // Reset drag op for non-move/gain operations
    if (currentDragOp_ != DragOperation::Move && currentDragOp_ != DragOperation::Gain) {
        currentDragOp_ = DragOperation::None;
        dragOperationPlacementId_ = 0;
        refreshRenderModel();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
    }

    if (isDraggingPlacement_)
    {
        auto delta = e.getPosition() - dragStartPos_;
        const float dragThreshold = 5.0f;
        bool isDraggedSignificantly = delta.getDistanceFromOrigin() > dragThreshold;

        const int mouseTrackId = trackIdForViewportY(e.y);

        if (selectedPlacements_.size() > 1 && !multiDragStartStates_.empty())
        {
            if (isDraggedSignificantly)
            {
                const int firstPlacementTrackId = multiDragStartStates_.front().trackId;
                bool isCrossTrack = (mouseTrackId != firstPlacementTrackId);

                if (isCrossTrack)
                {
                    std::vector<DragStartState> sortedStates = multiDragStartStates_;
                    std::sort(sortedStates.begin(), sortedStates.end(), [](const auto& a, const auto& b) {
                        return a.startSeconds > b.startSeconds;
                    });

                    for (const auto& state : sortedStates)
                    {
                        double currentStart = state.startSeconds;
                        for (const auto& preview : moveDragPreview_.placements) {
                            if (preview.sourceTrackId == state.trackId && preview.placementId == state.placementId) {
                                currentStart = preview.previewStartSeconds;
                                break;
                            }
                        }
                        moveStandalonePlacement(processor_, state.trackId, mouseTrackId, state.placementId, currentStart);
                    }

                    clearPlacementSelection();
                    selectedTrack_ = mouseTrackId;
                    selectedPlacementIndex_ = getStandaloneSelectedPlacementIndex(processor_, mouseTrackId);
                    if (selectedPlacementIndex_ >= 0 && selectedPlacementIndex_ < getStandalonePlacementCount(processor_, mouseTrackId)) {
                        selectedPlacementId_ = processor_.getPlacementId(mouseTrackId, selectedPlacementIndex_);
                    }
                    for (int i = 0; i < getStandalonePlacementCount(processor_, mouseTrackId); ++i)
                    {
                        const uint64_t movedPlacementId = processor_.getPlacementId(mouseTrackId, i);
                        for (const auto& state : multiDragStartStates_)
                        {
                            if (state.placementId == movedPlacementId)
                            {
                                selectedPlacements_.insert(PlacementSelectionKey{mouseTrackId, movedPlacementId});
                                break;
                            }
                        }
                    }

                    listeners_.call([this](Listener& l) {
                        l.placementSelectionChanged(selectedTrack_, selectedPlacementId_);
                    });
                }
                else
                {
                    for (const auto& state : multiDragStartStates_)
                    {
                        for (const auto& preview : moveDragPreview_.placements) {
                            if (preview.sourceTrackId == state.trackId && preview.placementId == state.placementId) {
                                setStandalonePlacementStartSeconds(processor_, state.trackId, state.placementId, preview.previewStartSeconds);
                                break;
                            }
                        }
                    }
                }
            }
        }
        else if (dragStartPlacementId_ != 0)
        {
            double currentStart = dragStartPlacementSeconds_;
            if (!moveDragPreview_.placements.empty())
                currentStart = moveDragPreview_.placements.front().previewStartSeconds;

            if (isDraggedSignificantly && mouseTrackId != dragStartTrackId_) {
                if (moveStandalonePlacement(processor_, dragStartTrackId_, mouseTrackId, dragStartPlacementId_, currentStart)) {
                    selectedTrack_ = mouseTrackId;
                    selectedPlacementIndex_ = getStandaloneSelectedPlacementIndex(processor_, mouseTrackId);
                    selectedPlacementId_ = dragStartPlacementId_;
                    listeners_.call([this](Listener& l) {
                        l.placementSelectionChanged(selectedTrack_, selectedPlacementId_);
                    });
                }
            } else if (isDraggedSignificantly) {
                setStandalonePlacementStartSeconds(processor_, dragStartTrackId_, dragStartPlacementId_, currentStart);
            }
        }
    }
    
    if (isAdjustingGain_ && dragStartPlacementId_ != 0) {
        float currentGain = 1.0f;
        getStandalonePlacementGain(processor_, selectedTrack_, dragStartPlacementId_, currentGain);
        if (currentGain != dragStartPlacementGain_) {
            processor_.getUndoManager().addAction(
                std::make_unique<GainChangeAction>(processor_, selectedTrack_, dragStartPlacementId_,
                                                    dragStartPlacementGain_, currentGain));
        }
    }

    // Record undo for single-placement move
    if (isDraggingPlacement_ && dragStartPlacementId_ != 0 && selectedPlacements_.size() <= 1) {
        const int finalTrack = selectedTrack_;
        double finalStart = 0.0;
        getStandalonePlacementStartSeconds(processor_, finalTrack, dragStartPlacementId_, finalStart);
        if (finalTrack != dragStartTrackId_ || finalStart != dragStartPlacementSeconds_) {
            processor_.getUndoManager().addAction(
                std::make_unique<MovePlacementAction>(processor_, dragStartTrackId_, finalTrack,
                                                      dragStartPlacementId_,
                                                      dragStartPlacementSeconds_, finalStart));
        }
    }

    isDraggingPlacement_ = false;
    isAdjustingGain_ = false;
    isDraggingPlayhead_ = false;
    isPanning_ = false;
    currentDragOp_ = DragOperation::None;
    dragOperationPlacementId_ = 0;
    dragStartTrackId_ = -1;
    multiDragStartStates_.clear();
    clearMoveDragPreview();
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void ArrangementViewComponent::mouseDoubleClick(const juce::MouseEvent& e)
{
    auto hit = hitTestPlacement(e.getPosition());
    if (hit.trackId >= 0 && hit.placementIndex >= 0)
    {
        listeners_.call([&](Listener& l) {
            l.placementDoubleClicked(hit.trackId, hit.placementIndex);
        });
    }
}

void ArrangementViewComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto& settings = zoomSensitivity_;
    
    // Shift + Wheel = Vertical Zoom (Track Height) - 与TrackPanel同步
    if (e.mods.isShiftDown())
    {
        if (wheel.deltaY != 0.0f)
        {
            int currentHeight = processor_.getTrackHeight();
            int change = static_cast<int>(wheel.deltaY * settings.verticalZoomFactor * 150);
            change = (change == 0) ? ((wheel.deltaY > 0) ? 10 : -10) : change;
            
            int newHeight = juce::jlimit(70, 300, currentHeight + change);
            
            if (newHeight != currentHeight)
            {
                processor_.setTrackHeight(newHeight);
                listeners_.call([newHeight](Listener& l) { l.trackHeightChanged(newHeight); });
                refreshRenderModel();
                FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
            }
        }
        return;
    }

    // Ctrl + Wheel = Horizontal Zoom at Pointer
    if (e.mods.isCtrlDown())
    {
        if (wheel.deltaY != 0.0f)
        {
            double zoomFactor = 1.0 + wheel.deltaY * settings.horizontalZoomFactor * 1.7;
            zoomFactor = juce::jlimit(0.5, 1.5, zoomFactor);
            double newZoom = juce::jlimit(0.02, 10.0, zoomLevel_ * zoomFactor);

            if (std::abs(newZoom - zoomLevel_) > 0.001)
            {
                double timeAtMouse = viewportXToAbsoluteTime(e.x);

                setZoomLevel(newZoom);
                userHasManuallyZoomed_ = true;

                int newOffset = absoluteTimeToContentX(timeAtMouse) + kArrangementContentStartX - e.x;
                setScrollOffset(newOffset);

                FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Normal);
            }
        }
        return;
    }

    // Alt + Wheel = Horizontal Scroll (Time)
    if (e.mods.isAltDown())
    {
        if (wheel.deltaY != 0.0f)
        {
            setScrollOffset(scrollOffset_ - static_cast<int>(wheel.deltaY * settings.scrollSpeed * 10.0f));
        }
        return;
    }

    // Default: Vertical Scroll (Tracks)
    if (wheel.deltaY != 0.0f)
    {
        int scrollDelta = static_cast<int>(wheel.deltaY * settings.scrollSpeed);
        int newOffset = verticalScrollOffset_ - scrollDelta;
        setVerticalScrollOffset(newOffset);
        listeners_.call([this](Listener& l) { l.verticalScrollChanged(verticalScrollOffset_); });
    }
    
    // Horizontal Scroll via Touchpad/Mouse Horizontal Wheel
    if (wheel.deltaX != 0.0f)
    {
        setScrollOffset(scrollOffset_ - static_cast<int>(wheel.deltaX * settings.scrollSpeed * 5.0f));
    }
}

bool ArrangementViewComponent::keyPressed(const juce::KeyPress& key)
{
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::PlayPause, key))
    {
        processor_.setPlaying(!processor_.isPlaying());
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::SelectAll, key))
    {
        selectAllPlacementsInTrack(selectedTrack_);
        return true;
    }

    // CopyClips — Ctrl+C
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Copy, key))
    {
        if (!selectedPlacements_.empty())
        {
            std::vector<PlacementClipEntry> entries;
            for (const auto& sel : selectedPlacements_)
            {
                StandaloneArrangement::Placement placement;
                if (getStandalonePlacementById(processor_, sel.trackId, sel.placementId, placement))
                {
                    PlacementClipEntry entry;
                    entry.sourceTrackId = sel.trackId;
                    entry.sourceContentKey = placement.contentKey;
                    entry.clipInSeconds = placement.clipInSeconds;
                    entry.durationSeconds = placement.durationSeconds;
                    entry.gain = placement.gain;
                    entry.fadeInDuration = placement.fadeInDuration;
                    entry.fadeOutDuration = placement.fadeOutDuration;
                    entry.name = placement.name;
                    entries.push_back(std::move(entry));
                }
            }
            processor_.getClipClipboard().store(std::move(entries));
        }
        return true;
    }

    // PasteClips — Ctrl+V (at playhead)
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Paste, key))
    {
        auto& clipboard = processor_.getClipClipboard();
        if (clipboard.hasEntries())
        {
            double pasteTime = processor_.getPosition();
            if (pasteTime < 0.0) pasteTime = 0.0;

            for (const auto& entry : clipboard.entries())
            {
                auto* arr = processor_.getStandaloneArrangement();
                if (!arr) break;

                // Copy content range
                ContentKey newContentKey = processor_.copyContentRange(
                    entry.sourceContentKey, entry.clipInSeconds, entry.durationSeconds);
                if (!newContentKey.isValid()) continue;

                StandaloneArrangement::Placement newPlacement;
                newPlacement.placementId = 0; // will be assigned by insertPlacement
                newPlacement.contentKey = newContentKey;
                newPlacement.mappingRevision = 1;
                newPlacement.timelineStartSeconds = pasteTime;
                newPlacement.durationSeconds = entry.durationSeconds;
                newPlacement.gain = entry.gain;
                newPlacement.fadeInDuration = entry.fadeInDuration;
                newPlacement.fadeOutDuration = entry.fadeOutDuration;
                newPlacement.name = entry.name;
                newPlacement.clipInSeconds = 0.0; // copy starts from beginning of new content

                if (!arr->insertPlacement(selectedTrack_, newPlacement)) {
                    // Rollback — delete the orphan content
                    processor_.getStandaloneContentRepository()->retireClip(newContentKey);
                    continue;
                }
                pasteTime += entry.durationSeconds; // chain placements sequentially
            }

            refreshRenderModel();
            FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        }
        return true;
    }

    // DuplicateClip — Ctrl+D
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::DuplicateClip, key))
    {
        if (selectedPlacementId_ != 0 && selectedTrack_ >= 0)
        {
            StandaloneArrangement::Placement placement;
            if (getStandalonePlacementById(processor_, selectedTrack_, selectedPlacementId_, placement))
            {
                ContentKey newContentKey = processor_.cloneContent(
                    placement.contentKey, placement.name + " Copy");
                if (newContentKey.isValid())
                {
                    auto* arr = processor_.getStandaloneArrangement();
                    if (arr)
                    {
                        StandaloneArrangement::Placement dup = placement;
                        dup.placementId = 0;
                        dup.contentKey = newContentKey;
                        dup.mappingRevision = 1;

                        const int count = arr->getNumPlacements(selectedTrack_);
                        if (!arr->insertPlacement(selectedTrack_, count, dup)) {
                            // Rollback — delete the orphan content
                            processor_.getStandaloneContentRepository()->retireClip(newContentKey);
                            return true;
                        }

                        arr->selectPlacement(selectedTrack_, dup.placementId);
                        selectedTrack_ = arr->getActiveTrackId();
                        selectedPlacementIndex_ = processor_.findPlacementIndexById(selectedTrack_, dup.placementId);
                        selectedPlacementId_ = dup.placementId;
                        clearPlacementSelection();
                        selectedPlacements_.insert(PlacementSelectionKey{selectedTrack_, selectedPlacementId_});
                        hasShiftAnchor_ = true;
                        shiftAnchor_ = PlacementSelectionKey{selectedTrack_, selectedPlacementId_};

                        listeners_.call([this](Listener& l) {
                            l.placementSelectionChanged(selectedTrack_, selectedPlacementId_);
                            l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
                        });
                        refreshRenderModel();
                        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
                    }
                }
            }
        }
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::SplitClip, key))
    {
        if (selectedTrack_ < 0 || selectedTrack_ >= OpenTuneAudioProcessor::MAX_TRACKS)
            return true;

        double splitSeconds = processor_.getPosition();
        bool anySplit = false;

        // Copy selected placements to a vector to avoid iterator invalidation during split
        std::vector<std::pair<int, uint64_t>> toSplit;
        for (const auto& sel : selectedPlacements_)
            toSplit.emplace_back(sel.trackId, sel.placementId);

        for (const auto& [trackId, placementId] : toSplit)
        {
            int idx = processor_.findPlacementIndexById(trackId, placementId);
            if (idx < 0) continue;

            StandaloneArrangement::Placement placement;
            if (!getStandalonePlacementById(processor_, trackId, placementId, placement)) continue;

            // Only split if playhead is within this placement's timeline range
            double start = placement.timelineStartSeconds;
            double end = placement.timelineEndSeconds();
            if (splitSeconds <= start || splitSeconds >= end) continue;

            auto splitOutcome = processor_.splitPlacementAtSeconds(trackId, idx, splitSeconds);
            if (splitOutcome.has_value())
            {
                processor_.getUndoManager().addAction(
                    std::make_unique<SplitPlacementAction>(processor_, *splitOutcome));
                anySplit = true;
            }
        }

        if (anySplit)
        {
            // Refresh selection state after splits
            selectedTrack_ = processor_.getStandaloneArrangement()
                ? processor_.getStandaloneArrangement()->getActiveTrackId()
                : selectedTrack_;
            selectedPlacementIndex_ = getStandaloneSelectedPlacementIndex(processor_, selectedTrack_);
            if (selectedPlacementIndex_ >= 0 && selectedPlacementIndex_ < getStandalonePlacementCount(processor_, selectedTrack_)) {
                selectedPlacementId_ = processor_.getPlacementId(selectedTrack_, selectedPlacementIndex_);
            } else {
                selectedPlacementId_ = 0;
            }
            listeners_.call([this](Listener& l) {
                l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
            });
            refreshRenderModel();
            FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        }
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::MergeClips, key))
    {
        if (selectedTrack_ < 0 || selectedTrack_ >= OpenTuneAudioProcessor::MAX_TRACKS)
            return true;

        const int placementCount = getStandalonePlacementCount(processor_, selectedTrack_);
        if (selectedPlacementIndex_ < 0 || selectedPlacementIndex_ + 1 >= placementCount)
            return true;

        StandaloneArrangement::Placement leadingPlacement;
        StandaloneArrangement::Placement trailingPlacement;
        if (!getStandalonePlacementByIndex(processor_, selectedTrack_, selectedPlacementIndex_, leadingPlacement)
            || !getStandalonePlacementByIndex(processor_, selectedTrack_, selectedPlacementIndex_ + 1, trailingPlacement)) {
            return true;
        }

        auto mergeOutcome = processor_.mergePlacements(selectedTrack_, leadingPlacement.placementId, trailingPlacement.placementId, selectedPlacementIndex_);
        if (mergeOutcome.has_value()) {
            processor_.getUndoManager().addAction(
                std::make_unique<MergePlacementAction>(processor_, *mergeOutcome));

            selectedPlacementIndex_ = getStandaloneSelectedPlacementIndex(processor_, selectedTrack_);
            if (selectedPlacementIndex_ >= 0 && selectedPlacementIndex_ < getStandalonePlacementCount(processor_, selectedTrack_)) {
                selectedPlacementId_ = processor_.getPlacementId(selectedTrack_, selectedPlacementIndex_);
            } else {
                selectedPlacementId_ = 0;
            }
            listeners_.call([this](Listener& l) {
                l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
            });
            refreshRenderModel();
            FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        }
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Delete, key))
    {
        bool anyDeleted = false;

        // Copy to avoid iterator invalidation
        std::vector<std::pair<int, uint64_t>> toDelete;
        for (const auto& sel : selectedPlacements_)
            toDelete.emplace_back(sel.trackId, sel.placementId);

        for (const auto& [trackId, placementId] : toDelete)
        {
            int idx = processor_.findPlacementIndexById(trackId, placementId);
            if (idx < 0) continue;

            auto deleteOutcome = processor_.deletePlacement(trackId, idx);
            if (deleteOutcome.has_value())
            {
                processor_.getUndoManager().addAction(
                    std::make_unique<DeletePlacementAction>(processor_, *deleteOutcome));
                anyDeleted = true;
            }
        }

        if (anyDeleted)
        {
            clearPlacementSelection();
            selectedPlacementIndex_ = getStandaloneSelectedPlacementIndex(processor_, selectedTrack_);
            if (selectedPlacementIndex_ >= 0 && selectedPlacementIndex_ < getStandalonePlacementCount(processor_, selectedTrack_)) {
                selectedPlacementId_ = processor_.getPlacementId(selectedTrack_, selectedPlacementIndex_);
            } else {
                selectedPlacementId_ = 0;
            }
            listeners_.call([this](Listener& l) {
                l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
            });
            refreshRenderModel();
            FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        }
        return true;
    }

    // Nudge Left — move selected placements earlier by 10ms
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::NudgeLeft, key))
    {
        auto* arr = processor_.getStandaloneArrangement();
        if (!arr) return true;

        std::vector<MultiMovePlacementAction::Entry> movedEntries;
        movedEntries.reserve(selectedPlacements_.size());
        for (const auto& sel : selectedPlacements_)
        {
            StandaloneArrangement::Placement placement;
            if (arr->getPlacementById(sel.trackId, sel.placementId, placement))
            {
                const double oldStart = placement.timelineStartSeconds;
                const double newStart = std::max(0.0, oldStart - 0.01);
                if (std::abs(newStart - oldStart) <= 1.0e-9)
                    continue;
                if (!arr->setPlacementTimelineStartSeconds(sel.trackId, sel.placementId, newStart))
                    continue;
                movedEntries.push_back({sel.trackId, sel.placementId, oldStart, newStart});
            }
        }

        if (!movedEntries.empty())
        {
            processor_.getUndoManager().addAction(
                std::make_unique<MultiMovePlacementAction>(processor_, std::move(movedEntries)));
            listeners_.call([this](Listener& l) {
                l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
            });
            refreshRenderModel();
            FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        }
        return true;
    }

    // Nudge Right — move selected placements later by 10ms
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::NudgeRight, key))
    {
        auto* arr = processor_.getStandaloneArrangement();
        if (!arr) return true;

        std::vector<MultiMovePlacementAction::Entry> movedEntries;
        movedEntries.reserve(selectedPlacements_.size());
        for (const auto& sel : selectedPlacements_)
        {
            StandaloneArrangement::Placement placement;
            if (arr->getPlacementById(sel.trackId, sel.placementId, placement))
            {
                const double oldStart = placement.timelineStartSeconds;
                const double newStart = std::max(0.0, oldStart + 0.01);
                if (std::abs(newStart - oldStart) <= 1.0e-9)
                    continue;
                if (!arr->setPlacementTimelineStartSeconds(sel.trackId, sel.placementId, newStart))
                    continue;
                movedEntries.push_back({sel.trackId, sel.placementId, oldStart, newStart});
            }
        }

        if (!movedEntries.empty())
        {
            processor_.getUndoManager().addAction(
                std::make_unique<MultiMovePlacementAction>(processor_, std::move(movedEntries)));
            listeners_.call([this](Listener& l) {
                l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
            });
            refreshRenderModel();
            FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        }
        return true;
    }

    // ToggleSnap — Ctrl+Shift+S
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::ToggleSnap, key))
    {
        auto snap = processor_.getSnapSettings();
        snap.enabled = !snap.enabled;
        if (!snap.enabled) {
            snap.mode = SnapSettings::Mode::Off;
        } else if (snap.mode == SnapSettings::Mode::Off) {
            snap.mode = SnapSettings::Mode::Beat;
        }
        processor_.setSnapSettings(snap);
        return true;
    }

    return false;
}

// ============================================================================
// 多选实现
// ============================================================================

bool ArrangementViewComponent::isPlacementSelected(int trackId, uint64_t placementId) const
{
    return selectedPlacements_.count(PlacementSelectionKey{trackId, placementId}) > 0;
}

void ArrangementViewComponent::togglePlacementSelection(int trackId, uint64_t placementId)
{
    juce::ignoreUnused(trackId);
    PlacementSelectionKey key{trackId, placementId};
    auto it = selectedPlacements_.find(key);
    if (it != selectedPlacements_.end())
    {
        if (selectedPlacements_.size() > 1)
        {
            selectedPlacements_.erase(it);
        }
    }
    else
    {
        selectedPlacements_.insert(key);
    }
}

void ArrangementViewComponent::clearPlacementSelection()
{
    selectedPlacements_.clear();
    hasShiftAnchor_ = false;
}

void ArrangementViewComponent::selectPlacementsInRange(const PlacementSelectionKey& from,
                                                       const PlacementSelectionKey& to)
{
    if (from.trackId == to.trackId)
    {
        double fromStart = 0.0;
        double toStart = 0.0;
        getStandalonePlacementStartSeconds(processor_, from.trackId, from.placementId, fromStart);
        getStandalonePlacementStartSeconds(processor_, to.trackId, to.placementId, toStart);
        double minTime = std::min(fromStart, toStart);
        double maxTime = std::max(fromStart, toStart);

        for (int i = 0; i < getStandalonePlacementCount(processor_, from.trackId); ++i)
        {
            const uint64_t placementId = processor_.getPlacementId(from.trackId, i);
            double placementStartSeconds = 0.0;
            getStandalonePlacementStartSeconds(processor_, from.trackId, placementId, placementStartSeconds);
            if (placementStartSeconds >= minTime && placementStartSeconds <= maxTime)
            {
                selectedPlacements_.insert(PlacementSelectionKey{from.trackId, placementId});
            }
        }
    }
    else
    {
        int minTrack = std::min(from.trackId, to.trackId);
        int maxTrack = std::max(from.trackId, to.trackId);
        double fromStart = 0.0;
        double toStart = 0.0;
        getStandalonePlacementStartSeconds(processor_, from.trackId, from.placementId, fromStart);
        getStandalonePlacementStartSeconds(processor_, to.trackId, to.placementId, toStart);
        double minTime = std::min(fromStart, toStart);
        double maxTime = std::max(fromStart, toStart);

        for (int trackId = minTrack; trackId <= maxTrack; ++trackId)
        {
            for (int i = 0; i < getStandalonePlacementCount(processor_, trackId); ++i)
            {
                const uint64_t placementId = processor_.getPlacementId(trackId, i);
                double placementStartSeconds = 0.0;
                getStandalonePlacementStartSeconds(processor_, trackId, placementId, placementStartSeconds);
                if (placementStartSeconds >= minTime && placementStartSeconds <= maxTime)
                {
                    selectedPlacements_.insert(PlacementSelectionKey{trackId, placementId});
                }
            }
        }
    }
}

void ArrangementViewComponent::selectAllPlacementsInTrack(int trackId)
{
    if (trackId < 0 || trackId >= OpenTuneAudioProcessor::MAX_TRACKS) return;

    clearPlacementSelection();

    const int placementCount = getStandalonePlacementCount(processor_, trackId);
    for (int i = 0; i < placementCount; ++i)
    {
        const uint64_t placementId = processor_.getPlacementId(trackId, i);
        selectedPlacements_.insert(PlacementSelectionKey{trackId, placementId});
    }

    if (!selectedPlacements_.empty())
    {
        selectedTrack_ = trackId;
        selectedPlacementIndex_ = placementCount > 0 ? 0 : -1;
        selectedPlacementId_ = placementCount > 0 ? processor_.getPlacementId(trackId, 0) : 0;
    }

    listeners_.call([this](Listener& l) {
        l.placementSelectionChanged(selectedTrack_, selectedPlacementId_);
    });
    refreshRenderModel();
    FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
}

// ============================================================================
// Analysis animation 状态管理
// ============================================================================

void ArrangementViewComponent::setClipAnalysisInProgress(uint64_t placementId, bool inProgress)
{
    auto& state = clipAnalysisStates_[placementId];
    if (state.isAnalysisInProgress != inProgress)
    {
        state.isAnalysisInProgress = inProgress;
        refreshRenderModel();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
    }
}

} // namespace OpenTune
