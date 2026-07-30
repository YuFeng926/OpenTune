#include "SharedPreferencePages.h"

#include <optional>

#include <juce_audio_utils/juce_audio_utils.h>

#include "Standalone/UI/UIColors.h"
#include "Utils/KeyShortcutConfig.h"
#include "Utils/LocalizationManager.h"

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

void initialiseSlider(juce::Slider& slider)
{
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 22);
    slider.setColour(juce::Slider::backgroundColourId, UIColors::backgroundMedium);
    slider.setColour(juce::Slider::trackColourId, UIColors::accent);
    slider.setColour(juce::Slider::thumbColourId, UIColors::textPrimary);
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

void initialiseToggleButton(juce::ToggleButton& toggleButton)
{
    toggleButton.setColour(juce::ToggleButton::textColourId, UIColors::textPrimary);
}

class SharedGeneralPage final : public juce::Component
{
public:
    SharedGeneralPage(AppPreferences& appPreferences,
                      std::function<void()> onPreferencesChanged)
        : appPreferences_(appPreferences)
        , onPreferencesChanged_(std::move(onPreferencesChanged))
    {
        auto state = appPreferences_.getState();

        initialiseLabel(themeLabel_, LOC(kTheme));
        addAndMakeVisible(themeLabel_);

        themeSelector_.addItem(LOC(kThemeBlueBreeze), 1);
        themeSelector_.addItem(LOC(kThemeDarkBlueGrey), 2);
        themeSelector_.addItem(LOC(kThemeAurora), 3);
        // themeSelector_.addItem(LOC(kThemeOverdose), 4);  // "升天" 主题暂时隐藏
        const int themeIdx = static_cast<int>(state.shared.theme);
        themeSelector_.setSelectedId(themeIdx >= static_cast<int>(ThemeId::Overdose) ? 1 : themeIdx + 1, juce::dontSendNotification);
        themeSelector_.onChange = [this] {
            appPreferences_.setTheme(static_cast<ThemeId>(themeSelector_.getSelectedId() - 1));
            notifyChanged();
        };
        initialiseComboBox(themeSelector_);
        addAndMakeVisible(themeSelector_);

        initialiseLabel(languageLabel_, LOC(kLanguageLabel));
        addAndMakeVisible(languageLabel_);

        languageSelector_.addItem(getLanguageNativeName(Language::English), 1);
        languageSelector_.addItem(getLanguageNativeName(Language::Chinese), 2);
        languageSelector_.addItem(getLanguageNativeName(Language::Japanese), 3);
        languageSelector_.addItem(getLanguageNativeName(Language::Russian), 4);
        languageSelector_.addItem(getLanguageNativeName(Language::Spanish), 5);
        languageSelector_.setSelectedId(static_cast<int>(state.shared.language) + 1, juce::dontSendNotification);
        languageSelector_.onChange = [this] {
            appPreferences_.setLanguage(static_cast<Language>(languageSelector_.getSelectedId() - 1));
            notifyChanged();
        };
        initialiseComboBox(languageSelector_);
        addAndMakeVisible(languageSelector_);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UIColors::backgroundDark);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(20);
        const int rowHeight = 34;
        const int labelWidth = 160;

        auto row = bounds.removeFromTop(rowHeight);
        themeLabel_.setBounds(row.removeFromLeft(labelWidth));
        themeSelector_.setBounds(row.removeFromLeft(240).reduced(0, 4));

        bounds.removeFromTop(12);
        row = bounds.removeFromTop(rowHeight);
        languageLabel_.setBounds(row.removeFromLeft(labelWidth));
        languageSelector_.setBounds(row.removeFromLeft(240).reduced(0, 4));
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
    juce::Label languageLabel_;
    juce::ComboBox languageSelector_;
};

