#include "AutomationLane.h"
#include "Note.h"
#include <algorithm>

namespace OpenTune {

namespace {

constexpr double kTimeEpsilon = 1.0e-9;
constexpr float kGainEpsilon = 1.0e-4f;

bool sameTime(double a, double b) noexcept
{
    return std::abs(a - b) <= kTimeEpsilon;
}

void normalizePoints(std::vector<AutomationPoint>& points)
{
    std::stable_sort(points.begin(), points.end(),
        [](const AutomationPoint& a, const AutomationPoint& b) {
            return a.timeSeconds < b.timeSeconds;
        });

    std::vector<AutomationPoint> canonical;
    canonical.reserve(points.size());
    for (const auto& point : points) {
        if (!canonical.empty() && sameTime(canonical.back().timeSeconds, point.timeSeconds))
            canonical.back() = point;
        else
            canonical.push_back(point);
    }

    std::vector<AutomationPoint> simplified;
    simplified.reserve(canonical.size());
    for (const auto& point : canonical) {
        simplified.push_back(point);
        while (simplified.size() >= 3) {
            const auto& a = simplified[simplified.size() - 3];
            const auto& b = simplified[simplified.size() - 2];
            const auto& c = simplified[simplified.size() - 1];
            const double duration = c.timeSeconds - a.timeSeconds;
            const float expected = a.gainDb + (c.gainDb - a.gainDb)
                * static_cast<float>((b.timeSeconds - a.timeSeconds) / duration);
            if (std::abs(b.gainDb - expected) > kGainEpsilon)
                break;
            simplified.erase(simplified.end() - 2);
        }
    }

    if (std::all_of(simplified.begin(), simplified.end(), [](const AutomationPoint& point) {
            return std::abs(point.gainDb) <= kGainEpsilon;
        })) {
        simplified.clear();
    }
    points = std::move(simplified);
}

} // namespace

void AutomationLane::setRegionGain(double startTime, double endTime, float gainDb,
                                    double rampDuration)
{
    const double rampStart = std::max(0.0, startTime - rampDuration);
    const double rampEnd = endTime + rampDuration;
    const float gainBefore = evalAt(rampStart);
    const float gainAfter = evalAt(rampEnd);

    points_.erase(
        std::remove_if(points_.begin(), points_.end(),
            [rampStart, rampEnd](const AutomationPoint& point) {
                return point.timeSeconds >= rampStart && point.timeSeconds <= rampEnd;
            }),
        points_.end());

    if (rampStart < startTime)
        points_.push_back({rampStart, gainBefore});
    points_.push_back({startTime, gainDb});
    points_.push_back({endTime, gainDb});
    points_.push_back({rampEnd, gainAfter});
    normalizePoints(points_);
}

AutomationLane AutomationLane::fromSnapshot(const std::vector<AutomationPoint>& pts)
{
    AutomationLane lane;
    lane.points_ = pts;
    normalizePoints(lane.points_);
    return lane;
}

AutomationLane AutomationLane::fromLegacyNoteGains(const std::vector<Note>& notes)
{
    AutomationLane lane;
    for (const auto& note : notes) {
        if (std::abs(note.outputGainDb) > kGainEpsilon)
            lane.setRegionGain(note.startTime, note.endTime, note.outputGainDb);
    }
    return lane;
}

AutomationLane AutomationLane::fromLegacyStepPoints(const std::vector<AutomationPoint>& points)
{
    auto sorted = points;
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const AutomationPoint& a, const AutomationPoint& b) {
            return a.timeSeconds < b.timeSeconds;
        });

    std::vector<AutomationPoint> migrated;
    migrated.reserve(sorted.size() * 2);
    float previousGainDb = 0.0f;
    for (const auto& point : sorted) {
        migrated.push_back({std::max(0.0, point.timeSeconds - 0.005), previousGainDb});
        migrated.push_back(point);
        previousGainDb = point.gainDb;
    }
    return fromSnapshot(migrated);
}

AutomationLane AutomationLane::sum(const AutomationLane& a, const AutomationLane& b)
{
    if (a.empty())
        return b;
    if (b.empty())
        return a;

    std::vector<double> times;
    times.reserve(a.points_.size() + b.points_.size());
    for (const auto& point : a.points_)
        times.push_back(point.timeSeconds);
    for (const auto& point : b.points_)
        times.push_back(point.timeSeconds);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), sameTime), times.end());

    std::vector<AutomationPoint> points;
    points.reserve(times.size());
    for (double time : times)
        points.push_back({time, a.evalAt(time) + b.evalAt(time)});
    return fromSnapshot(points);
}

} // namespace OpenTune
