#include "ArrangementViewComponent.h"
#include "AuroraTheme.h"
#include "FrameScheduler.h"
#include "UiAssets.h"
#include "TimelineViewportPolicy.h"
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
constexpr float kPlacementCornerRadius = 6.0f;

struct PlacementVisual {
    juce::Rectangle<float> bounds;
    juce::Colour colour;
    juce::String name;
    float gain = 1.0f;
    double fadeIn = 0.0;
    double fadeOut = 0.0;
    bool selected = false;
    bool preview = false;
    ContentKey contentKey;
    double clipInSeconds = 0.0;
    double timelineStartSeconds = 0.0;
    double durationSeconds = 0.0;
    double pixelsPerSecond = 1.0;
    double paintStartSeconds = 0.0;
    double paintEndSeconds = 0.0;
};

juce::String placementFallbackName(const PlacementVisual& visual)
{
    return visual.name.isNotEmpty() ? visual.name : juce::String(visual.contentKey.objectId);
}

juce::String formatGainLabel(float gain)
{
    float db = juce::Decibels::gainToDecibels(gain, -96.0f);
    if (std::abs(db) < 0.05f)
        db = 0.0f;

    auto text = juce::String(db, 1);
    if (db > 0.0f)
        text = "+" + text;

    return text + " dB";
}

juce::String formatFadeLabel(double seconds)
{
    return juce::String(seconds, seconds < 1.0 ? 2 : 1) + "s";
}

float placementRadiusFor(juce::Rectangle<float> bounds)
{
    return juce::jmin(kPlacementCornerRadius,
                      juce::jmax(0.0f, bounds.getWidth() * 0.5f),
                      juce::jmax(0.0f, bounds.getHeight() * 0.5f));
}

juce::Colour placementReadableTextColour(const PlacementVisual& visual)
{
    const auto trackColour = visual.colour;
    return trackColour.getPerceivedBrightness() > 0.62f
        ? juce::Colours::black.withAlpha(visual.preview ? 0.70f : 0.82f)
        : juce::Colours::white.withAlpha(visual.preview ? 0.70f : 0.88f);
}

void paintPlacementWaveform(juce::Graphics& g,
                            const PlacementVisual& visual,
                            const WaveformMipmapCache& waveformMipmapCache)
{
    const int width = juce::roundToInt(visual.bounds.getWidth());
    const int height = juce::roundToInt(visual.bounds.getHeight());
    const auto* mipmap = waveformMipmapCache.get(visual.contentKey);
    if (mipmap == nullptr || !mipmap->hasSource() || width <= 2 || height <= 2)
        return;

    const int bestLevelIdx = mipmap->selectBestLevelIndex(visual.pixelsPerSecond);
    const auto snapshot = mipmap->snapshotLevel(bestLevelIdx);
    if (snapshot.peaks.empty() || snapshot.samplesPerPeak <= 0)
        return;

    const int numPeaks = static_cast<int>(snapshot.peaks.size());
    const double timePerPeak = static_cast<double>(snapshot.samplesPerPeak)
        / static_cast<double>(WaveformMipmap::kBaseSampleRate);
    const float halfH = visual.bounds.getHeight() * 0.5f;
    const float centerY = visual.bounds.getCentreY();

    juce::Path wavePath;
    bool started = false;

    for (int x = 0; x < width; ++x) {
        const double timelineTime = visual.paintStartSeconds + static_cast<double>(x) / visual.pixelsPerSecond;
        const double contentTime = visual.clipInSeconds + (timelineTime - visual.timelineStartSeconds);
        const int peakIdx = static_cast<int>(contentTime / timePerPeak);
        if (peakIdx < 0 || peakIdx >= numPeaks)
            continue;

        const float mag = snapshot.peaks[static_cast<size_t>(peakIdx)].getMagnitude();
        const float barH = mag * halfH;
        const float drawX = visual.bounds.getX() + static_cast<float>(x);
        if (!started) {
            wavePath.startNewSubPath(drawX, centerY - barH);
            started = true;
        } else {
            wavePath.lineTo(drawX, centerY - barH);
        }
    }

    for (int x = width - 1; x >= 0; --x) {
        const double timelineTime = visual.paintStartSeconds + static_cast<double>(x) / visual.pixelsPerSecond;
        const double contentTime = visual.clipInSeconds + (timelineTime - visual.timelineStartSeconds);
        const int peakIdx = static_cast<int>(contentTime / timePerPeak);
        if (peakIdx < 0 || peakIdx >= numPeaks)
            continue;

        const float mag = snapshot.peaks[static_cast<size_t>(peakIdx)].getMagnitude();
        const float barH = mag * halfH;
        wavePath.lineTo(visual.bounds.getX() + static_cast<float>(x), centerY + barH);
    }

    if (!started)
        return;

    wavePath.closeSubPath();
    g.setColour(visual.colour.brighter(0.28f).withAlpha(visual.preview ? 0.30f : 0.48f));
    g.fillPath(wavePath);
}

void paintPlacementFadeShapes(juce::Graphics& g, const PlacementVisual& visual)
{
    const auto bounds = visual.bounds;
    const float radius = placementRadiusFor(bounds);
    juce::Path shell;
    shell.addRoundedRectangle(bounds, radius);

    juce::Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(shell);

    const auto shade = juce::Colours::black.withAlpha(visual.preview ? 0.16f : 0.24f);
    const auto line = UIColors::textPrimary.withAlpha(visual.preview ? 0.34f : 0.52f);
    const float handleSize = juce::jmin(7.0f, juce::jmax(4.0f, bounds.getHeight() * 0.18f));

    const double paintStart = visual.paintStartSeconds;
    const double paintEnd   = visual.paintEndSeconds;

    auto xForTimeline = [&](double t) {
        return visual.bounds.getX()
            + static_cast<float>((t - paintStart) * visual.pixelsPerSecond);
    };

    if (visual.fadeIn > 0.0) {
        const double fadeInStart = visual.timelineStartSeconds;
        const double fadeInEnd   = visual.timelineStartSeconds + visual.fadeIn;
        const double visibleFadeInStart = std::max(fadeInStart, paintStart);
        const double visibleFadeInEnd   = std::min(fadeInEnd, paintEnd);

        if (visibleFadeInEnd > visibleFadeInStart) {
            const float x0 = xForTimeline(visibleFadeInStart);
            const float x1 = xForTimeline(visibleFadeInEnd);

            juce::Path fadePath;
            fadePath.startNewSubPath(x0, bounds.getY());
            fadePath.lineTo(x0, bounds.getBottom());
            fadePath.lineTo(x1, bounds.getY());
            fadePath.closeSubPath();

            g.setColour(shade);
            g.fillPath(fadePath);
            g.setColour(line);
            g.drawLine(x0, bounds.getBottom(), x1, bounds.getY(), 1.2f);
            g.fillRoundedRectangle(x1 - handleSize * 0.5f,
                                   bounds.getY() + 2.0f,
                                   handleSize,
                                   handleSize,
                                   handleSize * 0.35f);
        }
    }

    if (visual.fadeOut > 0.0) {
        const double placementEnd = visual.timelineStartSeconds + visual.durationSeconds;
        const double fadeOutStart = placementEnd - visual.fadeOut;
        const double fadeOutEnd   = placementEnd;
        const double visibleFadeOutStart = std::max(fadeOutStart, paintStart);
        const double visibleFadeOutEnd   = std::min(fadeOutEnd, paintEnd);

        if (visibleFadeOutEnd > visibleFadeOutStart) {
            const float x0 = xForTimeline(visibleFadeOutStart);
            const float x1 = xForTimeline(visibleFadeOutEnd);

            juce::Path fadePath;
            fadePath.startNewSubPath(x0, bounds.getY());
            fadePath.lineTo(x1, bounds.getY());
            fadePath.lineTo(x1, bounds.getBottom());
            fadePath.closeSubPath();

            g.setColour(shade);
            g.fillPath(fadePath);
            g.setColour(line);
            g.drawLine(x0, bounds.getY(), x1, bounds.getBottom(), 1.2f);
            g.fillRoundedRectangle(x0 - handleSize * 0.5f,
                                   bounds.getY() + 2.0f,
                                   handleSize,
                                   handleSize,
                                   handleSize * 0.35f);
        }
    }
}

