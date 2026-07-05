// ============================================================================
// OpenTune Arrangement Move-Drag Behavior Tests
// ----------------------------------------------------------------------------
// Plan: docs/plans/2026-07-04-timeline-policy-overlay-projection-execution-review.md
// Section 4 Step B2.
//
// Proves the transient overlay contract of ArrangementViewComponent move drag:
//   - mouseDrag does NOT modify model and does NOT add undo entries
//   - mouseUp commits exactly once via MultiMovePlacementAction
//   - a single undo() rolls back ALL moved placements (single transaction)
//   - negative-time delta clamps to 0.0 on commit
//
// All helpers are test-local and call REAL StandaloneArrangement /
// ArrangementViewComponent / UndoManager APIs. No mocks.
// ============================================================================

#include "../Source/PluginProcessor.h"
#include "../Source/StandaloneArrangement.h"
#include "../Source/Standalone/StandaloneArrangementHelpers.h"
#include "../Source/Standalone/UI/ArrangementViewComponent.h"
#include "../Source/Standalone/UI/TimelineViewportPolicy.h"
#include "../Source/Content/ContentKey.h"
#include "../Source/Utils/TrackConstants.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace OpenTune;

namespace {

// ------------------------------------------------------------------
// Test geometry constants
// ------------------------------------------------------------------
// Must match ArrangementViewComponent.cpp anonymous-namespace constants.
// These are test-local geometry conversions (plan Section 4 Step B2).
constexpr double kTestPixelsPerSecond = 100.0;
constexpr int    kContentStartX       = 8;   // kArrangementContentStartX
constexpr double kTimeToleranceSec    = 0.05;

// ------------------------------------------------------------------
// Listener capture for verifying broadcast events
// ------------------------------------------------------------------
struct CaptureListener : ArrangementViewComponent::Listener {
    std::vector<std::string> events;
    int selectionTrack = -1;
    uint64_t selectionId = 0;
    int timingTrack = -1;
    int timingIndex = -1;

    void placementSelectionChanged(int trackId, uint64_t placementId) override
    {
        events.push_back("selection");
        selectionTrack = trackId;
        selectionId = placementId;
    }

    void placementTimingChanged(int trackId, int placementIndex) override
    {
        events.push_back("timing");
        timingTrack = trackId;
        timingIndex = placementIndex;
    }

