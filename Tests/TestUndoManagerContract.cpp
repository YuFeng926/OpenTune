/**
 * Tests/TestUndoManagerContract.cpp — Contract / integration tests for UndoManager + CompositeUndoAction
 *
 * Covers:
 *   - Nested CompositeUndoAction does not crash on undo
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
// Test: Nested composite does not crash
// ============================================================================

void runUndoManagerContractCompositeNotNestedTest()
{
    constexpr const char* testName = "UndoManagerContract_CompositeNotNested";

    // Create an inner composite with 1 sub-action
    auto inner = std::make_unique<CompositeUndoAction>("Inner");
    inner->addAction(std::make_unique<MockAction>("InnerSub"));

    // Create outer composite, add inner as a sub-action
    CompositeUndoAction outer("Outer");
    outer.addAction(std::move(inner));
    outer.addAction(std::make_unique<MockAction>("OuterSub"));

    // Calling undo() on the outer composite must not crash
    // (implementation may flat-merge or assert — test ensures no crash)
    outer.undo();

    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

void runUndoManagerContractSuite()
{
    logSection("UndoManagerContract");
    runUndoManagerContractCompositeNotNestedTest();
}
