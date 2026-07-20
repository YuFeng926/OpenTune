#include "ArrangementViewComponent.h"
#include "AuroraTheme.h"
#include "UiAssets.h"
#include "TimelineViewportPolicy.h"
#include "TimelineCompositeCache.h"
#include "TimelineLayerComposer.h"
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
constexpr float kPlacementDragThresholdPx = 5.0f;

struct ArrangementClipPaintInput {
    juce::Rectangle<float> fullBounds;
    juce::Rectangle<int> paintClip;
    juce::Colour trackColour;
    juce::String displayName;
    ContentKey contentKey;
    float gain = 1.0f;
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
    bool selected = false;
    bool preview = false;
    double clipInSeconds = 0.0;
    double timelineStartSeconds = 0.0;
    double durationSeconds = 0.0;
    double pixelsPerSecond = 1.0;
};

juce::String formatGainLabel(float gain)
{
    float db = juce::Decibels::gainToDecibels(gain, -96.0f);
    if (std::abs(db) < 0.05f)
        db = 0.0f;

    auto text = juce::String(db, 1);
    if (db >= 0.0f)
        text = "+" + text;

    return text + " dB";
}

juce::Rectangle<int> computeHistoricalWaveformDrawableBounds(juce::Rectangle<int> placementBounds) noexcept
{
    if (placementBounds.isEmpty())
        return {};

    const int horizontalInset = juce::jmin(6, juce::jmax(0, (placementBounds.getWidth() - 1) / 2));
    const int verticalInset = juce::jmin(6, juce::jmax(0, (placementBounds.getHeight() - 1) / 2));
    auto bounds = placementBounds.reduced(horizontalInset, verticalInset);

    if (bounds.getWidth() <= 0)
        bounds.setWidth(1);
    if (bounds.getHeight() <= 0)
        bounds.setHeight(1);

    return bounds;
}

