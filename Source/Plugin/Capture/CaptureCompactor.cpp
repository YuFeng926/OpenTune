
#include "CaptureCompactor.h"

namespace OpenTune::Capture {

namespace {
    bool fullyCovers(const CaptureSegment& outer, const CaptureSegment& inner) noexcept
    {
        const double outerStart = outer.T_start.load(std::memory_order_acquire);
        const double outerEnd = outerStart + outer.durationSeconds;
        const double innerStart = inner.T_start.load(std::memory_order_acquire);
        const double innerEnd = innerStart + inner.durationSeconds;
        return outerStart <= innerStart && innerEnd <= outerEnd;
    }
}

std::vector<std::unique_ptr<CaptureSegment>>
CaptureCompactor::removeFullyCovered(std::vector<std::unique_ptr<CaptureSegment>>& segments,
                                      const CaptureSegment& newlyEdited)
{
    std::vector<std::unique_ptr<CaptureSegment>> removed;
    auto it = segments.begin();
    while (it != segments.end()) {
        auto& seg = **it;
        const bool isCandidate = seg.id != newlyEdited.id
                              && seg.creationOrder < newlyEdited.creationOrder
                              && seg.state.load(std::memory_order_acquire) == SegmentState::Edited
                              && fullyCovers(newlyEdited, seg);
        if (isCandidate) {
            removed.push_back(std::move(*it));
            it = segments.erase(it);
        } else {
            ++it;
        }
    }
    return removed;
}

}  // namespace OpenTune::Capture