void paintPlacementShellAndWaveform(juce::Graphics& g,
                                    const PlacementVisual& visual,
                                    const WaveformMipmapCache& waveformMipmapCache)
{
    const auto bounds = visual.bounds;
    const float radius = placementRadiusFor(bounds);
    const auto themeId = UIColors::currentThemeId();
    const bool lightTheme = themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose;
    const float alphaScale = visual.preview ? 0.58f : 1.0f;

    juce::Path shell;
    shell.addRoundedRectangle(bounds, radius);

    const auto top = visual.colour.brighter(lightTheme ? 0.18f : 0.32f).withAlpha((lightTheme ? 0.66f : 0.52f) * alphaScale);
    const auto mid = visual.colour.withAlpha((lightTheme ? 0.58f : 0.44f) * alphaScale);
    const auto bottom = visual.colour.darker(lightTheme ? 0.06f : 0.34f).withAlpha((lightTheme ? 0.62f : 0.48f) * alphaScale);

    juce::ColourGradient fill(top, bounds.getX(), bounds.getY(), bottom, bounds.getX(), bounds.getBottom(), false);
    fill.addColour(0.52, mid);
    g.setGradientFill(fill);
    g.fillPath(shell);

    {
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(shell);
        juce::ColourGradient highlight(UIColors::textPrimary.withAlpha((lightTheme ? 0.30f : 0.16f) * alphaScale),
                                       bounds.getX(),
                                       bounds.getY(),
                                       UIColors::textPrimary.withAlpha(0.0f),
                                       bounds.getX(),
                                       bounds.getY() + bounds.getHeight() * 0.42f,
                                       false);
        g.setGradientFill(highlight);
        g.fillRect(bounds.withHeight(bounds.getHeight() * 0.48f));

        paintPlacementWaveform(g, visual, waveformMipmapCache);
    }

    paintPlacementFadeShapes(g, visual);

    const auto border = visual.selected
        ? visual.colour.brighter(0.92f).interpolatedWith(UIColors::textHighlight, 0.34f).withAlpha(visual.preview ? 0.72f : 0.96f)
        : visual.colour.brighter(lightTheme ? 0.12f : 0.42f).withAlpha(visual.preview ? 0.50f : 0.74f);
    const float stroke = visual.selected ? (visual.preview ? 1.6f : 2.0f) : (visual.preview ? 0.9f : 1.1f);

    g.setColour(border);
    g.drawRoundedRectangle(bounds.reduced(0.5f), radius, stroke);
}

void paintOverlapShading(juce::Graphics& g, const std::vector<PlacementVisual>& visuals)
{
    for (size_t i = 0; i < visuals.size(); ++i) {
        for (size_t j = i + 1; j < visuals.size(); ++j) {
            const auto overlap = visuals[i].bounds.getIntersection(visuals[j].bounds);
            if (overlap.isEmpty())
                continue;

            g.setColour(juce::Colours::black.withAlpha(0.18f));
            g.fillRect(overlap);
        }
    }
}

void paintPlacementTextFadeGain(juce::Graphics& g, const PlacementVisual& visual)
{
    const auto bounds = visual.bounds;
    if (bounds.getWidth() <= 30.0f || bounds.getHeight() <= 12.0f)
        return;

    auto label = placementFallbackName(visual);
    const int maxChars = juce::jmax(1, juce::roundToInt(bounds.getWidth() / 7.0f));
    if (label.length() > maxChars)
        label = label.substring(0, maxChars) + ".";

    const auto textColour = placementReadableTextColour(visual);
    const auto shadowColour = juce::Colours::black.withAlpha(visual.preview ? 0.32f : 0.46f);
    const auto textArea = bounds.reduced(6.0f, 3.0f).withTrimmedBottom(juce::jmin(12.0f, bounds.getHeight() * 0.34f));

    g.setFont(11.0f);
    const float labelWidth = juce::jmin(textArea.getWidth(), static_cast<float>(label.length()) * 7.0f + 8.0f);
    const auto labelBack = textArea.withWidth(labelWidth).reduced(0.0f, 1.0f);

    g.setColour(juce::Colours::black.withAlpha(visual.preview ? 0.16f : 0.26f));
    g.fillRoundedRectangle(labelBack, 3.0f);
    g.setColour(shadowColour);
    g.drawText(label, textArea.translated(0.8f, 0.8f), juce::Justification::centredLeft, false);
    g.setColour(textColour);
    g.drawText(label, textArea, juce::Justification::centredLeft, false);

    if (bounds.getWidth() > 58.0f && bounds.getHeight() > 24.0f) {
        const auto gainText = formatGainLabel(visual.gain);
        const auto gainArea = bounds.reduced(6.0f, 3.0f).removeFromBottom(12.0f);

        g.setFont(10.0f);
        const float gainWidth = juce::jmin(gainArea.getWidth(), static_cast<float>(gainText.length()) * 6.0f + 8.0f);
        const auto gainBack = gainArea.withLeft(gainArea.getRight() - gainWidth).reduced(0.0f, 1.0f);
        g.setColour(juce::Colours::black.withAlpha(visual.preview ? 0.13f : 0.22f));
        g.fillRoundedRectangle(gainBack, 3.0f);
        g.setColour(textColour.withAlpha(visual.preview ? 0.58f : 0.72f));
        g.drawText(gainText, gainArea, juce::Justification::centredRight, false);
    }

    if (bounds.getWidth() > 84.0f && bounds.getHeight() > 28.0f) {
        g.setFont(9.0f);
        g.setColour(textColour.withAlpha(visual.preview ? 0.52f : 0.68f));

        if (visual.fadeIn > 0.0) {
            const float fadeW = juce::jlimit(1.0f,
                                            bounds.getWidth(),
                                            static_cast<float>(visual.fadeIn * visual.pixelsPerSecond));
            const auto fadeArea = juce::Rectangle<float>(bounds.getX() + fadeW + 4.0f,
                                                         bounds.getY() + 2.0f,
                                                         42.0f,
                                                         10.0f);
            g.drawText(formatFadeLabel(visual.fadeIn), fadeArea, juce::Justification::centredLeft, false);
        }

        if (visual.fadeOut > 0.0) {
            const float fadeW = juce::jlimit(1.0f,
                                            bounds.getWidth(),
                                            static_cast<float>(visual.fadeOut * visual.pixelsPerSecond));
            const auto fadeArea = juce::Rectangle<float>(bounds.getRight() - fadeW - 46.0f,
                                                         bounds.getY() + 2.0f,
                                                         42.0f,
                                                         10.0f);
            g.drawText(formatFadeLabel(visual.fadeOut), fadeArea, juce::Justification::centredRight, false);
        }
    }
}

