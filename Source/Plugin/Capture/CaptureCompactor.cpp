
#include "CaptureCompactor.h"

namespace OpenTune::Capture {

namespace {
    bool fullyCovers(const CaptureSegment& outer, const CaptureSegment& inner) noexcept
    {
        const int64_t outerStart = outer.hostStartSample.load(std::memory_order_acquire);
        const int64_t outerCount = outer.hostSampleCount.load(std::memory_order_acquire);
        const int64_t innerStart = inner.hostStartSample.load(std::memory_order_acquire);
        const int64_t innerCount = inner.hostSampleCount.load(std::memory_order_acquire);
        return outerCount > 0 && innerCount > 0
            && outerStart <= innerStart
            && innerStart + innerCount <= outerStart + outerCount;
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
        const bool isCandidate = seg.contentKey != newlyEdited.contentKey
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

