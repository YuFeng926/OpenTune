#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include "../Utils/ZoomSensitivityConfig.h"
#include "UIColors.h"

namespace OpenTune {

class OptionsDialogComponent : public juce::Component
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void zoomSensitivityChanged() = 0;
    };

    OptionsDialogComponent()
    {
        addAndMakeVisible(tabbedComponent_);
        tabbedComponent_.setColour(juce::TabbedComponent::backgroundColourId, UIColors::backgroundDark);
        tabbedComponent_.setColour(juce::TabbedButtonBar::tabOutlineColourId, UIColors::panelBorder);
        tabbedComponent_.setTabBarDepth(30);
        
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
        {
            auto* audioPanel = new AudioSettingsPanel(holder);
            audioPanel->setSize(500, 420);
            tabbedComponent_.addTab("Audio", UIColors::backgroundDark, audioPanel, true);
        }
        
        auto* sensitivityPanel = new SensitivityPanel();
        sensitivityPanel->setSize(500, 220);
        tabbedComponent_.addTab("Keyswitch", UIColors::backgroundDark, sensitivityPanel, true);
        
        closeButton_.setButtonText("Close");
        closeButton_.setColour(juce::TextButton::buttonColourId, UIColors::accent);
        closeButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        closeButton_.onClick = [this] {
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState(0);
        };
        addAndMakeVisible(closeButton_);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UIColors::backgroundDark);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(15);
        
        auto tabBounds = bounds.withTrimmedBottom(40);
        tabbedComponent_.setBounds(tabBounds);
        
        closeButton_.setBounds(bounds.getRight() - 80, bounds.getBottom() - 32, 80, 28);
    }

    void addListener(Listener* listener) { listeners_.add(listener); }
    void removeListener(Listener* listener) { listeners_.remove(listener); }

