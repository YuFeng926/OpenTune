#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Short spoken messages for NVDA, JAWS and Narrator (Windows, UI Automation)
// and VoiceOver (macOS).
namespace OpenTune::a11y {

enum class SpeechQueue
{
    replacePrevious, // a new message interrupts the previous one: navigation, values
    keepAll          // the message must not be lost: results, errors
};

// Speak text on behalf of the source component. It must be on screen and in the
// accessibility tree (usually the focused control). Title and description are not changed.
// Message thread only.
void announce(juce::Component& source, const juce::String& text,
              SpeechQueue queue = SpeechQueue::replacePrevious);

// Tell the screen reader that a check box on the source component changed: it then says
// "checked" / "not checked" itself, in its own language, as for a native check box.
// JUCE raises this event only for a toggle made through UI Automation, not for key presses.
// Message thread only.
void notifyToggleChanged(juce::Component& source, bool checked);

} // namespace OpenTune::a11y
