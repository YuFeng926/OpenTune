/**
 * Tests/TimeToolHandlerTests.cpp — Phase F unit tests for the Time tool
 * (vocal-time-stretch §8.4).
 *
 * Coverage:
 *   - hitTestTimeGridHandle: returns 0 when no grid / endpoints excluded
 *   - mouseDown on a draggable handle selects it + arms drag state
 *   - mouseDrag rebuilds working snapshot with new output_seconds (clamped)
 *   - mouseUp commits via ctx.commitTimeGrid with affected source range
 *   - mouseDown on empty area clears selection
 *
 * NOTE: PianoRollToolHandler::Context is heavyweight (~50 std::function fields).
 * To exercise the Time-tool path in isolation we provide a stub Context with
 * only the fields the Time-tool methods touch — the rest stay default-empty
 * (uninvoked).
 *
 * Suite aggregator: runTimeToolHandlerSuite() — registered in TestMain.cpp.
 */
#include "TestSupport.h"
#include "Standalone/UI/PianoRoll/InteractionState.h"
#include "Standalone/UI/PianoRoll/PianoRollToolHandler.h"
#include "Utils/TimeGrid.h"

#include <vector>

namespace {

constexpr int kTimeAxisPxPerSec = 100;          // matches PianoRoll default zoom
constexpr int kPianoKeyWidth    = 60;
constexpr int kRulerHeight      = 30;

// Build a non-identity grid with a draggable middle handle at output_seconds=0.5
std::shared_ptr<const TimeGridSnapshot> makeTestGridForTimeTool()
{
    std::vector<TimeHandle> handles = {
        {1, 0.0,  0.0,  HandleKind::ClipStart, true},
        {2, 0.5,  0.5,  HandleKind::OnsetVoiced, false},
        {3, 1.0,  1.0,  HandleKind::ClipEnd,   true},
    };
    return TimeGridSnapshot::makeFromHandles(std::move(handles), /*revision=*/2);
}

// Convenience: turn a time (seconds) into pixel-X using the test scale.
int timeToTestX(double t)
{
    return kPianoKeyWidth + static_cast<int>(t * kTimeAxisPxPerSec);
}
double testXToTime(int x)
{
    return static_cast<double>(x - kPianoKeyWidth) / static_cast<double>(kTimeAxisPxPerSec);
}

// Captures commitTimeGrid invocations for assertion.
struct CommitCapture {
    int callCount = 0;
    std::shared_ptr<const TimeGridSnapshot> lastNew;
    std::shared_ptr<const TimeGridSnapshot> lastOld;
    int64_t lastAffectedStart = -1;
    int64_t lastAffectedEnd   = -1;
    juce::String lastDescription;
};

PianoRollToolHandler::Context makeStubContextForTimeTool(
    InteractionState& state,
    std::shared_ptr<const TimeGridSnapshot>& currentGrid,
    CommitCapture& capture)
{
    PianoRollToolHandler::Context ctx;
    ctx.getState        = [&]() -> InteractionState& { return state; };
    ctx.xToTime         = [](int x) { return testXToTime(x); };
    ctx.timeToX         = [](double t) { return timeToTestX(t); };
    ctx.yToFreq         = [](float) { return 440.0f; };
    ctx.freqToY         = [](float) { return 0.0f; };
    ctx.getPianoKeyWidth = []() { return kPianoKeyWidth; };

    static const std::vector<Note> sEmptyNotes;
    ctx.getCommittedNotes = []() -> const std::vector<Note>& { return sEmptyNotes; };
    ctx.getDisplayNotes   = []() -> const std::vector<Note>& { return sEmptyNotes; };

    ctx.invalidateVisual = [](const juce::Rectangle<int>&) {};
    ctx.setMouseCursor   = [](const juce::MouseCursor&) {};
    ctx.grabKeyboardFocus = []() {};
    ctx.setCurrentTool   = [](ToolId) {};
    ctx.showToolSelectionMenu = []() {};
    ctx.notifyPlayheadChange = [](double) {};

    ctx.getMaterializationIdForView = []() { return uint64_t(42); };
    ctx.getTimeGridSnapshot = [&]() { return currentGrid; };
    ctx.commitTimeGrid = [&](std::shared_ptr<const TimeGridSnapshot> newSnap,
                              std::shared_ptr<const TimeGridSnapshot> oldSnap,
                              int64_t affectedStart, int64_t affectedEnd,
                              juce::String desc) -> bool {
        capture.callCount++;
        capture.lastNew = newSnap;
        capture.lastOld = oldSnap;
        capture.lastAffectedStart = affectedStart;
        capture.lastAffectedEnd   = affectedEnd;
        capture.lastDescription   = desc;
        currentGrid = newSnap;
        return true;
    };
    ctx.notifyTimeGridChanged = []() {};

    return ctx;
}

juce::MouseEvent makeMouseEvent(int x, int y, juce::Component& comp)
{
    juce::ModifierKeys mods;
    juce::Time eventTime;
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),
                            juce::Point<float>(static_cast<float>(x), static_cast<float>(y)),
                            mods, /*pressure=*/0.0f, /*orientation=*/0.0f,
                            /*rotation=*/0.0f, /*tiltX=*/0.0f, /*tiltY=*/0.0f,
                            &comp, &comp, eventTime, juce::Point<float>(),
                            eventTime, 1, false);
}