void paintPlacementGhost(juce::Graphics& g,
                         PlacementVisual visual,
                         const WaveformMipmapCache& waveformMipmapCache)
{
    visual.preview = true;
    paintPlacementShellAndWaveform(g, visual, waveformMipmapCache);
    paintPlacementTextFadeGain(g, visual);
}

template <typename IsSelected>
std::vector<PlacementVisual> collectVisiblePlacements(const StandaloneArrangement& arrangement,
                                                      double tileStart,
                                                      double tileEnd,
                                                      const ArrangementVerticalWindow& verticalWindow,
                                                      double pixelsPerSecond,
                                                      int tileWidth,
                                                      IsSelected&& isSelected)
{
    std::vector<PlacementVisual> visuals;

    for (int trackId = verticalWindow.firstTrack(); trackId < verticalWindow.lastTrackExclusive(); ++trackId) {
        const int trackY = trackId * verticalWindow.trackHeight - verticalWindow.scrollTopPx;
        const int placementCount = arrangement.getNumPlacements(trackId);

        for (int placementIndex = 0; placementIndex < placementCount; ++placementIndex) {
            StandaloneArrangement::Placement placement;
            [[maybe_unused]] const bool placementFound = arrangement.getPlacementByIndex(trackId, placementIndex, placement);
            jassert(placementFound);
            if (placement.timelineStartSeconds >= tileEnd || placement.timelineEndSeconds() <= tileStart)
                continue;

            const double placementStart = placement.timelineStartSeconds;
            const double placementEnd = placement.timelineEndSeconds();
            const double visibleStart = std::max(placementStart, tileStart);
            const double visibleEnd = std::min(placementEnd, tileEnd);

            if (visibleEnd <= visibleStart)
                continue;

            const int x = static_cast<int>(std::llround((visibleStart - tileStart) * pixelsPerSecond));
            const int width = juce::jmax(1,
                static_cast<int>(std::llround((visibleEnd - visibleStart) * pixelsPerSecond)));
            const int y = trackY + 2;
            const int height = verticalWindow.trackHeight - 4;

            visuals.push_back({
                juce::Rectangle<float>(static_cast<float>(x),
                                       static_cast<float>(y),
                                       static_cast<float>(width),
                                       static_cast<float>(height)),
                arrangement.getTrackColour(trackId),
                placement.name,
                placement.gain,
                placement.fadeInDuration,
                placement.fadeOutDuration,
                isSelected(trackId, placement.placementId),
                false,
                placement.contentKey,
                placement.clipInSeconds,
                placement.timelineStartSeconds,
                placement.durationSeconds,
                pixelsPerSecond,
                visibleStart,
                visibleEnd
            });
        }
    }

    return visuals;
}

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
constexpr double kArrangementRenderBandOverscanScreens = 1.0;

ArrangementViewComponent::ArrangementViewComponent(OpenTuneAudioProcessor& processor)
    : processor_(processor)
{
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
        refreshVisualState();
        FrameScheduler::instance().requestContentInvalidation(*this, {}, FrameScheduler::Priority::Normal);
    };
    timeUnitToggleButton_.setColour(juce::TextButton::buttonColourId, UIColors::backgroundLight);
    timeUnitToggleButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
    addAndMakeVisible(timeUnitToggleButton_);
    timeUnitToggleButton_.setTooltip(LOC(kTooltipTimeUnit));

    addAndMakeVisible(fixedPlayhead_);
    timeUnitToggleButton_.toFront(false);
    scrollModeToggleButton_.toFront(false);
    fixedPlayhead_.setBounds(getLocalBounds());
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

void ArrangementViewComponent::applyResolvedCamera(TimelineViewportCamera next, juce::NotificationType notify)
{
    if (next == camera_)
        return;

    camera_ = next;

    if (notify == juce::sendNotification) {
        userHasManuallyZoomed_ = true;
    }

    prepareVisiblePatternTiles();
    prepareVisibleContentTiles();
    updateScrollBars();
    refreshVisualState();

    if (notify == juce::sendNotification) {
        listeners_.call([this](Listener& l) { l.timelineViewportChanged(camera_); });
    }

    repaint();
}

void ArrangementViewComponent::commitViewportRequest(TimelineViewportRequest req, juce::NotificationType notify)
{
    applyResolvedCamera(TimelineViewportPolicy::resolve(req), notify);
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
    
    // Rebuild pattern and content tiles with new vertical window
    prepareVisiblePatternTiles();
    prepareVisibleContentTiles();
    
    repaint();
    FrameScheduler::instance().requestContentInvalidation(*this, {}, FrameScheduler::Priority::Normal);
}

void ArrangementViewComponent::setVisibleTrackCount(int count)
{
    visibleTrackCount_ = juce::jlimit(1, OpenTuneAudioProcessor::MAX_TRACKS, count);
    // Re-clamp scroll offset for new track count
    setVerticalScrollOffset(verticalScrollOffset_);
    updateScrollBars();
    
    // Prepare pattern and content tiles for new track count
    prepareVisiblePatternTiles();
    prepareVisibleContentTiles();
}

