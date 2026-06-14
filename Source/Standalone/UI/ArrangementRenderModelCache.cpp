#include "ArrangementRenderModelCache.h"
#include "../../Utils/TrackConstants.h"

#include <cmath>

namespace OpenTune {

namespace {

int64_t timeToMs(double seconds) noexcept
{
    return static_cast<int64_t>(std::llround(seconds * 1000.0));
}

uint64_t hashCombine(uint64_t seed, uint64_t value) noexcept
{
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
}

} // namespace

ArrangementRenderModelCache::Key
ArrangementRenderModelCache::makeKey(OpenTuneAudioProcessor& processor,
                                      const TimelineViewportState& viewport,
                                      int selectedTrack,
                                      int selectedPlacementIndex,
                                      uint64_t hoveredPlacementId,
                                      bool mouseOverReferenceButton,
                                      double bandStartSeconds,
                                      double bandEndSeconds,
                                      int bandStartContentX,
                                      int bandWidthPx,
                                      int trackHeight,
                                      const MoveDragPreviewState& movePreview)
{
    Key key;
    key.bandTimeStartMs = timeToMs(bandStartSeconds);
    key.bandTimeEndMs = timeToMs(bandEndSeconds);
    key.bandStartContentX = bandStartContentX;
    key.bandWidthPx = bandWidthPx;
    key.viewportWidthPx = viewport.viewportWidthPx;
    key.viewportHeightPx = viewport.viewportHeightPx;
    key.zoomBucket = static_cast<int>(std::llround(viewport.zoomLevel * 100.0));
    key.trackHeight = trackHeight;
    key.selectedTrack = selectedTrack;
    key.selectedPlacementIndex = selectedPlacementIndex;
    key.hoveredPlacementId = hoveredPlacementId;
    key.mouseOverReferenceButton = mouseOverReferenceButton;
    key.previewRevision = movePreview.active ? movePreview.revision : 0;

    const auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement != nullptr) {
        uint64_t revision = 1469598103934665603ull;
        for (int trackId = 0; trackId < OpenTuneAudioProcessor::MAX_TRACKS; ++trackId) {
            const int count = arrangement->getNumPlacements(trackId);
            revision = hashCombine(revision, static_cast<uint64_t>(trackId + 1));
            revision = hashCombine(revision, static_cast<uint64_t>(count + 1));

            for (int i = 0; i < count; ++i) {
                StandaloneArrangement::Placement placement;
                if (!arrangement->getPlacementByIndex(trackId, i, placement))
                    continue;

                revision = hashCombine(revision, placement.placementId);
                revision = hashCombine(revision, placement.contentKey.objectId);
                revision = hashCombine(revision, placement.referencePlacementId);
                revision = hashCombine(revision, static_cast<uint64_t>(timeToMs(placement.timelineStartSeconds)));
                revision = hashCombine(revision, static_cast<uint64_t>(timeToMs(placement.durationSeconds)));
                revision = hashCombine(revision, static_cast<uint64_t>(timeToMs(placement.clipInSeconds)));
                revision = hashCombine(revision, static_cast<uint64_t>(std::llround(placement.gain * 1000.0f)));
            }

            // Include track colour so colour changes invalidate the cache
            revision = hashCombine(revision, static_cast<uint64_t>(arrangement->getTrackColour(trackId).getARGB()));
        }

        key.arrangementRevision = revision;
        key.snapshotRevision = revision;
    }

    return key;
}

juce::Rectangle<int>
ArrangementRenderModelCache::computeWaveformDrawableBounds(juce::Rectangle<int> placementBounds) noexcept
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

