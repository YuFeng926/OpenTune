#include "SharedPreferencePages.h"

#include "Standalone/UI/UIColors.h"

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
        themeSelector_.setSelectedId(static_cast<int>(state.shared.theme) + 1, juce::dontSendNotification);
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
                    std::function<void(bool)> onRenderingPriorityChanged)
        : appPreferences_(appPreferences)
        , onPreferencesChanged_(std::move(onPreferencesChanged))
        , onRenderingPriorityChanged_(std::move(onRenderingPriorityChanged))
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

        forceAlignToggle_.setButtonText(LOC(kForceAlignReferenceStart));
        forceAlignToggle_.setToggleState(state.shared.forceAlignReferenceStart,
                                         juce::dontSendNotification);
        forceAlignToggle_.setColour(juce::ToggleButton::textColourId, UIColors::textPrimary);
        forceAlignToggle_.setColour(juce::ToggleButton::tickColourId, UIColors::accent);
        forceAlignToggle_.onClick = [this] {
            appPreferences_.setForceAlignReferenceStart(forceAlignToggle_.getToggleState());
            notifyChanged();
        };
        addAndMakeVisible(forceAlignToggle_);
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

        auto row = bounds.removeFromTop(rowHeight);
        renderingPriorityLabel_.setBounds(row.removeFromLeft(labelWidth));
        renderingPrioritySelector_.setBounds(row.removeFromLeft(240).reduced(0, 4));

        auto alignRow = bounds.removeFromTop(rowHeight);
        alignRow.removeFromLeft(labelWidth);  // indent to match selector column
        forceAlignToggle_.setBounds(alignRow.removeFromLeft(300).reduced(0, 4));
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
    juce::Label renderingPriorityLabel_;
    juce::ComboBox renderingPrioritySelector_;
    juce::ToggleButton forceAlignToggle_;
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

        showChunkBoundariesToggle_.setButtonText(LOC(kShowChunkBoundaries));
        showChunkBoundariesToggle_.setToggleState(visualPreferences.showChunkBoundaries, juce::dontSendNotification);
        showChunkBoundariesToggle_.onClick = [this] {
            appPreferences_.setShowChunkBoundaries(showChunkBoundariesToggle_.getToggleState());
            notifyChanged();
        };
        initialiseToggleButton(showChunkBoundariesToggle_);
        addAndMakeVisible(showChunkBoundariesToggle_);

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
        showChunkBoundariesToggle_.setBounds(bounds.removeFromTop(rowHeight));

        bounds.removeFromTop(8);
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
    juce::ToggleButton showChunkBoundariesToggle_;
    juce::ToggleButton showUnvoicedFramesToggle_;
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
    return pages;
}

std::unique_ptr<juce::Component> SharedPreferencePages::createRenderingPriorityComponent(
    AppPreferences& appPreferences,
    std::function<void()> onPreferencesChanged,
    std::function<void(bool forceCpu)> onRenderingPriorityChanged)
{
    return std::make_unique<SharedAudioPage>(appPreferences, std::move(onPreferencesChanged), std::move(onRenderingPriorityChanged));
}

} // namespace OpenTune
