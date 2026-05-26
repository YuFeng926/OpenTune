#pragma once

#include "PianoRollRenderer.h"
#include "Utils/MaterializationTimelineProjection.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <atomic>

namespace OpenTune {

class OpenTuneAudioProcessor;

/**
 * @brief Prepared immutable render model cache for PianoRoll.
 *
 * Holds a single prepared PianoRollRenderer::RenderContext, keyed by a set of
 * revision counters that reflect the true dependencies of the rendered output.
 * paint() calls getRenderContext() which returns the cached model if the key
 * has not changed, or rebuilds it on demand.
 *
 * The cache is invalidated when any of the following changes:
 *   - materialization identity / pitch epoch / notes epoch
 *   - visual preference revision (show booleans, scale, tool, time-unit …)
 *   - zoom bucket (quantized to avoid tiny scroll-only rebuilds)
 *   - viewport visible range
 *   - TimeGrid revision
 *
 * Transport cursor movement is handled by the overlay component and does not
 * rebuild this model.
 */
class PianoRollRenderModelCache final {
public:
    /** Single-entry cache — memory is inherently bounded to one RenderContext. */
    static constexpr std::size_t kMaxEntries = 1;

    PianoRollRenderModelCache() = default;
    ~PianoRollRenderModelCache() = default;

    /** Key struct that uniquely identifies a cached render model. */
    struct Key {
        uint64_t materializationId = 0;
        uint64_t pitchEpoch = 0;
        uint64_t notesEpoch = 0;
        uint64_t visualPrefsRevision = 0;
        uint64_t timeGridRevision = 0;
        int64_t visibleStartMs = 0;
        int64_t visibleEndMs = 0;
        int64_t projectionStartMs = 0;
        int64_t projectionDurationMs = 0;
        uint64_t placementProjectionRevision = 0;

        /** Zoom quantised to 1 % buckets so tiny scroll-wheel deltas skip rebuild. */
        int zoomBucket = 0;

        /** Incremented when component is resized (viewport width/height change). */
        uint64_t viewportSizeRevision = 0;

        bool operator==(const Key& o) const noexcept {
            return materializationId == o.materializationId
                && pitchEpoch == o.pitchEpoch
                && notesEpoch == o.notesEpoch
                && visualPrefsRevision == o.visualPrefsRevision
                && timeGridRevision == o.timeGridRevision
                && visibleStartMs == o.visibleStartMs
                && visibleEndMs == o.visibleEndMs
                && projectionStartMs == o.projectionStartMs
                && projectionDurationMs == o.projectionDurationMs
                && placementProjectionRevision == o.placementProjectionRevision
                && zoomBucket == o.zoomBucket
                && viewportSizeRevision == o.viewportSizeRevision;
        }
        bool operator!=(const Key& o) const noexcept { return !(*this == o); }
    };

    /** Rebuild the render context from scratch. */
    void rebuild(const Key& newKey,
                 PianoRollRenderer::RenderContext ctx);

    /** Return the cached render context (must be rebuilt first). */
    const PianoRollRenderer::RenderContext& getRenderContext() const noexcept { return cachedCtx_; }

    /** Return the current key. */
    const Key& getCurrentKey() const noexcept { return currentKey_; }

    /** True when the cache has been built at least once. */
    bool isValid() const noexcept { return valid_; }

    /** Invalidate (force next get to rebuild). */
    void invalidate() noexcept { valid_ = false; }

private:
    Key currentKey_;
    PianoRollRenderer::RenderContext cachedCtx_;
    bool valid_ = false;
};

} // namespace OpenTune
