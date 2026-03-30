#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "UIColors.h"

namespace OpenTune {

class MainControlPanel : public juce::Component
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void scaleChanged(int rootNote, bool isMinor) = 0;
        virtual void showWaveformChanged(bool shouldShow) = 0;
        virtual void showLanesChanged(bool shouldShow) = 0;
    };

    MainControlPanel();
    ~MainControlPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

private:
    // Scale Selection
    juce::Label scaleLabel_;
    juce::ComboBox scaleRootSelector_;    // C, C#, D, etc.
    juce::ComboBox scaleTypeSelector_;    // Major, Minor

    // Toggle Buttons
    juce::ToggleButton showWaveformButton_;
    juce::ToggleButton showLanesButton_;

    // Listeners
    juce::ListenerList<Listener> listeners_;

    // Callbacks
    void onScaleChanged();
    void onShowWaveformToggled();
    void onShowLanesToggled();

    // Styling
    void applyCustomLookAndFeel();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainControlPanel)
};

} // namespace OpenTune
