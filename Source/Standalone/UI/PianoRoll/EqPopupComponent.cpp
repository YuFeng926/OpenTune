#include "EqPopupComponent.h"

namespace OpenTune {

EqPopupComponent::EqPopupComponent()
{
    // Setup buttons
    maximizeButton_.addListener(this);
    bypassButton_.addListener(this);
    removeButton_.addListener(this);
    closeButton_.addListener(this);
    
    addAndMakeVisible(maximizeButton_);
    addAndMakeVisible(bypassButton_);
    addAndMakeVisible(removeButton_);
    addAndMakeVisible(closeButton_);
    
    // Setup band controls
    for (int i = 0; i < EqSettings::kNumBands; ++i) {
        auto& ctrl = bandControls_[i];
        
        ctrl.gainSlider.setRange(-12.0, 12.0, 0.1);
        ctrl.gainSlider.setTextValueSuffix(" dB");
        ctrl.gainSlider.addListener(this);
        addAndMakeVisible(ctrl.gainSlider);
        
        ctrl.freqSlider.setRange(20.0, 20000.0, 1.0);
        ctrl.freqSlider.setTextValueSuffix(" Hz");
        ctrl.freqSlider.setSkewFactor(0.3);
        ctrl.freqSlider.addListener(this);
        addAndMakeVisible(ctrl.freqSlider);
        
        ctrl.gainLabel.setText("Gain", juce::dontSendNotification);
        ctrl.gainLabel.attachToComponent(&ctrl.gainSlider, true);
        addAndMakeVisible(ctrl.gainLabel);
        
        ctrl.freqLabel.setText("Freq", juce::dontSendNotification);
        ctrl.freqLabel.attachToComponent(&ctrl.freqSlider, true);
        addAndMakeVisible(ctrl.freqLabel);
    }
    
    // Setup renderer
    interaction_.setRenderer(&renderer_);
    
    // Start timer for updates
    startTimerHz(30);
}

EqPopupComponent::~EqPopupComponent()
{
    stopTimer();
    maximizeButton_.removeListener(this);
    bypassButton_.removeListener(this);
    removeButton_.removeListener(this);
    closeButton_.removeListener(this);
    
    for (auto& ctrl : bandControls_) {
        ctrl.gainSlider.removeListener(this);
        ctrl.freqSlider.removeListener(this);
    }
}

void EqPopupComponent::setEqSettings(const EqSettings& settings)
{
    settings_ = settings;
    updateBandControls();
    repaint();
}

void EqPopupComponent::paint(juce::Graphics& g)
{
    // Background
    if (isPreview_) {
        g.fillAll(noteColor_.withAlpha(0.8f));
    } else {
        g.fillAll(juce::Colour(30, 30, 30));
    }
    
    // Draw EQ graph
    renderer_.draw(g, settings_, !isPreview_);
    
    // Draw anchors
    renderer_.drawAnchors(g, settings_, selectedBand_);
    
    // Preview mode: minimal UI
    if (isPreview_) {
        g.setColour(juce::Colours::white);
        g.setFont(10.0f);
        g.drawText("EQ", getLocalBounds().reduced(4), juce::Justification::topLeft);
    }
    
    // Full mode: draw coordinate feedback
    if (!isPreview_ && selectedBand_ >= 0) {
        const auto& band = settings_.bands[selectedBand_];
        juce::String coordText = juce::String(band.frequency, 0) + " Hz, " 
                               + juce::String(band.gainDb, 1) + " dB";
        g.setColour(juce::Colours::white);
        g.setFont(12.0f);
        g.drawText(coordText, getLocalBounds().removeFromBottom(20).reduced(4), 
                   juce::Justification::centred);
    }
}

void EqPopupComponent::resized()
{
    if (isPreview_) {
        layoutPreview();
    } else {
        layoutFull();
    }
}

void EqPopupComponent::layoutPreview()
{
    auto bounds = getLocalBounds();
    
    // Minimal layout: just the close button
    closeButton_.setBounds(bounds.removeFromRight(20).removeFromTop(20));
    
    // Renderer fills remaining space
    renderer_.setGraphBounds(bounds.toFloat());
}

void EqPopupComponent::layoutFull()
{
    auto bounds = getLocalBounds();
    
    // Top bar: buttons
    auto topBar = bounds.removeFromTop(30);
    maximizeButton_.setBounds(topBar.removeFromLeft(30));
    bypassButton_.setBounds(topBar.removeFromLeft(30));
    removeButton_.setBounds(topBar.removeFromLeft(30));
    closeButton_.setBounds(topBar.removeFromRight(30));
    
    // Bottom: band controls
    auto bottomArea = bounds.removeFromBottom(120);
    const int sliderWidth = (bottomArea.getWidth() - 40) / EqSettings::kNumBands;
    
    for (int i = 0; i < EqSettings::kNumBands; ++i) {
        auto& ctrl = bandControls_[i];
        auto bandArea = bottomArea.removeFromLeft(sliderWidth).reduced(2);
        
        auto freqArea = bandArea.removeFromTop(40);
        ctrl.freqSlider.setBounds(freqArea);
        
        auto gainArea = bandArea;
        ctrl.gainSlider.setBounds(gainArea);
    }
    
    // Remaining: graph area
    renderer_.setGraphBounds(bounds.toFloat().reduced(10));
}

void EqPopupComponent::mouseDown(const juce::MouseEvent& event)
{
    if (isPreview_)
        return;
    
    const auto pos = event.position;
    const int band = renderer_.findBandAtPosition(pos, settings_);
    
    if (band >= 0) {
        interaction_.startDrag(band, pos, settings_);
        selectedBand_ = band;
        updateBandControls();
        repaint();
    }
}

void EqPopupComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (isPreview_ || !interaction_.isDragging())
        return;
    