juce::MouseEvent makeMouseEventWithMods(int x, int y, juce::Component& comp, juce::ModifierKeys mods)
{
    juce::Time eventTime;
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),
                            juce::Point<float>(static_cast<float>(x), static_cast<float>(y)),
                            mods, /*pressure=*/0.0f, /*orientation=*/0.0f,
                            /*rotation=*/0.0f, /*tiltX=*/0.0f, /*tiltY=*/0.0f,
                            &comp, &comp, eventTime, juce::Point<float>(),
                            eventTime, 1, false);
}

// Five-handle grid for group-drag tests:
//   ClipStart(1) at 0  | h2 at 0.3 | h3 at 0.5 | h4 at 0.7 | ClipEnd(5) at 1.0
std::shared_ptr<const TimeGridSnapshot> makeFiveHandleGrid()
{
    std::vector<TimeHandle> handles = {
        {1, 0.0, 0.0, HandleKind::ClipStart, true},
        {2, 0.3, 0.3, HandleKind::OnsetVoiced, false},
        {3, 0.5, 0.5, HandleKind::OnsetVoiced, false},
        {4, 0.7, 0.7, HandleKind::OnsetVoiced, false},
        {5, 1.0, 1.0, HandleKind::ClipEnd,   true},
    };
    return TimeGridSnapshot::makeFromHandles(std::move(handles), /*revision=*/2);
}

} // namespace

// ============================================================================
// Tests
// ============================================================================

void runTimeTool_HitTestNullGridReturnsZeroTest()
{
    constexpr const char* testName = "TimeTool_HitTest_NullGridReturnsZero";

    InteractionState state;
    std::shared_ptr<const TimeGridSnapshot> grid;   // null
    CommitCapture cap;
    auto ctx = makeStubContextForTimeTool(state, grid, cap);

    PianoRollToolHandler handler(ctx);
    handler.setTool(ToolId::TimeTool);

    juce::Component dummy;
    auto e = makeMouseEvent(timeToTestX(0.5), 100, dummy);

    handler.mouseMove(e);   // should not crash, no hover detected
    if (state.timeTool.hoveredHandleId != 0) {
        logFail(testName, "hoveredHandleId should be 0 when grid is null");
        return;
    }
    logPass(testName);
}

void runTimeTool_HoverDetectsMiddleHandleTest()
{
    constexpr const char* testName = "TimeTool_Hover_DetectsMiddleHandle";

    InteractionState state;
    auto grid = makeTestGridForTimeTool();
    std::shared_ptr<const TimeGridSnapshot> currentGrid = grid;
    CommitCapture cap;
    auto ctx = makeStubContextForTimeTool(state, currentGrid, cap);

    PianoRollToolHandler handler(ctx);
    handler.setTool(ToolId::TimeTool);

    juce::Component dummy;
    // Mouse at handle 2's output_seconds=0.5 → x=110
    auto e = makeMouseEvent(timeToTestX(0.5), 100, dummy);
    handler.mouseMove(e);

    if (state.timeTool.hoveredHandleId != 2) {
        logFail(testName, ("expected hoveredHandleId=2, got "
                           + std::to_string(state.timeTool.hoveredHandleId)).c_str());
        return;
    }
    logPass(testName);
}

