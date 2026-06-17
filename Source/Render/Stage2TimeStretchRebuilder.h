#pragma once

#include "../Content/ContentKey.h"
#include "../Content/EditableContentSnapshot.h"
#include <memory>
#include <cstdint>

namespace OpenTune {

class ContentRenderService;

/**
 * Stage2 time-stretch rebuilder — pure function of CRS + snapshot.
 *
 * Reads Stage1 PlaybackReadSource from the passed CRS, applies TimeGrid-based
 * time stretch via SoundTouch, writes result into the same CRS's TimeStretchCache.
 *
 * This helper is domain-neutral: both processor (non-ARA) and DC (ARA) call it
 * with their own CRS instance. It holds no state of its own.
 */
struct Stage2TimeStretchRebuilder
{
    struct Request
    {
        ContentKey contentKey;
        uint64_t pitchRevision{0};
        uint64_t pitchShiftRevision{0};
        uint64_t timeGridRevision{0};
    };

    /**
     * Execute a Stage2 rebuild for a given content key.
     *
     * @param crs         The ContentRenderService holding the Stage1 PlaybackReadSource
     *                    and the TimeStretchCache / StretcherPool to use.
     * @param request     Trigger information (content key + revisions). Used both as
     *                    identity and as a minimum-revision filter against ownerSnap.
     * @param ownerSnap   Owner snapshot providing timeGrid + revision tuple. The
     *                    snapshot is taken by the caller, not fetched here, so this
     *                    helper has no domain dependencies.
     * @return            true on success (cache stored or identity-timegrid invalidated);
     *                    false on missing snapshot / source / stretcher / stale revisions.
     */
    static bool rebuild(ContentRenderService& crs,
                        const Request& request,
                        std::shared_ptr<const EditableContentSnapshot> ownerSnap);
};

} // namespace OpenTune
