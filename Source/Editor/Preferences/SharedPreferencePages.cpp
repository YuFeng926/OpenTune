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
    static constexpr int kContentHeight = 120; // 20 + 34 + 12 + 34 + 20

    SharedGeneralPage(AppPreferences& appPreferences,
                      std::function<void()> onPreferencesChanged)
        : appPreferences_(appPreferences)
        , onPreferencesChanged_(std::move(onPreferencesChanged))
    {
        auto state = appPreferences_.getState();

        initialiseLabel(themeLabel_, LOC(kTheme));
        addAndMakeVisible(themeLabel_);

        // themeSelector_.addItem(LOC(kThemeBlueBreeze), 1);  // 临时隐藏
        // themeSelector_.addItem(LOC(kThemeDarkBlueGrey), 2); // 临时隐藏
        themeSelector_.addItem(LOC(kThemeAurora), 3);
        // themeSelector_.addItem(LOC(kThemeOverdose), 4);  // "升天" 主题暂时隐藏
        const int themeIdx = static_cast<int>(state.shared.theme);
        // 蓝色清风/深蓝灰临时隐藏，fallback到Aurora
        const int selectedId = (themeIdx <= static_cast<int>(ThemeId::DarkBlueGrey)) ? 3 : themeIdx + 1;
        themeSelector_.setSelectedId(selectedId, juce::dontSendNotification);
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
                    std::function<void(VocoderModelWeight)> onVocoderModelWeightChanged,
                    bool isVst3Plugin)
        : appPreferences_(appPreferences)
        , onPreferencesChanged_(std::move(onPreferencesChanged))
        , onRenderingPriorityChanged_(std::move(onRenderingPriorityChanged))
        , onVocoderModelWeightChanged_(std::move(onVocoderModelWeightChanged))
        , isVst3Plugin_(isVst3Plugin)
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

        if (!isVst3Plugin_) {
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
        }

        // Publish preferred height for parent containers
        {
            const int vPad = 4 * 2; // reduced(10, 4) vertical
            const int rows = isVst3Plugin_ ? 2 : 4; // 插件隐藏实验控件
            const int rowH = 34;
            const int gaps = 8 * (rows - 1) + (isVst3Plugin_ ? 0 : 4); // 行间 8px；实验模式下额外 hint 前 4px
            const int hintHeight = isVst3Plugin_ ? 0 : 42;
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

        if (!isVst3Plugin_) {
            bounds.removeFromTop(8);
            row = bounds.removeFromTop(rowHeight);
            experimentalFeaturesToggle_.setBounds(row.removeFromLeft(labelWidth + selectorWidth + 80));

            bounds.removeFromTop(4);
            experimentalFeaturesHintLabel_.setBounds(bounds.removeFromTop(42));

            bounds.removeFromTop(8);
            row = bounds.removeFromTop(rowHeight);
            experimentalReferenceAlignModeLabel_.setBounds(row.removeFromLeft(labelWidth));
            experimentalReferenceAlignModeSelector_.setBounds(row.removeFromLeft(selectorWidth).reduced(0, 4));
        }
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
    bool isVst3Plugin_ = false;
    juce::Label renderingPriorityLabel_;
    juce::ComboBox renderingPrioritySelector_;
    juce::Label vocoderWeightLabel_;
    juce::ComboBox vocoderWeightSelector_;
    juce::ToggleButton experimentalFeaturesToggle_;
    juce::Label experimentalFeaturesHintLabel_;
    juce::Label experimentalReferenceAlignModeLabel_;
    juce::ComboBox experimentalReferenceAlignModeSelector_;
};

class SharedEditingPage final : public juce::Component
{
public:
    static constexpr int kContentHeight = 296; // 20 + 34 + 10 + 34 + 10 + 34 + 10 + 34 + 10 + 34 + 18 + 28 + 20