    void clear()
    {
        events.clear();
        selectionTrack = -1;
        selectionId = 0;
        timingTrack = -1;
        timingIndex = -1;
    }
};

// ------------------------------------------------------------------
// Failure reporting (no exceptions, no defensive guards)
// ------------------------------------------------------------------
bool g_failed = false;

void fail(const std::string& msg)
{
    std::cout << "[FAIL] " << msg << "\n";
    g_failed = true;
}

void expect(bool cond, const std::string& msg)
{
    if (!cond) fail(msg);
}

// ------------------------------------------------------------------
// Placement helpers — call REAL StandaloneArrangement APIs
// ------------------------------------------------------------------

ContentKey makeDummyContentKey()
{
    // Test placement only needs a valid ContentKey so Placement::isValid() holds.
    // No audio data is read by the move-drag path.
    ContentKey key;
    key.domainKind              = DomainKind::StandaloneClip;
    key.objectId                = 1;
    key.sourceWindowDiscriminator = 0;
    return key;
}

uint64_t insertTestPlacement(StandaloneArrangement& arr, int trackId,
                             double startSeconds, double durationSeconds)
{
    StandaloneArrangement::Placement p;
    p.placementId           = 0;                     // let arrangement assign
    p.contentKey            = makeDummyContentKey();
    p.timelineStartSeconds  = startSeconds;
    p.durationSeconds       = durationSeconds;
    p.gain                  = 1.0f;

    if (!arr.insertPlacement(trackId, p))
    {
        fail("insertTestPlacement: insertPlacement failed track=" + std::to_string(trackId));
        return 0;
    }
    return p.placementId;
}

struct PlacementSnapshot {
    double timelineStartSeconds;
    double durationSeconds;
};

PlacementSnapshot requirePlacement(StandaloneArrangement& arr, int trackId, uint64_t placementId)
{
    StandaloneArrangement::Placement p;
    if (!arr.getPlacementById(trackId, placementId, p))
    {
        fail("requirePlacement: not found track=" + std::to_string(trackId)
             + " id=" + std::to_string(placementId));
        return { 0.0, 0.0 };
    }
    return { p.timelineStartSeconds, p.durationSeconds };
}

void expectPlacement(StandaloneArrangement& arr, int trackId, uint64_t placementId,
                     double expectedStartSeconds)
{
    StandaloneArrangement::Placement p;
    if (!arr.getPlacementById(trackId, placementId, p))
    {
        fail("expectPlacement: not found track=" + std::to_string(trackId)
             + " id=" + std::to_string(placementId));
        return;
    }
    if (std::abs(p.timelineStartSeconds - expectedStartSeconds) > kTimeToleranceSec)
    {
        fail("expectPlacement: start mismatch track=" + std::to_string(trackId)
             + " id=" + std::to_string(placementId)
             + " expected=" + std::to_string(expectedStartSeconds)
             + " actual="   + std::to_string(p.timelineStartSeconds));
    }
}

// ------------------------------------------------------------------
// Geometry conversions (test-local, plan Section 4 Step B2)
// ------------------------------------------------------------------

int xForTime(double seconds)
{
    return static_cast<int>(std::round(seconds * kTestPixelsPerSecond)) + kContentStartX;
}

int yForTrack(const ArrangementViewComponent& view,
              const OpenTuneAudioProcessor& proc, int trackId)
{
    const int th = proc.getTrackHeight();
    // Lane center — avoids 8px edge/handle hit zones.
    return view.getRulerHeight() + trackId * th + th / 2;
}

int placementCenterX(const PlacementSnapshot& snap)
{
    return xForTime(snap.timelineStartSeconds + snap.durationSeconds / 2.0);
}

// ------------------------------------------------------------------
// Real juce::MouseEvent synthesis
// ------------------------------------------------------------------
// ArrangementViewComponent only consumes e.getPosition()/e.x/e.y/e.mods,
// so a minimal MouseEvent built from Desktop's main mouse source suffices.
// withNewPosition() preserves mouseDownPos/mouseDownTime, which is the
// correct gesture semantics for drag/up events following a mouseDown.

juce::MouseEvent makeMouseEvent(ArrangementViewComponent& view,
                                int x, int y,
                                juce::ModifierKeys mods)
{
    auto& source = juce::Desktop::getInstance().getMainMouseSource();
    const juce::Point<float> pos(static_cast<float>(x), static_cast<float>(y));
    const auto now = juce::Time::getCurrentTime();

    return juce::MouseEvent(
        source,
        pos,
        mods,
        /*pressure*/    1.0f,
        /*orientation*/ 0.0f,
        /*rotation*/    0.0f,
        /*tiltX*/        0.0f,
        /*tiltY*/        0.0f,
        /*eventComponent*/ &view,
        /*originator*/     &view,
        /*eventTime*/      now,
        /*mouseDownPos*/   pos,
        /*mouseDownTime*/  now,
        /*numberOfClicks*/ 1,
        /*mouseWasDragged*/ false);
}

// ------------------------------------------------------------------
// Real input actions — call view's public mouse handlers
// ------------------------------------------------------------------

void mouseDownOnPlacement(ArrangementViewComponent& view,
                          const OpenTuneAudioProcessor& proc,
                          int trackId, uint64_t placementId)
{
    StandaloneArrangement::Placement p;
    if (!proc.getStandaloneArrangement()->getPlacementById(trackId, placementId, p))
    {
        fail("mouseDownOnPlacement: placement not found track=" + std::to_string(trackId)
             + " id=" + std::to_string(placementId));
        return;
    }
    const int cx = xForTime(p.timelineStartSeconds + p.durationSeconds / 2.0);
    const int cy = yForTrack(view, proc, trackId);
    view.mouseDown(makeMouseEvent(view, cx, cy, juce::ModifierKeys()));
}

void mouseDragTo(ArrangementViewComponent& view,
                 const juce::MouseEvent& downEvent, int x, int y)
{
    view.mouseDrag(downEvent.withNewPosition(juce::Point<int>(x, y)));
}

void mouseUpAt(ArrangementViewComponent& view,
               const juce::MouseEvent& downEvent, int x, int y)
{
    view.mouseUp(downEvent.withNewPosition(juce::Point<int>(x, y)));
}

void selectPlacementsWithRealInput(ArrangementViewComponent& view,
                                   const OpenTuneAudioProcessor& proc,
                                   const std::vector<std::pair<int, uint64_t>>& selections)
{
    // Ctrl+click toggles selection WITHOUT entering move drag (mouseDown
    // returns early in the Ctrl branch), so multiple placements can be
    // selected before the real move drag begins.
    for (const auto& entry : selections)
    {
        const int      trackId     = entry.first;
        const uint64_t placementId = entry.second;

        StandaloneArrangement::Placement p;
        if (!proc.getStandaloneArrangement()->getPlacementById(trackId, placementId, p))
        {
            fail("selectPlacementsWithRealInput: placement not found track=" + std::to_string(trackId));
            continue;
        }
        const int cx = xForTime(p.timelineStartSeconds + p.durationSeconds / 2.0);
        const int cy = yForTrack(view, proc, trackId);
        view.mouseDown(makeMouseEvent(view, cx, cy,
                       juce::ModifierKeys(juce::ModifierKeys::ctrlModifier)));
    }
}

void applyViewportForTest(ArrangementViewComponent& view,
                          double startSeconds, double pixelsPerSecond)
{
    TimelineViewportRequest req;
    req.kind            = TimelineViewportRequest::Kind::Manual;
    req.targetTime      = startSeconds;
    req.viewportWidth   = view.timelinePolicyViewportWidth();
    req.pixelsPerSecond = pixelsPerSecond;
    view.commitViewportRequest(req, juce::sendNotification);
}

} // namespace

