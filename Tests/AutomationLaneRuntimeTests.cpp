#include <cmath>
#include <iostream>
#include <vector>

#include "Utils/AutomationLane.h"
#include "Utils/Note.h"

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "[FAIL] " << message << '\n';
}

bool near(float a, float b)
{
    return std::abs(a - b) < 1.0e-4f;
}

void testEvaluation()
{
    using namespace OpenTune;
    AutomationLane empty;
    expect(near(empty.evalAt(1.0), 0.0f), "empty lane is unity gain");

    const auto lane = AutomationLane::fromSnapshot({{0.0, -6.0f}, {2.0, 6.0f}});
    expect(near(lane.evalAt(-1.0), -6.0f), "evaluation holds first value before first point");
    expect(near(lane.evalAt(1.0), 0.0f), "evaluation interpolates linearly");
    expect(near(lane.evalAt(3.0), 6.0f), "evaluation holds last value after last point");
}

void testRegionEdit()
{
    using namespace OpenTune;
    auto lane = AutomationLane::fromSnapshot({
        {0.0, -3.0f}, {1.0, -3.0f}, {2.0, 3.0f}, {3.0, 3.0f}
    });
    const float left = lane.evalAt(1.195);
    const float right = lane.evalAt(1.805);
    lane.setRegionGain(1.2, 1.8, 6.0f);

    expect(near(lane.evalAt(1.195), left), "region edit preserves gain before its ramp");
    expect(near(lane.evalAt(1.2), 6.0f), "region edit reaches target at note start");
    expect(near(lane.evalAt(1.8), 6.0f), "region edit holds target through note end");
    expect(near(lane.evalAt(1.805), right), "region edit restores independent gain after its ramp");

    const auto pointCount = lane.points().size();
    lane.setRegionGain(1.2, 1.8, 6.0f);
    expect(lane.points().size() == pointCount, "repeated region edit does not grow redundant points");

}

void testMigrationAndSum()
{
    using namespace OpenTune;
    Note note;
    note.startTime = 1.0;
    note.endTime = 2.0;
    note.outputGainDb = 4.0f;
    const auto noteLane = AutomationLane::fromLegacyNoteGains({note});
    expect(near(noteLane.evalAt(1.5), 4.0f), "legacy note gain migrates into the lane");

    const auto offset = AutomationLane::fromSnapshot({{0.0, -1.0f}, {3.0, -1.0f}});
    const auto combined = AutomationLane::sum(noteLane, offset);
    expect(near(combined.evalAt(1.5), 3.0f), "legacy note and sibilant lanes sum exactly");

    const auto legacyStep = AutomationLane::fromLegacyStepPoints({{1.0, -2.0f}});
    expect(near(legacyStep.evalAt(0.994), 0.0f), "legacy step stays at unity before its migration ramp");
    expect(near(legacyStep.evalAt(1.0), -2.0f), "legacy step reaches its value at the original timestamp");
}

void testUnityCleanup()
{
    using namespace OpenTune;
    AutomationLane lane;
    lane.setRegionGain(1.0, 2.0, 5.0f);
    lane.setRegionGain(1.0, 2.0, 0.0f);
    expect(lane.empty(), "returning the only edited region to 0 dB restores an empty lane");
}

} // namespace

int main()
{
    testEvaluation();
    testRegionEdit();
    testMigrationAndSum();
    testUnityCleanup();
    if (failures == 0)
        std::cout << "AutomationLane runtime tests passed\n";
    return failures == 0 ? 0 : 1;
}