    const auto pos = event.position;
    const bool shiftHeld = event.mods.isShiftDown();
    
    settings_ = interaction_.updateDrag(pos, shiftHeld);
    
    if (onSettingsChanged)
        onSettingsChanged(settings_);
    
    updateBandControls();
    repaint();
}

void EqPopupComponent::mouseUp(const juce::MouseEvent& event)
{
    interaction_.endDrag();
}

void EqPopupComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (isPreview_)
        return;
    
    const auto pos = event.position;
    const int band = renderer_.findBandAtPosition(pos, settings_);
    
    if (band >= 0) {
        settings_ = EqBandInteraction::resetBand(settings_, band);
        
        if (onSettingsChanged)
            onSettingsChanged(settings_);
        
        updateBandControls();
        repaint();
    }
}

void EqPopupComponent::buttonClicked(juce::Button* button)
{
    if (button == &maximizeButton_) {
        toggleMaximize();
    } else if (button == &bypassButton_) {
        settings_.active = !settings_.active;
        bypassButton_.setButtonText(settings_.active ? "B" : "B*");
        
        if (onSettingsChanged)
            onSettingsChanged(settings_);
        
        repaint();
    } else if (button == &removeButton_) {
        if (dontShowRemoveConfirmation_) {
            if (onRemoveEq)
                onRemoveEq();
        } else {
            showRemoveConfirmation_ = true;
            repaint();
        }
    } else if (button == &closeButton_) {
        setVisible(false);
    }
}

void EqPopupComponent::sliderValueChanged(juce::Slider* slider)
{
    updateSettingsFromControls();
    repaint();
}

void EqPopupComponent::timerCallback()
{
    // Periodic updates if needed
}

void EqPopupComponent::setPreviewMode(bool isPreview)
{
    isPreview_ = isPreview;
    resized();
    repaint();
}

void EqPopupComponent::toggleMaximize()
{
    isMaximized_ = !isMaximized_;
    
    if (isMaximized_) {
        setBounds(0, 0, kFullWidth, kFullHeight);
        centreWithSize(kFullWidth, kFullHeight);
    } else {
        setBounds(0, 0, kPreviewWidth, kPreviewHeight);
        centreWithSize(kPreviewWidth, kPreviewHeight);
    }
    
    setPreviewMode(!isMaximized_);
}

void EqPopupComponent::updateBandControls()
{
    for (int i = 0; i < EqSettings::kNumBands; ++i) {
        auto& ctrl = bandControls_[i];
        const auto& band = settings_.bands[i];
        
        ctrl.gainSlider.setValue(band.gainDb, juce::dontSendNotification);
        ctrl.freqSlider.setValue(band.frequency, juce::dontSendNotification);
        
        // Disable freq slider for fixed-frequency bands
        const bool hasFixedFreq = (i == EqSettings::kLowShelf || i == EqSettings::kHighShelf);
        ctrl.freqSlider.setEnabled(!hasFixedFreq && !isPreview_);
        ctrl.gainSlider.setEnabled(!isPreview_);
    }
}

void EqPopupComponent::updateSettingsFromControls()
{
    for (int i = 0; i < EqSettings::kNumBands; ++i) {
        auto& ctrl = bandControls_[i];
        auto& band = settings_.bands[i];
        
        band.gainDb = static_cast<float>(ctrl.gainSlider.getValue());
        band.frequency = static_cast<float>(ctrl.freqSlider.getValue());
    }
    
    if (onSettingsChanged)
        onSettingsChanged(settings_);
}

} // namespace OpenTune