void runTimeTool_HoverIgnoresEndpointsTest()
{
    constexpr const char* testName = "TimeTool_Hover_IgnoresLockedEndpoints";

    InteractionState state;
    auto grid = makeTestGridForTimeTool();
    std::shared_ptr<const TimeGridSnapshot> currentGrid = grid;
    CommitCapture cap;
    auto ctx = makeStubContextForTimeTool(state, currentGrid, cap);

    PianoRollToolHandler handler(ctx);
    handler.setTool(ToolId::TimeTool);

    juce::Component dummy;
    // ClipStart at output=0.0 → x=60; should NOT be hover-targetable
    auto e = makeMouseEvent(timeToTestX(0.0), 100, dummy);
    handler.mouseMove(e);
    if (state.timeTool.hoveredHandleId != 0) {
        logFail(testName, "ClipStart endpoint should not be hover-targetable");
        return;
    }

    // ClipEnd at output=1.0 → x=160; should NOT be hover-targetable
    auto e2 = makeMouseEvent(timeToTestX(1.0), 100, dummy);
    handler.mouseMove(e2);
    if (state.timeTool.hoveredHandleId != 0) {
        logFail(testName, "ClipEnd endpoint should not be hover-targetable");
        return;
    }
    logPass(testName);
}

void runTimeTool_DragMutatesWorkingSnapshotTest()
{
    constexpr const char* testName = "TimeTool_Drag_MutatesWorkingSnapshot";

    InteractionState state;
    auto grid = makeTestGridForTimeTool();
    std::shared_ptr<const TimeGridSnapshot> currentGrid = grid;
    CommitCapture cap;
    auto ctx = makeStubContextForTimeTool(state, currentGrid, cap);

    PianoRollToolHandler handler(ctx);
    handler.setTool(ToolId::TimeTool);

    juce::Component dummy;
    auto down = makeMouseEvent(timeToTestX(0.5), 100, dummy);
    handler.mouseDown(down);
    if (!state.timeTool.isDraggingHandle || state.timeTool.draggedHandleId != 2) {
        logFail(testName, "mouseDown on handle 2 should arm drag state");
        return;
    }

    // Drag to t=0.7 (within bounds [0+ε, 1-ε])
    auto drag = makeMouseEvent(timeToTestX(0.7), 100, dummy);
    handler.mouseDrag(drag);

    if (state.timeTool.dragWorkingSnapshot == nullptr) {
        logFail(testName, "dragWorkingSnapshot should be non-null during drag");
        return;
    }
    if (state.timeTool.dragWorkingSnapshot.get() == grid.get()) {
        logFail(testName, "dragWorkingSnapshot should be a NEW snapshot, not original");
        return;
    }

    const auto& newHandles = state.timeTool.dragWorkingSnapshot->handles();
    if (newHandles.size() != 3) {
        logFail(testName, "working snapshot should preserve handle count");
        return;
    }
    if (std::abs(newHandles[1].output_seconds - 0.7) > 1e-3) {
        logFail(testName, ("middle handle output should be ~0.7 after drag, got "
                           + std::to_string(newHandles[1].output_seconds)).c_str());
        return;
    }
    if (std::abs(newHandles[1].source_seconds - 0.5) > 1e-9) {
        logFail(testName, "middle handle source_seconds must NOT change during drag");
        return;
    }
    logPass(testName);
}

void runTimeTool_DragClampedAtNeighborsTest()
{
    constexpr const char* testName = "TimeTool_Drag_ClampedAtNeighbors";

    InteractionState state;
    auto grid = makeTestGridForTimeTool();
    std::shared_ptr<const TimeGridSnapshot> currentGrid = grid;
    CommitCapture cap;
    auto ctx = makeStubContextForTimeTool(state, currentGrid, cap);

    PianoRollToolHandler handler(ctx);
    handler.setTool(ToolId::TimeTool);

    juce::Component dummy;
    handler.mouseDown(makeMouseEvent(timeToTestX(0.5), 100, dummy));

    // Drag past ClipEnd at 1.0 → should clamp to (1.0 - 30ms minimum spacing)
    // = 0.970.  Phase G tightened the clamp from 1ms to 30ms (per spec
    // time-tool-interaction.md "30ms minimum gap").
    handler.mouseDrag(makeMouseEvent(timeToTestX(2.0), 100, dummy));

    const double newOutput = state.timeTool.dragWorkingSnapshot->handles()[1].output_seconds;
    if (newOutput >= 1.0) {
        logFail(testName, "dragged output must stay strictly less than ClipEnd");
        return;
    }
    constexpr double kMinSpacingSec = 0.030;
    const double expected = 1.0 - kMinSpacingSec;
    if (std::abs(newOutput - expected) > 1e-3) {
        logFail(testName, ("dragged output should clamp to ~" + std::to_string(expected)
                           + " (1.0 - 30ms), got " + std::to_string(newOutput)).c_str());
        return;
    }
    logPass(testName);
}

