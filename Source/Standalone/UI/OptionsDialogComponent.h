#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <functional>
#include "../Utils/ZoomSensitivityConfig.h"
#include "../../Utils/KeyShortcutConfig.h"
#include "../../Utils/LocalizationManager.h"
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
        virtual void languageChanged() {}
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
            tabbedComponent_.addTab(LOC(kAudio), UIColors::backgroundDark, audioPanel, true);
        }

        auto* sensitivityPanel = new SensitivityPanel();
        sensitivityPanel->setSize(500, 200);
        tabbedComponent_.addTab(LOC(kMouse), UIColors::backgroundDark, sensitivityPanel, true);

        auto* keyswitchPanel = new KeyswitchPanel();
        keyswitchPanel->setSize(500, 400);
        tabbedComponent_.addTab(LOC(kKeyswitch), UIColors::backgroundDark, keyswitchPanel, true);

        auto* languagePanel = new LanguagePanel();
        languagePanel->setSize(500, 150);
        tabbedComponent_.addTab(LOC(kLanguage), UIColors::backgroundDark, languagePanel, true);

        closeButton_.setButtonText(LOC(kClose));
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
    class LanguagePanel : public juce::Component
    {
    public:
        LanguagePanel()
        {
            languageLabel_.setText(LOC(kLanguageLabel), juce::dontSendNotification);
            languageLabel_.setColour(juce::Label::textColourId, UIColors::textPrimary);
            languageLabel_.setFont(UIColors::getUIFont(13.0f));
            addAndMakeVisible(languageLabel_);

            languageSelector_.addItem(getLanguageNativeName(Language::English), 1);
            languageSelector_.addItem(getLanguageNativeName(Language::Chinese), 2);
            languageSelector_.addItem(getLanguageNativeName(Language::Japanese), 3);
            languageSelector_.addItem(getLanguageNativeName(Language::Russian), 4);
            languageSelector_.addItem(getLanguageNativeName(Language::Spanish), 5);
            
            auto currentLang = LocalizationManager::getInstance().getLanguage();
            languageSelector_.setSelectedId(static_cast<int>(currentLang) + 1, juce::dontSendNotification);
            
            languageSelector_.onChange = [this] {
                int selectedId = languageSelector_.getSelectedId();
                Language newLang = static_cast<Language>(selectedId - 1);
                LocalizationManager::getInstance().setLanguage(newLang);
            };
            
            languageSelector_.setColour(juce::ComboBox::backgroundColourId, UIColors::backgroundMedium);
            languageSelector_.setColour(juce::ComboBox::textColourId, UIColors::textPrimary);
            languageSelector_.setColour(juce::ComboBox::outlineColourId, UIColors::panelBorder);
            languageSelector_.setColour(juce::ComboBox::arrowColourId, UIColors::accent);
            addAndMakeVisible(languageSelector_);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(UIColors::backgroundDark);
        }

        void resized() override
        {
            auto bounds = getLocalBounds().reduced(20);
            const int rowHeight = 35;
            const int labelWidth = 160;
            const int comboWidth = 200;

            languageLabel_.setBounds(bounds.getX(), bounds.getY(), labelWidth, rowHeight);
            languageSelector_.setBounds(bounds.getX() + labelWidth + 10, bounds.getY() + 5, comboWidth, 25);
        }

    private:
        juce::Label languageLabel_;
        juce::ComboBox languageSelector_;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LanguagePanel)
    };

    class SensitivityPanel : public juce::Component
    {
    public:
        SensitivityPanel()
        {
            auto& settings = ZoomSensitivityConfig::getMutableSettings();

            horizontalZoomLabel_.setText(LOC(kHorizontalZoomSensitivity), juce::dontSendNotification);
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

            verticalZoomLabel_.setText(LOC(kVerticalZoomSensitivity), juce::dontSendNotification);
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

            scrollSpeedLabel_.setText(LOC(kScrollSpeed), juce::dontSendNotification);
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

            resetButton_.setButtonText(LOC(kResetToDefaults));
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

    class KeyswitchPanel : public juce::Component
    {
    public:
        class CaptureOverlay : public juce::Component
        {
        public:
            using CaptureCallback = std::function<void(const KeyShortcutConfig::KeyBinding&)>;
            using CancelCallback = std::function<void()>;

            CaptureOverlay()
            {
                setVisible(false);
                setInterceptsMouseClicks(true, true);
                setWantsKeyboardFocus(true);

                titleLabel_.setText(LOC(kSetShortcut), juce::dontSendNotification);
                titleLabel_.setColour(juce::Label::textColourId, UIColors::textPrimary);
                titleLabel_.setFont(UIColors::getUIFont(20.0f));
                titleLabel_.setJustificationType(juce::Justification::centred);
                addAndMakeVisible(titleLabel_);

                actionLabel_.setColour(juce::Label::textColourId, UIColors::textSecondary);
                actionLabel_.setFont(UIColors::getUIFont(13.0f));
                actionLabel_.setJustificationType(juce::Justification::centred);
                addAndMakeVisible(actionLabel_);

                instructionLabel_.setText(LOC(kPressNewKeyCombination), juce::dontSendNotification);
                instructionLabel_.setColour(juce::Label::textColourId, UIColors::textPrimary);
                instructionLabel_.setFont(UIColors::getUIFont(14.0f));
                instructionLabel_.setJustificationType(juce::Justification::centred);
                addAndMakeVisible(instructionLabel_);

                currentBindingLabel_.setColour(juce::Label::textColourId, UIColors::textSecondary);
                currentBindingLabel_.setFont(UIColors::getUIFont(12.0f));
                currentBindingLabel_.setJustificationType(juce::Justification::centred);
                addAndMakeVisible(currentBindingLabel_);

                cancelButton_.setButtonText(LOC(kCancel));
                cancelButton_.setColour(juce::TextButton::buttonColourId, UIColors::buttonNormal);
                cancelButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
                cancelButton_.onClick = [this] { cancelCapture(); };
                addAndMakeVisible(cancelButton_);
            }

            void beginCapture(const juce::String& actionName,
                              const KeyShortcutConfig::ShortcutBinding& currentBinding,
                              CaptureCallback onCapture,
                              CancelCallback onCancel)
            {
                captureCallback_ = std::move(onCapture);
                cancelCallback_ = std::move(onCancel);

                actionLabel_.setText(LOC(kCurrent) + ": " + actionName, juce::dontSendNotification);
                currentBindingLabel_.setText(LOC(kCurrent) + ": " + currentBinding.getDisplayNames(), juce::dontSendNotification);
                setVisible(true);
                toFront(true);
                grabKeyboardFocus();
            }

            void endCapture()
            {
                setVisible(false);
                captureCallback_ = {};
                cancelCallback_ = {};
            }

            void paint(juce::Graphics& g) override
            {
                g.fillAll(juce::Colours::black.withAlpha(0.55f));

                auto panel = getPanelBounds();
                g.setColour(UIColors::backgroundDark);
                g.fillRoundedRectangle(panel.toFloat(), 8.0f);
                g.setColour(UIColors::panelBorder);
                g.drawRoundedRectangle(panel.toFloat(), 8.0f, 1.0f);
            }

            void resized() override
            {
                auto panel = getPanelBounds().reduced(16);
                titleLabel_.setBounds(panel.removeFromTop(30));
                panel.removeFromTop(4);
                actionLabel_.setBounds(panel.removeFromTop(22));
                panel.removeFromTop(8);
                instructionLabel_.setBounds(panel.removeFromTop(26));
                panel.removeFromTop(4);
                currentBindingLabel_.setBounds(panel.removeFromTop(20));
                panel.removeFromTop(12);
                cancelButton_.setBounds(panel.removeFromTop(28).withSizeKeepingCentre(90, 28));
            }

            bool keyPressed(const juce::KeyPress& key) override
            {
                if (!isVisible())
                    return false;

                int keyCode = key.getKeyCode();
                if (keyCode <= 0)
                    return true;

                if (keyCode >= 'a' && keyCode <= 'z')
                    keyCode = keyCode - ('a' - 'A');

                juce::ModifierKeys normalizedModifiers;
                auto keyModifiers = key.getModifiers();
                if (keyModifiers.isCtrlDown() || keyModifiers.isCommandDown())
                    normalizedModifiers = normalizedModifiers.withFlags(juce::ModifierKeys::commandModifier);
                if (keyModifiers.isShiftDown())
                    normalizedModifiers = normalizedModifiers.withFlags(juce::ModifierKeys::shiftModifier);
                if (keyModifiers.isAltDown())
                    normalizedModifiers = normalizedModifiers.withFlags(juce::ModifierKeys::altModifier);

                if (captureCallback_)
                    captureCallback_(KeyShortcutConfig::KeyBinding(keyCode, normalizedModifiers));

                return true;
            }

        private:
            void cancelCapture()
            {
                if (cancelCallback_)
                    cancelCallback_();
            }

            juce::Rectangle<int> getPanelBounds() const
            {
                return getLocalBounds().withSizeKeepingCentre(360, 170);
            }

            juce::Label titleLabel_;
            juce::Label actionLabel_;
            juce::Label instructionLabel_;
            juce::Label currentBindingLabel_;
            juce::TextButton cancelButton_;
            CaptureCallback captureCallback_;
            CancelCallback cancelCallback_;
        };

        KeyswitchPanel()
        {
            for (size_t i = 0; i < KeyShortcutConfig::kShortcutCount; ++i)
            {
                auto id = static_cast<KeyShortcutConfig::ShortcutId>(i);
                const auto& binding = KeyShortcutConfig::getShortcutBinding(id);

                auto* label = new juce::Label();
                label->setText(KeyShortcutConfig::getShortcutDisplayName(id), juce::dontSendNotification);
                label->setColour(juce::Label::textColourId, UIColors::textPrimary);
                label->setFont(UIColors::getUIFont(13.0f));
                shortcutLabels_.add(label);
                addAndMakeVisible(label);

                auto* button = new juce::TextButton(binding.getDisplayNames());
                button->setColour(juce::TextButton::buttonColourId, UIColors::backgroundMedium);
                button->setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
                button->onClick = [this, id, button] {
                    beginCaptureSession(id, button);
                };
                shortcutButtons_.add(button);
                addAndMakeVisible(button);
            }

            resetAllButton_.setButtonText(LOC(kResetAllToDefaults));
            resetAllButton_.setColour(juce::TextButton::buttonColourId, UIColors::buttonNormal);
            resetAllButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
            resetAllButton_.onClick = [this] {
                KeyShortcutConfig::resetAllShortcutBindings();
                refreshAllButtons();
            };
            addAndMakeVisible(resetAllButton_);

            addAndMakeVisible(captureOverlay_);
            captureOverlay_.setVisible(false);
        }

        void beginCaptureSession(KeyShortcutConfig::ShortcutId id, juce::TextButton* button)
        {
            currentEditingId_ = id;
            currentEditingButton_ = button;

            const auto currentBinding = KeyShortcutConfig::getShortcutBinding(id);

            captureOverlay_.beginCapture(
                KeyShortcutConfig::getShortcutDisplayName(id),
                currentBinding,
                [this](const KeyShortcutConfig::KeyBinding& binding) {
                    handleCapturedBinding(binding);
                },
                [this]() {
                    cancelCaptureSession();
                });
        }

        void handleCapturedBinding(const KeyShortcutConfig::KeyBinding& binding)
        {
            if (currentEditingId_ == KeyShortcutConfig::ShortcutId::Count)
                return;

            captureOverlay_.endCapture();

            auto conflict = KeyShortcutConfig::findConflictingShortcut(currentEditingId_, binding);
            if (conflict != KeyShortcutConfig::ShortcutId::Count)
            {
                auto conflictDisplayName = KeyShortcutConfig::getShortcutDisplayName(conflict);
                auto conflictMsg = LOC_RAW(Loc::Keys::kShortcutConflictMessage);
                conflictMsg = Loc::format(conflictMsg, conflictDisplayName);
                auto options = juce::MessageBoxOptions::makeOptionsYesNo(
                    juce::MessageBoxIconType::WarningIcon,
                    LOC(kShortcutConflict),
                    conflictMsg,
                    LOC(kYes), LOC(kNo), this
                );

                juce::Component::SafePointer<KeyswitchPanel> safeThis(this);
                juce::AlertWindow::showAsync(options, [safeThis, binding, conflict](int result) {
                    if (safeThis == nullptr)
                        return;
                    if (result == 1)
                    {
                        auto& settings = KeyShortcutConfig::getMutableSettings();
                        settings.bindings[static_cast<size_t>(conflict)].removeBinding(binding);
                        safeThis->applyKeyBinding(binding);
                    }
                    else
                    {
                        safeThis->cancelCaptureSession();
                    }
                });
                return;
            }

            applyKeyBinding(binding);
        }

        void cancelCaptureSession()
        {
            captureOverlay_.endCapture();
            currentEditingId_ = KeyShortcutConfig::ShortcutId::Count;
            currentEditingButton_ = nullptr;
        }

        void applyKeyBinding(const KeyShortcutConfig::KeyBinding& binding)
        {
            if (currentEditingId_ == KeyShortcutConfig::ShortcutId::Count)
                return;

            KeyShortcutConfig::setShortcutBinding(currentEditingId_, binding);
            if (currentEditingButton_)
            {
                auto shortcutBinding = KeyShortcutConfig::getShortcutBinding(currentEditingId_);
                currentEditingButton_->setButtonText(shortcutBinding.getDisplayNames());
            }
            currentEditingId_ = KeyShortcutConfig::ShortcutId::Count;
            currentEditingButton_ = nullptr;
        }

        void refreshAllButtons()
        {
            for (size_t i = 0; i < KeyShortcutConfig::kShortcutCount; ++i)
            {
                auto id = static_cast<KeyShortcutConfig::ShortcutId>(i);
                const auto& binding = KeyShortcutConfig::getShortcutBinding(id);
                if (shortcutButtons_[static_cast<int>(i)])
                    shortcutButtons_[static_cast<int>(i)]->setButtonText(binding.getDisplayNames());
            }
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(UIColors::backgroundDark);
        }

        void resized() override
        {
            auto bounds = getLocalBounds().reduced(20);
            const int rowHeight = 32;
            const int labelWidth = 180;
            const int buttonWidth = 200;
            const int buttonHeight = 26;

            for (size_t i = 0; i < KeyShortcutConfig::kShortcutCount; ++i)
            {
                auto y = bounds.getY() + static_cast<int>(i) * (rowHeight + 4);
                shortcutLabels_[static_cast<int>(i)]->setBounds(bounds.getX(), y, labelWidth, rowHeight);
                shortcutButtons_[static_cast<int>(i)]->setBounds(
                    bounds.getX() + labelWidth + 10, y + 3, buttonWidth, buttonHeight);
            }

            auto bottomY = bounds.getY() + static_cast<int>(KeyShortcutConfig::kShortcutCount) * (rowHeight + 4) + 20;
            resetAllButton_.setBounds(bounds.getX(), bottomY, 140, 28);

            captureOverlay_.setBounds(getLocalBounds());
        }

    private:
        juce::OwnedArray<juce::Label> shortcutLabels_;
        juce::OwnedArray<juce::TextButton> shortcutButtons_;
        juce::TextButton resetAllButton_;
        CaptureOverlay captureOverlay_;

        KeyShortcutConfig::ShortcutId currentEditingId_ = KeyShortcutConfig::ShortcutId::Count;
        juce::TextButton* currentEditingButton_ = nullptr;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KeyswitchPanel)
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
