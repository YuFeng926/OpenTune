#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "ComponentListenerSubscription.h"
#include "../Utils/PitchShiftSettings.h"
#include "../Utils/LocalizationManager.h"
#include "../Utils/ScreenReaderAnnouncer.h"
#include "../Standalone/UI/UIColors.h"

namespace OpenTune {

/**
 * Pitch Shift 对话框内容组件
 *
 * 提供半音 [-24, +24] 和音分 [-99, +99] 两个整数步进滑块，
 * 以及 "确认" / "重置" 两个按钮。
 */
class PitchShiftDialogContent : public juce::Component,
                                private juce::ComponentListener
{
public:
    // ============================================================================
    // Construction
    // ============================================================================

    explicit PitchShiftDialogContent(const PitchShiftSettings& initialSettings)
    {
        setSize(300, 180);

        // -- Semitone slider --
        semitoneSlider_.setSliderStyle(juce::Slider::LinearBar);
        semitoneSlider_.setRange(-24.0, 24.0, 1.0);
        semitoneSlider_.setValue(static_cast<double>(initialSettings.semitone), juce::dontSendNotification);
        semitoneSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
        semitoneSlider_.setColour(juce::Slider::trackColourId, UIColors::accent);
        semitoneSlider_.setColour(juce::Slider::backgroundColourId, UIColors::backgroundMedium);
        semitoneSlider_.setColour(juce::Slider::textBoxTextColourId, UIColors::textPrimary);
        semitoneSlider_.setColour(juce::Slider::textBoxBackgroundColourId, UIColors::backgroundDark);
        semitoneSlider_.setColour(juce::Slider::textBoxOutlineColourId, UIColors::panelBorder);
        semitoneSlider_.setTitle(LOC_RAW("Semitones"));
        semitoneSlider_.pageStep = 12.0; // Page Up/Down: one octave
        addAndMakeVisible(semitoneSlider_);

        // -- Semitone label --
        semitoneLabel_.setText(LOC_RAW("Semitones"), juce::dontSendNotification);
        semitoneLabel_.setColour(juce::Label::textColourId, UIColors::textPrimary);
        semitoneLabel_.setFont(UIColors::getUIFont(14.0f));
        semitoneLabel_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(semitoneLabel_);

        // -- Cents slider --
        centsSlider_.setSliderStyle(juce::Slider::LinearBar);
        centsSlider_.setRange(-99.0, 99.0, 1.0);
        centsSlider_.setValue(static_cast<double>(initialSettings.cents), juce::dontSendNotification);
        centsSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
        centsSlider_.setColour(juce::Slider::trackColourId, UIColors::accent);
        centsSlider_.setColour(juce::Slider::backgroundColourId, UIColors::backgroundMedium);
        centsSlider_.setColour(juce::Slider::textBoxTextColourId, UIColors::textPrimary);
        centsSlider_.setColour(juce::Slider::textBoxBackgroundColourId, UIColors::backgroundDark);
        centsSlider_.setColour(juce::Slider::textBoxOutlineColourId, UIColors::panelBorder);
        centsSlider_.setTitle(LOC_RAW("Cents"));
        centsSlider_.pageStep = 10.0;
        addAndMakeVisible(centsSlider_);

        // The sliders carry the names; the captions would be read a second time.
        semitoneLabel_.setAccessible(false);
        centsLabel_.setAccessible(false);
        keepValueBoxesOutOfTabOrder();

        // -- Cents label --
        centsLabel_.setText(LOC_RAW("Cents"), juce::dontSendNotification);
        centsLabel_.setColour(juce::Label::textColourId, UIColors::textPrimary);
        centsLabel_.setFont(UIColors::getUIFont(14.0f));
        centsLabel_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(centsLabel_);

        // -- Reset button --
        resetButton_.setButtonText(LOC_RAW("Reset"));
        resetButton_.setColour(juce::TextButton::buttonColourId, UIColors::backgroundMedium);
        resetButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        resetButton_.setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);
        resetButton_.onClick = [this] {
            if (actionTriggered_)
                return;
            actionTriggered_ = true;

            semitoneSlider_.setValue(0.0, juce::sendNotificationAsync);
            centsSlider_.setValue(0.0, juce::sendNotificationAsync);
            if (onReset_) {
                auto callback = std::move(onReset_);
                closeParentDialog();
                callback();
            }
            announceResult(PitchShiftSettings::identity());
        };
        addAndMakeVisible(resetButton_);

        // -- Confirm button --
        confirmButton_.setButtonText(LOC_RAW("Confirm"));
        confirmButton_.setColour(juce::TextButton::buttonColourId, UIColors::accent);
        confirmButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        confirmButton_.setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);
        confirmButton_.onClick = [this] {
            if (actionTriggered_)
                return;
            actionTriggered_ = true;

            const auto settings = getSettings();
            if (onConfirm_) {
                auto callback = std::move(onConfirm_);
                closeParentDialog();
                callback(settings);
            }
            announceResult(settings);
        };
        addAndMakeVisible(confirmButton_);
    }

    // ============================================================================
    // Public API
    // ============================================================================

    /** 从当前滑块值构建 PitchShiftSettings */
    PitchShiftSettings getSettings() const
    {
        PitchShiftSettings s;
        s.semitone = static_cast<int>(semitoneSlider_.getValue());
        s.cents = static_cast<int>(centsSlider_.getValue());
        return s;
    }

    /** 设置确认回调（用于 lambda 捕获） */
    void setOnConfirm(std::function<void(const PitchShiftSettings&)> cb)
    {
        onConfirm_ = std::move(cb);
    }

    /** 设置重置回调 */
    void setOnReset(std::function<void()> cb)
    {
        onReset_ = std::move(cb);
    }

    /** 注册父编辑器组件；父组件销毁时清空回调并关闭本对话框，避免回调访问已销毁对象 */
    void setDialogParent(juce::Component* parent)
    {
        if (parent != nullptr)
            parentSubscription_.watch(*parent);
        else
            parentSubscription_.reset();
        dialogParent_ = parent;
    }

    // ============================================================================
    // Component overrides
    // ============================================================================

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UIColors::backgroundDark);
    }

    void resized() override
    {
        constexpr int margin = 16;
        constexpr int rowHeight = 30;
        constexpr int rowGap = 10;
        constexpr int labelWidth = 80;
        constexpr int buttonHeight = 28;
        constexpr int buttonWidth = 90;
        constexpr int buttonGap = 8;

        auto bounds = getLocalBounds().reduced(margin);

        // Row 1: Semitone
        auto semitoneRow = bounds.removeFromTop(rowHeight);
        semitoneLabel_.setBounds(semitoneRow.removeFromLeft(labelWidth));
        semitoneSlider_.setBounds(semitoneRow);

        bounds.removeFromTop(rowGap);

        // Row 2: Cents
        auto centsRow = bounds.removeFromTop(rowHeight);
        centsLabel_.setBounds(centsRow.removeFromLeft(labelWidth));
        centsSlider_.setBounds(centsRow);

        // Buttons — centered at the bottom
        const int totalButtonWidth = buttonWidth * 2 + buttonGap;
        const int buttonY = getHeight() - margin - buttonHeight;
        const int buttonX = (getWidth() - totalButtonWidth) / 2;

        resetButton_.setBounds(buttonX, buttonY, buttonWidth, buttonHeight);
        confirmButton_.setBounds(buttonX + buttonWidth + buttonGap, buttonY, buttonWidth, buttonHeight);
    }

    /** 非原生标题栏时移除 DialogWindow 默认标题栏，仅保留主题内容 */
    void parentHierarchyChanged() override
    {
        if (auto* dialogWindow = findParentComponentOfClass<juce::DialogWindow>())
        {
            if (! dialogWindow->isUsingNativeTitleBar())
            {
                const int contentWidth = getWidth();
                const int contentHeight = getHeight();
                dialogWindow->setTitleBarHeight(0);
                dialogWindow->setContentComponentSize(contentWidth, contentHeight);
            }
        }

        // Moving into the window can recreate the value boxes (look-and-feel change),
        // and launchAsync gives focus to the window first, so finish this a tick later.
        juce::Component::SafePointer<PitchShiftDialogContent> safeThis(this);
        juce::MessageManager::callAsync([safeThis] {
            if (safeThis == nullptr || !safeThis->isShowing())
                return;
            safeThis->keepValueBoxesOutOfTabOrder();
            // Only the first time: parentHierarchyChanged fires more than once, and a repeated
            // grab would make NVDA repeat the focus or pull focus back from another control.
            if (!safeThis->initialFocusDone_) {
                safeThis->initialFocusDone_ = true;
                safeThis->semitoneSlider_.grabKeyboardFocus();
            }
        });
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        // Keys bubble up here from the focused slider or button.
        if (key == juce::KeyPress::spaceKey) {
            for (auto* button : { &resetButton_, &confirmButton_ }) {
                if (button->hasKeyboardFocus(false)) {
                    button->triggerClick();
                    return true;
                }
            }
        }
        // Enter on a slider confirms, as the default button of a dialog.
        if (key == juce::KeyPress::returnKey) {
            confirmButton_.triggerClick();
            return true;
        }
        return false;
    }

