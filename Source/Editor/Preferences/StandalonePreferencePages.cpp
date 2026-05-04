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

bool tryBuildCapturedBinding(const juce::KeyPress& key, KeyShortcutConfig::KeyBinding& outBinding)
{
    int keyCode = key.getKeyCode();
    if (keyCode <= 0) {
        return false;
    }

    if (keyCode >= 'a' && keyCode <= 'z') {
        keyCode = keyCode - ('a' - 'A');
    }

    juce::ModifierKeys modifiers;
    const auto keyModifiers = key.getModifiers();
    if (keyModifiers.isCtrlDown() || keyModifiers.isCommandDown()) {
        modifiers = modifiers.withFlags(juce::ModifierKeys::commandModifier);
    }
    if (keyModifiers.isShiftDown()) {
        modifiers = modifiers.withFlags(juce::ModifierKeys::shiftModifier);
    }
    if (keyModifiers.isAltDown()) {
        modifiers = modifiers.withFlags(juce::ModifierKeys::altModifier);
    }

    outBinding = KeyShortcutConfig::KeyBinding(keyCode, modifiers);
    return true;
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
        const int priorityRowHeight = 44;
        if (renderingPriorityComponent_ != nullptr) {
            renderingPriorityComponent_->setBounds(bounds.removeFromTop(priorityRowHeight));
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

class ShortcutSettingsPage final : public juce::Component
{
public:
    class CaptureWindow final : public juce::AlertWindow
    {
    public:
        CaptureWindow(KeyShortcutConfig::ShortcutId id,
                      const KeyShortcutConfig::ShortcutBinding& currentBinding,
                      juce::Component* associatedComponent)
            : juce::AlertWindow(LOC(kSetShortcut),
                                buildMessage(id, currentBinding),
                                juce::AlertWindow::NoIcon,
                                associatedComponent)
        {
            addButton(LOC(kCancel), 0);

            for (auto* child : getChildren()) {
                child->setWantsKeyboardFocus(false);
            }

            setWantsKeyboardFocus(true);
            grabKeyboardFocus();
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            KeyShortcutConfig::KeyBinding binding;
            if (!tryBuildCapturedBinding(key, binding)) {
                return true;
            }

            capturedBinding_ = binding;
            exitModalState(1);
            return true;
        }

        std::optional<KeyShortcutConfig::KeyBinding> takeCapturedBinding()
        {
            auto captured = capturedBinding_;
            capturedBinding_.reset();
            return captured;
        }

    private:
        static juce::String buildMessage(KeyShortcutConfig::ShortcutId id,
                                         const KeyShortcutConfig::ShortcutBinding& currentBinding)
        {
            juce::String message = KeyShortcutConfig::getShortcutDisplayName(id);
            message << "\n" << LOC(kPressNewKeyCombination);

            const auto currentBindingText = currentBinding.getDisplayNames();
            if (currentBindingText.isNotEmpty()) {
                message << "\n\n" << LOC(kCurrent) << ": " << currentBindingText;
            }

            return message;
        }

        std::optional<KeyShortcutConfig::KeyBinding> capturedBinding_;
    };

    ShortcutSettingsPage(AppPreferences& appPreferences, std::function<void()> onPreferencesChanged)
        : appPreferences_(appPreferences)
        , onPreferencesChanged_(std::move(onPreferencesChanged))
        , settings_(appPreferences_.getState().standalone.shortcuts)
    {
        for (size_t index = 0; index < KeyShortcutConfig::kShortcutCount; ++index) {
            auto id = static_cast<KeyShortcutConfig::ShortcutId>(index);

            auto* label = new juce::Label();
            initialiseLabel(*label, KeyShortcutConfig::getShortcutDisplayName(id));
            shortcutLabels_.add(label);
            addAndMakeVisible(label);

            auto* button = new juce::TextButton(KeyShortcutConfig::getShortcutBinding(settings_, id).getDisplayNames());
            button->setColour(juce::TextButton::buttonColourId, UIColors::backgroundMedium);
            button->setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
            button->onClick = [this, id] {
                beginCapture(id);
            };
            shortcutButtons_.add(button);
            addAndMakeVisible(button);
        }

        resetAllButton_.setButtonText(LOC(kResetAllToDefaults));
        resetAllButton_.setColour(juce::TextButton::buttonColourId, UIColors::buttonNormal);
        resetAllButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        resetAllButton_.onClick = [this] {
            KeyShortcutConfig::resetAllShortcutBindings(settings_);
            persist();
            refreshButtons();
        };
        addAndMakeVisible(resetAllButton_);
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
        const int buttonWidth = 220;

        for (size_t index = 0; index < KeyShortcutConfig::kShortcutCount; ++index) {
            auto row = bounds.removeFromTop(rowHeight);
            shortcutLabels_[static_cast<int>(index)]->setBounds(row.removeFromLeft(labelWidth));
            shortcutButtons_[static_cast<int>(index)]->setBounds(row.removeFromLeft(buttonWidth).reduced(0, 3));
            bounds.removeFromTop(6);
        }

        bounds.removeFromTop(12);
        resetAllButton_.setBounds(bounds.removeFromTop(28).removeFromLeft(180));
    }

private:
    void beginCapture(KeyShortcutConfig::ShortcutId id)
    {
        currentEditingId_ = id;
        captureWindow_ = std::make_unique<CaptureWindow>(id, KeyShortcutConfig::getShortcutBinding(settings_, id), this);

        juce::Component::SafePointer<ShortcutSettingsPage> safeThis(this);
        captureWindow_->enterModalState(true, juce::ModalCallbackFunction::create([safeThis](int result) {
                                           if (safeThis == nullptr) {
                                               return;
                                           }

                                           const auto capturedBinding = safeThis->captureWindow_ != nullptr
                                               ? safeThis->captureWindow_->takeCapturedBinding()
                                               : std::optional<KeyShortcutConfig::KeyBinding>{};
                                           safeThis->captureWindow_.reset();

                                           if (result == 1 && capturedBinding.has_value()) {
                                               safeThis->handleCapturedBinding(*capturedBinding);
                                               return;
                                           }

                                           safeThis->cancelCapture();
                                       }),
                                       false);
    }

    void handleCapturedBinding(const KeyShortcutConfig::KeyBinding& binding)
    {
        if (currentEditingId_ == KeyShortcutConfig::ShortcutId::Count) {
            return;
        }

        const auto conflict = KeyShortcutConfig::findConflictingShortcut(settings_, currentEditingId_, binding);
        if (conflict == KeyShortcutConfig::ShortcutId::Count) {
            applyBinding(binding);
            return;
        }

        auto options = juce::MessageBoxOptions::makeOptionsYesNo(juce::MessageBoxIconType::WarningIcon,
                                                                 LOC(kShortcutConflict),
                                                                 Loc::format(LOC_RAW(Loc::Keys::kShortcutConflictMessage),
                                                                             KeyShortcutConfig::getShortcutDisplayName(conflict)),
                                                                 LOC(kYes),
                                                                 LOC(kNo),
                                                                 this);
        juce::Component::SafePointer<ShortcutSettingsPage> safeThis(this);
        juce::AlertWindow::showAsync(options, [safeThis, binding, conflict](int result) {
            if (safeThis == nullptr) {
                return;
            }

            if (result == 1) {
                safeThis->settings_.bindings[static_cast<size_t>(conflict)].removeBinding(binding);
                safeThis->applyBinding(binding);
                return;
            }

            safeThis->cancelCapture();
        });
    }

    void applyBinding(const KeyShortcutConfig::KeyBinding& binding)
    {
        KeyShortcutConfig::setShortcutBinding(settings_, currentEditingId_, binding);
        persist();
        refreshButtons();
        cancelCapture();
    }

    void cancelCapture()
    {
        captureWindow_.reset();
        currentEditingId_ = KeyShortcutConfig::ShortcutId::Count;
    }

    void persist()
    {
        appPreferences_.setStandaloneShortcuts(settings_);
        if (onPreferencesChanged_) {
            onPreferencesChanged_();
        }
    }

    void refreshButtons()
    {
        for (size_t index = 0; index < KeyShortcutConfig::kShortcutCount; ++index) {
            auto id = static_cast<KeyShortcutConfig::ShortcutId>(index);
            if (auto* button = shortcutButtons_[static_cast<int>(index)]) {
                button->setButtonText(KeyShortcutConfig::getShortcutBinding(settings_, id).getDisplayNames());
            }
        }
    }

    AppPreferences& appPreferences_;
    std::function<void()> onPreferencesChanged_;
    KeyShortcutConfig::KeyShortcutSettings settings_;
    juce::OwnedArray<juce::Label> shortcutLabels_;
    juce::OwnedArray<juce::TextButton> shortcutButtons_;
    juce::TextButton resetAllButton_;
    std::unique_ptr<CaptureWindow> captureWindow_;
    KeyShortcutConfig::ShortcutId currentEditingId_ = KeyShortcutConfig::ShortcutId::Count;
};

} // namespace

std::vector<TabbedPreferencesDialog::PageSpec> StandalonePreferencePages::createAudioPages(
    juce::AudioDeviceManager* audioDeviceManager,
    AppPreferences& appPreferences,
    std::function<void()> onPreferencesChanged,
    std::function<void(bool forceCpu)> onRenderingPriorityChanged)
{
    std::vector<TabbedPreferencesDialog::PageSpec> pages;
    auto renderingPriorityComponent = SharedPreferencePages::createRenderingPriorityComponent(
        appPreferences, onPreferencesChanged, std::move(onRenderingPriorityChanged));
    if (audioDeviceManager != nullptr) {
        pages.push_back({ LOC(kAudio), std::make_unique<AudioSettingsPage>(audioDeviceManager, std::move(renderingPriorityComponent)) });
    }
    return pages;
}

std::vector<TabbedPreferencesDialog::PageSpec> StandalonePreferencePages::createStandaloneOnlyPages(AppPreferences& appPreferences,
                                                                                                     std::function<void()> onPreferencesChanged)
{
    std::vector<TabbedPreferencesDialog::PageSpec> pages;
    pages.push_back({ LOC(kKeyswitch), std::make_unique<ShortcutSettingsPage>(appPreferences, onPreferencesChanged) });
    pages.push_back({ LOC(kMouseTrail), std::make_unique<MouseTrailPage>(appPreferences, std::move(onPreferencesChanged)) });
    return pages;
}

} // namespace OpenTune
