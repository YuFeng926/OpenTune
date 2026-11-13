#include <juce_gui_basics/juce_gui_basics.h>

#include "Utils/AccessibleKnob.h"

#include <cstdio>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>

 #include <array>
#endif

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void configure(OpenTune::a11y::AccessibleKnob& knob, int& commits)
{
    knob.setRange(0.0, 100.0, 1.0);
    knob.setKeySteps(1.0, 0.1, 10.0);
    knob.onCommit = [&commits] { ++commits; };
}

#if JUCE_WINDOWS
class ScopedKeyboardState
{
public:
    ScopedKeyboardState()
    {
        captured_ = ::GetKeyboardState(original_.data()) != FALSE;
        if (captured_)
            state_ = original_;
    }

    ~ScopedKeyboardState()
    {
        if (captured_)
            ::SetKeyboardState(original_.data());
    }

    bool isValid() const { return captured_; }

    bool setKeyDown(int virtualKey, bool isDown)
    {
        if (!captured_)
            return false;

        auto& keyState = state_[static_cast<std::size_t>(virtualKey)];
        if (isDown)
            keyState |= 0x80;
        else
            keyState &= 0x7f;
        return ::SetKeyboardState(state_.data()) != FALSE;
    }

private:
    std::array<BYTE, 256> original_{};
    std::array<BYTE, 256> state_{};
    bool captured_ = false;
};
#endif

void testKeyboardDraftAndReleaseCommit()
{
    OpenTune::a11y::AccessibleKnob knob;
    int commits = 0;
    configure(knob, commits);
    knob.setValue(20.0, juce::dontSendNotification);

    knob.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    check(knob.getValue() == 21.0, "key press synchronously updates the displayed value");
    check(commits == 0, "key press keeps its change pending");

    knob.keyPressed(juce::KeyPress(juce::KeyPress::pageUpKey));
    check(knob.getValue() == 31.0, "multiple key presses accumulate on the slider value");
    check(commits == 0, "repeated key presses remain one pending edit");

    knob.keyStateChanged(false);
    check(commits == 1, "key release commits the keyboard draft once");
    knob.keyStateChanged(false);
    knob.commitPending();
    check(commits == 1, "repeated end notifications add no commit");
}

void testSeparateTapsCommitSeparately()
{
    OpenTune::a11y::AccessibleKnob knob;
    int commits = 0;
    configure(knob, commits);

    knob.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    knob.keyStateChanged(false);
    knob.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    knob.keyStateChanged(false);
    check(commits == 2, "separate key taps commit separately");
}

void testQuantizedKeyboardChanges()
{
    OpenTune::a11y::AccessibleKnob knob;
    int commits = 0;
    configure(knob, commits);
    knob.setValue(10.0, juce::dontSendNotification);

    const juce::KeyPress fineRight(juce::KeyPress::rightKey,
                                   juce::ModifierKeys::altModifier, 0);
    knob.keyPressed(fineRight);
    check(knob.getValue() == 10.0 && commits == 0,
          "quantized fine-step value change creates no commit");

    knob.setValue(20.0, juce::dontSendNotification);
    knob.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    check(knob.getValue() == 21.0 && commits == 0, "keyboard change creates a pending draft");
    knob.keyPressed(fineRight);
    check(knob.getValue() == 21.0 && commits == 0,
          "quantized no-op preserves an existing keyboard draft");
    knob.keyStateChanged(false);
    check(commits == 1, "preserved keyboard draft commits on release");
}

void testCancelProjectionAndFocusLost()
{
    OpenTune::a11y::AccessibleKnob knob;
    int commits = 0;
    configure(knob, commits);

    knob.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    knob.cancelPending();
    knob.setValue(40.0, juce::dontSendNotification);
    knob.focusLost(juce::Component::focusChangedByTabKey);
    check(knob.getValue() == 40.0, "model projection updates the displayed value after cancel");
    check(commits == 0, "cancelled draft is not submitted by model projection or focus loss");
}

void testFocusLostCommitsPending()
{
    OpenTune::a11y::AccessibleKnob knob;
    int commits = 0;
    configure(knob, commits);

    knob.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    knob.focusLost(juce::Component::focusChangedByTabKey);
    check(commits == 1, "focus loss commits the active keyboard draft");
}

void testBackspaceAtDefaultStillCommits()
{
    OpenTune::a11y::AccessibleKnob knob;
    int commits = 0;
    configure(knob, commits);
    knob.setValue(49.0, juce::dontSendNotification);
    knob.setResetValue(50.1);

    knob.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    check(knob.getValue() == 50.0 && commits == 0, "keyboard draft reaches the quantized default");
    knob.keyPressed(juce::KeyPress(juce::KeyPress::backspaceKey));
    check(commits == 1, "Backspace commits a draft when default quantizes to the same value");
    knob.keyStateChanged(false);
    check(commits == 1, "key release adds no commit after Backspace");
    knob.keyPressed(juce::KeyPress(juce::KeyPress::backspaceKey));
    check(commits == 1, "idle quantized Backspace adds no empty commit");

    knob.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    knob.keyPressed(juce::KeyPress(juce::KeyPress::backspaceKey));
    check(knob.getValue() == 50.0 && commits == 2, "Backspace immediately resets a different draft");
    knob.keyStateChanged(false);
    check(commits == 2, "reset completes the pending gesture");
}