void ArrangementViewComponent::fitToContent()
{
    // 如果用户已手动调整过缩放，不自动覆盖
    if (userHasManuallyZoomed_) {
        return;
    }

    double maxEndTime = 0.0;
    for (int t = 0; t < OpenTuneAudioProcessor::MAX_TRACKS; ++t) {
        const int placementCount = getStandalonePlacementCount(processor_, t);
        for (int i = 0; i < placementCount; ++i) {
            StandaloneArrangement::Placement placement;
            if (!getStandalonePlacementByIndex(processor_, t, i, placement)) {
                continue;
            }
            maxEndTime = juce::jmax(maxEndTime, placement.timelineEndSeconds());
        }
    }

    const int viewportWidth = getVisibleViewportWidth();
    if (maxEndTime <= 0.0 || viewportWidth <= 0) {
        return;
    }

    const int drawableWidth = juce::jmax(1, viewportWidth - 12);

    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Manual;
    req.targetTime = 0.0;
    req.viewportWidth = viewportWidth;
    req.pixelsPerSecond = static_cast<double>(drawableWidth) / maxEndTime;
    commitViewportRequest(req, juce::sendNotification);
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

    updateScrollBars();
    refreshVisualState();
    // Import drop preview highlight (transient, UI-only)

    // 固定播放头覆盖整个组件区域
    fixedPlayhead_.setBounds(getLocalBounds());
    timeUnitToggleButton_.toFront(false);
    scrollModeToggleButton_.toFront(false);
    updateOverlayPresentation();
}

void ArrangementViewComponent::scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart)
{
    if (scrollBar == &horizontalScrollBar_)
    {
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Manual;
    req.targetTime = newRangeStart / camera_.pixelsPerSecond;
    req.viewportWidth = getVisibleViewportWidth();
    req.pixelsPerSecond = camera_.pixelsPerSecond;
    commitViewportRequest(req, juce::sendNotification);
    }
    else if (scrollBar == &verticalScrollBar_)
    {
        verticalScrollOffset_ = static_cast<int>(newRangeStart);
        // 通知监听器垂直滚动偏移变化（用于同步TrackPanel）
        listeners_.call([this](Listener& l) { l.verticalScrollChanged(verticalScrollOffset_); });
        refreshVisualState();
        FrameScheduler::instance().requestContentInvalidation(*this, {}, FrameScheduler::Priority::Normal);
    }
}

int ArrangementViewComponent::getTotalContentWidth() const
{
    return juce::jmax(contentMetrics_.totalContentWidthPx,
                      makeViewMapper().timeToX(kArrangementDefaultSpanSeconds));
}

void ArrangementViewComponent::updateScrollBars()
{
    const int visibleWidth = getVisibleViewportWidth();
    const double pps = camera_.pixelsPerSecond;
    const double contentEndSeconds = (static_cast<double>(getTotalContentWidth()) + visibleWidth) / pps;

    const auto range = TimelineViewportPolicy::computeViewportRange(
        0.0,
        contentEndSeconds,
        camera_,
        visibleWidth,
        readPlayheadSeconds());

    horizontalScrollBar_.setRangeLimits(range.absoluteStartPx(), range.absoluteEndPx(), juce::dontSendNotification);
    horizontalScrollBar_.setCurrentRange(range.visibleStartPx(), range.visibleWidthPx(), juce::dontSendNotification);

    int totalTrackHeight = rulerHeight_ + visibleTrackCount_ * processor_.getTrackHeight();
    int visibleHeight = getHeight() - UIColors::scrollBarThickness;
    verticalScrollBar_.setRangeLimits(0.0, static_cast<double>(totalTrackHeight + visibleHeight));
    verticalScrollBar_.setCurrentRange(static_cast<double>(verticalScrollOffset_), static_cast<double>(visibleHeight));
}

int ArrangementViewComponent::getVisibleViewportWidth() const
{
    return juce::jmax(1,
                      getWidth() - UIColors::scrollBarThickness - kArrangementContentStartX);
}

juce::Rectangle<int> ArrangementViewComponent::getContentViewportBounds() const
{
    return { kArrangementContentStartX,
             rulerHeight_,
             getVisibleViewportWidth(),
             juce::jmax(0, getHeight() - rulerHeight_ - UIColors::scrollBarThickness) };
}

void ArrangementViewComponent::rebuildContentMetrics()
{
    double maxEndTime = kArrangementDefaultSpanSeconds;
    uint64_t revision = 1469598103934665603ull;

    for (int trackId = 0; trackId < OpenTuneAudioProcessor::MAX_TRACKS; ++trackId) {
        const int placementCount = getStandalonePlacementCount(processor_, trackId);
        revision = hashCombine(revision, static_cast<uint64_t>(trackId + 1));
        revision = hashCombine(revision, static_cast<uint64_t>(placementCount + 1));

        // Include track colour in revision (content tile renders with track colour)
        if (auto* arr = processor_.getStandaloneArrangement()) {
            revision = hashCombine(revision, static_cast<uint64_t>(arr->getTrackColour(trackId).getARGB()));
        }

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
            revision = hashCombine(revision, static_cast<uint64_t>(std::llround(placement.clipInSeconds * 1000.0)));
            revision = hashCombine(revision, static_cast<uint64_t>(std::llround(static_cast<double>(placement.gain) * 1000.0)));
            revision = hashCombine(revision, static_cast<uint64_t>(std::llround(placement.fadeInDuration * 1000.0)));
            revision = hashCombine(revision, static_cast<uint64_t>(std::llround(placement.fadeOutDuration * 1000.0)));
            revision = hashCombine(revision, isPlacementSelected(trackId, placement.placementId) ? 1ull : 0ull);
            // Include placement name in revision (content tile renders label)
            revision = hashCombine(revision, static_cast<uint64_t>(placement.name.hashCode()));
        }
    }

    // P1-5: Include clipInSeconds and waveformBuildGeneration_ in revision
    revision = hashCombine(revision, waveformBuildGeneration_);

    contentMetrics_.revision = revision;
    contentMetrics_.maxEndTimeSeconds = maxEndTime;
    contentMetrics_.totalContentWidthPx = makeViewMapper().timeToX(maxEndTime);
}

