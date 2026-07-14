#include "TimeGrid.h"
#include "AppLogger.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {

namespace {

constexpr double kTotalDurationEpsilon = 1e-6;

uint64_t makeStableId() noexcept
{
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

bool TimeGridSnapshot::hasMinimumSourceSpacing(double previousSourceSeconds,
                                               double currentSourceSeconds) noexcept
{
    if (!std::isfinite(previousSourceSeconds) || !std::isfinite(currentSourceSeconds)) {
        return false;
    }

    const int previousFrame = static_cast<int>(std::round(previousSourceSeconds * kSourceSpacingFrameRate));
    const int currentFrame = static_cast<int>(std::round(currentSourceSeconds * kSourceSpacingFrameRate));
    return currentFrame - previousFrame >= kMinSourceSpacingFrames;
}

bool TimeGridSnapshot::validate(const std::vector<TimeHandle>& handles, juce::String& outError)
{
    outError.clear();

    if (handles.size() < 2) {
        outError = "TimeGrid must have at least 2 handles (ClipStart + ClipEnd)";
        return false;
    }

    const auto& first = handles.front();
    const auto& last  = handles.back();

    if (first.kind != HandleKind::ClipStart) {
        outError = "First handle must be HandleKind::ClipStart";
        return false;
    }
    if (!first.locked) {
        outError = "ClipStart handle must be locked=true";
        return false;
    }
    if (first.source_seconds != 0.0 || first.output_seconds != 0.0) {
        outError = "ClipStart must have source_seconds == output_seconds == 0.0";
        return false;
    }

    if (last.kind != HandleKind::ClipEnd) {
        outError = "Last handle must be HandleKind::ClipEnd";
        return false;
    }
    if (!last.locked) {
        outError = "ClipEnd handle must be locked=true";
        return false;
    }

    // Check that all handles have finite source_seconds and output_seconds.
    // NaN/Inf would bypass inequality comparisons, polluting downstream
    // tauForward/tauInverse calculations.
    for (size_t i = 0; i < handles.size(); ++i) {
        const auto& h = handles[i];
        if (!std::isfinite(h.source_seconds)) {
            outError = "Handle at index " + juce::String((int)i)
                       + " has non-finite source_seconds";
            return false;
        }
        if (!std::isfinite(h.output_seconds)) {
            outError = "Handle at index " + juce::String((int)i)
                       + " has non-finite output_seconds";
            return false;
        }
    }

    // Strict monotonicity (source AND output)
    // Internal/internal and internal/endpoint spacing: 150 ms minimum source-time spacing
    for (size_t i = 1; i < handles.size(); ++i) {
        const auto& prev = handles[i - 1];
        const auto& curr = handles[i];

        // Endpoint spacing not checked: ClipStart→ClipEnd distance = total duration
        const bool isEndpointOnly = (handles.size() == 2) &&
                                    (i == 1);  // last handle is ClipEnd

        // Source monotonicity
        if (curr.source_seconds <= prev.source_seconds) {
            outError = "Handles must have strictly increasing source_seconds at index "
                       + juce::String((int) i)
                       + " (prev=" + juce::String(prev.source_seconds, 9)
                       + ", curr=" + juce::String(curr.source_seconds, 9) + ")";
            return false;
        }
        // Output monotonicity
        if (curr.output_seconds <= prev.output_seconds) {
            outError = "Handles must have strictly increasing output_seconds at index "
                       + juce::String((int) i)
                       + " (prev=" + juce::String(prev.output_seconds, 9)
                       + ", curr=" + juce::String(curr.output_seconds, 9) + ")";
            return false;
        }
        // Internal handle spacing: require 150 ms minimum source-time spacing
        if (!isEndpointOnly && !hasMinimumSourceSpacing(prev.source_seconds, curr.source_seconds)) {
            const double srcGapMs = (curr.source_seconds - prev.source_seconds) * 1000.0;
            outError = "Handles must have source_seconds spacing >= "
                       + juce::String(static_cast<int>(kMinSourceSpacingSeconds * 1000.0))
                       + " ms at index "
                       + juce::String((int)i)
                       + " (prev=" + juce::String(prev.source_seconds, 9)
                       + ", curr=" + juce::String(curr.source_seconds, 9)
                       + ", gap=" + juce::String(srcGapMs, 3) + " ms)";
            return false;
        }
    }

    // Total duration preservation (locked endpoints invariant).
    // last.source_seconds is the total source duration; output must equal source.
    const double totalSrc = last.source_seconds - first.source_seconds;
    const double totalOut = last.output_seconds - first.output_seconds;
    if (std::abs(totalSrc - totalOut) > kTotalDurationEpsilon) {
        outError = "Total duration must be preserved (locked endpoints): src="
                   + juce::String(totalSrc, 9)
                   + ", out=" + juce::String(totalOut, 9);
        return false;
    }

    // Locked semantics: only ClipStart/ClipEnd may have locked=true
    // ClipStart/ClipEnd MUST have confidence=Default (endpoints don't participate in note merge)
    for (size_t i = 0; i < handles.size(); ++i) {
        const auto& h = handles[i];
        const bool isEndpoint = (i == 0) || (i + 1 == handles.size());
        if (h.locked && !isEndpoint) {
            outError = "Non-endpoint handle has locked=true at index " + juce::String((int) i);
            return false;
        }
        if (isEndpoint && h.confidence != Confidence::Default) {
            outError = "Endpoint handle (ClipStart/ClipEnd) must have confidence=Default at index "
                       + juce::String((int) i);
            return false;
        }
    }

    return true;
}

std::shared_ptr<const TimeGridSnapshot> TimeGridSnapshot::makeIdentity(double totalDurationSeconds)
{
    if (!std::isfinite(totalDurationSeconds) || totalDurationSeconds <= 0.0) {
        AppLogger::warn("[TimeGrid] makeIdentity rejected: non-positive total duration "
                        + juce::String(totalDurationSeconds, 9));
        return nullptr;
    }

    std::vector<TimeHandle> handles;
    handles.reserve(2);

    TimeHandle clipStart;
    clipStart.id = makeStableId();
    clipStart.source_seconds = 0.0;
    clipStart.output_seconds = 0.0;
    clipStart.kind = HandleKind::ClipStart;
    clipStart.locked = true;
    handles.push_back(clipStart);

    TimeHandle clipEnd;
    clipEnd.id = makeStableId();
    clipEnd.source_seconds = totalDurationSeconds;
    clipEnd.output_seconds = totalDurationSeconds;
    clipEnd.kind = HandleKind::ClipEnd;
    clipEnd.locked = true;
    handles.push_back(clipEnd);

    return makeFromHandles(std::move(handles), /*revision=*/1);
}

std::shared_ptr<const TimeGridSnapshot> TimeGridSnapshot::makeFromHandles(std::vector<TimeHandle> handles,
                                                                          uint64_t revision)
{
    juce::String err;
    if (!validate(handles, err)) {
        AppLogger::warn("[TimeGrid] makeFromHandles rejected: " + err);
        return nullptr;
    }
    return std::shared_ptr<const TimeGridSnapshot>(
        new TimeGridSnapshot(std::move(handles), revision));
}

double TimeGridSnapshot::totalDurationSeconds() const noexcept
{
    if (handles_.size() < 2) return 0.0;
    return handles_.back().source_seconds - handles_.front().source_seconds;
}

double TimeGridSnapshot::tauForward(double sourceSeconds) const noexcept
{
    if (handles_.size() < 2) return sourceSeconds;
    if (sourceSeconds <= handles_.front().source_seconds) return handles_.front().output_seconds;
    if (sourceSeconds >= handles_.back().source_seconds)  return handles_.back().output_seconds;

    // Binary search: find segment [i, i+1] such that handles[i].src <= sourceSeconds < handles[i+1].src
    auto it = std::upper_bound(handles_.begin(), handles_.end(), sourceSeconds,
        [](double s, const TimeHandle& h) { return s < h.source_seconds; });
    // it now points to the first handle with source > sourceSeconds; the segment is [it-1, it]
    const auto& left  = *(it - 1);
    const auto& right = *it;

    // Anchor exactness: if sourceSeconds matches a handle exactly, return its output_seconds bit-exact
    if (sourceSeconds == left.source_seconds) return left.output_seconds;
    if (sourceSeconds == right.source_seconds) return right.output_seconds;

    const double srcSpan = right.source_seconds - left.source_seconds;
    if (srcSpan <= 0.0) return left.output_seconds;
    const double t = (sourceSeconds - left.source_seconds) / srcSpan;
    return left.output_seconds + t * (right.output_seconds - left.output_seconds);
}

double TimeGridSnapshot::tauInverse(double outputSeconds) const noexcept
{
    if (handles_.size() < 2) return outputSeconds;
    if (outputSeconds <= handles_.front().output_seconds) return handles_.front().source_seconds;
    if (outputSeconds >= handles_.back().output_seconds)  return handles_.back().source_seconds;

    auto it = std::upper_bound(handles_.begin(), handles_.end(), outputSeconds,
        [](double o, const TimeHandle& h) { return o < h.output_seconds; });
    const auto& left  = *(it - 1);
    const auto& right = *it;

    if (outputSeconds == left.output_seconds) return left.source_seconds;
    if (outputSeconds == right.output_seconds) return right.source_seconds;

    const double outSpan = right.output_seconds - left.output_seconds;
    if (outSpan <= 0.0) return left.source_seconds;
    const double t = (outputSeconds - left.output_seconds) / outSpan;
    return left.source_seconds + t * (right.source_seconds - left.source_seconds);
}

bool TimeGridSnapshot::isIdentityInRange(double t_src_a, double t_src_b) const noexcept
{
    if (handles_.empty()) return true;
    if (t_src_b <= t_src_a) return true;

    // Find first handle with source_seconds > t_src_a, and walk until source_seconds >= t_src_b.
    auto it = std::upper_bound(handles_.begin(), handles_.end(), t_src_a,
        [](double s, const TimeHandle& h) { return s < h.source_seconds; });
    // Also include the handle immediately before (if any) since the segment starts there
    if (it != handles_.begin()) --it;

    while (it != handles_.end() && it->source_seconds <= t_src_b) {
        if (it->output_seconds != it->source_seconds) return false;
        ++it;
    }
    return true;
}

bool TimeGridSnapshot::isIdentity() const noexcept
{
    for (const auto& h : handles_) {
        if (h.output_seconds != h.source_seconds) return false;
    }
    return true;
}

// ─── TimeGrid (slot wrapper) ──────────────────────────────────────────────

TimeGrid::TimeGrid()
    : snapshot_(TimeGridSnapshot::makeIdentity(/*totalDurationSeconds=*/0.001))
{
    // Bootstrap with a tiny identity grid; caller is expected to setSnapshot()
    // with a real content duration.
}

TimeGrid::TimeGrid(std::shared_ptr<const TimeGridSnapshot> initial)
    : snapshot_(std::move(initial))
{}

std::shared_ptr<const TimeGridSnapshot> TimeGrid::getSnapshot() const noexcept
{
    return std::atomic_load(&snapshot_);
}

void TimeGrid::setSnapshot(std::shared_ptr<const TimeGridSnapshot> newSnapshot) noexcept
{
    std::atomic_store(&snapshot_, std::move(newSnapshot));
}

} // namespace OpenTune