static void paintHistoricalClipWaveform(juce::Graphics& g,
                                        const ArrangementClipPaintInput& clip,
                                        const WaveformMipmapCache& waveformMipmapCache)
{
    const auto placementBounds = clip.fullBounds.getSmallestIntegerContainer();
    const auto waveformBounds = computeHistoricalWaveformDrawableBounds(placementBounds);
    const auto visibleWaveformBounds = waveformBounds.getIntersection(clip.paintClip);
    const auto* mipmap = waveformMipmapCache.get(clip.contentKey);
    if (mipmap == nullptr || !mipmap->hasSource() || visibleWaveformBounds.isEmpty() || clip.durationSeconds <= 0.0)
        return;

    const int levelIndex = mipmap->selectBestLevelIndex(clip.pixelsPerSecond);
    const auto& level = mipmap->getLevel(levelIndex);
    if (level.peaks.empty())
        return;

    const int64_t numPeaks = static_cast<int64_t>(level.peaks.size());
    const int64_t builtPeaks = level.complete ? numPeaks : level.buildProgress;
    if (builtPeaks <= 0)
        return;

    const float midY = static_cast<float>(waveformBounds.getCentreY());
    const float halfH = waveformBounds.getHeight() * 0.45f;
    const int samplesPerPeak = WaveformMipmap::kSamplesPerPeak[levelIndex];
    const double timePerPeak = static_cast<double>(samplesPerPeak) / WaveformMipmap::kBaseSampleRate;
    const double timelineEndSeconds = clip.timelineStartSeconds + clip.durationSeconds;
    const double sourceEndSeconds = clip.clipInSeconds + clip.durationSeconds;

    juce::Path wavePath;

    for (int x = visibleWaveformBounds.getX(); x < visibleWaveformBounds.getRight(); ++x) {
        const double timelineTime = clip.timelineStartSeconds
            + (static_cast<double>(x) - static_cast<double>(clip.fullBounds.getX())) / clip.pixelsPerSecond;
        if (timelineTime < clip.timelineStartSeconds || timelineTime >= timelineEndSeconds)
            continue;

        const double contentTime = clip.clipInSeconds + (timelineTime - clip.timelineStartSeconds);
        if (contentTime < clip.clipInSeconds || contentTime >= sourceEndSeconds)
            continue;

        const int64_t peakIndex = static_cast<int64_t>(contentTime / timePerPeak);
        if (peakIndex < 0 || peakIndex >= builtPeaks)
            continue;

        const double timelineTimeNext = clip.timelineStartSeconds
            + (static_cast<double>(x + 1) - static_cast<double>(clip.fullBounds.getX())) / clip.pixelsPerSecond;
        const double contentTimeNext = clip.clipInSeconds + (timelineTimeNext - clip.timelineStartSeconds);
        int64_t idxStart = peakIndex;
        int64_t idxEnd = static_cast<int64_t>(contentTimeNext / timePerPeak);
        if (idxEnd <= idxStart)
            idxEnd = idxStart + 1;

        float aggMin = 0.0f;
        float aggMax = 0.0f;
        bool hasData = false;

        for (int64_t i = idxStart; i < idxEnd && i < builtPeaks; ++i) {
            if (i < 0)
                continue;

            const auto& pk = level.peaks[static_cast<std::size_t>(i)];
            if (pk.isZero())
                continue;

            if (!hasData) {
                aggMin = pk.getMin();
                aggMax = pk.getMax();
                hasData = true;
            } else {
                aggMin = std::min(aggMin, pk.getMin());
                aggMax = std::max(aggMax, pk.getMax());
            }
        }

        if (!hasData)
            continue;

        const float displayTop = aggMax * clip.gain * halfH;
        const float displayBottom = aggMin * clip.gain * halfH;
        float y1 = midY - displayTop;
        float y2 = midY - displayBottom;

        if ((y2 - y1) < 2.0f) {
            const float expand = (2.0f - (y2 - y1)) * 0.5f;
            y1 -= expand;
            y2 += expand;
        }

        const float fx = static_cast<float>(x) + 0.5f;
        wavePath.startNewSubPath(fx, y1);
        wavePath.lineTo(fx, y2);
    }

    if (wavePath.isEmpty())
        return;

    const auto themeId = UIColors::currentThemeId();
    juce::PathStrokeType glowStroke(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    juce::PathStrokeType mainStroke(1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    if (themeId == ThemeId::Aurora) {
        g.setColour(juce::Colours::white.withAlpha(0.22f));
        g.strokePath(wavePath, glowStroke);
        g.setColour(juce::Colours::white.withAlpha(0.78f));
        g.strokePath(wavePath, mainStroke);
    } else if (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) {
        g.setColour(UIColors::pianoRollWaveform.withAlpha(0.09f));
        g.strokePath(wavePath, glowStroke);
        g.setColour(UIColors::pianoRollWaveform.withAlpha(0.24f));
        g.strokePath(wavePath, mainStroke);
    } else {
        g.setColour(juce::Colour(0xFF3E4652).withAlpha(0.22f));
        g.strokePath(wavePath, glowStroke);
        g.setColour(juce::Colour(0xFF3E4652).withAlpha(0.78f));
        g.strokePath(wavePath, mainStroke);
    }
}

static void paintHistoricalClipShellAndWaveform(juce::Graphics& g,
                                                const ArrangementClipPaintInput& clip,
                                                const WaveformMipmapCache& waveformMipmapCache)
{
    juce::Graphics::ScopedSaveState scoped(g);
    g.reduceClipRegion(clip.paintClip);

    const auto bounds = clip.fullBounds;
    const auto themeId = UIColors::currentThemeId();
    if (themeId == ThemeId::DarkBlueGrey && clip.selected) {
        juce::ColourGradient sel(juce::Colour{0xFFF7F3EA}, bounds.getX(), bounds.getBottom(),
                                 juce::Colour{0xFFBFE0EF}, bounds.getX(), bounds.getY(), false);
        g.setGradientFill(sel);
        g.fillRoundedRectangle(bounds, 6.0f);
        g.setColour(UIColors::panelBorder.withAlpha(0.55f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 1.0f);
    } else if (themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) {
        const auto topColor = clip.selected
            ? UIColors::buttonHover.interpolatedWith(UIColors::glassHighlight, 0.12f)
            : UIColors::buttonNormal.interpolatedWith(UIColors::glassHighlight, 0.075f);
        const auto bottomColor = clip.selected
            ? UIColors::buttonPressed.interpolatedWith(UIColors::pianoRollBackground, 0.22f)
            : UIColors::buttonNormal.interpolatedWith(UIColors::pianoRollBackground, 0.22f);

        juce::ColourGradient grad(topColor, bounds.getX(), bounds.getY(),
                                  bottomColor, bounds.getRight(), bounds.getBottom(), false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(bounds, 6.0f);

        juce::ColourGradient source(UIColors::glassHighlight.withAlpha(clip.selected ? 0.15f : 0.085f),
                                    bounds.getX() + bounds.getWidth() * 0.18f,
                                    bounds.getY() + bounds.getHeight() * 0.12f,
                                    juce::Colours::transparentBlack,
                                    bounds.getRight(),
                                    bounds.getBottom(),
                                    true);
        g.setGradientFill(source);
        g.fillRoundedRectangle(bounds.reduced(1.0f), 5.0f);

        g.setColour((clip.selected ? UIColors::accent : UIColors::panelBorder)
                        .withAlpha(clip.selected ? 0.72f : 0.42f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, clip.selected ? 1.2f : 0.9f);
    } else if (themeId == ThemeId::Aurora) {
        if (clip.selected) {
            g.setColour(clip.trackColour.withAlpha(0.45f));
            g.fillRoundedRectangle(bounds, 6.0f);
            g.setColour(juce::Colour(Aurora::Colors::Cyan));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 2.0f);
        } else {
            g.setColour(clip.trackColour.withAlpha(0.30f));
            g.fillRoundedRectangle(bounds, 6.0f);
            g.setColour(clip.trackColour.withAlpha(0.6f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 1.0f);
        }
    } else {
        juce::Colour fill = clip.selected ? UIColors::primaryPurple : UIColors::buttonNormal;
        g.setColour(fill);
        g.fillRoundedRectangle(bounds, 6.0f);
        g.setColour(UIColors::panelBorder);
        g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 1.0f);
    }

    if (clip.preview) {
        g.setColour(UIColors::accent.withAlpha(0.18f));
        g.fillRoundedRectangle(bounds.reduced(1.0f), 5.0f);
        g.setColour(UIColors::accent.withAlpha(0.82f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 1.6f);
    }

    paintHistoricalClipWaveform(g, clip, waveformMipmapCache);

    if (clip.fadeInSeconds > 0.001) {
        const float fadePixels = static_cast<float>(clip.fadeInSeconds * clip.pixelsPerSecond);
        juce::Path p;
        p.addTriangle(bounds.getX(), bounds.getY(),
                      bounds.getX() + fadePixels, bounds.getY(),
                      bounds.getX(), bounds.getBottom());
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.fillPath(p);
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.fillRect(juce::Rectangle<float>(bounds.getX(), bounds.getY(), 10.0f, 10.0f));
    }

    if (clip.fadeOutSeconds > 0.001) {
        const float fadePixels = static_cast<float>(clip.fadeOutSeconds * clip.pixelsPerSecond);
        juce::Path p;
        p.addTriangle(bounds.getRight(), bounds.getY(),
                      bounds.getRight() - fadePixels, bounds.getY(),
                      bounds.getRight(), bounds.getBottom());
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.fillPath(p);
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.fillRect(juce::Rectangle<float>(bounds.getRight() - 10.0f, bounds.getY(), 10.0f, 10.0f));
    }
}

static void paintOverlapShading(juce::Graphics& g, const std::vector<ArrangementClipPaintInput>& clips)
{
    for (size_t i = 0; i < clips.size(); ++i) {
        for (size_t j = i + 1; j < clips.size(); ++j) {
            const auto overlap = clips[i].fullBounds.getIntersection(clips[j].fullBounds);
            if (overlap.isEmpty())
                continue;

            g.setColour(juce::Colours::black.withAlpha(0.30f));
            g.fillRect(overlap);
        }
    }
}

static void paintHistoricalClipTextFadeGain(juce::Graphics& g, const ArrangementClipPaintInput& clip)
{
    juce::Graphics::ScopedSaveState scoped(g);
    g.reduceClipRegion(clip.paintClip);

    const auto bounds = clip.fullBounds;
    if (bounds.getWidth() <= 30.0f || bounds.getHeight() <= 12.0f)
        return;

    const auto textArea = bounds.getSmallestIntegerContainer().reduced(6, 4);

    if (clip.displayName.isNotEmpty()) {
        auto displayName = clip.displayName;
        if (displayName.length() > 20)
            displayName = displayName.substring(0, 17) + "...";

        g.setColour(UIColors::textPrimary.withAlpha(0.85f));
        g.setFont(UIColors::getUIFont(10.0f));
        g.drawText(displayName, textArea, juce::Justification::topLeft);
    }

    g.setColour(UIColors::textSecondary.withAlpha(0.9f));
    g.setFont(UIColors::getUIFont(11.0f));
    g.drawText(formatGainLabel(clip.gain), textArea, juce::Justification::topRight);
}

static void paintHistoricalArrangementClips(juce::Graphics& g,
                                            const std::vector<ArrangementClipPaintInput>& clips,
                                            const WaveformMipmapCache& waveformMipmapCache)
{
    for (const auto& clip : clips)
        paintHistoricalClipShellAndWaveform(g, clip, waveformMipmapCache);

    paintOverlapShading(g, clips);

    for (const auto& clip : clips)
        paintHistoricalClipTextFadeGain(g, clip);
}

template <typename IsSelected>
std::vector<ArrangementClipPaintInput> collectVisibleArrangementClips(const StandaloneArrangement& arrangement,
                                                       double tileStart,
                                                       double tileEnd,
                                                       const ArrangementVerticalWindow& verticalWindow,
                                                       double pixelsPerSecond,
                                                       int tileWidth,
                                                       IsSelected&& isSelected)
{
    std::vector<ArrangementClipPaintInput> clips;

    for (int trackId = verticalWindow.firstTrack(); trackId < verticalWindow.lastTrackExclusive(); ++trackId) {
        const int trackY = trackId * verticalWindow.trackHeight - verticalWindow.worldTopY;
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

            const int x = static_cast<int>(std::llround((placementStart - tileStart) * pixelsPerSecond));
            const int width = juce::jmax(8,
                static_cast<int>(std::llround(placement.durationSeconds * pixelsPerSecond)));
            const int y = trackY + 2;
            const int height = verticalWindow.trackHeight - 4;

            const auto fullBounds = juce::Rectangle<float>(static_cast<float>(x),
                                                           static_cast<float>(y),
                                                           static_cast<float>(width),
                                                           static_cast<float>(height));
            const auto tileBounds = juce::Rectangle<int>(0, 0, tileWidth, verticalWindow.viewportContentHeight);
            const auto laneBounds = juce::Rectangle<int>(0, y, tileWidth, height);
            const auto paintClip = fullBounds.getSmallestIntegerContainer()
                .getIntersection(tileBounds)
                .getIntersection(laneBounds);

            if (paintClip.isEmpty())
                continue;

            clips.push_back({
                fullBounds,
                paintClip,
                arrangement.getTrackColour(trackId),
                placement.name,
                placement.contentKey,
                placement.gain,
                placement.fadeInDuration,
                placement.fadeOutDuration,
                isSelected(trackId, placement.placementId),
                false,
                placement.clipInSeconds,
                placement.timelineStartSeconds,
                placement.durationSeconds,
                pixelsPerSecond
            });
        }
    }

    return clips;
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

constexpr double kArrangementDefaultSpanSeconds = 60.0 * 5.0;
constexpr double kArrangementTrailingPaddingSeconds = 10.0;
constexpr double kArrangementRenderBandOverscanScreens = 1.0;

ArrangementViewComponent::ArrangementViewComponent(OpenTuneAudioProcessor& processor)
    : processor_(processor)
    , playHeadState_(processor.getPlayHeadState())
{
    // Initial playhead presentation comes straight from processor-owned state.
    playheadTimeForPaint_ = playHeadState_.timeInSeconds.load(std::memory_order_relaxed);
    lastContextBpm_ = processor_.getBpm();
    lastContextTimeSigNum_ = processor_.getTimeSigNumerator();
    lastContextTimeSigDenom_ = processor_.getTimeSigDenominator();

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
        transitionActive_ = false;
        requestTransition_ = false;
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
        rebuildTimelineCoverage();
        repaint();
        };
    timeUnitToggleButton_.setColour(juce::TextButton::buttonColourId, UIColors::backgroundLight);
    timeUnitToggleButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
    addAndMakeVisible(timeUnitToggleButton_);
    timeUnitToggleButton_.setTooltip(LOC(kTooltipTimeUnit));

    scrollVBlankAttachment_ = std::make_unique<juce::VBlankAttachment>(
        this, [this](double timestampSec) { onScrollVBlankCallback(timestampSec); });
}

ArrangementViewComponent::~ArrangementViewComponent()
{
    scrollVBlankAttachment_.reset();
    scrollModeToggleButton_.setLookAndFeel(nullptr);
    timeUnitToggleButton_.setLookAndFeel(nullptr);
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

void ArrangementViewComponent::commitViewportRequest(TimelineViewportRequest req)
{
    activateTimelineCamera(TimelineViewportPolicy::resolve(req));
}

void ArrangementViewComponent::activateTimelineCamera(TimelineViewportCamera camera)
{
    transitionActive_ = false;
    requestTransition_ = false;
    camera_ = camera;
    rebuildTimelineCoverage();
    updateScrollBars();
    repaint();
}

juce::Rectangle<int> ArrangementViewComponent::timeAxisRect() const noexcept
{
    const auto vp = getContentViewportBounds();
    return { kArrangementContentStartX, 0, vp.getWidth(), rulerHeight_ + vp.getHeight() };
}

void ArrangementViewComponent::surfaceInvalidate()
{
    viewportSurface_ = juce::Image();
    surfaceOriginPx_ = 0;
    surfacePps_ = 0.0;
}

void ArrangementViewComponent::surfaceRebuildFromReadyTiles(int64_t firstTimeTile, int64_t lastTimeTile)
{
    const auto rect = getContentViewportBounds();
    const int sw = rect.getWidth();
    const int sh = rect.getHeight();
    if (sw <= 0 || sh <= 0) return;

    const double pps = std::round(camera_.pixelsPerSecond * 1000.0) / 1000.0;
    if (pps <= 0.0) return;

    viewportSurface_ = juce::Image(juce::Image::ARGB, sw, sh, true);
    surfaceOriginPx_ = static_cast<int64_t>(std::llround(camera_.visibleStartSeconds * pps));
    surfacePps_ = pps;

    juce::Graphics g(viewportSurface_);
    if (themeBackdrop_.isValid())
        g.drawImageAt(themeBackdrop_, -rect.getX(), -rect.getY(), false);

    const int visibleTopY = verticalScrollOffset_;
    const int firstVertRow = visibleTopY / TimelineCompositeCache::kWorldTileHeight;
    const int lastVertRow = (visibleTopY + sh + TimelineCompositeCache::kWorldTileHeight - 1)
        / TimelineCompositeCache::kWorldTileHeight;

    for (int64_t tt = firstTimeTile; tt <= lastTimeTile; ++tt) {
        for (int vr = firstVertRow; vr <= lastVertRow; ++vr) {
            TimelineCompositeCache::TileKey key{tt, vr};
            if (const auto* entry = compositeCache_.findTile(key)) {
                const int destX = static_cast<int>(tt * TimelineCompositeCache::kTileWidthPx - surfaceOriginPx_);
                const float destY = static_cast<float>(vr * TimelineCompositeCache::kWorldTileHeight - visibleTopY);
                g.drawImageTransformed(entry->background,
                    juce::AffineTransform::translation(static_cast<float>(destX), destY), false);
            }
        }
    }
}

void ArrangementViewComponent::surfaceScrollAndFillExposed(int64_t newOriginPx, int64_t firstTimeTile, int64_t lastTimeTile)
{
    const auto rect = getContentViewportBounds();
    const int sw = rect.getWidth();
    const int sh = rect.getHeight();
    if (sw <= 0 || sh <= 0) return;

    if (!viewportSurface_.isValid()) {
        surfaceRebuildFromReadyTiles(firstTimeTile, lastTimeTile);
        return;
    }

    const int64_t deltaPx = newOriginPx - surfaceOriginPx_;
    const int absDelta = static_cast<int>(std::llabs(deltaPx));
    if (absDelta >= sw) {
        surfaceRebuildFromReadyTiles(firstTimeTile, lastTimeTile);
        return;
    }

    if (deltaPx > 0) {
        viewportSurface_.moveImageSection(0, 0, absDelta, 0, sw - absDelta, sh);
        viewportSurface_.clear({sw - absDelta, 0, absDelta, sh}, juce::Colours::transparentBlack);
    } else if (deltaPx < 0) {
        viewportSurface_.moveImageSection(absDelta, 0, 0, 0, sw - absDelta, sh);
        viewportSurface_.clear({0, 0, absDelta, sh}, juce::Colours::transparentBlack);
    }

    const juce::Rectangle<int> exposed = deltaPx > 0
        ? juce::Rectangle<int>(sw - absDelta, 0, absDelta, sh)
        : juce::Rectangle<int>(0, 0, absDelta, sh);

    const int visibleTopY = verticalScrollOffset_;
    const int firstVertRow = visibleTopY / TimelineCompositeCache::kWorldTileHeight;
    const int lastVertRow = (visibleTopY + sh + TimelineCompositeCache::kWorldTileHeight - 1)
        / TimelineCompositeCache::kWorldTileHeight;

    juce::Graphics g(viewportSurface_);
    juce::Graphics::ScopedSaveState st(g);
    g.reduceClipRegion(exposed);
    if (themeBackdrop_.isValid())
        g.drawImageAt(themeBackdrop_, -(rect.getX() + exposed.getX()), -rect.getY(), false);

    for (int64_t tt = firstTimeTile; tt <= lastTimeTile; ++tt) {
        for (int vr = firstVertRow; vr <= lastVertRow; ++vr) {
            TimelineCompositeCache::TileKey key{tt, vr};
            if (const auto* entry = compositeCache_.findTile(key)) {
                const int destX = static_cast<int>(tt * TimelineCompositeCache::kTileWidthPx - newOriginPx);
                const float destY = static_cast<float>(vr * TimelineCompositeCache::kWorldTileHeight - visibleTopY);
                g.drawImageTransformed(entry->background,
                    juce::AffineTransform::translation(static_cast<float>(destX), destY), false);
            }
        }
    }

    surfaceOriginPx_ = newOriginPx;
}

void ArrangementViewComponent::rebuildTimelineCoverage()
{
    const int contentViewportWidth = getVisibleViewportWidth();
    const double tileDuration = static_cast<double>(TimelineCompositeCache::kTileWidthPx) / camera_.pixelsPerSecond;
    const double visibleStart = camera_.visibleStartSeconds;
    const double visibleEnd = visibleStart + contentViewportWidth / camera_.pixelsPerSecond;

    if (playHeadState_.isPlaying.load(std::memory_order_relaxed)) {
        const double viewportDur = contentViewportWidth / camera_.pixelsPerSecond;
        constexpr int kMaxAheadTiles = 16;
        const double ahead = std::min(6.0 * viewportDur, kMaxAheadTiles * tileDuration);
        tileCoverageStartSeconds_ = std::max(0.0,
            std::floor((visibleStart - 2.0 * tileDuration) / tileDuration) * tileDuration);
        tileCoverageEndSeconds_ = std::ceil((visibleEnd + ahead) / tileDuration) * tileDuration;
    } else {
        tileCoverageStartSeconds_ = std::max(0.0,
            std::floor((visibleStart - tileDuration) / tileDuration) * tileDuration);
        tileCoverageEndSeconds_ = std::ceil((visibleEnd + tileDuration) / tileDuration) * tileDuration;
    }

    prepareCoverageCompositeTilesNew();

    const int64_t firstTimeTile = std::max<int64_t>(0,
        static_cast<int64_t>(std::floor(visibleStart / tileDuration)));
    const int64_t lastTimeTile = static_cast<int64_t>(
        std::floor((visibleEnd - 1.0e-9) / tileDuration));

    surfaceRebuildFromReadyTiles(firstTimeTile, lastTimeTile);
}

void ArrangementViewComponent::invalidateStableScene()
{
    rebuildContentMetrics();
    updateScrollBars();
    rebuildTimelineCoverage();
    repaint();
}

void ArrangementViewComponent::rebuildThemeBackdrop()
{
    const auto bounds = getLocalBounds();
    const int w = bounds.getWidth();
    const int h = bounds.getHeight();
    if (w <= 0 || h <= 0) return;

    themeBackdrop_ = juce::Image(juce::Image::ARGB, w, h, true);
    juce::Graphics g(themeBackdrop_);

    const auto themeId = UIColors::currentThemeId();
    const auto bf = bounds.toFloat();
    if (themeId == ThemeId::Aurora)
        UIColors::fillAuroraTimelineBackground(g, bf, 0.0f);
    else if (themeId == ThemeId::BlueBreeze)
        UIColors::fillMistedTimelineField(g, bf, 0.0f);
    else if (themeId == ThemeId::Overdose)
        UiAssets::drawAssetStretch(g, UiAssetId::PanelEditorMain, bf);
    else if (themeId == ThemeId::DarkBlueGrey)
        UIColors::fillSoothe2SpectrumBackground(g, bf, 0.0f);
    else
        g.fillAll(UIColors::rollBackground);
}

void ArrangementViewComponent::preparePlaybackCoverage()
{
    const int width = getVisibleViewportWidth();
    const double pps = camera_.pixelsPerSecond;
    if (width <= 0 || pps <= 0.0)
        return;

    const double visibleStart = camera_.visibleStartSeconds;
    const double visibleDuration = width / pps;
    const double visibleEnd = visibleStart + visibleDuration;
    const double tileDuration = static_cast<double>(TimelineCompositeCache::kTileWidthPx) / pps;
    constexpr int kMaxAheadTiles = 16;
    const double ahead = std::min(6.0 * visibleDuration, kMaxAheadTiles * tileDuration);

    tileCoverageStartSeconds_ = std::max(0.0,
        std::floor((visibleStart - 2.0 * tileDuration) / tileDuration) * tileDuration);
    tileCoverageEndSeconds_ = std::ceil((visibleEnd + ahead) / tileDuration) * tileDuration;
    prepareCoverageCompositeTilesNew();
    if (!viewportSurface_.isValid()) {
        const int64_t firstTimeTile = std::max<int64_t>(0,
            static_cast<int64_t>(std::floor(camera_.visibleStartSeconds / tileDuration)));
        const int64_t lastTimeTile = static_cast<int64_t>(
            std::floor((camera_.visibleStartSeconds + visibleDuration) / tileDuration));
        surfaceRebuildFromReadyTiles(firstTimeTile, lastTimeTile);
    }
}

void ArrangementViewComponent::requestContentRedraw()
{
    invalidateStableScene();
}

void ArrangementViewComponent::requestThemeRedraw()
{
    rebuildThemeBackdrop();
    invalidateStableScene();
}

TimelineViewportRequest ArrangementViewComponent::makeViewportRequest(
    TimelineViewportRequest::Kind kind,
    double targetTime,
    double anchorViewportX,
    double pps) const
{
    TimelineViewportRequest req;
    req.kind = kind;
    req.viewKind = TimelineViewportRequest::ViewKind::Arrangement;
    req.targetTime = targetTime;
    req.anchorViewportX = anchorViewportX;
    req.viewportWidth = getVisibleViewportWidth();
    req.pixelsPerSecond = pps;
    return req;
}

void ArrangementViewComponent::setVerticalScrollOffset(int offset)
{
    // 璁＄畻鏈€澶ф粴鍔ㄥ亸绉伙紙鍙杞ㄩ亾楂樺害 + ruler楂樺害 - 鍙楂樺害锛?
    const int totalContentHeight = rulerHeight_ + visibleTrackCount_ * processor_.getTrackHeight();
    const int visibleHeight = getHeight() - UIColors::scrollBarThickness;
    const int maxScrollOffset = juce::jmax(0, totalContentHeight - visibleHeight);
    
    // 闄愬埗婊氬姩鑼冨洿 [0, maxScrollOffset]
    verticalScrollOffset_ = juce::jlimit(0, maxScrollOffset, offset);
    verticalScrollBar_.setCurrentRangeStart(verticalScrollOffset_, juce::dontSendNotification);

    prepareCoverageCompositeTilesNew();
    const double pps = std::round(camera_.pixelsPerSecond * 1000.0) / 1000.0;
    const double tileDuration = TimelineCompositeCache::kTileWidthPx / pps;
    const int64_t firstTimeTile = std::max<int64_t>(0,
        static_cast<int64_t>(std::floor(camera_.visibleStartSeconds / tileDuration)));
    const int64_t lastTimeTile = static_cast<int64_t>(
        std::floor((camera_.visibleStartSeconds + getVisibleViewportWidth() / pps) / tileDuration));
    surfaceRebuildFromReadyTiles(firstTimeTile, lastTimeTile);
    repaint();
}

void ArrangementViewComponent::setVisibleTrackCount(int count)
{
    visibleTrackCount_ = juce::jlimit(1, OpenTuneAudioProcessor::MAX_TRACKS, count);
    // Re-clamp scroll offset for new track count
    const int totalContentHeight = rulerHeight_ + visibleTrackCount_ * processor_.getTrackHeight();
    const int visibleHeight = getHeight() - UIColors::scrollBarThickness;
    const int maxScrollOffset = juce::jmax(0, totalContentHeight - visibleHeight);
    verticalScrollOffset_ = juce::jlimit(0, maxScrollOffset, verticalScrollOffset_);
    updateScrollBars();
    invalidateStableScene();
}

void ArrangementViewComponent::fitToContent()
{
    // 濡傛灉鐢ㄦ埛宸叉墜鍔ㄨ皟鏁磋繃缂╂斁锛屼笉鑷姩瑕嗙洊
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

    const auto req = makeViewportRequest(
        TimelineViewportRequest::Kind::Manual,
        0.0,
        0.0,
        static_cast<double>(drawableWidth) / maxEndTime);
    commitViewportRequest(req);
}

void ArrangementViewComponent::setExperimentalReferenceControlsEnabled(bool enabled)
{
    if (experimentalReferenceControlsEnabled_ == enabled) {
        return;
    }

    experimentalReferenceControlsEnabled_ = enabled;
    if (!experimentalReferenceControlsEnabled_) {
        mouseOverReferenceButton_ = false;
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    repaint();
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
    rebuildThemeBackdrop();
    rebuildTimelineCoverage();
    // Import drop preview highlight (transient, UI-only)

    timeUnitToggleButton_.toFront(false);
    scrollModeToggleButton_.toFront(false);
    repaint();
}

void ArrangementViewComponent::scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart)
{
    if (scrollBar == &horizontalScrollBar_)
    {
        const auto req = makeViewportRequest(
            TimelineViewportRequest::Kind::Manual,
            newRangeStart / camera_.pixelsPerSecond,
            0.0,
            camera_.pixelsPerSecond);
        commitViewportRequest(req);
    }
    else if (scrollBar == &verticalScrollBar_)
    {
        setVerticalScrollOffset(static_cast<int>(newRangeStart));
        // 閫氱煡鐩戝惉鍣ㄥ瀭鐩存粴鍔ㄥ亸绉诲彉鍖栵紙鐢ㄤ簬鍚屾TrackPanel锛?
        listeners_.call([this](Listener& l) { l.verticalScrollChanged(verticalScrollOffset_); });
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

    const int totalTrackHeight = rulerHeight_ + visibleTrackCount_ * processor_.getTrackHeight();
    const int visibleHeight = getHeight() - UIColors::scrollBarThickness;
    verticalScrollBar_.setRangeLimits(
        0.0, static_cast<double>(totalTrackHeight), juce::dontSendNotification);
    verticalScrollBar_.setCurrentRange(
        static_cast<double>(verticalScrollOffset_),
        static_cast<double>(visibleHeight),
        juce::dontSendNotification);
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
            // Include placement name in revision (content tile renders label)
            revision = hashCombine(revision, static_cast<uint64_t>(placement.name.hashCode()));
        }
    }

    // P1-5: Include clipInSeconds in revision (waveform visual updates go through pending mechanism)
    // Include waveform build state (tiles embed waveform pixels)
    revision = hashCombine(revision, waveformRevision_);
    // Include visible track count (tile builder depends on it for lane rendering)
    revision = hashCombine(revision, static_cast<uint64_t>(visibleTrackCount_));

    contentMetrics_.revision = revision;
    contentMetrics_.maxEndTimeSeconds = maxEndTime;
    contentMetrics_.totalContentWidthPx = makeViewMapper().timeToX(maxEndTime);
}

// ============================================================================
// Composite cache pipeline (new)
// ============================================================================

GenerationSignature ArrangementViewComponent::makeGenerationSignature() const
{
    GenerationSignature sig;
    sig.ppsMilli = static_cast<int64_t>(std::round(camera_.pixelsPerSecond * 1000.0));
    sig.dpiMilli = static_cast<int64_t>(std::round(getDesktopScaleFactor() * 1000.0));

    sig.geometry.minMidi = 0.0f;
    sig.geometry.maxMidi = 127.0f;
    sig.geometry.pixelsPerSemitone = 0.0f;
    sig.geometry.trackHeight = processor_.getTrackHeight();

    sig.themeId = static_cast<int>(UIColors::currentThemeId());
    sig.laneStyle = 0;  // Arrangement 涓嶄娇鐢?lane style
    sig.timeUnit = (timeUnit_ == TimeUnit::Bars) ? 1 : 0;
    sig.tempo = static_cast<int>(lastContextBpm_ > 0.0 ? lastContextBpm_ : 120.0);
    sig.timeSigNumerator = lastContextTimeSigNum_ > 0 ? lastContextTimeSigNum_ : 4;
    sig.timeSigDenominator = lastContextTimeSigDenom_ > 0 ? lastContextTimeSigDenom_ : 4;
    sig.contentRevision = contentMetrics_.revision;

    return sig;
}

void ArrangementViewComponent::buildCompositeTile(
    juce::Graphics& g,
    juce::Rectangle<int> tileBounds,
    TimelineCompositeCache::TileKey key)
{
    const double ppsCanonical = std::round(camera_.pixelsPerSecond * 1000.0) / 1000.0;
    const double tileDuration = static_cast<double>(TimelineCompositeCache::kTileWidthPx) / ppsCanonical;
    const double tileStartSec = key.timeTile * tileDuration;
    const double tileEndSec = tileStartSec + tileDuration;
    const int worldTopY = key.vertRow * TimelineCompositeCache::kWorldTileHeight;

    const auto themeId = UIColors::currentThemeId();
    const int trackHeight = processor_.getTrackHeight();

    // 1. Pattern layer: track lanes
    if (themeId == ThemeId::Aurora || themeId == ThemeId::BlueBreeze || themeId == ThemeId::Overdose) {
        const int visibleTracks = juce::jmax(1, visibleTrackCount_);
        const int firstTrack = std::max(0, worldTopY / trackHeight);
        const int lastTrack = std::min(visibleTracks,
            (worldTopY + tileBounds.getHeight() + trackHeight - 1) / trackHeight);
        for (int trackId = firstTrack; trackId < lastTrack; ++trackId) {
            const int trackCanvasY = trackId * trackHeight;
            const float laneY = static_cast<float>(trackCanvasY - worldTopY);
            auto lane = juce::Rectangle<float>(0.0f, laneY,
                                               static_cast<float>(tileBounds.getWidth()),
                                               static_cast<float>(trackHeight));
            if (lane.getBottom() < 0.0f || lane.getY() > tileBounds.getHeight()) continue;

            const auto laneFill = themeId == ThemeId::Aurora
                ? ((trackId % 2 == 0) ? UIColors::glassSurface.withAlpha(0.055f) : UIColors::pianoRollLane.withAlpha(0.030f))
                : ((trackId % 2 == 0) ? UIColors::pianoRollLane.withAlpha(0.060f) : UIColors::glassSurface.withAlpha(0.022f));
            g.setColour(laneFill);
            g.fillRect(lane);
            g.setColour((themeId == ThemeId::Aurora ? UIColors::gridLine : UIColors::pianoRollGrid)
                            .withAlpha(themeId == ThemeId::Aurora ? 0.026f : 0.036f));
            g.drawHorizontalLine(juce::roundToInt(lane.getBottom()), lane.getX(), lane.getRight());
        }
    }

    // 2. Grid lines
    RenderParams gridParams;
    gridParams.visibleStartSeconds = tileStartSec;
    gridParams.visibleEndSeconds = tileEndSec;
    gridParams.pixelsPerSecond = ppsCanonical;
    gridParams.timeUnit = (timeUnit_ == TimeUnit::Bars) ? 1 : 0;
    gridParams.tempo = static_cast<int>(lastContextBpm_ > 0.0 ? lastContextBpm_ : 120.0);
    gridParams.themeId = static_cast<int>(themeId);
    gridParams.pixelsPerSemitone = 0.0f;
    gridParams.worldTopY = worldTopY;
    gridParams.rulerHeight = 0;
    gridParams.viewportWidth = tileBounds.getWidth();
    gridParams.viewportHeight = tileBounds.getHeight();
    gridParams.viewKind = "arrangement";
    TimelineLayerComposer::drawGridLines(g, gridParams);

    // 3. Content layer: clips
    const ArrangementVerticalWindow vwin{trackHeight, worldTopY, tileBounds.getHeight()};
    auto& arrangement = *processor_.getStandaloneArrangement();
    auto clips = collectVisibleArrangementClips(arrangement, tileStartSec, tileEndSec, vwin, ppsCanonical,
        tileBounds.getWidth(), [](int, uint64_t) { return false; });
    paintHistoricalArrangementClips(g, clips, waveformMipmapCache_);
}

void ArrangementViewComponent::prepareCoverageCompositeTilesNew()
{
    const auto sig = makeGenerationSignature();
    const double ppsCanonical = sig.ppsMilli / 1000.0;
    const double tileDuration = TimelineCompositeCache::kTileWidthPx / ppsCanonical;

    const int64_t firstTimeTile = std::max(0LL,
        static_cast<int64_t>(std::floor(tileCoverageStartSeconds_ / tileDuration)));
    const int64_t lastTimeTile = static_cast<int64_t>(
        std::floor((tileCoverageEndSeconds_ - 1e-9) / tileDuration));

    const int vpHeight = getContentViewportBounds().getHeight();
    const float visibleTopY = static_cast<float>(verticalScrollOffset_);
    const float visibleBottomY = visibleTopY + static_cast<float>(vpHeight);
    const int firstVertRow = static_cast<int>(std::floor(
        visibleTopY / static_cast<float>(TimelineCompositeCache::kWorldTileHeight)));
    const int lastVertRow = static_cast<int>(std::ceil(
        visibleBottomY / static_cast<float>(TimelineCompositeCache::kWorldTileHeight))) - 1;
    const int totalTrackHeight = rulerHeight_ + visibleTrackCount_ * processor_.getTrackHeight();
    const int requiredHeight = std::max(totalTrackHeight, static_cast<int>(visibleBottomY));
    const int totalRows = (requiredHeight + TimelineCompositeCache::kWorldTileHeight - 1)
        / TimelineCompositeCache::kWorldTileHeight;
    const int effFirst = std::max(0, firstVertRow - 1);
    const int effLast = std::min(totalRows - 1, lastVertRow + 1);

    compositeCache_.prepare(sig, firstTimeTile, lastTimeTile, effFirst, effLast,
        [this](juce::Graphics& g, juce::Rectangle<int> b, TimelineCompositeCache::TileKey k) {
            buildCompositeTile(g, b, k);
        },
        [](juce::Graphics&, juce::Rectangle<int>, TimelineCompositeCache::TileKey) {
            // Foreground plane: no-op for Arrangement (单 plane)
        },
        false);
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

    if (contentMetrics_.revision == lastWaveformSyncRevision_
        && waveformMipmapCache_.isComplete())
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
                waveformMipmapCache_.setAudioSource(key, audioBuffer);
        }
    }

    waveformMipmapCache_.prune(alive);

    const bool progressed = waveformMipmapCache_.buildIncremental(timeBudgetMs);

    if (!progressed && waveformMipmapCache_.isComplete())
        lastWaveformSyncRevision_ = contentMetrics_.revision;

    return progressed;
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

    std::vector<MoveDragStartState> states;

    const auto addState = [&](int trackId, uint64_t placementId)
    {
        StandaloneArrangement::Placement placement;
        [[maybe_unused]] const bool found = getStandalonePlacementById(processor_, trackId, placementId, placement);
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
    const double rawStart = juce::jmax(0.0, state.startSeconds + deltaSeconds);
    const double bpm = lastContextBpm_ > 0.0 ? lastContextBpm_ : 120.0;
    const double snappedStart = SnapUtils::snapTime(rawStart, bpm, processor_.getSnapSettings());
    return {
        juce::jlimit(0, OpenTuneAudioProcessor::MAX_TRACKS - 1, state.trackId + trackDelta),
        snappedStart
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

    const PlacementKey primaryBefore{moveDragPrimaryStart_.trackId, moveDragPrimaryStart_.placementId};
    const PlacementKey primaryAfter{primaryAfterMove.trackId, primaryAfterMove.placementId};
    processor_.getUndoManager().addAction(
        std::make_unique<MultiMovePlacementAction>(processor_, std::move(undoEntries),
                                                     primaryBefore, primaryAfter));

    selectedPlacements_ = std::move(movedSelection);
    jassert(primaryAfterMove.trackId >= 0 && primaryAfterMove.placementId != 0);
    commitPlacementSelection(primaryAfterMove);

    moveDragStartStates_.clear();
    currentDragOp_ = DragOperation::None;
    isDraggingPlacement_ = false;

            listeners_.call([this](Listener& l) {
                l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
            });
            invalidateStableScene();
        }

void ArrangementViewComponent::drawSelectionOverlay(juce::Graphics& g)
{
    if (selectedPlacements_.empty())
        return;

    const auto viewport = getContentViewportBounds();
    const double pps = camera_.pixelsPerSecond;
    const int trackHeight = processor_.getTrackHeight();

    for (const auto& sel : selectedPlacements_) {
        StandaloneArrangement::Placement placement;
        if (!getStandalonePlacementById(processor_, sel.trackId, sel.placementId, placement))
            continue;

        const double placementStart = placement.timelineStartSeconds;
        const double placementEnd = placement.timelineEndSeconds();

        // Early out if completely off-screen
        if (placementEnd <= camera_.visibleStartSeconds
            || placementStart >= camera_.visibleStartSeconds + viewport.getWidth() / pps)
            continue;

        const int x = absoluteTimeToViewportX(placementStart);
        const int width = juce::jmax(8, static_cast<int>(std::llround(placement.durationSeconds * pps)));
        const int y = rulerHeight_ + sel.trackId * trackHeight - verticalScrollOffset_ + 2;
        const int height = trackHeight - 4;

        juce::Rectangle<float> bounds(static_cast<float>(x), static_cast<float>(y),
                                      static_cast<float>(width), static_cast<float>(height));

        // Skip if entirely outside clip
        if (bounds.getBottom() < static_cast<float>(rulerHeight_) || bounds.getY() > viewport.getBottom())
            continue;

        const auto themeId = UIColors::currentThemeId();
        if (themeId == ThemeId::Aurora) {
            g.setColour(juce::Colour(Aurora::Colors::Cyan).withAlpha(0.35f));
            g.fillRoundedRectangle(bounds, 6.0f);
            g.setColour(juce::Colour(Aurora::Colors::Cyan).withAlpha(0.85f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 2.0f);
        } else {
            g.setColour(UIColors::accent.withAlpha(0.12f));
            g.fillRoundedRectangle(bounds, 6.0f);
            g.setColour(UIColors::accent.withAlpha(0.65f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 1.5f);
        }
    }
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
                g.drawText(juce::String::fromUTF8(u8"+ 鏂板缓杞ㄩ亾"),
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
        const int width = juce::jmax(8, static_cast<int>(std::round(state.durationSeconds * camera_.pixelsPerSecond)));
        const auto lane = getTrackLaneBounds(target.trackId);
        const juce::Rectangle<float> bounds(static_cast<float>(x),
                                            static_cast<float>(lane.getY() + 2),
                                            static_cast<float>(width),
                                            static_cast<float>(lane.getHeight() - 4));

        StandaloneArrangement::Placement placement;
        [[maybe_unused]] const bool found = arrangement.getPlacementById(state.trackId, state.placementId, placement);
        jassert(found);

        const auto targetColour = arrangement.getTrackColour(target.trackId);

        const auto paintClip = bounds.getSmallestIntegerContainer()
            .getIntersection(getContentViewportBounds());
        if (paintClip.isEmpty())
            continue;

        ArrangementClipPaintInput clip{
            bounds,
            paintClip,
            targetColour,
            state.name,
            placement.contentKey,
            placement.gain,
            placement.fadeInDuration,
            placement.fadeOutDuration,
            isPlacementSelected(state.trackId, state.placementId),
            true,
            placement.clipInSeconds,
            target.startSeconds,
            state.durationSeconds,
            camera_.pixelsPerSecond
        };
        paintHistoricalClipShellAndWaveform(g, clip, waveformMipmapCache_);
        paintHistoricalClipTextFadeGain(g, clip);
    }
}

void ArrangementViewComponent::drawPlayhead(juce::Graphics& g)
{
    const auto viewportBounds = getContentViewportBounds();

    const int timeDerivedX = makeViewMapper().timeToX(playheadTimeForPaint_);
    const int viewportCentreX = viewportBounds.getCentreX();
    const int viewportRight = viewportBounds.getRight();
    const int viewLeftGuardX = viewportBounds.getX();

    const bool playing = playHeadState_.isPlaying.load(std::memory_order_relaxed);
    const bool continuousMode = scrollMode_ == ScrollMode::Continuous;

    const auto pres = TimelineViewportPolicy::computePlayheadPresentation(
        timeDerivedX, viewportCentreX, viewportRight, viewLeftGuardX,
        playing, continuousMode);

    if (!pres.visible)
        return;

    const float anchorX = static_cast<float>(pres.anchorX);
    const float height = static_cast<float>(getHeight());

    g.setColour(playheadColour_);
    g.drawLine(anchorX, 0.0f, anchorX, height, 2.0f);

    static const juce::Path kPlayheadTriangle = [] {
        juce::Path p;
        p.addTriangle(-6.0f, 0.0f, 6.0f, 0.0f, 0.0f, 6.0f);
        return p;
    }();
    g.fillPath(kPlayheadTriangle, juce::AffineTransform::translation(anchorX, 0.0f));
}

juce::Rectangle<int> ArrangementViewComponent::playheadDirtyRect() const
{
    const auto viewportBounds = getContentViewportBounds();
    const int timeDerivedX = makeViewMapper().timeToX(playheadTimeForPaint_);
    const auto presentation = TimelineViewportPolicy::computePlayheadPresentation(
        timeDerivedX,
        viewportBounds.getCentreX(),
        viewportBounds.getRight(),
        viewportBounds.getX(),
        playHeadState_.isPlaying.load(std::memory_order_relaxed),
        scrollMode_ == ScrollMode::Continuous);
    if (!presentation.visible)
        return {};

    const int anchorX = static_cast<int>(std::lround(presentation.anchorX));
    return juce::Rectangle<int>(anchorX - 6, 0, 14, getHeight())
        .getIntersection(getLocalBounds());
}

void ArrangementViewComponent::paint(juce::Graphics& g)
{
    if (themeBackdrop_.isValid())
        g.drawImageAt(themeBackdrop_, 0, 0, false);

    // Ruler + separator
    {
        const double ppsCanonical = std::round(camera_.pixelsPerSecond * 1000.0) / 1000.0;
        const int64_t surfaceOriginPx = static_cast<int64_t>(
            std::llround(camera_.visibleStartSeconds * ppsCanonical));
        RenderParams rulerParams;
        rulerParams.visibleStartSeconds = static_cast<double>(surfaceOriginPx) / ppsCanonical;
        rulerParams.visibleEndSeconds = rulerParams.visibleStartSeconds
            + getVisibleViewportWidth() / ppsCanonical;
        rulerParams.pixelsPerSecond = ppsCanonical;
        rulerParams.timeUnit = (timeUnit_ == TimeUnit::Bars) ? 1 : 0;
        rulerParams.tempo = static_cast<int>(lastContextBpm_ > 0.0 ? lastContextBpm_ : 120.0);
        rulerParams.themeId = static_cast<int>(UIColors::currentThemeId());
        rulerParams.pixelsPerSemitone = 0.0f;
        rulerParams.worldTopY = 0;
        rulerParams.rulerHeight = rulerHeight_;
        rulerParams.viewportWidth = getVisibleViewportWidth();
        rulerParams.viewportHeight = rulerHeight_;
        rulerParams.viewKind = "arrangement";

        juce::Graphics::ScopedSaveState rulerSt(g);
        g.addTransform(juce::AffineTransform::translation(
            static_cast<float>(kArrangementContentStartX), 0.0f));
        g.reduceClipRegion(0, 0, getVisibleViewportWidth(), rulerHeight_);
        TimelineLayerComposer::drawTimeRuler(g, rulerParams);

        // Separator line（drawTimeRuler 对 arrangement 跳过，需手动画）
        const auto style = TimelineLayerComposer::resolveRulerStyle("arrangement", UIColors::currentThemeId());
        g.setColour(style.separatorColour);
        g.drawLine(static_cast<float>(kArrangementContentStartX), static_cast<float>(rulerHeight_),
                   static_cast<float>(kArrangementContentStartX + getVisibleViewportWidth()),
                   static_cast<float>(rulerHeight_), style.tickStroke);
    }

    // Content surface
    {
        juce::Graphics::ScopedSaveState contentSave(g);
        const auto axis = getContentViewportBounds();
        g.reduceClipRegion(axis);
        if (viewportSurface_.isValid())
            g.drawImageAt(viewportSurface_, axis.getX(), axis.getY(), false);

        // Selection overlay
        {
            juce::Graphics::ScopedSaveState selSave(g);
            g.reduceClipRegion(axis);
            drawSelectionOverlay(g);
        }

        // Import/move overlays
        {
            juce::Graphics::ScopedSaveState ovSave(g);
            g.reduceClipRegion(axis);
            drawImportDropPreview(g);
            drawMoveDragOverlay(g);
        }
    }

    drawPlayhead(g);
}

void ArrangementViewComponent::onHeartbeatTick()
{
    if (!isShowing())
        return;

    const bool playingNow = playHeadState_.isPlaying.load(std::memory_order_relaxed);

    if (playingNow != lastObservedPlayHeadPlaying_) {
        if (playingNow) {
            preparePlaybackCoverage();
            // Stop→play edge: arm a one-shot transition so the viewport eases
            // from the current visible origin to the playhead-anchored target
            // instead of snapping. Normal playback afterwards never re-arms it.
            requestTransition_ = true;
        }

        playheadTimeForPaint_ = playHeadState_.timeInSeconds.load(std::memory_order_relaxed);
        lastObservedPlayHeadPlaying_ = playingNow;
        lastPlayheadRect_ = playheadDirtyRect();
        repaint();
    }

    const int64_t currentDpiMilli = static_cast<int64_t>(
        std::llround(getDesktopScaleFactor() * 1000.0));
    if (currentDpiMilli != lastDpiMilli_) {
        lastDpiMilli_ = currentDpiMilli;
        rebuildThemeBackdrop();
        invalidateStableScene();
    }

    const double currentBpm = processor_.getBpm();
    const int currentTimeSigNum = processor_.getTimeSigNumerator();
    const int currentTimeSigDenom = processor_.getTimeSigDenominator();
    if (std::abs(currentBpm - lastContextBpm_) > 0.001
        || currentTimeSigNum != lastContextTimeSigNum_
        || currentTimeSigDenom != lastContextTimeSigDenom_) {
        lastContextBpm_ = currentBpm;
        lastContextTimeSigNum_ = currentTimeSigNum;
        lastContextTimeSigDenom_ = currentTimeSigDenom;
        invalidateStableScene();
    }

    if (!playingNow) {
        const double currentPlayheadTime = readPlayheadSeconds();
        if (currentPlayheadTime != playheadTimeForPaint_) {
            const auto oldRect = lastPlayheadRect_;
            playheadTimeForPaint_ = currentPlayheadTime;
            const auto newRect = playheadDirtyRect();
            lastPlayheadRect_ = newRect;
            const auto dirty = newRect.getUnion(oldRect);
            if (!dirty.isEmpty())
                repaint(dirty);
        }
    }

    // 非播放期 waveform 构建限频：低频小预算
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
        ++waveformRevision_;
        if (playingNow) {
            waveformVisualRefreshPending_ = true;
        } else {
            waveformVisualRefreshPending_ = false;
            invalidateStableScene();
        }
    }

    if (!playingNow && waveformVisualRefreshPending_) {
        waveformVisualRefreshPending_ = false;
        invalidateStableScene();
    }

    // 播放中维护有界覆盖窗口（与PianoRoll一致）
    if (playingNow) {
        const int w = getVisibleViewportWidth();
        if (w > 0) {
            const double p = camera_.pixelsPerSecond;
            const double tileDur = static_cast<double>(TimelineCompositeCache::kTileWidthPx) / p;
            const double viewportDur = static_cast<double>(w) / p;
            const double playhead = readPlayheadSeconds();
            constexpr int kMaxAheadTiles = 16;
            constexpr int kMaxBehindTiles = 4;
            const double ahead = std::min(6.0 * viewportDur, kMaxAheadTiles * tileDur);
            const double behind = kMaxBehindTiles * tileDur;
            if (playhead + ahead > tileCoverageEndSeconds_
                || playhead - behind > tileCoverageStartSeconds_ + behind) {
                tileCoverageStartSeconds_ = std::max(0.0,
                    std::floor((playhead - behind) / tileDur) * tileDur);
                const double camEnd = camera_.visibleStartSeconds + viewportDur;
                tileCoverageEndSeconds_ = std::ceil(
                    std::max(camEnd + ahead, playhead + ahead) / tileDur) * tileDur;
                prepareCoverageCompositeTilesNew();
            }
        }
    }
}

void ArrangementViewComponent::onScrollVBlankCallback(double timestampSec)
{
    if (!isShowing() || !playHeadState_.isPlaying.load(std::memory_order_relaxed))
        return;

    const double playheadTime = readPlayheadSeconds();
    const auto oldPlayheadRect = lastPlayheadRect_;
    const int64_t oldOriginPx = surfaceOriginPx_;
    const double oldPps = surfacePps_;

    TimelineViewportRequest::Kind kind = (scrollMode_ == ScrollMode::Continuous)
        ? TimelineViewportRequest::Kind::Cont
        : TimelineViewportRequest::Kind::Page;

    auto req = makeViewportRequest(kind, playheadTime, 0.0, camera_.pixelsPerSecond);
    TimelineViewportCamera targetCamera = TimelineViewportPolicy::resolve(req);

    // Page mode: resolvedCamera == targetCamera (direct assign below).
    // Continuous follow: camera tracks the policy target directly. An ease-out
    // transition is started only on an explicit requestTransition_ arm (user
    // ruler/empty seek, playhead drag, or the stop→play edge). Normal playback
    // never arms it, so the camera follows the target with no subpixel
    // threshold that would re-trigger a transition every VBlank at high pps.
    // The animation is driven by timestampSec (not a per-frame low-pass) so
    // steady-state playback tracks the target with no offset. The half-pixel
    // snap avoids perpetual animation causing raster instability. Arrangement
    // keeps its existing follow semantics; no user-scroll suppression state.
    TimelineViewportCamera resolvedCamera = targetCamera;
    const bool continuousFollow = (scrollMode_ == ScrollMode::Continuous);
    if (continuousFollow) {
        if (transitionActive_) {
            const double elapsed = timestampSec - transitionStartTimestamp_;
            const double progress = std::clamp(elapsed / kContinuousTransitionDurationSec, 0.0, 1.0);
            const double eased = progress * (2.0 - progress);
            resolvedCamera.visibleStartSeconds = transitionStartVisibleSeconds_
                + (targetCamera.visibleStartSeconds - transitionStartVisibleSeconds_) * eased;
            const double remainingPx = std::abs(
                targetCamera.visibleStartSeconds - resolvedCamera.visibleStartSeconds)
                * targetCamera.pixelsPerSecond;
            if (progress >= 1.0 || remainingPx <= 0.5) {
                resolvedCamera.visibleStartSeconds = targetCamera.visibleStartSeconds;
                transitionActive_ = false;
            }
        } else if (requestTransition_) {
            transitionActive_ = true;
            transitionStartTimestamp_ = timestampSec;
            transitionStartVisibleSeconds_ = camera_.visibleStartSeconds;
            resolvedCamera.visibleStartSeconds = camera_.visibleStartSeconds;
        }
    }
    requestTransition_ = false;

    camera_ = resolvedCamera;
    playheadTimeForPaint_ = playheadTime;
    const auto newPlayheadRect = playheadDirtyRect();
    const int64_t newOriginPx = static_cast<int64_t>(
        std::llround(resolvedCamera.visibleStartSeconds * resolvedCamera.pixelsPerSecond));
    const bool ppsChanged = static_cast<int64_t>(std::llround(oldPps * 1000.0))
        != static_cast<int64_t>(std::llround(resolvedCamera.pixelsPerSecond * 1000.0));
    const bool cameraRasterChanged = !viewportSurface_.isValid()
        || ppsChanged
        || oldOriginPx != newOriginPx;

    if (cameraRasterChanged) {
        const double tileDuration = TimelineCompositeCache::kTileWidthPx / resolvedCamera.pixelsPerSecond;
        const int64_t firstTimeTile = std::max<int64_t>(0,
            static_cast<int64_t>(std::floor(resolvedCamera.visibleStartSeconds / tileDuration)));
        const int64_t lastTimeTile = static_cast<int64_t>(std::floor(
            (resolvedCamera.visibleStartSeconds + getVisibleViewportWidth() / resolvedCamera.pixelsPerSecond)
            / tileDuration));

        if (!viewportSurface_.isValid() || ppsChanged
            || std::llabs(newOriginPx - oldOriginPx) >= getContentViewportBounds().getWidth()) {
            surfaceRebuildFromReadyTiles(firstTimeTile, lastTimeTile);
        } else {
            surfaceScrollAndFillExposed(newOriginPx, firstTimeTile, lastTimeTile);
        }

        lastPlayheadRect_ = newPlayheadRect;
        repaint(timeAxisRect());
        return;
    }

    lastPlayheadRect_ = newPlayheadRect;
    const auto dirty = newPlayheadRect.getUnion(oldPlayheadRect);
    if (!dirty.isEmpty() && newPlayheadRect != oldPlayheadRect)
        repaint(dirty);
}

double ArrangementViewComponent::readPlayheadSeconds() const
{
    // Canonical time comes directly from processor-owned PlayHeadState; no
    // fallback to 0.0 and no second source of truth.
    return playHeadState_.timeInSeconds.load(std::memory_order_relaxed);
}

void ArrangementViewComponent::mouseMove(const juce::MouseEvent& e)
{
    mouseOverReferenceButton_ = false;

    auto moveHit = hitTestPlacement(e.getPosition());
    if (moveHit.trackId >= 0 && moveHit.placementIndex >= 0)
    {
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

    auto hit = hitTestPlacement(e.getPosition());

    // Ctrl+drag cursor preview 鈥?only on empty area, consistent with mouseDown
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

    // Ctrl+drag panning 鈥?only on empty area (not on a placement, to avoid
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

    // Hit-test placement before seek 鈥?reference button intercepts without seeking
    auto hit = hitTestPlacement(e.getPosition());

    // Check reference button click (bottom-right corner) 鈥?always active, before seek
    if (experimentalReferenceControlsEnabled_ && hit.trackId >= 0 && hit.placementBounds.getWidth() > 30)
    {
        juce::Rectangle<int> refBtnArea(hit.placementBounds.getRight() - 20,
                                         hit.placementBounds.getBottom() - 20, 20, 20);
        if (refBtnArea.contains(e.getPosition()))
        {
            // Select this placement so downstream context is correct
            const uint64_t hitPlacementId = processor_.getPlacementId(hit.trackId, hit.placementIndex);
            selectedPlacements_.clear();
            selectedPlacements_.insert(PlacementSelectionKey{hit.trackId, hitPlacementId});
            commitPlacementSelection(PlacementSelectionKey{hit.trackId, hitPlacementId});

            // Compute reference button screen area for popup menu positioning
            auto refBtnScreenArea = localAreaToGlobal(refBtnArea.toFloat()).toNearestInt();
            listeners_.call([&](Listener& l) {
                l.referenceButtonClicked(hit.trackId, hitPlacementId, refBtnScreenArea);
            });
            return;
        }
    }

    // Clicked on placement body/edge/handle 鈥?do NOT seek playhead, only select/drag
    if (hit.trackId >= 0)
    {
        // Fall through to placement selection/drag logic below
    }
    else if (e.y <= rulerHeight_)
    {
        // Clicked on ruler — emit seek request; the editor decides whether to
        // drive processor.setPosition (Standalone) or ARA requestSetPlaybackPosition.
        const double newPosSeconds = juce::jmax(0.0, viewportXToAbsoluteTime(e.x));
        listeners_.call([newPosSeconds](Listener& l) { l.playheadPositionChangeRequested(newPosSeconds); });
        playheadTimeForPaint_ = newPosSeconds;
        requestTransition_ = true;
        repaint();
        isDraggingPlayhead_ = true;
        dragStartPos_ = e.getPosition();
        return;
    }
    else
    {
        // Clicked on empty area — emit seek request and clear selection
        const double newPosSeconds = juce::jmax(0.0, viewportXToAbsoluteTime(e.x));
        listeners_.call([newPosSeconds](Listener& l) { l.playheadPositionChangeRequested(newPosSeconds); });
        playheadTimeForPaint_ = newPosSeconds;
        requestTransition_ = true;
        repaint();

        if (!e.mods.isCtrlDown() && !e.mods.isShiftDown())
        {
            commitEmptyPlacementSelection();
        }
        else
        {
            repaint();
            }
        return;
    }

    // --- Placement hit: selection and drag logic (no playhead seek) ---

    const uint64_t hitPlacementId = processor_.getPlacementId(hit.trackId, hit.placementIndex);

    if (e.mods.isCtrlDown() && !e.mods.isShiftDown())
    {
        togglePlacementSelection(hit.trackId, hitPlacementId);

        PlacementSelectionKey primary;
        if (isPlacementSelected(hit.trackId, hitPlacementId))
            primary = PlacementSelectionKey{hit.trackId, hitPlacementId};
        else if (!selectedPlacements_.empty())
            primary = *selectedPlacements_.begin();
        else
        {
            commitEmptyPlacementSelection();
            return;
        }
        commitPlacementSelection(primary);
        return;
    }

    if (e.mods.isShiftDown() && hasShiftAnchor_)
    {
        PlacementSelectionKey toKey{hit.trackId, hitPlacementId};
        selectPlacementsInRange(shiftAnchor_, toKey);
        commitPlacementSelection(toKey);
        return;
    }

    if (!e.mods.isCtrlDown() && !e.mods.isShiftDown())
    {
        if (!isPlacementSelected(hit.trackId, hitPlacementId))
        {
            clearPlacementSelection();
        }
        // Always ensure clicked placement is in the selection
        if (!isPlacementSelected(hit.trackId, hitPlacementId))
        {
            selectedPlacements_.insert(PlacementSelectionKey{hit.trackId, hitPlacementId});
        }
        shiftAnchor_ = PlacementSelectionKey{hit.trackId, hitPlacementId};
        hasShiftAnchor_ = true;
    }

    // Commit primary selection (all branches converge here)
    commitPlacementSelection(PlacementSelectionKey{hit.trackId, hitPlacementId});

    dragStartPos_ = e.getPosition();
    getStandalonePlacementStartSeconds(processor_, selectedTrack_, selectedPlacementId_, dragStartPlacementSeconds_);
    getStandalonePlacementGain(processor_, selectedTrack_, selectedPlacementId_, dragStartPlacementGain_);
    dragStartPlacementId_ = selectedPlacementId_;
    dragStartTrackId_ = selectedTrack_;

    // Check Fade handles first (before trim 鈥?to give priority to 16x16 fade handle areas
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
        return;
    }

    // Check Trim edges (after fade 鈥?8px edge zones should not steal hits from 16x16 fade handles)
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
        return;
    }

    currentDragOp_ = hit.isTopEdge ? DragOperation::Gain : DragOperation::Move;
    isAdjustingGain_ = hit.isTopEdge;
    isDraggingPlacement_ = !isAdjustingGain_;

    if (currentDragOp_ == DragOperation::Move)
    {
        beginMoveDrag(hit, e.getPosition());
        return;
    }

    clearMoveDragOverlay();
    }

void ArrangementViewComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (isPanning_)
    {
        auto delta = e.getPosition() - lastMousePos_;
        
        // Horizontal Scroll 鈥?pan via camera
        const double deltaTime = static_cast<double>(-delta.x) / camera_.pixelsPerSecond;
        const auto req = makeViewportRequest(
            TimelineViewportRequest::Kind::Manual,
            camera_.visibleStartSeconds + deltaTime,
            0.0,
            camera_.pixelsPerSecond);
        commitViewportRequest(req);

        // Vertical Scroll 鈥?use setVerticalScrollOffset() to properly update tile cache
        const int newVerticalOffset = verticalScrollOffset_ - delta.y;
        setVerticalScrollOffset(newVerticalOffset);
        listeners_.call([this](Listener& l) { l.verticalScrollChanged(verticalScrollOffset_); });
        
        lastMousePos_ = e.getPosition();
        return;
    }

    if (isDraggingPlayhead_)
    {
        const double newPosSeconds = juce::jmax(0.0, viewportXToAbsoluteTime(e.x));
        listeners_.call([newPosSeconds](Listener& l) { l.playheadPositionChangeRequested(newPosSeconds); });
        playheadTimeForPaint_ = newPosSeconds;
        requestTransition_ = true;
        repaint();
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

    if (isDraggingPlacement_)
    {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        updateMoveDragOverlay(e);
        return;
    }
    else if (isAdjustingGain_)
    {
        auto delta = e.getPosition() - dragStartPos_;
        double factor = std::pow(10.0, (-static_cast<double>(delta.y)) / 200.0);
        setStandalonePlacementGain(processor_, selectedTrack_, selectedPlacementId_, static_cast<float>(dragStartPlacementGain_ * factor));

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

    // Reset drag op for non-move/gain operations (skip plain clicks — None — to avoid invalidating stable scene)
    if (currentDragOp_ != DragOperation::None && currentDragOp_ != DragOperation::Move && currentDragOp_ != DragOperation::Gain) {
        currentDragOp_ = DragOperation::None;
        dragOperationPlacementId_ = 0;
        invalidateStableScene();
        }

    if (isDraggingPlacement_ && currentDragOp_ == DragOperation::Move)
    {
        const auto delta = e.getPosition() - dragStartPos_;
        if (delta.getDistanceFromOrigin() > kPlacementDragThresholdPx) {
            finishMoveDrag(e);  // finishMoveDrag already calls invalidateStableScene
        }
        // else: click only, fall through to cleanup below
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
            invalidateStableScene();
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
    
    // Shift + Wheel = Vertical Zoom (Track Height) - 涓嶵rackPanel鍚屾
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
                invalidateStableScene();
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
                userHasManuallyZoomed_ = true;
                const auto req = makeViewportRequest(
                    TimelineViewportRequest::Kind::Zoom,
                    mouseTime,
                    static_cast<double>(e.x - kArrangementContentStartX),
                    newPps);
                commitViewportRequest(req);
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
            const auto req = makeViewportRequest(
                TimelineViewportRequest::Kind::Manual,
                camera_.visibleStartSeconds + deltaTime,
                0.0,
                camera_.pixelsPerSecond);
            commitViewportRequest(req);
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
        const auto req = makeViewportRequest(
            TimelineViewportRequest::Kind::Manual,
            camera_.visibleStartSeconds + deltaTime,
            0.0,
            camera_.pixelsPerSecond);
        commitViewportRequest(req);
    }
}

bool ArrangementViewComponent::keyPressed(const juce::KeyPress& key)
{
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::PlayPause, key))
    {
        // Emit play/pause toggle request; editor drives the actual transport.
        listeners_.call([](Listener& l) { l.playPauseToggleRequested(); });
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::SelectAll, key))
    {
        selectAllPlacementsInTrack(selectedTrack_);
        if (!selectedPlacements_.empty()) {
            const auto& firstKey = *selectedPlacements_.begin();
            hasShiftAnchor_ = true;
            shiftAnchor_ = firstKey;
            commitPlacementSelection(firstKey);
        } else {
            commitEmptyPlacementSelection();
        }
        return true;
    }

    // CopyClips 鈥?Ctrl+C
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

    // PasteClips 鈥?Ctrl+V (at playhead)
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Paste, key))
    {
        auto& clipboard = processor_.getClipClipboard();
        if (clipboard.hasEntries())
        {
            double pasteTime = playHeadState_.timeInSeconds.load(std::memory_order_relaxed);
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
                    // Rollback 鈥?delete the orphan content
                    processor_.getStandaloneContentRepository()->retireClip(newContentKey);
                    continue;
                }
                pasteTime += entry.durationSeconds; // chain placements sequentially
            }

            invalidateStableScene();
            }
        return true;
    }

    // DuplicateClip 鈥?Ctrl+D
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
                            // Rollback 鈥?delete the orphan content
                            processor_.getStandaloneContentRepository()->retireClip(newContentKey);
                            return true;
                        }

                        const int dupTrack = selectedTrack_;
                        clearPlacementSelection();
                        selectedPlacements_.insert(PlacementSelectionKey{dupTrack, dup.placementId});
                        hasShiftAnchor_ = true;
                        shiftAnchor_ = PlacementSelectionKey{dupTrack, dup.placementId};
                        commitPlacementSelection(PlacementSelectionKey{dupTrack, dup.placementId});

                        listeners_.call([this](Listener& l) {
                            l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
                        });
                        invalidateStableScene();
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

        double splitSeconds = playHeadState_.timeInSeconds.load(std::memory_order_relaxed);
        bool anySplit = false;

        // Copy selected placements to a vector to avoid iterator invalidation during split
        std::vector<std::pair<int, uint64_t>> toSplit;
        std::vector<SplitOutcome> outcomes;
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
                outcomes.push_back(*splitOutcome);
                anySplit = true;
            }
        }

        if (anySplit)
        {
            // Rebuild selection from split outcomes: each split's trailing placement
            selectedPlacements_.clear();
            PlacementSelectionKey primary{-1, 0};
            for (const auto& outcome : outcomes)
            {
                if (outcome.trailingPlacementId != 0)
                {
                    selectedPlacements_.insert(PlacementSelectionKey{outcome.trackId, outcome.trailingPlacementId});
                    primary = PlacementSelectionKey{outcome.trackId, outcome.trailingPlacementId};
                }
            }
            if (primary.trackId >= 0 && primary.placementId != 0)
                commitPlacementSelection(primary);
            else
                commitEmptyPlacementSelection();

            listeners_.call([this](Listener& l) {
                l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
            });
            invalidateStableScene();
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

            selectedPlacements_.clear();
            if (mergeOutcome->mergedPlacementId != 0) {
                selectedPlacements_.insert(PlacementSelectionKey{selectedTrack_, mergeOutcome->mergedPlacementId});
                commitPlacementSelection(PlacementSelectionKey{selectedTrack_, mergeOutcome->mergedPlacementId});
            } else {
                commitEmptyPlacementSelection();
            }
            listeners_.call([this](Listener& l) {
                l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
            });
            invalidateStableScene();
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
            invalidateStableScene();
            commitEmptyPlacementSelection();
        }
        return true;
    }

    // Nudge Left 鈥?move selected placements earlier by 10ms
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
            const PlacementKey nudgeKey{selectedTrack_, selectedPlacementId_};
            processor_.getUndoManager().addAction(
                std::make_unique<MultiMovePlacementAction>(processor_, std::move(movedEntries),
                                                             nudgeKey, nudgeKey));
            listeners_.call([this](Listener& l) {
                l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
            });
            invalidateStableScene();
            }
        return true;
    }

    // Nudge Right 鈥?move selected placements later by 10ms
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
            const PlacementKey nudgeKey{selectedTrack_, selectedPlacementId_};
            processor_.getUndoManager().addAction(
                std::make_unique<MultiMovePlacementAction>(processor_, std::move(movedEntries),
                                                             nudgeKey, nudgeKey));
            listeners_.call([this](Listener& l) {
                l.placementTimingChanged(selectedTrack_, selectedPlacementIndex_);
            });
            invalidateStableScene();
            }
        return true;
    }

    // ToggleSnap 鈥?Ctrl+Shift+S
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
// 澶氶€夊疄鐜?
// ============================================================================

