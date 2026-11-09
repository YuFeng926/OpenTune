#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace OpenTune::a11y {

// ComboBox with Home/End (first/last item), which the stock JUCE ComboBox ignores.
// NVDA speaks the new item itself (JUCE raises a UIA value change); an extra
// announcement here was heard twice. Pitch snap once seemed silent only because
// it was scrolled out of the panel (NVDA: offscreen), see ParameterPanel.
class AccessibleComboBox : public juce::ComboBox
{
public:
    using juce::ComboBox::ComboBox;

    bool keyPressed(const juce::KeyPress& key) override
    {
        bool handled = false;

        if (key == juce::KeyPress::homeKey || key == juce::KeyPress::endKey)
        {
            // Walk from the edge so disabled items are skipped, like the arrows do.
            const bool fromStart = key == juce::KeyPress::homeKey;
            for (int i = fromStart ? 0 : getNumItems() - 1; i >= 0 && i < getNumItems(); i += fromStart ? 1 : -1)
            {
                if (isItemEnabled(getItemId(i)))
                {
                    setSelectedItemIndex(i);
                    break;
                }
            }
            handled = true;
        }
        else
        {
            handled = juce::ComboBox::keyPressed(key);
        }

        return handled;
    }
};

} // namespace OpenTune::a11y
