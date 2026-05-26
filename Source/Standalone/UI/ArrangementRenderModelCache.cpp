#include "ArrangementRenderModelCache.h"

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
                                      int trackHeight)
{
    Key key;
    key.visibleTimeStartMs = timeToMs(viewport.visibleTimeStart());
    key.visibleTimeEndMs = timeToMs(viewport.visibleTimeEnd());
    key.scrollOffsetPx = viewport.scrollOffsetPx;
    key.viewportWidthPx = viewport.viewportWidthPx;
    key.viewportHeightPx = viewport.viewportHeightPx;
    key.zoomBucket = static_cast<int>(std::llround(viewport.zoomLevel * 100.0));
    key.trackHeight = trackHeight;
    key.selectedTrack = selectedTrack;
    key.selectedPlacementIndex = selectedPlacementIndex;
    key.hoveredPlacementId = hoveredPlacementId;
    key.mouseOverReferenceButton = mouseOverReferenceButton;

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
                revision = hashCombine(revision, placement.materializationId);
                revision = hashCombine(revision, placement.referencePlacementId);
                revision = hashCombine(revision, static_cast<uint64_t>(timeToMs(placement.timelineStartSeconds)));
                revision = hashCombine(revision, static_cast<uint64_t>(timeToMs(placement.durationSeconds)));
                revision = hashCombine(revision, static_cast<uint64_t>(std::llround(placement.gain * 1000.0f)));
            }
        }

        key.arrangementRevision = revision;
        key.snapshotRevision = revision;
    }

    return key;
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
                                    WaveformTileCache& tileCache,
                                    WaveformMipmapCache& mipmapCache,
                                     int trackHeight,
                                     bool forceRebuild)
{
    const auto nextKey = makeKey(processor,
                                 viewport,
                                 selectedTrack,
                                 selectedPlacementIndex,
                                 hoveredPlacementId,
                                 mouseOverReferenceButton,
                                 trackHeight);

    if (!needsRebuild_ && !forceRebuild && currentKey_ == nextKey)
        return model_;

    currentKey_ = nextKey;
    model_.lanes.clear();
    model_.placements.clear();
    model_.generation = ++modelGeneration_;
    model_.bpm = processor.getBpm();
    model_.timeSigNumerator = processor.getTimeSigNumerator();
    model_.timeSigDenominator = processor.getTimeSigDenominator();

    const auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr) {
        model_.totalTrackCount = 0;
        needsRebuild_ = false;
        return model_;
    }

    constexpr int kMaxTracks = OpenTuneAudioProcessor::MAX_TRACKS;
    constexpr int kRulerHeight = 30;

    const double visibleTimeStart = viewport.visibleTimeStart();
    const double visibleTimeEnd = viewport.visibleTimeEnd();

    for (int trackId = 0; trackId < kMaxTracks; ++trackId)
    {
        auto laneBounds = [&]() -> juce::Rectangle<int> {
            auto bounds = juce::Rectangle<int>(0, 0, viewport.viewportWidthPx, viewport.viewportHeightPx)
                              .withTrimmedTop(kRulerHeight);
            int h = trackHeight;
            return bounds.withY(kRulerHeight + trackId * h).withHeight(h);
        }();

        if (laneBounds.getBottom() < kRulerHeight)
            continue;

        if (laneBounds.getY() <= viewport.viewportHeightPx) {
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

            if (placement.timelineEndSeconds() < visibleTimeStart
                || placement.timelineStartSeconds > visibleTimeEnd)
                continue;

            if (placement.durationSeconds <= 0.0)
                continue;

            auto lane = laneBounds.reduced(6, 8);
            const int x1 = viewport.timeToViewportX(placement.timelineStartSeconds);
            const int x2 = viewport.timeToViewportX(placement.timelineEndSeconds());
            const int width = juce::jmax(8, x2 - x1);
            juce::Rectangle<int> placementBounds{x1, lane.getY(), width, lane.getHeight()};

            if (placementBounds.isEmpty())
                continue;

            const uint64_t placementId = placement.placementId;
            const uint64_t materializationId = placement.materializationId;

            bool isSelected = (trackId == selectedTrack && i == selectedPlacementIndex)
                           || (isPlacementSelected && isPlacementSelected(trackId, placementId));

            VisiblePlacement vp;
            vp.placementId = placementId;
            vp.materializationId = materializationId;
            vp.referencePlacementId = placement.referencePlacementId;
            vp.pixelBounds = placementBounds;
            vp.pixelArea = placementBounds.toFloat();
            vp.isSelected = isSelected;
            vp.gain = placement.gain;
            vp.name = placement.name;
            vp.isHovered = (hoveredPlacementId == placementId);
            vp.mouseOverReferenceButton = (hoveredPlacementId == placementId) && mouseOverReferenceButton;

            if (getAnalysisState)
                vp.analysisInProgress = getAnalysisState(placementId);

            auto audioBuffer = processor.getMaterializationAudioBufferById(materializationId);
            vp.hasAudioBuffer = (audioBuffer != nullptr);

            if (audioBuffer != nullptr)
            {
                auto& mipmap = mipmapCache.getOrCreate(materializationId);
                mipmap.setAudioSource(audioBuffer);

                const double visibleMaterializationStart = juce::jmax(0.0,
                    visibleTimeStart - placement.timelineStartSeconds);
                const double visibleMaterializationEnd = juce::jmin(placement.durationSeconds,
                    visibleTimeEnd - placement.timelineStartSeconds);
                auto waveformBounds = placementBounds.reduced(6, 6);
                const int zoomBucket = static_cast<int>(viewport.zoomLevel * 10.0 + 0.5);
                const uint64_t waveformSourceId = materializationId;
                const uint64_t waveformStyleHash = static_cast<uint64_t>(placement.gain * 1000.0f);
                tileCache.getOrCreate(materializationId,
                                      waveformSourceId,
                                      zoomBucket,
                                      mipmap,
                                      placement.gain,
                                      waveformBounds,
                                      visibleMaterializationStart,
                                      visibleMaterializationEnd,
                                      waveformStyleHash,
                                      0);
                vp.waveformSourceId = waveformSourceId;
                vp.waveformZoomBucket = zoomBucket;
                vp.waveformVisibleStartSeconds = visibleMaterializationStart;
                vp.waveformVisibleEndSeconds = visibleMaterializationEnd;
                vp.waveformStyleHash = waveformStyleHash;
                vp.waveformTimeGridRevision = 0;
            }

            model_.placements.push_back(std::move(vp));
        }
    }

    model_.totalTrackCount = kMaxTracks;
    model_.lastVisibleTrack = kMaxTracks - 1;
    needsRebuild_ = false;
    return model_;
}

} // namespace OpenTune
