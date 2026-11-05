#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <optional>
#include <utility>

namespace OpenTune::a11y {

// Slider or knob for the keyboard, with the same keys everywhere in the plugin:
// arrows step, Alt is a fine step, Page Up/Down a big one, Home = maximum and
// End = minimum (the user's rule), Backspace = default value, Enter = type a number
// (Enter keeps it, Esc drops it, Tab keeps it and goes to the next control).
// The stock JUCE slider moves by its interval (often 0.01) and has no Home/End.
//
// Changes from the arrow keys reach onCommit once the keys stop, so a held arrow is one
// undo step and one render; other changes reach it at once. Use onCommit, not onValueChange.
class AccessibleKnob : public juce::Slider, private juce::Timer
{
public:
    AccessibleKnob()
    {
        setWantsKeyboardFocus(true);
        // The value box is an editable Label: as a Tab stop it opened a text field
        // ("Retune Speed edit selected 15.00%"). A focus container keeps its children
        // out of the Tab order; Enter opens the field and the mouse can still click it.
        setFocusContainerType(FocusContainerType::keyboardFocusContainer);
    }

    ~AccessibleKnob() override { stopTimer(); }

    void setKeySteps(double normal, double fine, double big)
    {
        step_ = normal;
        fineStep_ = fine;
        bigStep_ = big;
    }

    // Backspace value when the slider has no double-click return value.
    void setResetValue(double value) { resetValue_ = value; }

    std::function<void()> onCommit;

    bool keyPressed(const juce::KeyPress& key) override
    {
        const int code = key.getKeyCode();
        const auto mods = key.getModifiers();

        if (auto* box = valueBox(); box != nullptr && box->isBeingEdited())
        {
            // The field takes the other keys itself; these bubble up from it.
            if (code == juce::KeyPress::tabKey)
            {
                tabAfterEditor_ = mods.isShiftDown() ? -1 : 1;
                box->hideEditor(false);
            }
            else if (code == juce::KeyPress::escapeKey)
            {
                box->hideEditor(true);
            }
            return true;
        }

        // The key that closed the field bubbles up here as well; it is done already.
        if (editorJustClosed_)
            return true;

        if (mods.isCtrlDown() || mods.isCommandDown() || mods.isShiftDown())
            return juce::Slider::keyPressed(key);

        if (code == juce::KeyPress::returnKey && !mods.isAltDown())
        {
            openValueEditor();
            return true;
        }

        const double small = mods.isAltDown() ? fineStep_ : step_;
        double target = getValue();
        if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey)
            target += small;
        else if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey)
            target -= small;
        else if (code == juce::KeyPress::pageUpKey)
            target += bigStep_;
        else if (code == juce::KeyPress::pageDownKey)
            target -= bigStep_;
        else if (code == juce::KeyPress::homeKey)
            target = getMaximum();
        else if (code == juce::KeyPress::endKey)
            target = getMinimum();
        else if (code == juce::KeyPress::backspaceKey && !mods.isAltDown())
            return resetToDefault();
        else
            return juce::Slider::keyPressed(key);

        target = juce::jlimit(getMinimum(), getMaximum(), target);
        // At the edge the value does not change and NVDA says nothing.
        if (target != getValue())
        {
            keyboardChange_ = true;
            // NVDA speaks the new value of the focused slider itself (JUCE value change event).
            setValue(target, juce::sendNotificationSync);
            keyboardChange_ = false;
        }
        return true;
    }

    void focusLost(FocusChangeType cause) override
    {
        commitPending();
        juce::Slider::focusLost(cause);
    }

private:
    juce::Label* valueBox() const
    {
        for (auto* child : getChildren())
            if (auto* label = dynamic_cast<juce::Label*>(child))
                return label;
        return nullptr;
    }

    void openValueEditor()
    {
        auto* box = valueBox();
        if (box == nullptr || !isTextBoxEditable())
            return;

        commitPending();
        juce::Component::SafePointer<AccessibleKnob> safeThis(this);
        // Set every time: the Label is recreated when the look and feel changes.
        box->onEditorHide = [safeThis] {
            if (safeThis == nullptr)
                return;
            safeThis->editorJustClosed_ = true;
            // Focus moves a tick later: moving it while the field is being removed
            // crashed REAPER in another plugin (skill pitfall 7).
            juce::MessageManager::callAsync([safeThis] {
                if (safeThis == nullptr)
                    return;
                safeThis->editorJustClosed_ = false;
                const int tab = std::exchange(safeThis->tabAfterEditor_, 0);
                if (!safeThis->isShowing())
                    return;
                // Not when a click already took focus somewhere else.
                auto* focused = juce::Component::getCurrentlyFocusedComponent();
                if (focused != nullptr && focused != safeThis.getComponent() && !safeThis->isParentOf(focused))
                    return;
                if (tab != 0)
                    safeThis->moveKeyboardFocusToSibling(tab > 0);
                else if (!safeThis->hasKeyboardFocus(false))
                    safeThis->grabKeyboardFocus();
            });
        };
        showTextBox();
        // Only a number can be typed; the unit is added back by the slider.
        if (auto* editor = box->getCurrentTextEditor())
            editor->setInputRestrictions(0, "-0123456789.");
    }

    bool resetToDefault()
    {
        std::optional<double> target = resetValue_;
        if (!target.has_value() && isDoubleClickReturnEnabled())
            target = getDoubleClickReturnValue();
        if (!target.has_value())
            return false;

        const double value = juce::jlimit(getMinimum(), getMaximum(), *target);
        if (value != getValue())
            setValue(value, juce::sendNotificationSync);
        return true;
    }

    void valueChanged() override
    {
        if (keyboardChange_)
        {
            startTimer(400);
            return;
        }
        stopTimer();
        if (onCommit)
            onCommit();
    }

    void timerCallback() override
    {
        stopTimer();
        if (onCommit)
            onCommit();
    }

    void commitPending()
    {
        if (isTimerRunning())
            timerCallback();
    }

    double step_ = 1.0;
    double fineStep_ = 0.1;
    double bigStep_ = 10.0;
    std::optional<double> resetValue_;
    bool keyboardChange_ = false;
    bool editorJustClosed_ = false;
    int tabAfterEditor_ = 0; // after Tab in the field: +1 next control, -1 previous
};

} // namespace OpenTune::a11y
