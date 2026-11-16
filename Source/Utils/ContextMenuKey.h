#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace OpenTune::a11y {

// Shift+F10: the standard Windows keyboard replacement for a right-click.
bool isContextMenuKey(const juce::KeyPress& key);

// ID of the first enabled top-level item that has one (submenus and separators have none),
// or 0. Pass it to PopupMenu::Options::withInitiallySelectedItem: otherwise NVDA reads the
// first item when the menu opens, but JUCE has not highlighted it and the first Enter is lost.
int firstSelectableItemId(const juce::PopupMenu& menu);

// The Applications (menu) key. JUCE treats it like a modifier key and never calls
// keyPressed for it, so call pressedNow() from keyStateChanged: it returns true once
// per press. Always false outside Windows.
class ApplicationsKeyWatcher
{
public:
    bool pressedNow();

private:
    bool wasDown_ = false;
};

} // namespace OpenTune::a11y