void testCommitClearsPendingBeforeCallback()
{
    OpenTune::a11y::AccessibleKnob knob;
    int commits = 0;
    configure(knob, commits);
    knob.onCommit = [&] {
        ++commits;
        knob.commitPending();
    };
    knob.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    knob.commitPending();
    check(commits == 1, "pending state ends before invoking the commit callback");
}

void testNonKeyboardChangeCommitsImmediately()
{
    OpenTune::a11y::AccessibleKnob knob;
    int commits = 0;
    configure(knob, commits);

    knob.setValue(12.0, juce::sendNotificationSync);
    check(commits == 1, "non-keyboard value changes commit immediately");
}

class NestedValueChangeListener : public juce::Slider::Listener
{
public:
    explicit NestedValueChangeListener(OpenTune::a11y::AccessibleKnob& knob) : knob_(knob) {}

    void sliderValueChanged(juce::Slider*) override
    {
        if (!changed_)
        {
            changed_ = true;
            knob_.setValue(30.0, juce::sendNotificationSync);
        }
    }

private:
    OpenTune::a11y::AccessibleKnob& knob_;
    bool changed_ = false;
};

void testNestedNonKeyboardChangeCommitsImmediately()
{
    OpenTune::a11y::AccessibleKnob knob;
    int commits = 0;
    configure(knob, commits);
    knob.setValue(20.0, juce::dontSendNotification);
    NestedValueChangeListener listener(knob);
    knob.addListener(&listener);

    knob.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    check(knob.getValue() == 30.0 && commits == 1,
          "synchronous nested non-keyboard change commits immediately");

    knob.removeListener(&listener);
}

void testAdjustmentBoundariesDoNotCommit()
{
    OpenTune::a11y::AccessibleKnob knob;
    int commits = 0;
    configure(knob, commits);

    knob.setValue(0.0, juce::dontSendNotification);
    knob.keyPressed(juce::KeyPress(juce::KeyPress::leftKey));
    knob.keyStateChanged(false);
    check(knob.getValue() == 0.0 && commits == 0, "minimum boundary no-op does not commit");

    knob.setValue(100.0, juce::dontSendNotification);
    knob.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    knob.keyStateChanged(false);
    check(knob.getValue() == 100.0 && commits == 0, "maximum boundary no-op does not commit");
}

#if JUCE_WINDOWS
void testWindowsKeyupUsesThreadKeyboardState()
{
    ScopedKeyboardState keyboardState;
    check(keyboardState.isValid(), "capture test-thread keyboard state");
    if (!keyboardState.isValid())
        return;

    OpenTune::a11y::AccessibleKnob knob;
    int commits = 0;
    configure(knob, commits);
    knob.setValue(20.0, juce::dontSendNotification);

    check(keyboardState.setKeyDown(VK_RIGHT, true), "set test-thread adjustment key down");
    knob.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    knob.keyStateChanged(false);
    check(commits == 0, "unrelated keyup preserves draft while adjustment key remains down");

    check(keyboardState.setKeyDown(VK_RIGHT, false), "set test-thread adjustment key up");
    knob.keyStateChanged(false);
    check(commits == 1, "adjustment key release commits the draft once");

    check(keyboardState.setKeyDown(VK_RIGHT, true), "set next test-thread tap down");
    knob.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    check(keyboardState.setKeyDown(VK_RIGHT, false), "set next test-thread tap up");
    knob.keyStateChanged(false);
    check(commits == 2, "next independent key tap commits separately");
}
#endif

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    testKeyboardDraftAndReleaseCommit();
    testSeparateTapsCommitSeparately();
    testQuantizedKeyboardChanges();
    testCancelProjectionAndFocusLost();
    testFocusLostCommitsPending();
    testBackspaceAtDefaultStillCommits();
    testCommitClearsPendingBeforeCallback();
    testNonKeyboardChangeCommitsImmediately();
    testNestedNonKeyboardChangeCommitsImmediately();
    testAdjustmentBoundariesDoNotCommit();
#if JUCE_WINDOWS
    testWindowsKeyupUsesThreadKeyboardState();
#endif

    if (failures == 0)
    {
        std::puts("AccessibleKnobTests: all checks passed.");
        return 0;
    }

    std::fprintf(stderr, "AccessibleKnobTests: %d check(s) failed.\n", failures);
    return 1;
}