juce::Path ArrangementRenderModelCache::buildWaveformPathForPlacement(const WaveformMipmap& mipmap,
                                                                      const TimelineViewportState& viewport,
                                                                      juce::Rectangle<int> placementBounds,
                                                                      double timelineStartSeconds,
                                                                      double durationSeconds,
                                                                      double clipInSeconds,
                                                                      float gain)
{
    juce::Path path;
    if (placementBounds.isEmpty() || durationSeconds <= 0.0)
        return path;

    const auto waveformBounds = computeWaveformDrawableBounds(placementBounds);
    if (waveformBounds.isEmpty())
        return path;

    const int levelIndex = mipmap.selectBestLevelIndex(viewport.pixelsPerSecond());
    const auto& level = mipmap.getLevel(levelIndex);
    if (level.peaks.empty())
        return path;

    const int64_t numPeaks = static_cast<int64_t>(level.peaks.size());
    const int64_t builtPeaks = level.complete ? numPeaks : level.buildProgress;
    if (builtPeaks <= 0)
        return path;

    const float midY = static_cast<float>(waveformBounds.getCentreY());
    const float halfH = waveformBounds.getHeight() * 0.45f;
    const int samplesPerPeak = WaveformMipmap::kSamplesPerPeak[levelIndex];
    const double timePerPeak = static_cast<double>(samplesPerPeak) / WaveformMipmap::kBaseSampleRate;
    const double visibleTimeStart = viewport.visibleTimeStart();
    const double visibleTimeEnd = viewport.visibleTimeEnd();
    const double timelineEndSeconds = timelineStartSeconds + durationSeconds;
    const double sourceEndSeconds = clipInSeconds + durationSeconds;
    if (timelineEndSeconds <= visibleTimeStart || timelineStartSeconds >= visibleTimeEnd)
        return path;

    for (int x = waveformBounds.getX(); x < waveformBounds.getRight(); ++x) {
        const double timelineTime = viewport.viewportXToTime(x);
        if (timelineTime < visibleTimeStart || timelineTime >= visibleTimeEnd)
            continue;

        if (timelineTime < timelineStartSeconds || timelineTime >= timelineEndSeconds)
            continue;

        const double contentTime = clipInSeconds + (timelineTime - timelineStartSeconds);
        if (contentTime < clipInSeconds || contentTime >= sourceEndSeconds)
            continue;

        const int64_t peakIndex = static_cast<int64_t>(contentTime / timePerPeak);
        if (peakIndex < 0 || peakIndex >= builtPeaks)
            continue;

        // Aggregate all peaks covered by this pixel
        const double timelineTimeNext = viewport.viewportXToTime(x + 1);
        const double matTimeNext = clipInSeconds + (timelineTimeNext - timelineStartSeconds);
        int64_t idxStart = peakIndex;
        int64_t idxEnd = static_cast<int64_t>(matTimeNext / timePerPeak);
        if (idxEnd <= idxStart)
            idxEnd = idxStart + 1;

        float aggMin = 0.0f;
        float aggMax = 0.0f;
        bool hasData = false;

        for (int64_t i = idxStart; i < idxEnd && i < builtPeaks; ++i)
        {
            if (i < 0) continue;
            const auto& pk = level.peaks[static_cast<std::size_t>(i)];
            if (pk.isZero()) continue;
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

        const float displayTop = aggMax * gain * halfH;
        const float displayBottom = aggMin * gain * halfH;
        float y1 = midY - displayTop;
        float y2 = midY - displayBottom;

        // Ensure minimum display height for non-silent peaks
        if ((y2 - y1) < 2.0f)
        {
            const float expand = (2.0f - (y2 - y1)) * 0.5f;
            y1 -= expand;
            y2 += expand;
        }

        const float fx = static_cast<float>(x) + 0.5f;
        path.startNewSubPath(fx, y1);
        path.lineTo(fx, y2);
    }

    return path;
}

const ArrangementRenderModelCache::RenderModel&
ArrangementRenderModelCache::update(OpenTuneAudioProcessor& processor,
                                    const TimelineViewportState& viewport,
                                    int selectedTrack,
                                    int selectedPlacementIndex,
                                    std::function<bool(int, uint64_t)> isPlacementSelected,
                                    std::function<bool(uint64_t)> getAnalysisState,
                                    uint64_t hoveredPlacementId,
                                    bool mouseOverReferenceButton,
                                    WaveformMipmapCache& mipmapCache,
                                    double bandStartSeconds,
                                    double bandEndSeconds,
                                    int bandStartContentX,
                                    int bandWidthPx,
                                    int trackHeight,
                                    const MoveDragPreviewState& movePreview,
                                    bool forceRebuild)
{
    const auto nextKey = makeKey(processor,
                                 viewport,
                                 selectedTrack,
                                 selectedPlacementIndex,
                                 hoveredPlacementId,
                                 mouseOverReferenceButton,
                                 bandStartSeconds,
                                 bandEndSeconds,
                                 bandStartContentX,
                                 bandWidthPx,
                                 trackHeight,
                                 movePreview);

    if (!needsRebuild_ && !forceRebuild && currentKey_ == nextKey)
        return model_;

    currentKey_ = nextKey;
    model_.lanes.clear();
    model_.placements.clear();
    model_.generation = ++modelGeneration_;
    model_.bpm = processor.getBpm();
    model_.timeSigNumerator = processor.getTimeSigNumerator();
    model_.timeSigDenominator = processor.getTimeSigDenominator();
    model_.bandStartContentX = bandStartContentX;
    model_.bandWidthPx = bandWidthPx;
    model_.viewportHeightPx = viewport.viewportHeightPx;

    const auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr) {
        model_.totalTrackCount = 0;
        needsRebuild_ = false;
        return model_;
    }

    constexpr int kMaxTracks = OpenTuneAudioProcessor::MAX_TRACKS;

    const double visibleTimeStart = bandStartSeconds;
    const double visibleTimeEnd = bandEndSeconds;

    auto findPreviewForPlacement = [&movePreview](int trackId, uint64_t placementId)
        -> const MoveDragPreviewPlacement*
    {
        if (!movePreview.active)
            return nullptr;

        for (const auto& preview : movePreview.placements) {
            if (preview.sourceTrackId == trackId && preview.placementId == placementId)
                return &preview;
        }

        return nullptr;
    };

    auto appendVisiblePlacement = [&](int sourceTrackId,
                                      int displayTrackId,
                                      const StandaloneArrangement::Placement& placement,
                                      int placementIndex,
                                      double timelineStartSeconds,
                                      bool isPreview)
    {
        const double timelineEndSeconds = timelineStartSeconds + placement.durationSeconds;
        if (timelineEndSeconds <= visibleTimeStart || timelineStartSeconds >= visibleTimeEnd)
            return;

        if (placement.durationSeconds <= 0.0)
            return;

        auto laneBounds = juce::Rectangle<int>(0, displayTrackId * trackHeight, bandWidthPx, trackHeight);
        auto lane = laneBounds.reduced(kClipShellInsetX, kClipShellInsetY);
        const int x1 = viewport.timeToContentX(timelineStartSeconds) - bandStartContentX;
        const int x2 = viewport.timeToContentX(timelineEndSeconds) - bandStartContentX;
        const int width = juce::jmax(8, x2 - x1);
        juce::Rectangle<int> placementBounds{x1, lane.getY(), width, lane.getHeight()};

        if (placementBounds.isEmpty())
            return;

        const uint64_t placementId = placement.placementId;

        bool isSelected = (sourceTrackId == selectedTrack && placementIndex == selectedPlacementIndex)
                       || (isPlacementSelected && isPlacementSelected(sourceTrackId, placementId));

        VisiblePlacement vp;
        vp.placementId = placementId;
        vp.contentKey = placement.contentKey;
        vp.referencePlacementId = placement.referencePlacementId;
        vp.pixelBounds = placementBounds;
        vp.pixelArea = placementBounds.toFloat();
        vp.contentBounds = juce::Rectangle<int>(viewport.timeToContentX(timelineStartSeconds),
                                                placementBounds.getY(),
                                                width,
                                                placementBounds.getHeight());
        vp.isSelected = isSelected;
        vp.gain = placement.gain;
        vp.name = placement.name;
        vp.isHovered = (hoveredPlacementId == placementId);
        vp.mouseOverReferenceButton = (hoveredPlacementId == placementId) && mouseOverReferenceButton;
        vp.trackId = displayTrackId;
        vp.fadeInDuration = placement.fadeInDuration;
        vp.fadeOutDuration = placement.fadeOutDuration;
        vp.isPreview = isPreview;
        vp.colour = arrangement->getTrackColour(displayTrackId);
        vp.timelineStartSeconds = timelineStartSeconds;
        vp.durationSeconds = placement.durationSeconds;
        vp.clipInSeconds = placement.clipInSeconds;

        if (getAnalysisState)
            vp.analysisInProgress = getAnalysisState(placementId);

        auto snap = processor.getContentSnapshot(placement.contentKey);
        auto audioBuffer = snap ? snap->audioBuffer : nullptr;
        vp.hasAudioBuffer = (audioBuffer != nullptr);

        if (audioBuffer != nullptr) {
            auto& mipmap = mipmapCache.getOrCreate(placement.contentKey);
            mipmap.setAudioSource(audioBuffer);
            auto bandViewport = viewport;
            bandViewport.scrollOffsetPx = bandStartContentX;
            bandViewport.viewportWidthPx = bandWidthPx;
            bandViewport.contentStartX = 0;
            vp.waveformPath = buildWaveformPathForPlacement(mipmap,
                                                            bandViewport,
                                                            placementBounds,
                                                            timelineStartSeconds,
                                                            placement.durationSeconds,
                                                            placement.clipInSeconds,
                                                            placement.gain);
        }

        model_.placements.push_back(std::move(vp));
    };

    for (int trackId = 0; trackId < kMaxTracks; ++trackId)
    {
        // Lanes use component-space Y (consumed in paint() directly)
        auto laneBounds = juce::Rectangle<int>(0, kTrackLaneTopOffset + trackId * trackHeight,
                                                   bandWidthPx, trackHeight);

        if (laneBounds.getY() <= viewport.viewportHeightPx + kTrackLaneTopOffset) {
            RenderModel::VisibleLane lane;
            lane.trackId = trackId;
            lane.area = laneBounds.toFloat();
            lane.area.setX(0.0f);
            lane.area.setWidth(static_cast<float>(viewport.viewportWidthPx));
            lane.selected = trackId == selectedTrack;
            model_.lanes.push_back(lane);
        }

        const int placementCount = arrangement->getNumPlacements(trackId);
        for (int i = 0; i < placementCount; ++i)
        {
            StandaloneArrangement::Placement placement;
            if (!arrangement->getPlacementByIndex(trackId, i, placement))
                continue;

            const auto* preview = findPreviewForPlacement(trackId, placement.placementId);
            if (preview == nullptr
                && (placement.timelineEndSeconds() <= visibleTimeStart
                    || placement.timelineStartSeconds >= visibleTimeEnd))
                continue;

            if (preview == nullptr)
                appendVisiblePlacement(trackId, trackId, placement, i, placement.timelineStartSeconds, false);
            else
                appendVisiblePlacement(trackId, preview->previewTrackId, placement, i, preview->previewStartSeconds, true);
        }
    }

    model_.totalTrackCount = kMaxTracks;
    model_.lastVisibleTrack = kMaxTracks - 1;
    needsRebuild_ = false;
    return model_;
}

} // namespace OpenTune
