#pragma once

/**
 * ArrangementRenderModelCache
 *
 * Prepares a snapshot of visible arrangement content for paint() to consume.
 * Scans only the visible time range. paint() iterates the prepared model —
 * it does NOT call processor/arrangement data accessors per-paint.
 *
 * This cache is Standalone-only UI state. NOT stored in the processor.
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <map>
#include <functional>
#include "WaveformTileCache.h"
#include "WaveformMipmap.h"
#include "TimelineViewportState.h"
#include "../PluginProcessor.h"

namespace OpenTune {

class ArrangementRenderModelCache {
public:
    /** Prepared visible placement for paint(). */
    struct VisiblePlacement {
        uint64_t placementId = 0;
        uint64_t materializationId = 0;
        uint64_t referencePlacementId = 0;
        juce::Rectangle<int> pixelBounds;
        juce::Rectangle<float> pixelArea;
        bool isSelected = false;
        float gain = 1.0f;
        juce::String name;
        bool hasAudioBuffer = false;
        bool analysisInProgress = false;
        bool isHovered = false;
        bool mouseOverReferenceButton = false;
        uint64_t waveformSourceId = 0;
        int waveformZoomBucket = 0;
        double waveformVisibleStartSeconds = 0.0;
        double waveformVisibleEndSeconds = 0.0;
        uint64_t waveformStyleHash = 0;
        uint64_t waveformTimeGridRevision = 0;
        int trackId = 0;
        double fadeInDuration = 0.0;
        double fadeOutDuration = 0.0;
        bool isPreview = false;
        juce::Colour colour{};
    };

    struct RenderModel {
        struct VisibleLane {
            int trackId = 0;
            juce::Rectangle<float> area;
            bool selected = false;
        };

        std::vector<VisibleLane> lanes;
        std::vector<VisiblePlacement> placements;
        int firstVisibleTrack = 0;
        int lastVisibleTrack = 0;
        int totalTrackCount = 0;
        double bpm = 120.0;
        int timeSigNumerator = 4;
        int timeSigDenominator = 4;
        int64_t generation = 0;
    };

    struct MoveDragPreviewPlacement {
        int sourceTrackId = -1;
        int previewTrackId = -1;
        uint64_t placementId = 0;
        double previewStartSeconds = 0.0;
    };

    struct MoveDragPreviewState {
        bool active = false;
        uint64_t revision = 0;
        std::vector<MoveDragPreviewPlacement> placements;
    };

    /** Single-entry cache — memory is inherently bounded to one RenderModel. */
    static constexpr std::size_t kMaxEntries = 1;

    struct Key {
        int64_t visibleTimeStartMs = 0;
        int64_t visibleTimeEndMs = 0;
        int scrollOffsetPx = 0;
        int viewportWidthPx = 0;
        int viewportHeightPx = 0;
        int zoomBucket = 0;
        int trackHeight = 0;
        int selectedTrack = -1;
        int selectedPlacementIndex = -1;
        uint64_t hoveredPlacementId = 0;
        bool mouseOverReferenceButton = false;
        uint64_t arrangementRevision = 0;
        uint64_t snapshotRevision = 0;
        uint64_t previewRevision = 0;

        bool operator==(const Key& other) const noexcept
        {
            return visibleTimeStartMs == other.visibleTimeStartMs
                && visibleTimeEndMs == other.visibleTimeEndMs
                && scrollOffsetPx == other.scrollOffsetPx
                && viewportWidthPx == other.viewportWidthPx
                && viewportHeightPx == other.viewportHeightPx
                && zoomBucket == other.zoomBucket
                && trackHeight == other.trackHeight
                && selectedTrack == other.selectedTrack
                && selectedPlacementIndex == other.selectedPlacementIndex
                && hoveredPlacementId == other.hoveredPlacementId
                && mouseOverReferenceButton == other.mouseOverReferenceButton
                && arrangementRevision == other.arrangementRevision
                && snapshotRevision == other.snapshotRevision
                && previewRevision == other.previewRevision;
        }

        bool operator!=(const Key& other) const noexcept { return !(*this == other); }
    };

    ArrangementRenderModelCache() = default;

    /**
     * Update the render model from current arrangement/viewport state.
     *
     * @param isPlacementSelected - callback(trackId, placementId) -> bool
     * @param getAnalysisState - callback(placementId) -> bool (analysis in progress)
     */
    const RenderModel& update(OpenTuneAudioProcessor& processor,
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
                              const MoveDragPreviewState& movePreview = {},
                              bool forceRebuild = false);

    const RenderModel& getModel() const noexcept { return model_; }

    static juce::Rectangle<int> computeWaveformDrawableBounds(juce::Rectangle<int> placementBounds) noexcept;

    void invalidate() { needsRebuild_ = true; }
    void clear() { model_.placements.clear(); needsRebuild_ = true; }

private:
    static Key makeKey(OpenTuneAudioProcessor& processor,
                       const TimelineViewportState& viewport,
                       int selectedTrack,
                       int selectedPlacementIndex,
                       uint64_t hoveredPlacementId,
                       bool mouseOverReferenceButton,
                       int trackHeight,
                       const MoveDragPreviewState& movePreview);

    RenderModel model_;
    Key currentKey_;
    int64_t modelGeneration_ = 0;
    bool needsRebuild_ = true;
};

} // namespace OpenTune