    SharedEditingPage(AppPreferences& appPreferences,
                      std::function<void()> onPreferencesChanged,
                      bool isVst3Plugin)
        : appPreferences_(appPreferences)
        , onPreferencesChanged_(std::move(onPreferencesChanged))
        , isVst3Plugin_(isVst3Plugin)
        , zoomSettings_(appPreferences_.getState().shared.zoomSensitivity)
    {
        initialiseLabel(schemeLabel_, LOC(kAudioEditingScheme));
        schemeLabel_.setComponentID("Audio Editing Scheme");
        addAndMakeVisible(schemeLabel_);

        const auto state = appPreferences_.getState();
        schemeSelector_.addItem(LOC(kSchemeOpenTune), 1);
        schemeSelector_.addItem(LOC(kSchemeOpenDyne), 2);
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

        initialiseLabel(tuningLabel_, LOC(kTuningHz));
        addAndMakeVisible(tuningLabel_);
        tuningSlider_.setRange(TuningConfig::kMinTuningHz, TuningConfig::kMaxTuningHz, 1.0);
        tuningSlider_.setValue(appPreferences_.getState().shared.tuning.tuningHz, juce::dontSendNotification);
        tuningSlider_.onValueChange = [this] {
            TuningConfig::TuningSettings tuning;
            tuning.tuningHz = static_cast<float>(tuningSlider_.getValue());
            appPreferences_.setTuning(tuning);
            notifyChanged();
        };
        initialiseSlider(tuningSlider_);
        addAndMakeVisible(tuningSlider_);

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

        // Grid style selector
        initialiseLabel(gridStyleLabel_, LOC(kGridStyle));
        addAndMakeVisible(gridStyleLabel_);
        const auto currentGridStyle = appPreferences_.getState().shared.gridStyle;
        gridStyleSelector_.addItem(LOC(kGridStylePianoLanes), 1);
        gridStyleSelector_.addItem(LOC(kGridStyleEqualSpacing), 2);
        gridStyleSelector_.setSelectedId(currentGridStyle == PianoGridStyle::EqualSpacing ? 2 : 1,
                                         juce::dontSendNotification);
        gridStyleSelector_.onChange = [this] {
            const auto style = gridStyleSelector_.getSelectedId() == 2
                ? PianoGridStyle::EqualSpacing
                : PianoGridStyle::PianoLanes;
            appPreferences_.setGridStyle(style);
            notifyChanged();
        };
        initialiseComboBox(gridStyleSelector_);
        addAndMakeVisible(gridStyleSelector_);
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

        bounds.removeFromTop(10);
        row = bounds.removeFromTop(rowHeight);
        tuningLabel_.setBounds(row.removeFromLeft(labelWidth));
        tuningSlider_.setBounds(row);

        bounds.removeFromTop(18);
        auto resetRow = bounds.removeFromTop(28);
        resetButton_.setBounds(resetRow.removeFromLeft(150));
        resetRow.removeFromLeft(16); // spacing
        gridStyleLabel_.setBounds(resetRow.removeFromLeft(80));
        gridStyleSelector_.setBounds(resetRow.reduced(0, 2));
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
    bool isVst3Plugin_ = false;
    ZoomSensitivityConfig::ZoomSensitivitySettings zoomSettings_;
    juce::Label schemeLabel_;
    juce::ComboBox schemeSelector_;
    juce::Label horizontalLabel_;
    juce::Slider horizontalSlider_;
    juce::Label verticalLabel_;
    juce::Slider verticalSlider_;
    juce::Label scrollLabel_;
    juce::Slider scrollSlider_;
    juce::Label tuningLabel_;
    juce::Slider tuningSlider_;
    juce::TextButton resetButton_;
    juce::Label gridStyleLabel_;
    juce::ComboBox gridStyleSelector_;
};

class SharedVisualPage final : public juce::Component
{
public:
    static constexpr int kContentHeight = 170; // 20 + 34 + 14 + 34 + 14 + 34 + 20

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

