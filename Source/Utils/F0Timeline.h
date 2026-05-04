#pragma once

#include <cmath>
#include <utility>
#include <cassert>

namespace OpenTune {

struct F0FrameRange {
    int startFrame = 0;
    int endFrameExclusive = 0;

    bool isEmpty() const noexcept { return endFrameExclusive <= startFrame; }
};

class F0Timeline {
public:
    F0Timeline() = default;

    F0Timeline(int hopSize, double sampleRate, int frameCount) noexcept
        : secondsPerFrame_(static_cast<double>(hopSize) / sampleRate),
          endFrameExclusive_(frameCount)
    {}

    bool isEmpty() const noexcept { return endFrameExclusive_ <= 0; }
    int endFrameExclusive() const noexcept { return endFrameExclusive_; }

    double timeAtFrame(int frame) const noexcept {
        assert(!isEmpty());
        return static_cast<double>(frame) * secondsPerFrame_;
    }

    int frameAtOrBefore(double seconds) const noexcept {
        assert(!isEmpty());
        return clamp(static_cast<int>(std::floor(seconds / secondsPerFrame_)), 0, endFrameExclusive_ - 1);
    }

    int exclusiveFrameAt(double seconds) const noexcept {
        assert(!isEmpty());
        return clamp(static_cast<int>(std::ceil(seconds / secondsPerFrame_)), 0, endFrameExclusive_);
    }

    F0FrameRange rangeForTimes(double startSeconds, double endSeconds) const noexcept {
        if (isEmpty()) return {};
        if (endSeconds < startSeconds) std::swap(startSeconds, endSeconds);
        return { frameAtOrBefore(startSeconds), exclusiveFrameAt(endSeconds) };
    }

    F0FrameRange nonEmptyRangeForTimes(double startSeconds, double endSeconds) const noexcept {
        auto r = rangeForTimes(startSeconds, endSeconds);
        if (r.isEmpty() && !isEmpty()) r.endFrameExclusive = clamp(r.startFrame + 1, 0, endFrameExclusive_);
        return r;
    }

    F0FrameRange rangeForFrames(int startFrame, int endFrameExclusive) const noexcept {
        if (isEmpty()) return {};
        return { clamp(startFrame, 0, endFrameExclusive_ - 1), clamp(endFrameExclusive, 0, endFrameExclusive_) };
    }

    F0FrameRange rangeForTimesWithMargin(double startSeconds, double endSeconds, int margin) const noexcept {
        auto r = rangeForTimes(startSeconds, endSeconds);
        if (r.isEmpty()) return r;
        r.startFrame = clamp(r.startFrame - margin, 0, endFrameExclusive_ - 1);
        r.endFrameExclusive = clamp(r.endFrameExclusive + margin, 0, endFrameExclusive_);
        return r;
    }

private:
    static int clamp(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

    double secondsPerFrame_ = 1.0;
    int endFrameExclusive_ = 0;
};

} // namespace OpenTune
