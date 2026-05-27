#include "StandalonePreferencePages.h"

#include <optional>

#include <juce_audio_utils/juce_audio_utils.h>

#include "Standalone/UI/UIColors.h"
#include "Editor/Preferences/SharedPreferencePages.h"

namespace OpenTune {

namespace {

void initialiseLabel(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setColour(juce::Label::textColourId, UIColors::textPrimary);
    label.setFont(UIColors::getUIFont(13.0f));
}

void initialiseComboBox(juce::ComboBox& comboBox)
{
    comboBox.setColour(juce::ComboBox::backgroundColourId, UIColors::backgroundMedium);
    comboBox.setColour(juce::ComboBox::textColourId, UIColors::textPrimary);
    comboBox.setColour(juce::ComboBox::outlineColourId, UIColors::panelBorder);
    comboBox.setColour(juce::ComboBox::arrowColourId, UIColors::accent);
}


class AudioSettingsPage final : public juce::Component
{
public:
    AudioSettingsPage(juce::AudioDeviceManager* audioDeviceManager,
                      std::unique_ptr<juce::Component> renderingPriorityComponent)
        : renderingPriorityComponent_(std::move(renderingPriorityComponent))
    {
        if (audioDeviceManager != nullptr) {
            audioSelector_ = std::make_unique<juce::AudioDeviceSelectorComponent>(*audioDeviceManager,
                                                                                  0,
                                                                                  256,
                                                                                  0,
                                                                                  256,
                                                                                  false,
                                                                                  false,
                                                                                  true,
                                                                                  false);
            applyThemeToChildren(*audioSelector_);
            audioSelector_->setItemHeight(26);
            addAndMakeVisible(audioSelector_.get());
        }

        if (renderingPriorityComponent_ != nullptr) {
            addAndMakeVisible(renderingPriorityComponent_.get());
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UIColors::backgroundDark);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(20);
        const int sharedAudioPageHeight = 76;
        if (renderingPriorityComponent_ != nullptr) {
            renderingPriorityComponent_->setBounds(bounds.removeFromTop(sharedAudioPageHeight));
            bounds.removeFromTop(8);
        }
        if (audioSelector_ != nullptr) {
            audioSelector_->setBounds(bounds);
        }
    }

private:
    static void applyThemeToChildren(juce::Component& root)
    {
        for (auto* child : root.getChildren()) {
            if (auto* cb = dynamic_cast<juce::ComboBox*>(child)) {
                cb->setColour(juce::ComboBox::backgroundColourId, UIColors::backgroundMedium);
                cb->setColour(juce::ComboBox::textColourId, UIColors::textPrimary);
                cb->setColour(juce::ComboBox::outlineColourId, UIColors::panelBorder);
                cb->setColour(juce::ComboBox::arrowColourId, UIColors::accent);
            } else if (auto* lb = dynamic_cast<juce::Label*>(child)) {
                lb->setColour(juce::Label::textColourId, UIColors::textPrimary);
                lb->setFont(UIColors::getUIFont(13.0f));
            } else if (auto* tb = dynamic_cast<juce::TextButton*>(child)) {
                tb->setColour(juce::TextButton::buttonColourId, UIColors::buttonNormal);
                tb->setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
            } else if (auto* listBox = dynamic_cast<juce::ListBox*>(child)) {
                listBox->setColour(juce::ListBox::backgroundColourId, UIColors::backgroundMedium);
                listBox->setColour(juce::ListBox::textColourId, UIColors::textPrimary);
                listBox->setColour(juce::ListBox::outlineColourId, UIColors::panelBorder);
            } else if (auto* toggle = dynamic_cast<juce::ToggleButton*>(child)) {
                toggle->setColour(juce::ToggleButton::textColourId, UIColors::textPrimary);
                toggle->setColour(juce::ToggleButton::tickColourId, UIColors::accent);
            }
            applyThemeToChildren(*child);
        }
    }

    std::unique_ptr<juce::AudioDeviceSelectorComponent> audioSelector_;
    std::unique_ptr<juce::Component> renderingPriorityComponent_;
};

class MouseTrailPage final : public juce::Component
{
public:
    MouseTrailPage(AppPreferences& appPreferences, std::function<void()> onPreferencesChanged)
        : appPreferences_(appPreferences)
        , onPreferencesChanged_(std::move(onPreferencesChanged))
    {
        initialiseLabel(themeLabel_, LOC(kMouseTrail));
        addAndMakeVisible(themeLabel_);

        themeSelector_.addItem(LOC(kOff), 1);
        themeSelector_.addItem(LOC(kClassic), 2);
        themeSelector_.addItem(LOC(kNeon), 3);
        themeSelector_.addItem(LOC(kFire), 4);
        themeSelector_.addItem(LOC(kOcean), 5);
        themeSelector_.addItem(LOC(kGalaxy), 6);
        themeSelector_.addItem(LOC(kCherryBlossom), 7);
        themeSelector_.addItem(LOC(kMatrix), 8);
        themeSelector_.setSelectedId(static_cast<int>(appPreferences_.getState().standalone.mouseTrailTheme) + 1,
                                     juce::dontSendNotification);
        themeSelector_.onChange = [this] {
            appPreferences_.setMouseTrailTheme(static_cast<MouseTrailConfig::TrailTheme>(themeSelector_.getSelectedId() - 1));
            notifyChanged();
        };
        initialiseComboBox(themeSelector_);
        addAndMakeVisible(themeSelector_);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UIColors::backgroundDark);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(20);
        auto row = bounds.removeFromTop(34);
        themeLabel_.setBounds(row.removeFromLeft(160));
        themeSelector_.setBounds(row.removeFromLeft(220).reduced(0, 4));
    }

private:
    void notifyChanged()
    {
        if (onPreferencesChanged_) {
            onPreferencesChanged_();
        }
    }

    AppPreferences& appPreferences_;
    std::function<void()> onPreferencesChanged_;
    juce::Label themeLabel_;
    juce::ComboBox themeSelector_;
};

} // namespace

std::vector<TabbedPreferencesDialog::PageSpec> StandalonePreferencePages::createAudioPages(
    juce::AudioDeviceManager* audioDeviceManager,
    AppPreferences& appPreferences,
    std::function<void()> onPreferencesChanged,
    std::function<void(bool forceCpu)> onRenderingPriorityChanged,
    std::function<void(VocoderModelWeight)> onVocoderModelWeightChanged)
{
    std::vector<TabbedPreferencesDialog::PageSpec> pages;
    auto renderingPriorityComponent = SharedPreferencePages::createRenderingPriorityComponent(
        appPreferences, onPreferencesChanged,
        std::move(onRenderingPriorityChanged),
        std::move(onVocoderModelWeightChanged));
    if (audioDeviceManager != nullptr) {
        pages.push_back({ LOC(kAudio), std::make_unique<AudioSettingsPage>(audioDeviceManager, std::move(renderingPriorityComponent)) });
    }
    return pages;
}

std::vector<TabbedPreferencesDialog::PageSpec> StandalonePreferencePages::createStandaloneOnlyPages(AppPreferences& appPreferences,
                                                                                                     std::function<void()> onPreferencesChanged)
{
    std::vector<TabbedPreferencesDialog::PageSpec> pages;
    pages.push_back({ LOC(kMouseTrail), std::make_unique<MouseTrailPage>(appPreferences, std::move(onPreferencesChanged)) });
    return pages;
}

} // namespace OpenTune