class SharedAudioPage final : public juce::Component
{
public:
    SharedAudioPage(AppPreferences& appPreferences,
                    std::function<void()> onPreferencesChanged,
                    std::function<void(bool)> onRenderingPriorityChanged,
                    std::function<void(VocoderModelWeight)> onVocoderModelWeightChanged)
        : appPreferences_(appPreferences)
        , onPreferencesChanged_(std::move(onPreferencesChanged))
        , onRenderingPriorityChanged_(std::move(onRenderingPriorityChanged))
        , onVocoderModelWeightChanged_(std::move(onVocoderModelWeightChanged))
    {
        auto state = appPreferences_.getState();

        initialiseLabel(renderingPriorityLabel_, LOC(kRenderingPriority));
        addAndMakeVisible(renderingPriorityLabel_);

        renderingPrioritySelector_.addItem(LOC(kGpuFirst), 1);
        renderingPrioritySelector_.addItem(LOC(kCpuFirst), 2);
        renderingPrioritySelector_.setSelectedId(
            static_cast<int>(state.shared.renderingPriority) + 1,
            juce::dontSendNotification);
        renderingPrioritySelector_.onChange = [this] {
            const auto priority = static_cast<RenderingPriority>(
                renderingPrioritySelector_.getSelectedId() - 1);
            appPreferences_.setRenderingPriority(priority);
            if (onRenderingPriorityChanged_) {
                onRenderingPriorityChanged_(priority == RenderingPriority::CpuFirst);
            }
            notifyChanged();
        };
        initialiseComboBox(renderingPrioritySelector_);
        addAndMakeVisible(renderingPrioritySelector_);

        initialiseLabel(vocoderWeightLabel_, LOC(kVocoderWeight));
        addAndMakeVisible(vocoderWeightLabel_);

        vocoderWeightSelector_.addItem(LOC(kVocoderWeightCommunity), 1);
        vocoderWeightSelector_.addItem(LOC(kVocoderWeightCoulin9), 2);
        auto weight = appPreferences_.getState().shared.vocoderModelWeight;
        vocoderWeightSelector_.setSelectedId(static_cast<int>(weight) + 1, juce::dontSendNotification);
        initialiseComboBox(vocoderWeightSelector_);
        addAndMakeVisible(vocoderWeightSelector_);

        vocoderWeightSelector_.onChange = [this] {
            const auto w = static_cast<VocoderModelWeight>(vocoderWeightSelector_.getSelectedId() - 1);
            appPreferences_.setVocoderModelWeight(w);
            if (onVocoderModelWeightChanged_)
                onVocoderModelWeightChanged_(w);
            if (onPreferencesChanged_)
                onPreferencesChanged_();
        };

        initialiseToggleButton(experimentalFeaturesToggle_);
        experimentalFeaturesToggle_.setButtonText(juce::String::fromUTF8(u8"启用实验性功能（参考轨、伸缩工具）"));
        experimentalFeaturesToggle_.setToggleState(state.shared.experimentalFeaturesEnabled,
                                                   juce::dontSendNotification);
        experimentalFeaturesToggle_.onClick = [this] {
            appPreferences_.setExperimentalFeaturesEnabled(experimentalFeaturesToggle_.getToggleState());
            notifyChanged();
        };
        addAndMakeVisible(experimentalFeaturesToggle_);

        initialiseLabel(experimentalFeaturesHintLabel_,
                        juce::String::fromUTF8(u8"提示：参考轨与伸缩工具目前仍不完善，属于实验性功能，可能存在 Bug。"));
        experimentalFeaturesHintLabel_.setColour(juce::Label::textColourId, UIColors::textSecondary);
        experimentalFeaturesHintLabel_.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(experimentalFeaturesHintLabel_);

        initialiseLabel(experimentalReferenceAlignModeLabel_, juce::String::fromUTF8(u8"AUTO Ref 模式"));
        addAndMakeVisible(experimentalReferenceAlignModeLabel_);

        experimentalReferenceAlignModeSelector_.addItem(juce::String::fromUTF8(u8"普通 AUTO"), 1);
        experimentalReferenceAlignModeSelector_.addItem(juce::String::fromUTF8(u8"GAME"), 2);
        experimentalReferenceAlignModeSelector_.setSelectedId(
            static_cast<int>(state.shared.experimentalReferenceAlignMode) + 1,
            juce::dontSendNotification);
        experimentalReferenceAlignModeSelector_.onChange = [this] {
            const auto mode = static_cast<ExperimentalReferenceAlignMode>(
                experimentalReferenceAlignModeSelector_.getSelectedId() - 1);
            appPreferences_.setExperimentalReferenceAlignMode(mode);
            notifyChanged();
        };
        initialiseComboBox(experimentalReferenceAlignModeSelector_);
        addAndMakeVisible(experimentalReferenceAlignModeSelector_);

        initialiseToggleButton(lightPitchCorrectionToggle_);
        lightPitchCorrectionToggle_.setButtonText(
            juce::String::fromUTF8(u8"音质优化模式（小幅修正使用周期重采样，超低延迟零相位伪影）"));
        lightPitchCorrectionToggle_.setToggleState(state.shared.lightPitchCorrectionEnabled,
                                                   juce::dontSendNotification);
        lightPitchCorrectionToggle_.onClick = [this] {
            appPreferences_.setLightPitchCorrectionEnabled(lightPitchCorrectionToggle_.getToggleState());
            notifyChanged();
        };
        addAndMakeVisible(lightPitchCorrectionToggle_);

        // Publish preferred height for parent containers
        {
            const int vPad = 4 * 2; // reduced(10, 4) vertical
            const int rows = 5;     // renderingPriority, vocoderWeight, experimentalToggle, refAlignMode, lightPitchToggle
            const int rowH = 34;
            const int gaps = 8 * 4 + 4; // four 8px gaps + one 4px gap
            const int hintHeight = 42;
            getProperties().set("preferredHeight", vPad + rows * rowH + gaps + hintHeight);
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UIColors::backgroundDark);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(10, 4);
        const int rowHeight = 34;
        const int labelWidth = 160;
        const int selectorWidth = 240;

        auto row = bounds.removeFromTop(rowHeight);
        renderingPriorityLabel_.setBounds(row.removeFromLeft(labelWidth));
        renderingPrioritySelector_.setBounds(row.removeFromLeft(selectorWidth).reduced(0, 4));

        bounds.removeFromTop(8);
        row = bounds.removeFromTop(rowHeight);
        vocoderWeightLabel_.setBounds(row.removeFromLeft(labelWidth));
        vocoderWeightSelector_.setBounds(row.removeFromLeft(selectorWidth).reduced(0, 4));

        bounds.removeFromTop(8);
        row = bounds.removeFromTop(rowHeight);
        experimentalFeaturesToggle_.setBounds(row.removeFromLeft(labelWidth + selectorWidth + 80));

        bounds.removeFromTop(4);
        experimentalFeaturesHintLabel_.setBounds(bounds.removeFromTop(42));

        bounds.removeFromTop(8);
        row = bounds.removeFromTop(rowHeight);
        experimentalReferenceAlignModeLabel_.setBounds(row.removeFromLeft(labelWidth));
        experimentalReferenceAlignModeSelector_.setBounds(row.removeFromLeft(selectorWidth).reduced(0, 4));

        bounds.removeFromTop(8);
        row = bounds.removeFromTop(rowHeight);
        lightPitchCorrectionToggle_.setBounds(row.removeFromLeft(labelWidth + selectorWidth + 80));
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
    std::function<void(bool)> onRenderingPriorityChanged_;
    std::function<void(VocoderModelWeight)> onVocoderModelWeightChanged_;
    juce::Label renderingPriorityLabel_;
    juce::ComboBox renderingPrioritySelector_;
    juce::Label vocoderWeightLabel_;
    juce::ComboBox vocoderWeightSelector_;
    juce::ToggleButton experimentalFeaturesToggle_;
    juce::Label experimentalFeaturesHintLabel_;
    juce::Label experimentalReferenceAlignModeLabel_;
    juce::ComboBox experimentalReferenceAlignModeSelector_;
    juce::ToggleButton lightPitchCorrectionToggle_;
};

class SharedEditingPage final : public juce::Component
{
public:
    SharedEditingPage(AppPreferences& appPreferences, std::function<void()> onPreferencesChanged)
        : appPreferences_(appPreferences)
        , onPreferencesChanged_(std::move(onPreferencesChanged))
        , zoomSettings_(appPreferences_.getState().shared.zoomSensitivity)
    {
        initialiseLabel(schemeLabel_, LOC(kAudioEditingScheme));
        schemeLabel_.setComponentID("Audio Editing Scheme");
        addAndMakeVisible(schemeLabel_);

        const auto state = appPreferences_.getState();
        schemeSelector_.addItem(LOC(kCorrectedF0First), 1);
        schemeSelector_.addItem(LOC(kNotesFirst), 2);
        schemeSelector_.setSelectedId(state.shared.audioEditingScheme == AudioEditingScheme::Scheme::NotesPrimary ? 2 : 1,
                                      juce::dontSendNotification);
        schemeSelector_.onChange = [this] {
            const auto scheme = schemeSelector_.getSelectedId() == 2
                ? AudioEditingScheme::Scheme::NotesPrimary
                : AudioEditingScheme::Scheme::CorrectedF0Primary;
            appPreferences_.setAudioEditingScheme(scheme);
            notifyChanged();
        };
        initialiseComboBox(schemeSelector_);
        addAndMakeVisible(schemeSelector_);

        initialiseLabel(horizontalLabel_, LOC(kHorizontalZoomSensitivity));
        addAndMakeVisible(horizontalLabel_);
        horizontalSlider_.setRange(ZoomSensitivityConfig::kMinHorizontalZoomFactor,
                                   ZoomSensitivityConfig::kMaxHorizontalZoomFactor,
                                   0.01);
        horizontalSlider_.setValue(zoomSettings_.horizontalZoomFactor, juce::dontSendNotification);
        horizontalSlider_.onValueChange = [this] {
            zoomSettings_.horizontalZoomFactor = static_cast<float>(horizontalSlider_.getValue());
            persistZoom();
        };
        initialiseSlider(horizontalSlider_);
        addAndMakeVisible(horizontalSlider_);

        initialiseLabel(verticalLabel_, LOC(kVerticalZoomSensitivity));
        addAndMakeVisible(verticalLabel_);
        verticalSlider_.setRange(ZoomSensitivityConfig::kMinVerticalZoomFactor,
                                 ZoomSensitivityConfig::kMaxVerticalZoomFactor,
                                 0.01);
        verticalSlider_.setValue(zoomSettings_.verticalZoomFactor, juce::dontSendNotification);
        verticalSlider_.onValueChange = [this] {
            zoomSettings_.verticalZoomFactor = static_cast<float>(verticalSlider_.getValue());
            persistZoom();
        };
        initialiseSlider(verticalSlider_);
        addAndMakeVisible(verticalSlider_);

        initialiseLabel(scrollLabel_, LOC(kScrollSpeed));
        addAndMakeVisible(scrollLabel_);
        scrollSlider_.setRange(ZoomSensitivityConfig::kMinScrollSpeed,
                               ZoomSensitivityConfig::kMaxScrollSpeed,
                               1.0);
        scrollSlider_.setValue(zoomSettings_.scrollSpeed, juce::dontSendNotification);
        scrollSlider_.onValueChange = [this] {
            zoomSettings_.scrollSpeed = static_cast<float>(scrollSlider_.getValue());
            persistZoom();
        };
        initialiseSlider(scrollSlider_);
        addAndMakeVisible(scrollSlider_);

        resetButton_.setButtonText(LOC(kResetToDefaults));
        resetButton_.setColour(juce::TextButton::buttonColourId, UIColors::buttonNormal);
        resetButton_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        resetButton_.onClick = [this] {
            zoomSettings_ = ZoomSensitivityConfig::ZoomSensitivitySettings::getDefault();
            horizontalSlider_.setValue(zoomSettings_.horizontalZoomFactor, juce::dontSendNotification);
            verticalSlider_.setValue(zoomSettings_.verticalZoomFactor, juce::dontSendNotification);
            scrollSlider_.setValue(zoomSettings_.scrollSpeed, juce::dontSendNotification);
            persistZoom();
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
        const int rowHeight = 34;
        const int labelWidth = 200;

        auto row = bounds.removeFromTop(rowHeight);
        schemeLabel_.setBounds(row.removeFromLeft(labelWidth));
        schemeSelector_.setBounds(row.removeFromLeft(240).reduced(0, 4));

        bounds.removeFromTop(10);
        row = bounds.removeFromTop(rowHeight);
        horizontalLabel_.setBounds(row.removeFromLeft(labelWidth));
        horizontalSlider_.setBounds(row);

        bounds.removeFromTop(10);
        row = bounds.removeFromTop(rowHeight);
        verticalLabel_.setBounds(row.removeFromLeft(labelWidth));
        verticalSlider_.setBounds(row);

        bounds.removeFromTop(10);
        row = bounds.removeFromTop(rowHeight);
        scrollLabel_.setBounds(row.removeFromLeft(labelWidth));
        scrollSlider_.setBounds(row);

        bounds.removeFromTop(18);
        resetButton_.setBounds(bounds.removeFromTop(28).removeFromLeft(150));
    }

private:
    void notifyChanged()
    {
        if (onPreferencesChanged_) {
            onPreferencesChanged_();
        }
    }

    void persistZoom()
    {
        appPreferences_.setZoomSensitivity(zoomSettings_);
        notifyChanged();
    }

    AppPreferences& appPreferences_;
    std::function<void()> onPreferencesChanged_;
    ZoomSensitivityConfig::ZoomSensitivitySettings zoomSettings_;
    juce::Label schemeLabel_;
    juce::ComboBox schemeSelector_;
    juce::Label horizontalLabel_;
    juce::Slider horizontalSlider_;
    juce::Label verticalLabel_;
    juce::Slider verticalSlider_;
    juce::Label scrollLabel_;
    juce::Slider scrollSlider_;
    juce::TextButton resetButton_;
};

class SharedVisualPage final : public juce::Component
{
public:
    SharedVisualPage(AppPreferences& appPreferences, std::function<void()> onPreferencesChanged)
        : appPreferences_(appPreferences)
        , onPreferencesChanged_(std::move(onPreferencesChanged))
    {
        const auto visualPreferences = appPreferences_.getState().shared.pianoRollVisualPreferences;

        initialiseLabel(noteNameModeLabel_, LOC(kNoteLabels));
        noteNameModeLabel_.setComponentID("noteNameMode");
        addAndMakeVisible(noteNameModeLabel_);

        noteNameModeSelector_.addItem(LOC(kNoteLabelsShowAll), 1);
        noteNameModeSelector_.addItem(LOC(kNoteLabelsCOnly), 2);
        noteNameModeSelector_.addItem(LOC(kNoteLabelsHide), 3);
        noteNameModeSelector_.setSelectedId(toComboBoxId(visualPreferences.noteNameMode), juce::dontSendNotification);
        noteNameModeSelector_.onChange = [this] {
            appPreferences_.setNoteNameMode(fromComboBoxId(noteNameModeSelector_.getSelectedId()));
            notifyChanged();
        };
        initialiseComboBox(noteNameModeSelector_);
        addAndMakeVisible(noteNameModeSelector_);

        showUnvoicedFramesToggle_.setButtonText(LOC(kShowUnvoicedFrames));
        showUnvoicedFramesToggle_.setToggleState(visualPreferences.showUnvoicedFrames, juce::dontSendNotification);
        showUnvoicedFramesToggle_.onClick = [this] {
            appPreferences_.setShowUnvoicedFrames(showUnvoicedFramesToggle_.getToggleState());
            notifyChanged();
        };
        initialiseToggleButton(showUnvoicedFramesToggle_);
        addAndMakeVisible(showUnvoicedFramesToggle_);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UIColors::backgroundDark);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(20);
        const int rowHeight = 34;
        const int labelWidth = 200;