private:
    // ============================================================================
    // Members
    // ============================================================================

    juce::Label semitoneLabel_;
    juce::Label centsLabel_;
    // Keyboard slider: arrows change by 1, Page Up/Down by pageStep, Home/End go to the limits,
    // Backspace to 0. Enter stays the dialog's OK, as in every dialog.
    // The new value is spoken, because NVDA does not read JUCE's own value change here.
    class StepSlider final : public juce::Slider
    {
    public:
        double pageStep = 10.0;

        StepSlider() { setWantsKeyboardFocus(true); }

        bool keyPressed(const juce::KeyPress& key) override
        {
            if (key.getModifiers().isAnyModifierKeyDown())
                return juce::Slider::keyPressed(key);

            const int code = key.getKeyCode();
            double target = getValue();
            if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey)        target += 1.0;
            else if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey)  target -= 1.0;
            else if (code == juce::KeyPress::pageUpKey)                                   target += pageStep;
            else if (code == juce::KeyPress::pageDownKey)                                 target -= pageStep;
            // Home is the top and End the bottom, as in the EQ knobs (user's choice).
            else if (code == juce::KeyPress::homeKey)                                     target = getMaximum();
            else if (code == juce::KeyPress::endKey)                                      target = getMinimum();
            else if (code == juce::KeyPress::backspaceKey)                                target = 0.0;
            else
                return juce::Slider::keyPressed(key);

            target = juce::jlimit(getMinimum(), getMaximum(), target);
            // At a limit stay silent instead of repeating the same value.
            if (target != getValue()) {
                setValue(target, juce::sendNotificationSync);
                a11y::announce(*this, signedText(getValue()));
            }
            return true;
        }
    };

    static juce::String signedText(double value)
    {
        const int v = juce::roundToInt(value);
        return v > 0 ? "+" + juce::String(v) : juce::String(v);
    }

    StepSlider semitoneSlider_;
    StepSlider centsSlider_;
    juce::TextButton resetButton_;
    juce::TextButton confirmButton_;
    std::function<void(const PitchShiftSettings&)> onConfirm_;
    std::function<void()> onReset_;
    bool actionTriggered_ = false;
    bool closing_ = false;
    ComponentListenerSubscription parentSubscription_{*this};
    juce::Component::SafePointer<juce::Component> dialogParent_;
    bool initialFocusDone_ = false;

    // The editable value box is a Label that takes focus and opens an unnamed text
    // editor: a second Tab stop per slider. The mouse can still edit it.
    void keepValueBoxesOutOfTabOrder()
    {
        for (auto* slider : { &semitoneSlider_, &centsSlider_ })
            for (auto* child : slider->getChildren())
                if (auto* valueBox = dynamic_cast<juce::Label*>(child))
                    valueBox->setWantsKeyboardFocus(false);
    }

    // The dialog closes without a trace, so say what was applied. keepAll queues it
    // after the focus speech of the editor.
    void announceResult(const PitchShiftSettings& settings)
    {
        if (dialogParent_ == nullptr || !dialogParent_->isShowing())
            return;
        a11y::announce(*dialogParent_,
                       LOC(kPitchShift) + ": " + LOC_RAW("Semitones") + " " + signedText(settings.semitone)
                           + ", " + LOC_RAW("Cents") + " " + signedText(settings.cents),
                       a11y::SpeechQueue::keepAll);
    }

    void componentBeingDeleted(juce::Component& comp) override
    {
        if (!parentSubscription_.resetIfWatching(comp))
            return;
        onConfirm_ = nullptr;
        onReset_ = nullptr;
        closeParentDialog();
    }

    /** 关闭所在 DialogWindow */
    void closeParentDialog()
    {
        if (closing_)
            return;

        closing_ = true;
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(0);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchShiftDialogContent)
};

} // namespace OpenTune
