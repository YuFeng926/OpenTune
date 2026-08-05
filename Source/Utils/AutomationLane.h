#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace OpenTune {

struct Note;

struct AutomationPoint {
    double timeSeconds = 0.0;
    float gainDb = 0.0f;

    bool operator==(const AutomationPoint& other) const noexcept
    {
        return timeSeconds == other.timeSeconds && gainDb == other.gainDb;
    }
};

class AutomationLane {
public:
    AutomationLane() = default;

    const std::vector<AutomationPoint>& points() const noexcept { return points_; }

    // O(log n) binary search + linear interpolation
    float evalAt(double timeSeconds) const {
        if (points_.empty()) return 0.0f;
        if (timeSeconds <= points_.front().timeSeconds) return points_.front().gainDb;
        if (timeSeconds >= points_.back().timeSeconds) return points_.back().gainDb;

        auto it = std::upper_bound(points_.begin(), points_.end(), timeSeconds,
            [](double val, const AutomationPoint& pt) { return val < pt.timeSeconds; });
        auto prev = std::prev(it);

        const double segDuration = it->timeSeconds - prev->timeSeconds;
        if (segDuration <= 0.0) return prev->gainDb;
        const float t01 = static_cast<float>((timeSeconds - prev->timeSeconds) / segDuration);
        return prev->gainDb + (it->gainDb - prev->gainDb) * t01;
    }

    // Insert/modify breakpoints for a note's time region with ramp transitions.
    // rampDuration: smooth transition in seconds (default 5ms).
    void setRegionGain(double startTime, double endTime, float gainDb,
                       double rampDuration = 0.005);

    // Build canonical sorted points.
    static AutomationLane fromSnapshot(const std::vector<AutomationPoint>& pts);

    // Old projects stored gain only on Note::outputGainDb.
    static AutomationLane fromLegacyNoteGains(const std::vector<Note>& notes);

    // Old sibilant points were step changes; migrate each step to a fixed 5 ms ramp.
    static AutomationLane fromLegacyStepPoints(const std::vector<AutomationPoint>& points);

    // Migration helper: exact sum of two piecewise-linear lanes.
    static AutomationLane sum(const AutomationLane& a, const AutomationLane& b);

    // Clear all points.
    void clear() { points_.clear(); }

    bool empty() const noexcept { return points_.empty(); }
    bool operator==(const AutomationLane& other) const noexcept { return points_ == other.points_; }

private:
    std::vector<AutomationPoint> points_;
};

} // namespace OpenTune