        auto row = bounds.removeFromTop(rowHeight);
        noteNameModeLabel_.setBounds(row.removeFromLeft(labelWidth));
        noteNameModeSelector_.setBounds(row.removeFromLeft(240).reduced(0, 4));

        bounds.removeFromTop(14);
        showUnvoicedFramesToggle_.setBounds(bounds.removeFromTop(rowHeight));
    }

private:
    static int toComboBoxId(NoteNameMode noteNameMode) noexcept
    {
        switch (noteNameMode) {
            case NoteNameMode::ShowAll: return 1;
            case NoteNameMode::COnly: return 2;
            case NoteNameMode::Hide: return 3;
        }

        return 2;
    }

    static NoteNameMode fromComboBoxId(int selectedId) noexcept
    {
        switch (selectedId) {
            case 1: return NoteNameMode::ShowAll;
            case 3: return NoteNameMode::Hide;
            default: return NoteNameMode::COnly;
        }
    }

    void notifyChanged()
    {
        if (onPreferencesChanged_) {
            onPreferencesChanged_();
        }
    }

    AppPreferences& appPreferences_;
    std::function<void()> onPreferencesChanged_;
    juce::Label noteNameModeLabel_;
    juce::ComboBox noteNameModeSelector_;
    juce::ToggleButton showUnvoicedFramesToggle_;
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
        , settings_(appPreferences_.getState().shared.shortcuts)
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
        appPreferences_.setShortcuts(settings_);
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

std::vector<TabbedPreferencesDialog::PageSpec> SharedPreferencePages::create(
    AppPreferences& appPreferences,
    std::function<void()> onPreferencesChanged)
{
    std::vector<TabbedPreferencesDialog::PageSpec> pages;
    pages.push_back({ LOC(kTheme), std::make_unique<SharedGeneralPage>(appPreferences, onPreferencesChanged) });
    pages.push_back({ LOC(kEditing), std::make_unique<SharedEditingPage>(appPreferences, onPreferencesChanged) });
    pages.push_back({ LOC(kView), std::make_unique<SharedVisualPage>(appPreferences, onPreferencesChanged) });
    pages.push_back({ LOC(kKeyswitch), std::make_unique<ShortcutSettingsPage>(appPreferences, onPreferencesChanged) });
    return pages;
}

std::unique_ptr<juce::Component> SharedPreferencePages::createRenderingPriorityComponent(
    AppPreferences& appPreferences,
    std::function<void()> onPreferencesChanged,
    std::function<void(bool forceCpu)> onRenderingPriorityChanged,
    std::function<void(VocoderModelWeight)> onVocoderModelWeightChanged)
{
    return std::make_unique<SharedAudioPage>(appPreferences,
                                              std::move(onPreferencesChanged),
                                              std::move(onRenderingPriorityChanged),
                                              std::move(onVocoderModelWeightChanged));
}

} // namespace OpenTune