void runTimeTool_MouseUpCommitsTest()
{
    constexpr const char* testName = "TimeTool_MouseUp_CommitsViaContext";

    InteractionState state;
    auto grid = makeTestGridForTimeTool();
    std::shared_ptr<const TimeGridSnapshot> currentGrid = grid;
    CommitCapture cap;
    auto ctx = makeStubContextForTimeTool(state, currentGrid, cap);

    PianoRollToolHandler handler(ctx);
    handler.setTool(ToolId::TimeTool);

    juce::Component dummy;
    handler.mouseDown(makeMouseEvent(timeToTestX(0.5), 100, dummy));
    handler.mouseDrag(makeMouseEvent(timeToTestX(0.7), 100, dummy));
    handler.mouseUp(makeMouseEvent(timeToTestX(0.7), 100, dummy));

    if (cap.callCount != 1) {
        logFail(testName, ("expected commitTimeGrid invoked once, got "
                           + std::to_string(cap.callCount)).c_str());
        return;
    }
    if (cap.lastNew == nullptr || cap.lastOld == nullptr) {
        logFail(testName, "commit should pass non-null new+old snapshots");
        return;
    }
    if (cap.lastDescription.isEmpty()) {
        logFail(testName, "commit description should be non-empty");
        return;
    }
    if (state.timeTool.isDraggingHandle) {
        logFail(testName, "mouseUp should clear isDraggingHandle");
        return;
    }
    logPass(testName);
}

