#pragma once


#include "CaptureSegment.h"

#include <memory>
#include <vector>

namespace OpenTune::Capture {

/**
 * Compaction utility: when a new Edited segment fully covers the time range of
 * an older segment, the older segment becomes unreachable in playback (because
 * processBlock reverse-iterates and the newer one wins). Removing it frees PCM.
 *
 * Partial overlaps are intentionally NOT split — keeping older segments simpler;
 * playback resolution handles them correctly via reverse-iterate.
 */
class CaptureCompactor
{
public:
    /**
     * Walk 'segments' and remove every segment whose [T_start, T_start+duration)
     * is fully contained in 'newlyEdited'. The newlyEdited segment itself is
     * never removed.
     *
     * Returns the removed unique_ptrs (caller is responsible for deferred
     * reclamation; CaptureSession parks them in pendingReclaim_ for a grace
     * period before destruction).
     *
     * Must be called under CaptureSession's mutableMutex_.
     */
    static std::vector<std::unique_ptr<CaptureSegment>>
    removeFullyCovered(std::vector<std::unique_ptr<CaptureSegment>>& segments,
                       const CaptureSegment& newlyEdited);
};

}  // namespace OpenTune::Capture

