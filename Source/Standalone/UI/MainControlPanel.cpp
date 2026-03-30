#include "MainControlPanel.h"

namespace OpenTune {

MainControlPanel::MainControlPanel()
{
    // Setup scale selector
    scaleLabel_.setText("Scale:", juce::dontSendNotification);
    scaleLabel_.setFont(UIColors::getUIFont(14.0f));
    scaleLabel_.setColour(juce::Label::textColourId, UIColors::textSecondary);
    addAndMakeVisible(scaleLabel_);

    // Root note selector
    const char* notes[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    for (int i = 0; i < 12; ++i)
    {
        scaleRootSelector_.addItem(notes[i], i + 1);
    }
    scaleRootSelector_.setSelectedId(1, juce::dontSendNotification); // Default to C
    scaleRootSelector_.onChange = [this] { onScaleChanged(); };
    scaleRootSelector_.getProperties().set("noArrow", true);
    scaleRootSelector_.getProperties().set("fontHeight", 18.0);
    scaleRootSelector_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(scaleRootSelector_);

    // Scale type selector
    scaleTypeSelector_.addItem("Maj.", 1);
    scaleTypeSelector_.addItem("Min.", 2);
    scaleTypeSelector_.addItem("Chr.", 3);
    scaleTypeSelector_.setSelectedId(1, juce::dontSendNotification);
    scaleTypeSelector_.onChange = [this] { onScaleChanged(); };
    scaleTypeSelector_.getProperties().set("noArrow", true);
    scaleTypeSelector_.getProperties().set("fontHeight", 18.0);
    scaleTypeSelector_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(scaleTypeSelector_);

    // HIDDEN: Show Waveform/Lanes buttons (functionality available in View menu)
    // showWaveformButton_.setButtonText("Show Waveform");
    // showWaveformButton_.setToggleState(true, juce::dontSendNotification);
    // showWaveformButton_.onClick = [this] { onShowWaveformToggled(); };
    // addAndMakeVisible(showWaveformButton_);
    //
    // showLanesButton_.setButtonText("Show Lanes");
    // showLanesButton_.setToggleState(true, juce::dontSendNotification);
    // showLanesButton_.onClick = [this] { onShowLanesToggled(); };
    // addAndMakeVisible(showLanesButton_);

    // Apply custom styling
    applyCustomLookAndFeel();

    // Note: F0 Model and Vocoder selectors have been moved to Preferences Dialog
    // to keep the main control panel clean and focused on essential controls
}

MainControlPanel::~MainControlPanel()
{
}

void MainControlPanel::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    UIColors::fillPanelBackground(g, bounds.toFloat(), 0.0f);

    // Bottom border
    g.setColour(UIColors::panelBorder);
    g.drawLine(0.0f, static_cast<float>(bounds.getHeight()),
               static_cast<float>(bounds.getWidth()),
               static_cast<float>(bounds.getHeight()),
               2.0f);
}

void MainControlPanel::resized()
{
    auto bounds = getLocalBounds().reduced(10);
    const int comboHeight = 24;

    // Single row: Scale selectors only
    auto firstRow = bounds.removeFromTop(comboHeight);

    // Scale selector
    scaleLabel_.setBounds(firstRow.removeFromLeft(60));
    scaleRootSelector_.setBounds(firstRow.removeFromLeft(50));  // 加宽以完整显示升降号
    firstRow.removeFromLeft(5);
    scaleTypeSelector_.setBounds(firstRow.removeFromLeft(80));

    // HIDDEN: Toggle buttons (removed - functionality in View menu)
    // firstRow.removeFromLeft(spacing);
    // showWaveformButton_.setBounds(firstRow.removeFromLeft(140));
    // firstRow.removeFromLeft(spacing);
    // showLanesButton_.setBounds(firstRow.removeFromLeft(120));
}

void MainControlPanel::addListener(Listener* listener)
{
    listeners_.add(listener);
}

void MainControlPanel::removeListener(Listener* listener)
{
    listeners_.remove(listener);
}

void MainControlPanel::onScaleChanged()
{
    int rootNote = scaleRootSelector_.getSelectedId() - 1;  // 0-11
    bool isMinor = (scaleTypeSelector_.getSelectedId() == 2);
    listeners_.call([rootNote, isMinor](Listener& l) { l.scaleChanged(rootNote, isMinor); });
}

void MainControlPanel::onShowWaveformToggled()
{
    bool shouldShow = showWaveformButton_.getToggleState();
    listeners_.call([shouldShow](Listener& l) { l.showWaveformChanged(shouldShow); });
}

void MainControlPanel::onShowLanesToggled()
{
    bool shouldShow = showLanesButton_.getToggleState();
    listeners_.call([shouldShow](Listener& l) { l.showLanesChanged(shouldShow); });
}

void MainControlPanel::applyCustomLookAndFeel()
{
    // ComboBox styling
    scaleRootSelector_.setColour(juce::ComboBox::backgroundColourId, UIColors::backgroundLight);
    scaleRootSelector_.setColour(juce::ComboBox::textColourId, UIColors::textPrimary);
    scaleRootSelector_.setColour(juce::ComboBox::outlineColourId, UIColors::primaryPurple);

    scaleTypeSelector_.setColour(juce::ComboBox::backgroundColourId, UIColors::backgroundLight);
    scaleTypeSelector_.setColour(juce::ComboBox::textColourId, UIColors::textPrimary);
    scaleTypeSelector_.setColour(juce::ComboBox::outlineColourId, UIColors::primaryPurple);

    // HIDDEN: Toggle button styling (buttons removed)
    // showWaveformButton_.setColour(juce::ToggleButton::textColourId, UIColors::textPrimary);
    // showWaveformButton_.setColour(juce::ToggleButton::tickColourId, UIColors::lightPurple);
    // showWaveformButton_.setColour(juce::ToggleButton::tickDisabledColourId, UIColors::textDisabled);
    //
    // showLanesButton_.setColour(juce::ToggleButton::textColourId, UIColors::textPrimary);
    // showLanesButton_.setColour(juce::ToggleButton::tickColourId, UIColors::lightPurple);
    // showLanesButton_.setColour(juce::ToggleButton::tickDisabledColourId, UIColors::textDisabled);
}

} // namespace OpenTune
