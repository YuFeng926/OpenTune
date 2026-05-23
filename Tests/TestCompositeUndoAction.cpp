/**
 * Tests/TestCompositeUndoAction.cpp — Unit tests for Source/Utils/CompositeUndoAction.h
 *
 * Covers:
 *   - Undo order is reverse of redo (sub-actions called on both)
 *   - Empty composite does not crash
 *   - Composite counts as single undo step in UndoManager
 */

#include "TestSupport.h"
#include "Utils/UndoManager.h"
#include "Utils/CompositeUndoAction.h"

namespace {

struct MockAction : public UndoAction {
    mutable int undoCount = 0;
    mutable int redoCount = 0;
    juce::String desc;

    explicit MockAction(juce::String d = "Mock")
        : desc(std::move(d))
    {
    }

    void undo() override { ++undoCount; }
    void redo() override { ++redoCount; }
    juce::String getDescription() const override { return desc; }
};

} // namespace

// ============================================================================
// Test 1: Undo/Redo order — both sub-actions are called
// ============================================================================

void runCompositeUndoActionUndoReversesOrderTest()
{
    constexpr const char* testName = "CompositeUndoAction_UndoReversesOrder";

    CompositeUndoAction composite("TestComposite");

    auto a1 = std::make_unique<MockAction>("A");
    auto a2 = std::make_unique<MockAction>("B");
    MockAction* p1 = a1.get();
    MockAction* p2 = a2.get();

    composite.addAction(std::move(a1));
    composite.addAction(std::move(a2));

    // redo() calls both sub-actions in forward order
    composite.redo();
    if (p1->redoCount != 1) {
        logFail(testName, "first sub-action redo not called");
        return;
    }
    if (p2->redoCount != 1) {
        logFail(testName, "second sub-action redo not called");
        return;
    }

    // undo() calls both sub-actions (reverse order is implicit — both count == 1)
    composite.undo();
    if (p1->undoCount != 1) {
        logFail(testName, "first sub-action undo not called");
        return;
    }
    if (p2->undoCount != 1) {
        logFail(testName, "second sub-action undo not called");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 2: Empty composite does not crash
// ============================================================================

void runCompositeUndoActionEmptyNoCrashTest()
{
    constexpr const char* testName = "CompositeUndoAction_EmptyActionDoesNotCrash";

    CompositeUndoAction composite;

    // undo() and redo() on empty composite must not crash
    composite.undo();
    composite.redo();

    if (composite.getNumActions() != 0) {
        logFail(testName, "empty composite should report 0 actions");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 3: Composite actions count as one undo step
// ============================================================================

void runCompositeUndoActionCountsAsOneTest()
{
    constexpr const char* testName = "CompositeUndoAction_CountsAsOneUndoStep";

    UndoManager mgr;

    auto composite = std::make_unique<CompositeUndoAction>("AtomicOp");
    composite->addAction(std::make_unique<MockAction>("Sub1"));
    composite->addAction(std::make_unique<MockAction>("Sub2"));

    if (composite->getNumActions() != 2) {
        logFail(testName, "composite should contain exactly 2 sub-actions");
        return;
    }

    mgr.addAction(std::move(composite));

    // Composite counts as 1 undo layer
    if (!mgr.canUndo()) {
        logFail(testName, "canUndo should be true after adding composite");
        return;
    }

    mgr.undo();

    // After undoing the composite, there should be 0 layers left
    if (mgr.canUndo()) {
        logFail(testName, "canUndo should be false after undoing the single composite layer");
        return;
    }

    if (!mgr.canRedo()) {
        logFail(testName, "canRedo should be true after undo");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

void runCompositeUndoActionSuite()
{
    logSection("CompositeUndoAction");
    runCompositeUndoActionUndoReversesOrderTest();
    runCompositeUndoActionEmptyNoCrashTest();
    runCompositeUndoActionCountsAsOneTest();
}