bool ArrangementViewComponent::isPlacementSelected(int trackId, uint64_t placementId) const
{
    return selectedPlacements_.count(PlacementSelectionKey{trackId, placementId}) > 0;
}

void ArrangementViewComponent::togglePlacementSelection(int trackId, uint64_t placementId)
{
    PlacementSelectionKey key{trackId, placementId};
    auto it = selectedPlacements_.find(key);
    if (it != selectedPlacements_.end())
    {
        selectedPlacements_.erase(it);
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
}

void ArrangementViewComponent::commitPlacementSelection(PlacementSelectionKey primary)
{
    selectedTrack_ = primary.trackId;
    selectedPlacementId_ = primary.placementId;
    selectedPlacementIndex_ = processor_.findPlacementIndexById(primary.trackId, primary.placementId);

    if (auto* arrangement = processor_.getStandaloneArrangement())
        arrangement->selectPlacement(selectedTrack_, selectedPlacementId_);

    listeners_.call([this](Listener& l) {
        l.placementSelectionChanged(selectedTrack_, selectedPlacementId_);
    });

    // Selection is transient overlay — just repaint, don't rebuild tiles
    repaint();
    }

void ArrangementViewComponent::commitEmptyPlacementSelection()
{
    selectedPlacements_.clear();
    selectedTrack_ = -1;
    selectedPlacementIndex_ = -1;
    selectedPlacementId_ = 0;
    hasShiftAnchor_ = false;

    if (auto* arrangement = processor_.getStandaloneArrangement())
        arrangement->clearAllSelections();

    listeners_.call([this](Listener& l) {
        l.placementSelectionChanged(selectedTrack_, selectedPlacementId_);
    });
    // Selection is transient overlay — just repaint, don't rebuild tiles
    repaint();
    }

// ============================================================================
// Analysis animation 鐘舵€佺鐞?
// ============================================================================

void ArrangementViewComponent::setClipAnalysisInProgress(uint64_t placementId, bool inProgress)
{
    auto& state = clipAnalysisStates_[placementId];
    if (state.isAnalysisInProgress != inProgress)
    {
        state.isAnalysisInProgress = inProgress;
        rebuildTimelineCoverage();
        repaint();
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

} // namespace OpenTune