// Pre-build pattern tiles for paint() consumption
void ArrangementViewComponent::prepareVisiblePatternTiles()
{
    preparedPatternTiles_.clear();

    const auto vpBounds = getContentViewportBounds();
    const double pps = camera_.pixelsPerSecond;
    const double tileDurationSec = static_cast<double>(TimelinePatternCache::kPatternTileWidthPx) / pps;
    const double viewStart = camera_.visibleStartSeconds;
    const double viewEnd = viewStart + vpBounds.getWidth() / pps;

    double firstTileStart = std::floor(viewStart / tileDurationSec) * tileDurationSec;
    if (firstTileStart < 0.0) firstTileStart = 0.0;

    for (double tileStart = firstTileStart; tileStart < viewEnd; tileStart += tileDurationSec)
    {
        PatternTileKey key;
        key.viewKind = "arrangement";
        key.startSeconds = tileStart;
        key.endSeconds = tileStart + tileDurationSec;
        key.pixelsPerSecond = pps;
        key.timeUnit = (timeUnit_ == TimeUnit::Bars) ? 1 : 0;
        key.tempo = static_cast<int>(lastContextBpm_ > 0.0 ? lastContextBpm_ : 120.0);
        key.timeSigNumerator = lastContextTimeSigNum_ > 0 ? lastContextTimeSigNum_ : 4;
        key.timeSigDenominator = lastContextTimeSigDenom_ > 0 ? lastContextTimeSigDenom_ : 4;
        key.themeId = static_cast<int>(UIColors::currentThemeId());
        key.verticalGeometry = encodeArrangementVerticalGeometry(vpBounds.getHeight(), rulerHeight_);
        key.laneStyle = 0;
        key.trackHeight = processor_.getTrackHeight();

        const juce::Image& tile = patternCache_.getPatternTile(key, TimelineLayerComposer::buildPatternTile);
        preparedPatternTiles_.push_back({ key, &tile });
    }
}

// P0-1: Pre-build content tiles for paint() consumption
void ArrangementViewComponent::prepareVisibleContentTiles()
{
    preparedContentTiles_.clear();

    const auto vpBounds = getContentViewportBounds();
    const auto vwin = makeArrangementVerticalWindow();
    const int visibleCount = juce::jmax(1, visibleTrackCount_);
    const int trackH = vwin.trackHeight;

    // P0-3: Use valid ContentKey for arrangement content (objectId != 0)
    ContentKey arrangementKey{DomainKind::StandaloneArrangement, 1, 0};

    const double tileDurationSec = static_cast<double>(TimelinePatternCache::kPatternTileWidthPx) / camera_.pixelsPerSecond;
    const double viewStart = camera_.visibleStartSeconds;
    const double viewEnd = viewStart + vpBounds.getWidth() / camera_.pixelsPerSecond;

    // Compute tile boundaries aligned to kPatternTileWidthPx
    double firstTileStart = std::floor(viewStart / tileDurationSec) * tileDurationSec;
    if (firstTileStart < 0.0) firstTileStart = 0.0;

    for (double tileStart = firstTileStart; tileStart < viewEnd; tileStart += tileDurationSec)
    {
        ContentTileKey key;
        key.viewKind = "arrangement";
        key.slot = ContentSlot::ArrangementClips;
        key.contentKey = arrangementKey;
        key.startSeconds = tileStart;
        key.endSeconds = tileStart + tileDurationSec;
        key.pixelsPerSecond = camera_.pixelsPerSecond;
        // Encode full vertical window state into verticalGeometry (64-bit)
        key.verticalGeometry = vwin.encode();
        key.revision = contentMetrics_.revision;

        const int tileContentH = visibleCount * trackH;
        const auto& tile = contentCache_.getOrBuildTile(key, tileContentH,
            [&](juce::Graphics& g, const ContentTileKey& k, juce::Rectangle<int> bounds) {
                const int tileW = bounds.getWidth();
                const ArrangementVerticalWindow verticalWindow{
                    static_cast<int>(k.verticalGeometry & 0xFFFF),
                    static_cast<int>((k.verticalGeometry >> 16) & 0xFFFF),
                    static_cast<int>((k.verticalGeometry >> 32) & 0xFFFF),
                    static_cast<int>((k.verticalGeometry >> 48) & 0xFFFF)
                };
                auto& arrangement = *processor_.getStandaloneArrangement();
                auto visuals = collectVisiblePlacements(arrangement,
                                                        k.startSeconds,
                                                        k.endSeconds,
                                                        verticalWindow,
                                                        k.pixelsPerSecond,
                                                        tileW,
                                                        [this](int trackId, uint64_t placementId) {
                                                            return isPlacementSelected(trackId, placementId);
                                                        });
                for (const auto& v : visuals) paintPlacementShellAndWaveform(g, v, waveformMipmapCache_);
                paintOverlapShading(g, visuals);
                for (const auto& v : visuals) paintPlacementTextFadeGain(g, v);
            });

        preparedContentTiles_.push_back({key, &tile});
    }
}

void ArrangementViewComponent::updateOverlayPresentation()
{
    // Fixed playhead — anchor at viewport content center
    // Independent of timeline time. Camera moves, playhead stays fixed.
    const auto viewportBounds = getContentViewportBounds();
    const int anchorX = viewportBounds.getCentreX();
    fixedPlayhead_.setAnchorBounds(anchorX, getHeight());
    fixedPlayhead_.setVisible(true);
}

int ArrangementViewComponent::absoluteTimeToViewportX(double seconds) const
{
    return makeViewMapper().timeToX(seconds);
}

