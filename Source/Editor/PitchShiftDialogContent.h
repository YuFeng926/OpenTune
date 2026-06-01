#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "../Utils/PitchShiftSettings.h"
#include "../Standalone/UI/UIColors.h"

namespace OpenTune {

/**
 * Pitch Shift 对话框内容组件
 *
 * 提供半音 [-24, +24] 和音分 [-99, +99] 两个整数步进滑块，
 * 以及 "确认" / "重置" 两个按钮。
 */
class PitchShiftDialogContent : public juce::Component
{
public:
    // ============================================================================
    // Listener
    // ============================================================================

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void pitchShiftConfirmed(const PitchShiftSettings& settings) { juce::ignoreUnused(settings); }
        virtual void pitchShiftReset() {}
    };

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
        addAndMakeVisible(semitoneSlider_);

        // -- Semitone label --
        semitoneLabel_.setText(juce::String::fromUTF8(u8"半音"), juce::dontSendNotification);
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
        addAndMakeVisible(centsSlider_);

        // -- Cents label --
        centsLabel_.setText(juce::String::fromUTF8(u8"音分"), juce::dontSendNotification);
        centsLabel_.setColour(juce::Label::textColourId, UIColors::textPrimary);
        centsLabel_.setFont(UIColors::getUIFont(14.0f));
        centsLabel_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(centsLabel_);

        // -- Reset button --
        resetButton_.setButtonText(juce::String::fromUTF8(u8"重置"));
        resetButton_.setColour(juce::TextButton::buttonColourId, UIColors::backgroundMedium);
        resetButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        resetButton_.setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);
        resetButton_.onClick = [this] {
            semitoneSlider_.setValue(0.0, juce::sendNotificationAsync);
            centsSlider_.setValue(0.0, juce::sendNotificationAsync);
            if (onReset_) {
                onReset_();
                closeParentDialog();
            } else {
                listeners_.call([](Listener& l) { l.pitchShiftReset(); });
            }
        };
        addAndMakeVisible(resetButton_);

        // -- Confirm button --
        confirmButton_.setButtonText(juce::String::fromUTF8(u8"确认"));
        confirmButton_.setColour(juce::TextButton::buttonColourId, UIColors::accent);
        confirmButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        confirmButton_.setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);
        confirmButton_.onClick = [this] {
            const auto settings = getSettings();
            if (onConfirm_) {
                onConfirm_(settings);
                closeParentDialog();
            } else {
                listeners_.call([&settings](Listener& l) { l.pitchShiftConfirmed(settings); });
            }
        };
        addAndMakeVisible(confirmButton_);
    }

    // ============================================================================
    // Listener management
    // ============================================================================

    void addListener(Listener* listener)
    {
        listeners_.add(listener);
    }

    void removeListener(Listener* listener)
    {
        listeners_.remove(listener);
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

    /** 设置确认回调（替代 Listener 模式，用于 lambda 捕获） */
    void setOnConfirm(std::function<void(const PitchShiftSettings&)> cb)
    {
        onConfirm_ = std::move(cb);
    }

    /** 设置重置回调 */
    void setOnReset(std::function<void()> cb)
    {
        onReset_ = std::move(cb);
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

private:
    // ============================================================================
    // Members
    // ============================================================================

    juce::Label semitoneLabel_;
    juce::Label centsLabel_;
    juce::Slider semitoneSlider_;
    juce::Slider centsSlider_;
    juce::TextButton resetButton_;
    juce::TextButton confirmButton_;
    juce::ListenerList<Listener> listeners_;
    std::function<void(const PitchShiftSettings&)> onConfirm_;
    std::function<void()> onReset_;

    /** 关闭所在 DialogWindow */
    void closeParentDialog()
    {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->closeButtonPressed();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchShiftDialogContent)
};

} // namespace OpenTune