void runTimeTool_MouseDownOnEmptyClearsSelectionTest()
{
    constexpr const char* testName = "TimeTool_MouseDownOnEmpty_ClearsSelection";

    InteractionState state;
    auto grid = makeTestGridForTimeTool();
    std::shared_ptr<const TimeGridSnapshot> currentGrid = grid;
    CommitCapture cap;
    auto ctx = makeStubContextForTimeTool(state, currentGrid, cap);

    PianoRollToolHandler handler(ctx);
    handler.setTool(ToolId::TimeTool);

    juce::Component dummy;
    // First select a handle
    handler.mouseDown(makeMouseEvent(timeToTestX(0.5), 100, dummy));
    handler.mouseUp  (makeMouseEvent(timeToTestX(0.5), 100, dummy));
    if (state.timeTool.selectedHandleId != 2) {
        logFail(testName, "precondition: handle should be selected after click+release");
        return;
    }

    // Click empty area
    handler.mouseDown(makeMouseEvent(timeToTestX(0.2), 100, dummy));

    if (state.timeTool.selectedHandleId != 0) {
        logFail(testName, "click on empty area should clear selection");
        return;
    }
    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

// ============================================================================
// Phase G — double-click insert + delete + 30ms spacing tests
// ============================================================================

void runTimeTool_DoubleClickInsertsHandleTest()
{
    constexpr const char* testName = "TimeTool_DoubleClick_InsertsUserAddedHandle";

    InteractionState state;
    auto grid = makeTestGridForTimeTool();
    std::shared_ptr<const TimeGridSnapshot> currentGrid = grid;
    CommitCapture cap;
    auto ctx = makeStubContextForTimeTool(state, currentGrid, cap);

    PianoRollToolHandler handler(ctx);
    handler.setTool(ToolId::TimeTool);

    juce::Component dummy;
    // Double-click at t=0.3 (between ClipStart=0 and middle handle=0.5)
    handler.mouseDoubleClick(makeMouseEvent(timeToTestX(0.3), 100, dummy));

    if (cap.callCount != 1) {
        logFail(testName, ("expected commitTimeGrid called once, got "
                           + std::to_string(cap.callCount)).c_str());
        return;
    }
    if (cap.lastNew == nullptr) { logFail(testName, "new snap is null"); return; }

    const auto& newHandles = cap.lastNew->handles();
    if (newHandles.size() != 4) {
        logFail(testName, ("expected 4 handles after insert (ClipStart + new + middle + ClipEnd), got "
                           + std::to_string(newHandles.size())).c_str());
        return;
    }

    // Verify the new handle is at t=0.3, kind=UserAdded, identity (src==out)
    const auto& inserted = newHandles[1];
    if (std::abs(inserted.source_seconds - 0.3) > 1e-3
        || std::abs(inserted.output_seconds - 0.3) > 1e-3) {
        logFail(testName, "inserted handle must be at clicked time, identity (src==out)");
        return;
    }
    if (inserted.kind != HandleKind::UserAdded) {
        logFail(testName, "inserted handle kind should be UserAdded");
        return;
    }
    if (inserted.locked) {
        logFail(testName, "inserted handle must not be locked");
        return;
    }
    logPass(testName);
}

void runTimeTool_DoubleClickRejectsTooCloseTest()
{
    constexpr const char* testName = "TimeTool_DoubleClick_RejectsWithin30msOfNeighbor";

    InteractionState state;
    auto grid = makeTestGridForTimeTool();
    std::shared_ptr<const TimeGridSnapshot> currentGrid = grid;
    CommitCapture cap;
    auto ctx = makeStubContextForTimeTool(state, currentGrid, cap);

    PianoRollToolHandler handler(ctx);
    handler.setTool(ToolId::TimeTool);

    juce::Component dummy;
    // Click at t=0.51, only 10ms from existing handle at t=0.5 → should reject
    handler.mouseDoubleClick(makeMouseEvent(timeToTestX(0.51), 100, dummy));

    if (cap.callCount != 0) {
        logFail(testName, "insert too close to neighbor must not commit");
        return;
    }
    logPass(testName);
}

void runTimeTool_DeleteSelectedRemovesHandleTest()
{
    constexpr const char* testName = "TimeTool_Delete_RemovesSelectedHandle";

    InteractionState state;
    auto grid = makeTestGridForTimeTool();
    std::shared_ptr<const TimeGridSnapshot> currentGrid = grid;
    CommitCapture cap;
    auto ctx = makeStubContextForTimeTool(state, currentGrid, cap);
    // Provide a stub for KeyShortcutSettings so keyPressed() doesn't crash.
    static const KeyShortcutConfig::KeyShortcutSettings sStubShortcuts;
    ctx.getShortcutSettings = []() -> const KeyShortcutConfig::KeyShortcutSettings& {
        return sStubShortcuts;
    };
    ctx.notifyEscapeKey = []() {};

    PianoRollToolHandler handler(ctx);
    handler.setTool(ToolId::TimeTool);

    juce::Component dummy;
    // Select the middle handle
    handler.mouseDown(makeMouseEvent(timeToTestX(0.5), 100, dummy));
    handler.mouseUp  (makeMouseEvent(timeToTestX(0.5), 100, dummy));
    if (state.timeTool.selectedHandleId != 2) {
        logFail(testName, "precondition: handle 2 should be selected");
        return;
    }

    // Use '1' alias (project-wide alias for delete) instead of deleteKey,
    // since the stub KeyShortcutSettings has no Delete binding registered.
    juce::KeyPress oneKey('1', juce::ModifierKeys{}, '1');
    handler.keyPressed(oneKey);

    if (cap.callCount != 1) {
        logFail(testName, ("expected commit called once, got "
                           + std::to_string(cap.callCount)).c_str());
        return;
    }
    if (cap.lastNew->handles().size() != 2) {
        logFail(testName, "after delete, only ClipStart + ClipEnd should remain");
        return;
    }
    if (state.timeTool.selectedHandleId != 0) {
        logFail(testName, "after delete, selection should be cleared");
        return;
    }
    logPass(testName);
}

void runTimeTool_DeleteEndpointRejectedTest()
{
    constexpr const char* testName = "TimeTool_Delete_EndpointRejected";

    InteractionState state;
    auto grid = makeTestGridForTimeTool();
    std::shared_ptr<const TimeGridSnapshot> currentGrid = grid;
    CommitCapture cap;
    auto ctx = makeStubContextForTimeTool(state, currentGrid, cap);
    static const KeyShortcutConfig::KeyShortcutSettings sStubShortcuts;
    ctx.getShortcutSettings = []() -> const KeyShortcutConfig::KeyShortcutSettings& {
        return sStubShortcuts;
    };
    ctx.notifyEscapeKey = []() {};

    PianoRollToolHandler handler(ctx);
    handler.setTool(ToolId::TimeTool);

    // Force-set selection to ClipStart (id=1, locked endpoint)
    state.timeTool.selectedHandleId = 1;

    juce::KeyPress oneKey('1', juce::ModifierKeys{}, '1');
    handler.keyPressed(oneKey);

    // Endpoint deletion should NOT trigger commit
    if (cap.callCount != 0) {
        logFail(testName, "deleting locked endpoint must not commit");
        return;
    }
    logPass(testName);
}

// ============================================================================
// Phase H — group drag (Shift multi-select + uniform-Δ shift)
// ============================================================================

void runTimeTool_GroupDragUniformDeltaTest()
{
    constexpr const char* testName = "TimeTool_GroupDrag_UniformDelta";

    InteractionState state;
    auto grid = makeFiveHandleGrid();
    std::shared_ptr<const TimeGridSnapshot> currentGrid = grid;
    CommitCapture cap;
    auto ctx = makeStubContextForTimeTool(state, currentGrid, cap);

    PianoRollToolHandler handler(ctx);
    handler.setTool(ToolId::TimeTool);

    juce::Component dummy;

    // Step 1: select handle 3 with a primary click.
    handler.mouseDown(makeMouseEvent(timeToTestX(0.5), 100, dummy));
    handler.mouseUp  (makeMouseEvent(timeToTestX(0.5), 100, dummy));
    if (state.timeTool.selectedHandleId != 3) {
        logFail(testName, "precondition: handle 3 should be primary-selected");
        return;
    }

    // Step 2: Shift+click handle 4 → adds id=4 to additionalSelectedIds.
    juce::ModifierKeys shift = juce::ModifierKeys{}.withFlags(juce::ModifierKeys::shiftModifier);
    handler.mouseDown(makeMouseEventWithMods(timeToTestX(0.7), 100, dummy, shift));
    handler.mouseUp  (makeMouseEventWithMods(timeToTestX(0.7), 100, dummy, shift));
    bool h4InSelection = false;
    for (auto id : state.timeTool.additionalSelectedIds) if (id == 4) h4InSelection = true;
    if (!h4InSelection) {
        logFail(testName, "shift+click should add handle 4 to additionalSelectedIds");
        return;
    }

    // Step 3: drag handle 3 by +0.05s.  Shift held during drag preserves
    // the multi-select (bare click on a handle clears additionalSelectedIds
    // per current Phase H toolHandler logic; matches DAW convention of
    // Shift+drag for group operations).
    handler.mouseDown(makeMouseEventWithMods(timeToTestX(0.5), 100, dummy, shift));
    handler.mouseDrag(makeMouseEventWithMods(timeToTestX(0.55), 100, dummy, shift));

    if (state.timeTool.dragWorkingSnapshot == nullptr) {
        logFail(testName, "dragWorkingSnapshot should be non-null during drag");
        return;
    }
    const auto& newH = state.timeTool.dragWorkingSnapshot->handles();
    if (newH.size() != 5) { logFail(testName, "snapshot must preserve 5 handles"); return; }

    const double dxH3 = newH[2].output_seconds - 0.5;
    const double dxH4 = newH[3].output_seconds - 0.7;

    // Δ should be ~0.05s on both selected handles (within clamp tolerance).
    if (std::abs(dxH3 - 0.05) > 1e-3) {
        logFail(testName, ("handle 3 should shift by ~0.05s (got " + std::to_string(dxH3) + ")").c_str());
        return;
    }
    if (std::abs(dxH4 - dxH3) > 1e-3) {
        logFail(testName, ("group drag should produce uniform Δ; "
                           "h3 Δ=" + std::to_string(dxH3)
                           + " h4 Δ=" + std::to_string(dxH4)).c_str());
        return;
    }
    // Non-selected middle handle 2 must NOT have moved.
    if (std::abs(newH[1].output_seconds - 0.3) > 1e-9) {
        logFail(testName, "non-selected handle 2 should not move during group drag");
        return;
    }
    logPass(testName);
}

void runTimeToolHandlerSuite()
{
    logSection("TimeTool-Handler");
    runTimeTool_HitTestNullGridReturnsZeroTest();
    runTimeTool_HoverDetectsMiddleHandleTest();
    runTimeTool_HoverIgnoresEndpointsTest();
    runTimeTool_DragMutatesWorkingSnapshotTest();
    runTimeTool_DragClampedAtNeighborsTest();
    runTimeTool_MouseUpCommitsTest();
    runTimeTool_MouseDownOnEmptyClearsSelectionTest();
    runTimeTool_DoubleClickInsertsHandleTest();
    runTimeTool_DoubleClickRejectsTooCloseTest();
    runTimeTool_DeleteSelectedRemovesHandleTest();
    runTimeTool_DeleteEndpointRejectedTest();
    runTimeTool_GroupDragUniformDeltaTest();
}