double ArrangementViewComponent::viewportXToAbsoluteTime(int x) const
{
    return makeViewMapper().xToTime(x);
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

void ArrangementViewComponent::requestVisualRefresh()
{
    rebuildContentMetrics();
    // BPM/TimeSig are now read directly from processor context
    lastContextBpm_ = 120.0;
    lastContextTimeSigNum_ = 4;
    lastContextTimeSigDenom_ = 4;
}

void ArrangementViewComponent::refreshVisualState()
{
    requestVisualRefresh();
    prepareVisiblePatternTiles();
    prepareVisibleContentTiles();
    repaint();
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

void ArrangementViewComponent::clearMoveDragOverlay()
{
    moveDragStartStates_.clear();
    repaint();
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

void ArrangementViewComponent::updateMoveDragOverlay(const juce::MouseEvent& e)
{
    if (!isDraggingPlacement_ || currentDragOp_ != DragOperation::Move)
        return;

    dragCurrentPos_ = e.getPosition();
    repaint();
}

std::vector<ArrangementViewComponent::MoveDragStartState>
ArrangementViewComponent::resolveMoveDragParticipants(const HitTestResult& hit) const
{
    const uint64_t hitPlacementId = processor_.getPlacementId(hit.trackId, hit.placementIndex);
    jassert(hitPlacementId != 0);

    const auto* arrangement = processor_.getStandaloneArrangement();
    jassert(arrangement != nullptr);

    std::vector<MoveDragStartState> states;

    const auto addState = [&](int trackId, uint64_t placementId)
    {
        StandaloneArrangement::Placement placement;
        const bool found = getStandalonePlacementById(processor_, trackId, placementId, placement);
        jassert(found);

        states.push_back({
            trackId,
            placementId,
            placement.timelineStartSeconds,
            placement.durationSeconds,
            placement.name
        });
    };

    if (isPlacementSelected(hit.trackId, hitPlacementId))
    {
        for (const auto& key : selectedPlacements_)
            addState(key.trackId, key.placementId);
    }
    else
    {
        addState(hit.trackId, hitPlacementId);
    }

    return states;
}

void ArrangementViewComponent::beginMoveDrag(const HitTestResult& hit, juce::Point<int> mousePos)
{
    moveDragStartStates_.clear();

    const uint64_t hitPlacementId = processor_.getPlacementId(hit.trackId, hit.placementIndex);
    jassert(hitPlacementId != 0);
    moveDragPrimaryStart_ = {hit.trackId, hitPlacementId};

    moveDragStartStates_ = resolveMoveDragParticipants(hit);
    dragStartPos_ = mousePos;
    dragCurrentPos_ = mousePos;
}

auto ArrangementViewComponent::resolveMoveDragTarget(
    const MoveDragStartState& state,
    double deltaSeconds,
    int trackDelta) const -> MoveDragResolvedTarget
{
    return {
        juce::jlimit(0, OpenTuneAudioProcessor::MAX_TRACKS - 1, state.trackId + trackDelta),
        juce::jmax(0.0, state.startSeconds + deltaSeconds)
    };
}

void ArrangementViewComponent::finishMoveDrag(const juce::MouseEvent& e)
{
    if (currentDragOp_ != DragOperation::Move || moveDragStartStates_.empty())
        return;

    jassert(moveDragPrimaryStart_.trackId >= 0 && moveDragPrimaryStart_.placementId != 0);

    const double deltaSeconds = viewportXToAbsoluteTime(e.x) - viewportXToAbsoluteTime(dragStartPos_.x);
    const int trackDelta = trackIdForViewportY(e.y) - moveDragPrimaryStart_.trackId;

    std::vector<MultiMovePlacementAction::Entry> undoEntries;
    std::set<PlacementSelectionKey> movedSelection;
    PlacementSelectionKey primaryAfterMove{-1, 0};

    for (const auto& state : moveDragStartStates_) {
        const auto target = resolveMoveDragTarget(state, deltaSeconds, trackDelta);

        if (target.trackId == state.trackId)
            setStandalonePlacementStartSeconds(processor_, state.trackId, state.placementId, target.startSeconds);
        else
            moveStandalonePlacement(processor_, state.trackId, target.trackId, state.placementId, target.startSeconds);

        undoEntries.push_back({state.trackId, target.trackId, state.placementId, state.startSeconds, target.startSeconds});
        movedSelection.insert({target.trackId, state.placementId});

        if (state.trackId == moveDragPrimaryStart_.trackId
            && state.placementId == moveDragPrimaryStart_.placementId) {
            primaryAfterMove = {target.trackId, state.placementId};
        }
    }

    processor_.getUndoManager().addAction(
        std::make_unique<MultiMovePlacementAction>(processor_, std::move(undoEntries)));

    selectedPlacements_ = std::move(movedSelection);
    jassert(primaryAfterMove.trackId >= 0 && primaryAfterMove.placementId != 0);
    selectedTrack_ = primaryAfterMove.trackId;
    selectedPlacementId_ = primaryAfterMove.placementId;
    selectedPlacementIndex_ = processor_.findPlacementIndexById(primaryAfterMove.trackId, primaryAfterMove.placementId);

    // Sync StandaloneArrangement model selection
    auto* arrangement = processor_.getStandaloneArrangement();
    jassert(arrangement != nullptr);
    arrangement->selectPlacement(selectedTrack_, selectedPlacementId_);

    moveDragStartStates_.clear();
    currentDragOp_ = DragOperation::None;
    isDraggingPlacement_ = false;

    refreshVisualState();
    listeners_.call([this](Listener& l) {
        l.placementSelectionChanged(selectedTrack_, selectedPlacementId_);
        l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
    });
    FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
}

void ArrangementViewComponent::drawImportDropPreview(juce::Graphics& g)
{
    // ---- Import drop preview highlight (transient, UI-only) ----
    if (importDropPreview_.active)
    {
        if (importDropPreview_.isNewTrack)
        {
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
            const int trackHeight = processor_.getTrackHeight();
            const int y = rulerHeight_ + importDropPreview_.targetTrackId * trackHeight - verticalScrollOffset_;
            juce::Rectangle<float> laneBounds(0.0f, static_cast<float>(y),
                static_cast<float>(getWidth()), static_cast<float>(trackHeight));

            const juce::Colour previewFill = UIColors::panelGlow.withAlpha(0.10f);
            const juce::Colour previewBorder = UIColors::panelGlow.withAlpha(0.30f);

            g.setColour(previewFill);
            g.fillRect(laneBounds);
            g.setColour(previewBorder);
            g.drawRect(laneBounds, 1.5f);
        }
    }
}

void ArrangementViewComponent::drawMoveDragOverlay(juce::Graphics& g)
{
    if (!isDraggingPlacement_ || currentDragOp_ != DragOperation::Move || moveDragStartStates_.empty())
        return;

    const double deltaSeconds = viewportXToAbsoluteTime(dragCurrentPos_.x)
        - viewportXToAbsoluteTime(dragStartPos_.x);
    const int trackDelta = trackIdForViewportY(dragCurrentPos_.y) - moveDragPrimaryStart_.trackId;
    auto& arrangement = *processor_.getStandaloneArrangement();

    for (const auto& state : moveDragStartStates_) {
        const auto target = resolveMoveDragTarget(state, deltaSeconds, trackDelta);
        const int x = absoluteTimeToViewportX(target.startSeconds);
        const int width = juce::jmax(1, static_cast<int>(std::round(state.durationSeconds * camera_.pixelsPerSecond)));
        const auto lane = getTrackLaneBounds(target.trackId);
        const juce::Rectangle<float> bounds(static_cast<float>(x),
                                            static_cast<float>(lane.getY() + 2),
                                            static_cast<float>(width),
                                            static_cast<float>(lane.getHeight() - 4));

        StandaloneArrangement::Placement placement;
        [[maybe_unused]] const bool found = arrangement.getPlacementById(state.trackId, state.placementId, placement);
        jassert(found);

        const auto targetColour = arrangement.getTrackColour(target.trackId);

        PlacementVisual visual{
            bounds,
            targetColour,
            state.name,
            placement.gain,
            placement.fadeInDuration,
            placement.fadeOutDuration,
            isPlacementSelected(state.trackId, state.placementId),
            true,
            placement.contentKey,
            placement.clipInSeconds,
            target.startSeconds,
            state.durationSeconds,
            camera_.pixelsPerSecond,
            target.startSeconds,
            target.startSeconds + state.durationSeconds
        };
        paintPlacementGhost(g, visual, waveformMipmapCache_);
    }
}

void ArrangementViewComponent::drawTransientOverlay(juce::Graphics& g)
{
    drawImportDropPreview(g);
    drawMoveDragOverlay(g);
}

void ArrangementViewComponent::paint(juce::Graphics& g)
{
    // --- Pattern Layer: Ruler, Grid, Background ---
    for (const auto& pt : preparedPatternTiles_) {
        if (pt.image && pt.image->isValid()) {
            RenderParams patternParams;
            patternParams.visibleStartSeconds = camera_.visibleStartSeconds;
            patternParams.pixelsPerSecond = camera_.pixelsPerSecond;
            patternParams.viewportBoundsX = kArrangementContentStartX;
            patternParams.contentOffsetY = 0;
            TimelineLayerComposer::drawPatternTile(g, *pt.image, pt.key.startSeconds, patternParams);
        }
    }

    // --- Content Layer: Arrangement Clips, Waveforms ---
    for (const auto& pt : preparedContentTiles_) {
        if (pt.image && pt.image->isValid()) {
            RenderParams contentParams;
            contentParams.visibleStartSeconds = camera_.visibleStartSeconds;
            contentParams.pixelsPerSecond = camera_.pixelsPerSecond;
            contentParams.viewportBoundsX = kArrangementContentStartX;
            contentParams.contentOffsetY = rulerHeight_;
            TimelineLayerComposer::drawContentTile(g, *pt.image, pt.key.startSeconds, contentParams);
        }
    }

    // --- Transient Overlay: import drop, drag, hover (post-content) ---
    drawTransientOverlay(g);
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

void ArrangementViewComponent::timerCallback()
{
    onHeartbeatTick();
}

void ArrangementViewComponent::onHeartbeatTick()
{
    if (!isShowing())
        return;

    const bool playingNow = processor_.isPlaying();

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
        ++waveformBuildGeneration_;  // Increment generation for revision tracking
        if (playingNow) {
            waveformVisualRefreshPending_ = true;
        } else {
            waveformVisualRefreshPending_ = false;
            prepareVisibleContentTiles();
            refreshVisualState();
            FrameScheduler::instance().requestContentInvalidation(*this, {}, FrameScheduler::Priority::Background);
        }
    }

    if (!playingNow && waveformVisualRefreshPending_) {
        waveformVisualRefreshPending_ = false;
        // No-op — render model cache removed
        refreshVisualState();
        FrameScheduler::instance().requestContentInvalidation(*this, {}, FrameScheduler::Priority::Background);
    }

    if (!playingNow)
        return;

    // Playback follow remains overlay-only unless waveform mipmaps advanced above.
}

void ArrangementViewComponent::performPageScroll(double playheadTime)
{
    TimelineViewportRequest req;
    req.kind = TimelineViewportRequest::Kind::Page;
    req.targetTime = playheadTime;
    req.viewportWidth = getVisibleViewportWidth();
    req.pixelsPerSecond = camera_.pixelsPerSecond;

    const auto resolved = TimelineViewportPolicy::resolve(req);

    if (std::abs(resolved.visibleStartSeconds - camera_.visibleStartSeconds) > 0.001) {
        commitViewportRequest(req, juce::sendNotification);
    }
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
    juce::ignoreUnused(timestampSec);
    if (!isShowing() || !isPlaying_.load(std::memory_order_relaxed))
        return;

    const double playheadTime = readPlayheadSeconds();

    if (scrollMode_ == ScrollMode::Continuous)
    {
        TimelineViewportRequest req;
        req.kind = TimelineViewportRequest::Kind::Cont;
        req.targetTime = playheadTime;
        req.viewportWidth = getVisibleViewportWidth();
        req.pixelsPerSecond = camera_.pixelsPerSecond;
        commitViewportRequest(req, juce::sendNotification);
        return;
    }

    if (scrollMode_ == ScrollMode::Page)
        performPageScroll(playheadTime);
}

double ArrangementViewComponent::readPlayheadSeconds() const
{
    if (auto source = positionSource_.lock())
        return source->load(std::memory_order_relaxed);

    return 0.0;
}

void ArrangementViewComponent::syncFixedPlayhead()
{
    updateOverlayPresentation();
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
            refreshVisualState();
            FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
            return;
        }
    }

    // Clicked on placement body/edge/handle — do NOT seek playhead, only select/drag
    if (hit.trackId >= 0)
    {
        // Fall through to placement selection/drag logic below
    }
    else if (e.y <= rulerHeight_)
    {
        // Clicked on ruler — seek playhead and start drag
        double newPosSeconds = juce::jmax(0.0, viewportXToAbsoluteTime(e.x));
        processor_.setPosition(newPosSeconds);
        syncFixedPlayhead();
        isDraggingPlayhead_ = true;
        dragStartPos_ = e.getPosition();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }
    else
    {
        // Clicked on empty area — seek playhead and clear selection
        double newPosSeconds = juce::jmax(0.0, viewportXToAbsoluteTime(e.x));
        processor_.setPosition(newPosSeconds);
        syncFixedPlayhead();

        if (!e.mods.isCtrlDown() && !e.mods.isShiftDown())
        {
            clearPlacementSelection();
        }
        refreshVisualState();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    // --- Placement hit: selection and drag logic (no playhead seek) ---

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
        refreshVisualState();
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
        refreshVisualState();
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
        refreshVisualState();
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
        refreshVisualState();
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
        refreshVisualState();
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
        refreshVisualState();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    currentDragOp_ = hit.isTopEdge ? DragOperation::Gain : DragOperation::Move;
    isAdjustingGain_ = hit.isTopEdge;
    isDraggingPlacement_ = !isAdjustingGain_;

    if (currentDragOp_ == DragOperation::Move)
    {
        beginMoveDrag(hit, e.getPosition());
        repaint();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    clearMoveDragOverlay();

    refreshVisualState();
    FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
}

void ArrangementViewComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (isPanning_)
    {
        auto delta = e.getPosition() - lastMousePos_;
        
        // Horizontal Scroll — pan via camera
        const double deltaTime = static_cast<double>(-delta.x) / camera_.pixelsPerSecond;
        TimelineViewportRequest req;
        req.kind = TimelineViewportRequest::Kind::Manual;
        req.targetTime = camera_.visibleStartSeconds + deltaTime;
        req.viewportWidth = getVisibleViewportWidth();
        req.pixelsPerSecond = camera_.pixelsPerSecond;
        commitViewportRequest(req, juce::sendNotification);

        // Vertical Scroll — use setVerticalScrollOffset() to properly update tile cache
        const int newVerticalOffset = verticalScrollOffset_ - delta.y;
        setVerticalScrollOffset(newVerticalOffset);
        listeners_.call([this](Listener& l) { l.verticalScrollChanged(verticalScrollOffset_); });
        
        lastMousePos_ = e.getPosition();
        return;
    }

    if (isDraggingPlayhead_)
    {
        double newPosSeconds = juce::jmax(0.0, viewportXToAbsoluteTime(e.x));
        processor_.setPosition(newPosSeconds);
        syncFixedPlayhead();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
    }

    if (currentDragOp_ == DragOperation::TrimLeft || currentDragOp_ == DragOperation::TrimRight) {
        const double pixelsPerSec = camera_.pixelsPerSecond;
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
        const double pixelsPerSec = camera_.pixelsPerSecond;
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
        updateMoveDragOverlay(e);
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
        return;
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

    // Record Fade undo + notify dirty
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
                    listeners_.call([this](Listener& l) {
                        l.placementTimingChanged(dragStartTrackId_, selectedPlacementIndex_);
                    });
                }
            }
        }
    }

    // Reset drag op for non-move/gain operations
    if (currentDragOp_ != DragOperation::Move && currentDragOp_ != DragOperation::Gain) {
        currentDragOp_ = DragOperation::None;
        dragOperationPlacementId_ = 0;
        refreshVisualState();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
    }

    if (isDraggingPlacement_ && currentDragOp_ == DragOperation::Move)
    {
        finishMoveDrag(e);
    }

    if (isAdjustingGain_ && dragStartPlacementId_ != 0) {
        float currentGain = 1.0f;
        getStandalonePlacementGain(processor_, selectedTrack_, dragStartPlacementId_, currentGain);
        if (currentGain != dragStartPlacementGain_) {
            processor_.getUndoManager().addAction(
                std::make_unique<GainChangeAction>(processor_, selectedTrack_, dragStartPlacementId_,
                                                    dragStartPlacementGain_, currentGain));
            listeners_.call([this](Listener& l) {
                l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
            });
        }
    }

    isDraggingPlacement_ = false;
    isAdjustingGain_ = false;
    isDraggingPlayhead_ = false;
    isPanning_ = false;
    currentDragOp_ = DragOperation::None;
    dragOperationPlacementId_ = 0;
    dragStartTrackId_ = -1;
    clearMoveDragOverlay();
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
                refreshVisualState();
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
            const double oldPps = camera_.pixelsPerSecond;
            const double newPps = oldPps * zoomFactor;

            if (std::abs(newPps - oldPps) > 0.001)
            {
                const double mouseTime = viewportXToAbsoluteTime(e.x);
                TimelineViewportRequest req;
                req.kind = TimelineViewportRequest::Kind::Zoom;
                req.targetTime = mouseTime;
                req.anchorViewportX = static_cast<double>(e.x - kArrangementContentStartX);
                req.viewportWidth = getVisibleViewportWidth();
                req.pixelsPerSecond = newPps;
                commitViewportRequest(req, juce::sendNotification);
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
            const double deltaTime = static_cast<double>(-wheel.deltaY * settings.scrollSpeed * 10.0f) / camera_.pixelsPerSecond;
            TimelineViewportRequest req;
            req.kind = TimelineViewportRequest::Kind::Manual;
            req.targetTime = camera_.visibleStartSeconds + deltaTime;
            req.viewportWidth = getVisibleViewportWidth();
            req.pixelsPerSecond = camera_.pixelsPerSecond;
            commitViewportRequest(req, juce::sendNotification);
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
        const double deltaTime = static_cast<double>(-wheel.deltaX * settings.scrollSpeed * 5.0f) / camera_.pixelsPerSecond;
        TimelineViewportRequest req;
        req.kind = TimelineViewportRequest::Kind::Manual;
        req.targetTime = camera_.visibleStartSeconds + deltaTime;
        req.viewportWidth = getVisibleViewportWidth();
        req.pixelsPerSecond = camera_.pixelsPerSecond;
        commitViewportRequest(req, juce::sendNotification);
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

            refreshVisualState();
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
                        refreshVisualState();
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
            refreshVisualState();
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
            refreshVisualState();
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
            refreshVisualState();
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
                movedEntries.push_back({sel.trackId, sel.trackId, sel.placementId, oldStart, newStart});
            }
        }

        if (!movedEntries.empty())
        {
            processor_.getUndoManager().addAction(
                std::make_unique<MultiMovePlacementAction>(processor_, std::move(movedEntries)));
            listeners_.call([this](Listener& l) {
                l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
            });
            refreshVisualState();
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
                movedEntries.push_back({sel.trackId, sel.trackId, sel.placementId, oldStart, newStart});
            }
        }

        if (!movedEntries.empty())
        {
            processor_.getUndoManager().addAction(
                std::make_unique<MultiMovePlacementAction>(processor_, std::move(movedEntries)));
            listeners_.call([this](Listener& l) {
                l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
            });
            refreshVisualState();
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
    refreshVisualState();
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
        refreshVisualState();
        FrameScheduler::instance().requestInvalidate(*this, FrameScheduler::Priority::Interactive);
    }
}

// ============================================================================
// ViewMapper construction
// ============================================================================

ViewMapper ArrangementViewComponent::makeViewMapper() const noexcept
{
    const auto viewportBounds = getContentViewportBounds();
    return ViewMapper{
        camera_.visibleStartSeconds,
        camera_.pixelsPerSecond,
        kArrangementContentStartX,
        viewportBounds.getWidth(),
        viewportBounds.getHeight(),
        1.0f,  // pixelsPerSemitone (not used in ArrangementView)
        0.0f,  // verticalScrollOffset (not used in ArrangementView)
        127.0f // maxMidi (not used in ArrangementView)
    };
}

ArrangementVerticalWindow ArrangementViewComponent::makeArrangementVerticalWindow() const noexcept
{
    const auto vpBounds = getContentViewportBounds();
    return ArrangementVerticalWindow{
        rulerHeight_,
        processor_.getTrackHeight(),
        verticalScrollOffset_,
        vpBounds.getHeight()
    };
}

} // namespace OpenTune