        initialiseLabel(backgroundBrightnessLabel_, LOC(kBackgroundBrightness));
        backgroundBrightnessLabel_.setComponentID("backgroundBrightness");
        addAndMakeVisible(backgroundBrightnessLabel_);

        backgroundBrightnessSlider_.setRange(0.0, 2.0, 0.01);
        backgroundBrightnessSlider_.setValue(visualPreferences.backgroundBrightness, juce::dontSendNotification);
        backgroundBrightnessSlider_.onValueChange = [this] {
            appPreferences_.setBackgroundBrightness(static_cast<float>(backgroundBrightnessSlider_.getValue()));
            notifyChanged();
        };
        initialiseSlider(backgroundBrightnessSlider_);
        addAndMakeVisible(backgroundBrightnessSlider_);
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

        bounds.removeFromTop(14);
        auto brightnessRow = bounds.removeFromTop(rowHeight);
        backgroundBrightnessLabel_.setBounds(brightnessRow.removeFromLeft(labelWidth));
        backgroundBrightnessSlider_.setBounds(brightnessRow.reduced(0, 4));
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
    juce::Label backgroundBrightnessLabel_;
    juce::Slider backgroundBrightnessSlider_;
};

class ShortcutSettingsPage final : public juce::Component
{
public:
    int getContentHeight() const { return contentHeight_; }

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
        // 三段式布局：通用 → OpenTune 专属 → OpenDyne 参考
        // 通用: PlayPause, Stop, PlayFromStart, Undo, Redo, Cut, Copy, Paste, SelectAll,
        //        Delete, SplitClip, MergeClips, DuplicateClip, NudgeLeft, NudgeRight,
        //        ToggleSnap, ToolAutoTune, ToolTimeTool, CancelSelection
        generalIds_ = {
            KeyShortcutConfig::ShortcutId::PlayPause, KeyShortcutConfig::ShortcutId::Stop,
            KeyShortcutConfig::ShortcutId::PlayFromStart,
            KeyShortcutConfig::ShortcutId::Undo, KeyShortcutConfig::ShortcutId::Redo,
            KeyShortcutConfig::ShortcutId::Cut, KeyShortcutConfig::ShortcutId::Copy,
            KeyShortcutConfig::ShortcutId::Paste, KeyShortcutConfig::ShortcutId::SelectAll,
            KeyShortcutConfig::ShortcutId::Delete,
            KeyShortcutConfig::ShortcutId::SplitClip, KeyShortcutConfig::ShortcutId::MergeClips,
            KeyShortcutConfig::ShortcutId::DuplicateClip,
            KeyShortcutConfig::ShortcutId::NudgeLeft, KeyShortcutConfig::ShortcutId::NudgeRight,
            KeyShortcutConfig::ShortcutId::ToggleSnap,
            KeyShortcutConfig::ShortcutId::ToolAutoTune, KeyShortcutConfig::ShortcutId::ToolTimeTool,
            KeyShortcutConfig::ShortcutId::CancelSelection,
        };
        // OpenTune 专属: DrawNote, Select, LineAnchor, HandDraw
        opentuneIds_ = {
            KeyShortcutConfig::ShortcutId::ToolDrawNote, KeyShortcutConfig::ShortcutId::ToolSelect,
            KeyShortcutConfig::ShortcutId::ToolLineAnchor, KeyShortcutConfig::ShortcutId::ToolHandDraw,
        };
        // OpenDyne 专属: Select, Pitch, VolumeEnvelope, Scissors
        opendyneIds_ = {
            KeyShortcutConfig::ShortcutId::ToolODSelect, KeyShortcutConfig::ShortcutId::ToolODPitch,
            KeyShortcutConfig::ShortcutId::ToolODVolumeEnvelope, KeyShortcutConfig::ShortcutId::ToolODScissors,
        };