// ==================================================================
// Test 1: move drag is transient until mouseUp
// ==================================================================
bool arrangementMoveDrag_isTransientUntilMouseUp()
{
    g_failed = false;

    OpenTuneAudioProcessor processor;
    auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr) { fail("no standalone arrangement"); return false; }

    const auto idA = insertTestPlacement(*arrangement, /*track*/1, /*start*/5.0, /*dur*/2.0);
    const auto idB = insertTestPlacement(*arrangement, /*track*/2, /*start*/7.0, /*dur*/2.0);
    processor.getUndoManager().clear();

    ArrangementViewComponent view(processor);
    CaptureListener capture;
    view.addListener(&capture);
    view.setBounds(0, 0, 1200, 600);
    applyViewportForTest(view, 0.0, kTestPixelsPerSecond);

    selectPlacementsWithRealInput(view, processor, { {1, idA}, {2, idB} });
    capture.clear();

    const auto beforeA = requirePlacement(*arrangement, 1, idA);
    const auto beforeB = requirePlacement(*arrangement, 2, idB);

    // --- Begin move drag on placement A (already selected) ---
    const int downX = placementCenterX(beforeA);
    const int downY = yForTrack(view, processor, 1);
    const auto downEvent = makeMouseEvent(view, downX, downY, juce::ModifierKeys());
    view.mouseDown(downEvent);

    // mouseDown fires placementSelectionChanged before entering move drag;
    // clear it so we only measure what mouseDrag and mouseUp produce.
    capture.clear();

    // --- Drag: +1.0s delta, cursor on track 3 ---
    // mouseDown landed on placement center (time = start + dur/2 = 6.0).
    // Drag cursor to time = 6.0 + 1.0 = 7.0  => deltaSeconds = +1.0.
    const int dragX = xForTime(beforeA.timelineStartSeconds + beforeA.durationSeconds / 2.0 + 1.0);
    const int dragY = yForTrack(view, processor, 3);
    mouseDragTo(view, downEvent, dragX, dragY);

    // 预选后的 drag preview 不得触发 listener
    expect(capture.events.empty(),
           "drag preview must not emit selection/timing");

    // DURING DRAG: model unchanged, no undo entry added.
    expectPlacement(*arrangement, 1, idA, beforeA.timelineStartSeconds);
    expectPlacement(*arrangement, 2, idB, beforeB.timelineStartSeconds);
    expect(!processor.getUndoManager().canUndo(),
           "undo must be empty during drag — drag is transient");

    // --- mouseUp commits ---
    mouseUpAt(view, downEvent, dragX, dragY);

    // AFTER MOUSE UP: model updated. trackDelta = 3 - 1 = 2,
    // so A: 1->3, B: 2->4. start += 1.0.
    expectPlacement(*arrangement, 3, idA, beforeA.timelineStartSeconds + 1.0);
    expectPlacement(*arrangement, 4, idB, beforeB.timelineStartSeconds + 1.0);
    expect(processor.getUndoManager().canUndo(),
           "undo must be available after mouseUp commit");

    // Verify active track and selected placement point to moved primary.
    // Primary was idA on track 1, moved to track 3.
    expect(getStandaloneActiveTrack(processor) == 3,
           "active track must be target of moved primary");
    const int idxA = processor.findPlacementIndexById(3, idA);
    expect(idxA >= 0,
           "moved primary placement must exist in target track");
    expect(getStandaloneSelectedPlacementIndex(processor, 3) == idxA,
           "selected placement index must point to moved primary");

    // Listener: mouseUp 仅触发了 selection + timing 两个事件
    expect(capture.events.size() == 2,
           "mouseUp must emit exactly selection + timing events");
    expect(capture.events[0] == "selection",
           "selection event must fire before timing event");
    expect(capture.events[1] == "timing",
           "timing event must fire after selection event");
    expect(capture.selectionTrack == 3,
           "listener selection track must be target of moved primary");
    expect(capture.selectionId == idA,
           "listener selection id must be moved primary");
    expect(capture.timingTrack == 3,
           "listener timing track must be target");
    expect(capture.timingIndex == processor.findPlacementIndexById(3, idA),
           "listener timing index must match position");

    // --- Single undo rolls back ALL moved placements (single transaction) ---
    processor.getUndoManager().undo();
    expectPlacement(*arrangement, 1, idA, beforeA.timelineStartSeconds);
    expectPlacement(*arrangement, 2, idB, beforeB.timelineStartSeconds);

    return !g_failed;
}

