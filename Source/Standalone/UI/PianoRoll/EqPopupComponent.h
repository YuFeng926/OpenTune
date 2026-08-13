/**
 * EQ Popup Component — per-note EQ editor popup
 * 
 * Two modes:
 * - Preview: 3×icon width × 2×icon height, no axes/grid/coord feedback
 * - Full: Maximized, complete Qt-equivalent UI with axes, grid, legend, coord feedback
 * 
 * Features:
 * - Maximize/Minimize/Close buttons
 * - Bypass toggle
 * - Remove EQ with confirmation dialog
 * - Background color = note theme color
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utils/NoteEqSettings.h"
#include "EqGraphRenderer.h"
#include "EqBandInteraction.h"

namespace OpenTune {

class EqPopupComponent : public juce::Component,
                          public juce::Button::Listener,
                          public juce::Slider::Listener,
                          public juce::Timer {
public:
    EqPopupComponent();
    ~EqPopupComponent() override;
    
    // Set the EQ settings to edit
    void setEqSettings(const EqSettings& settings);
    
    // Get the current EQ settings
    const EqSettings& getEqSettings() const { return settings_; }
    
    // Set the note theme color for background
    void setNoteColor(juce::Colour color) { noteColor_ = color; }
    
    // Set callback for when settings change
    std::function<void(const EqSettings&)> onSettingsChanged;
    
    // Set callback for when EQ is removed
    std::function<void()> onRemoveEq;
    
    // Component interface
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    
    // Button::Listener interface
    void buttonClicked(juce::Button* button) override;
    
    // Slider::Listener interface
    void sliderValueChanged(juce::Slider* slider) override;
    
    // Timer interface
    void timerCallback() override;
    
    // Toggle between preview and full mode
    void setPreviewMode(bool isPreview);
    bool isPreviewMode() const { return isPreview_; }
    
    // Toggle maximize/minimize
    void toggleMaximize();

private:
    // UI Elements
    juce::TextButton maximizeButton_{"M"};
    juce::TextButton bypassButton_{"B"};
    juce::TextButton removeButton_{"X"};
    juce::TextButton closeButton_{"C"};
    
    // Band controls
    struct BandControl {
        juce::Slider gainSlider;
        juce::Slider freqSlider;
        juce::Label gainLabel;
        juce::Label freqLabel;
    };
    std::array<BandControl, EqSettings::kNumBands> bandControls_;
    
    // Renderer and interaction
    EqGraphRenderer renderer_;
    EqBandInteraction interaction_;
    
    // State
    EqSettings settings_;
    bool isPreview_ = true;
    bool isMaximized_ = false;
    juce::Colour noteColor_ = juce::Colours::grey;
    int selectedBand_ = -1;
    
    // Confirmation dialog
    bool showRemoveConfirmation_ = false;
    bool dontShowRemoveConfirmation_ = false;
    
    // Layout constants
    static constexpr int kPreviewWidth = 180;  // 3 × icon width (60)
    static constexpr int kPreviewHeight = 80;  // 2 × icon height (40)
    static constexpr int kFullWidth = 600;
    static constexpr int kFullHeight = 400;
    
    void layoutPreview();
    void layoutFull();
    void updateBandControls();
    void updateSettingsFromControls();
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqPopupComponent)
};

} // namespace OpenTune