        // 内容高度，与 resized() 布局一一对应：
        // reduced(20) 的顶部+底部 40 + 每段(header 24 + gap 4 + rows*32 + gap 6) + 段间 10 + 底部 12 + 重置按钮 28
        const auto sectionH = [](int itemCount) {
            return 24 + 4 + itemCount * 32 + 6; // header + gapAfter + rows * rowH + trailingGap
        };
        contentHeight_ = 40
                       + sectionH(static_cast<int>(generalIds_.size())) + 10
                       + sectionH(static_cast<int>(opentuneIds_.size())) + 10
                       + sectionH(static_cast<int>(opendyneIds_.size()))
                       + 12 + 28;

        auto makeSectionHeader = [this](const juce::String& text) {
            auto* label = new juce::Label();
            label->setText(text, juce::dontSendNotification);
            label->setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
            label->setColour(juce::Label::textColourId, UIColors::textPrimary);
            label->setJustificationType(juce::Justification::centredLeft);
            sectionHeaders_.add(label);
            addAndMakeVisible(label);
        };

        auto makeShortcutRow = [this](KeyShortcutConfig::ShortcutId id) {
            auto* label = new juce::Label();
            initialiseLabel(*label, KeyShortcutConfig::getShortcutDisplayName(id));
            shortcutLabels_.add(label);
            addAndMakeVisible(label);

            auto* button = new juce::TextButton(KeyShortcutConfig::getShortcutBinding(settings_, id).getDisplayNames());
            button->setColour(juce::TextButton::buttonColourId, UIColors::backgroundMedium);
            button->setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
            button->onClick = [this, id] { beginCapture(id); };
            shortcutButtons_.add(button);
            addAndMakeVisible(button);
        };

        // === Section1: General ===
        makeSectionHeader("General");
        for (auto id : generalIds_)
            makeShortcutRow(id);

        // === Section 2: OpenTune Mode ===
        makeSectionHeader("OpenTune Mode");
        for (auto id : opentuneIds_)
            makeShortcutRow(id);

        // === Section 3: OpenDyne Mode ===
        makeSectionHeader("OpenDyne Mode");
        for (auto id : opendyneIds_)
            makeShortcutRow(id);

        // Reset button
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
        int idx = 0;

        auto layoutSection = [&](const juce::String& /*headerLabel*/, int itemCount, auto getRow) {
            // Section header
            if (idx < sectionHeaders_.size())
                sectionHeaders_[idx++]->setBounds(bounds.removeFromTop(24));
            bounds.removeFromTop(4);
            // Rows
            for (int i = 0; i < itemCount; ++i)
                getRow(i, bounds.removeFromTop(rowHeight));
            bounds.removeFromTop(6);
        };

        int shortcutIdx = 0;

        // Section1: General
        layoutSection("General", static_cast<int>(generalIds_.size()), [&](int /*i*/, juce::Rectangle<int> row) {
            shortcutLabels_[shortcutIdx]->setBounds(row.removeFromLeft(labelWidth));
            shortcutButtons_[shortcutIdx]->setBounds(row.removeFromLeft(buttonWidth).reduced(0, 3));
            ++shortcutIdx;
        });

        bounds.removeFromTop(10);

        // Section2: OpenTune Mode
        layoutSection("OpenTune Mode", static_cast<int>(opentuneIds_.size()), [&](int /*i*/, juce::Rectangle<int> row) {
            shortcutLabels_[shortcutIdx]->setBounds(row.removeFromLeft(labelWidth));
            shortcutButtons_[shortcutIdx]->setBounds(row.removeFromLeft(buttonWidth).reduced(0, 3));
            ++shortcutIdx;
        });

        bounds.removeFromTop(10);