// ==================================================================
// Test 2: negative-time delta clamps to 0.0 on commit
// ==================================================================
bool arrangementMoveDrag_negativeTimeClamp()
{
    g_failed = false;

    OpenTuneAudioProcessor processor;
    auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr) { fail("no standalone arrangement"); return false; }

    const auto idA = insertTestPlacement(*arrangement, /*track*/1, /*start*/5.0, /*dur*/2.0);
    processor.getUndoManager().clear();

    ArrangementViewComponent view(processor);
    view.setBounds(0, 0, 1200, 600);
    applyViewportForTest(view, 0.0, kTestPixelsPerSecond);

    const auto beforeA = requirePlacement(*arrangement, 1, idA);

    // mouseDown on placement A center (time 6.0).
    const int downX = placementCenterX(beforeA);
    const int downY = yForTrack(view, processor, 1);
    const auto downEvent = makeMouseEvent(view, downX, downY, juce::ModifierKeys());
    view.mouseDown(downEvent);

    // Drag cursor to time -4.0 => deltaSeconds = -4.0 - 6.0 = -10.0
    // => target start = 5.0 + (-10.0) = -5.0, clamp to 0.0.
    const int dragX = xForTime(-4.0);
    const int dragY = yForTrack(view, processor, 1);
    mouseDragTo(view, downEvent, dragX, dragY);

    // DURING DRAG: model unchanged.
    expectPlacement(*arrangement, 1, idA, beforeA.timelineStartSeconds);

    // mouseUp commits with clamp.
    mouseUpAt(view, downEvent, dragX, dragY);
    expectPlacement(*arrangement, 1, idA, 0.0);   // clamped to 0.0

    // Undo restores original.
    processor.getUndoManager().undo();
    expectPlacement(*arrangement, 1, idA, beforeA.timelineStartSeconds);

    return !g_failed;
}

// ==================================================================
// Entry point
// ==================================================================
int main()
{
    juce::ScopedJuceInitialiser_GUI guiInitialiser;

    bool ok = true;
    ok &= arrangementMoveDrag_isTransientUntilMouseUp();
    ok &= arrangementMoveDrag_negativeTimeClamp();

    std::cout << (ok ? "ALL TESTS PASSED\n" : "TESTS FAILED\n");
    return ok ? 0 : 1;
}