private:
    class SensitivityPanel : public juce::Component
    {
    public:
        SensitivityPanel()
        {
            auto& settings = ZoomSensitivityConfig::getMutableSettings();
            
            horizontalZoomLabel_.setText("Horizontal Zoom Sensitivity", juce::dontSendNotification);
            horizontalZoomLabel_.setColour(juce::Label::textColourId, UIColors::textPrimary);
            horizontalZoomLabel_.setFont(UIColors::getUIFont(13.0f));
            addAndMakeVisible(horizontalZoomLabel_);
            
            horizontalZoomSlider_.setRange(ZoomSensitivityConfig::kMinHorizontalZoomFactor,
                                            ZoomSensitivityConfig::kMaxHorizontalZoomFactor,
                                            0.01);
            horizontalZoomSlider_.setValue(settings.horizontalZoomFactor, juce::dontSendNotification);
            horizontalZoomSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
            horizontalZoomSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
            horizontalZoomSlider_.setColour(juce::Slider::backgroundColourId, UIColors::backgroundMedium);
            horizontalZoomSlider_.setColour(juce::Slider::trackColourId, UIColors::accent);
            horizontalZoomSlider_.setColour(juce::Slider::thumbColourId, UIColors::textPrimary);
            addAndMakeVisible(horizontalZoomSlider_);
            
            verticalZoomLabel_.setText("Vertical Zoom Sensitivity", juce::dontSendNotification);
            verticalZoomLabel_.setColour(juce::Label::textColourId, UIColors::textPrimary);
            verticalZoomLabel_.setFont(UIColors::getUIFont(13.0f));
            addAndMakeVisible(verticalZoomLabel_);
            
            verticalZoomSlider_.setRange(ZoomSensitivityConfig::kMinVerticalZoomFactor,
                                          ZoomSensitivityConfig::kMaxVerticalZoomFactor,
                                          0.01);
            verticalZoomSlider_.setValue(settings.verticalZoomFactor, juce::dontSendNotification);
            verticalZoomSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
            verticalZoomSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
            verticalZoomSlider_.setColour(juce::Slider::backgroundColourId, UIColors::backgroundMedium);
            verticalZoomSlider_.setColour(juce::Slider::trackColourId, UIColors::accent);
            verticalZoomSlider_.setColour(juce::Slider::thumbColourId, UIColors::textPrimary);
            addAndMakeVisible(verticalZoomSlider_);
            
            scrollSpeedLabel_.setText("Scroll Speed", juce::dontSendNotification);
            scrollSpeedLabel_.setColour(juce::Label::textColourId, UIColors::textPrimary);
            scrollSpeedLabel_.setFont(UIColors::getUIFont(13.0f));
            addAndMakeVisible(scrollSpeedLabel_);
            
            scrollSpeedSlider_.setRange(ZoomSensitivityConfig::kMinScrollSpeed,
                                          ZoomSensitivityConfig::kMaxScrollSpeed,
                                          1.0);
            scrollSpeedSlider_.setValue(settings.scrollSpeed, juce::dontSendNotification);
            scrollSpeedSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
            scrollSpeedSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
            scrollSpeedSlider_.setColour(juce::Slider::backgroundColourId, UIColors::backgroundMedium);
            scrollSpeedSlider_.setColour(juce::Slider::trackColourId, UIColors::accent);
            scrollSpeedSlider_.setColour(juce::Slider::thumbColourId, UIColors::textPrimary);
            addAndMakeVisible(scrollSpeedSlider_);
            
            resetButton_.setButtonText("Reset to Defaults");
            resetButton_.setColour(juce::TextButton::buttonColourId, UIColors::buttonNormal);
            resetButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
            resetButton_.onClick = [this] {
                auto& s = ZoomSensitivityConfig::getMutableSettings();
                s = ZoomSensitivityConfig::ZoomSensitivitySettings::getDefault();
                horizontalZoomSlider_.setValue(s.horizontalZoomFactor, juce::dontSendNotification);
                verticalZoomSlider_.setValue(s.verticalZoomFactor, juce::dontSendNotification);
                scrollSpeedSlider_.setValue(s.scrollSpeed, juce::dontSendNotification);
            };
            addAndMakeVisible(resetButton_);
        }
        
        void paint(juce::Graphics& g) override
        {
            g.fillAll(UIColors::backgroundDark);
        }
        
        void resized() override
        {
            auto bounds = getLocalBounds().reduced(20);
            const int rowHeight = 35;
            const int labelWidth = 200;
            const int sliderWidth = bounds.getWidth() - labelWidth - 80;
            
            horizontalZoomLabel_.setBounds(bounds.getX(), bounds.getY(), labelWidth, rowHeight);
            horizontalZoomSlider_.setBounds(bounds.getX() + labelWidth, bounds.getY(), sliderWidth, rowHeight);
            bounds.removeFromTop(rowHeight + 10);
            
            verticalZoomLabel_.setBounds(bounds.getX(), bounds.getY(), labelWidth, rowHeight);
            verticalZoomSlider_.setBounds(bounds.getX() + labelWidth, bounds.getY(), sliderWidth, rowHeight);
            bounds.removeFromTop(rowHeight + 10);
            
            scrollSpeedLabel_.setBounds(bounds.getX(), bounds.getY(), labelWidth, rowHeight);
            scrollSpeedSlider_.setBounds(bounds.getX() + labelWidth, bounds.getY(), sliderWidth, rowHeight);
            bounds.removeFromTop(rowHeight + 20);
            
            resetButton_.setBounds(bounds.getX(), bounds.getY(), 120, 30);
        }
        
    private:
        juce::Label horizontalZoomLabel_;
        juce::Slider horizontalZoomSlider_;
        juce::Label verticalZoomLabel_;
        juce::Slider verticalZoomSlider_;
        juce::Label scrollSpeedLabel_;
        juce::Slider scrollSpeedSlider_;
        juce::TextButton resetButton_;
        
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SensitivityPanel)
    };
    
    class AudioSettingsPanel : public juce::Component
    {
    public:
        AudioSettingsPanel(juce::StandalonePluginHolder* holder)
        {
            if (holder)
            {
                auto* selector = new juce::AudioDeviceSelectorComponent(
                    holder->deviceManager,
                    0, 256,
                    0, 256,
                    false,
                    false,
                    true,
                    false
                );
                selector->setSize(450, 380);
                audioSelector_.reset(selector);
                addAndMakeVisible(audioSelector_.get());
            }
        }
        
        void paint(juce::Graphics& g) override
        {
            g.fillAll(UIColors::backgroundDark);
        }
        
        void resized() override
        {
            if (audioSelector_)
                audioSelector_->setBounds(getLocalBounds().reduced(10));
        }
        
    private:
        std::unique_ptr<juce::AudioDeviceSelectorComponent> audioSelector_;
        
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioSettingsPanel)
    };
    
    juce::TabbedComponent tabbedComponent_{juce::TabbedButtonBar::TabsAtTop};
    juce::TextButton closeButton_;
    
    juce::ListenerList<Listener> listeners_;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OptionsDialogComponent)
};

} // namespace OpenTune