        // Section3: OpenDyne Mode
        layoutSection("OpenDyne Mode", static_cast<int>(opendyneIds_.size()), [&](int /*i*/, juce::Rectangle<int> row) {
            shortcutLabels_[shortcutIdx]->setBounds(row.removeFromLeft(labelWidth));
            shortcutButtons_[shortcutIdx]->setBounds(row.removeFromLeft(buttonWidth).reduced(0, 3));
            ++shortcutIdx;
        });

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
        for (size_t index = 0; index < shortcutButtons_.size(); ++index) {
            if (index < generalIds_.size()) {
                if (auto* button = shortcutButtons_[static_cast<int>(index)])
                    button->setButtonText(KeyShortcutConfig::getShortcutBinding(settings_, generalIds_[index]).getDisplayNames());
            } else if (index < generalIds_.size() + opentuneIds_.size()) {
                const auto otIdx = index - generalIds_.size();
                if (auto* button = shortcutButtons_[static_cast<int>(index)])
                    button->setButtonText(KeyShortcutConfig::getShortcutBinding(settings_, opentuneIds_[otIdx]).getDisplayNames());
            } else {
                const auto odIdx = index - generalIds_.size() - opentuneIds_.size();
                if (auto* button = shortcutButtons_[static_cast<int>(index)])
                    button->setButtonText(KeyShortcutConfig::getShortcutBinding(settings_, opendyneIds_[odIdx]).getDisplayNames());
            }
        }
    }

    AppPreferences& appPreferences_;
    std::function<void()> onPreferencesChanged_;
    KeyShortcutConfig::KeyShortcutSettings settings_;
    int contentHeight_ = 0;
    std::vector<KeyShortcutConfig::ShortcutId> generalIds_;
    std::vector<KeyShortcutConfig::ShortcutId> opentuneIds_;
    std::vector<KeyShortcutConfig::ShortcutId> opendyneIds_;
    juce::OwnedArray<juce::Label> sectionHeaders_;
    juce::OwnedArray<juce::Label> shortcutLabels_;
    juce::OwnedArray<juce::TextButton> shortcutButtons_;
    juce::TextButton resetAllButton_;
    std::unique_ptr<CaptureWindow> captureWindow_;
    KeyShortcutConfig::ShortcutId currentEditingId_ = KeyShortcutConfig::ShortcutId::Count;
};

} // namespace

std::vector<TabbedPreferencesDialog::PageSpec> SharedPreferencePages::create(
    AppPreferences& appPreferences,
    std::function<void()> onPreferencesChanged,
    bool isVst3Plugin)
{
    std::vector<TabbedPreferencesDialog::PageSpec> pages;

    {
        auto page = std::make_unique<SharedGeneralPage>(appPreferences, onPreferencesChanged);
        pages.push_back({ LOC(kTheme), std::move(page), SharedGeneralPage::kContentHeight });
    }
    {
        auto page = std::make_unique<SharedEditingPage>(appPreferences, onPreferencesChanged, isVst3Plugin);
        pages.push_back({ LOC(kEditing), std::move(page), SharedEditingPage::kContentHeight });
    }
    {
        auto page = std::make_unique<SharedVisualPage>(appPreferences, onPreferencesChanged);
        pages.push_back({ LOC(kView), std::move(page), SharedVisualPage::kContentHeight });
    }
    {
        auto page = std::make_unique<ShortcutSettingsPage>(appPreferences, onPreferencesChanged);
        const int h = page->getContentHeight();
        pages.push_back({ LOC(kKeyswitch), std::move(page), h });
    }
    return pages;
}

std::unique_ptr<juce::Component> SharedPreferencePages::createRenderingPriorityComponent(
    AppPreferences& appPreferences,
    std::function<void()> onPreferencesChanged,
    std::function<void(bool forceCpu)> onRenderingPriorityChanged,
    std::function<void(VocoderModelWeight)> onVocoderModelWeightChanged,
    bool isVst3Plugin)
{
    return std::make_unique<SharedAudioPage>(appPreferences,
                                              std::move(onPreferencesChanged),
                                              std::move(onRenderingPriorityChanged),
                                              std::move(onVocoderModelWeightChanged),
                                              isVst3Plugin);
}

int SharedPreferencePages::getRenderingPriorityPageHeight(
    const juce::Component& component)
{
    return static_cast<int>(component.getProperties().getWithDefault("preferredHeight", 260));
}

} // namespace OpenTune
