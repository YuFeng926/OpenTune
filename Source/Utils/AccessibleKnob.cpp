#include "AccessibleKnob.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace OpenTune::a11y {

bool AccessibleKnob::isAdjustmentKeyDown()
{
#if JUCE_WINDOWS
    return (::GetKeyState(VK_UP) & 0x8000) != 0
        || (::GetKeyState(VK_RIGHT) & 0x8000) != 0
        || (::GetKeyState(VK_DOWN) & 0x8000) != 0
        || (::GetKeyState(VK_LEFT) & 0x8000) != 0
        || (::GetKeyState(VK_PRIOR) & 0x8000) != 0
        || (::GetKeyState(VK_NEXT) & 0x8000) != 0
        || (::GetKeyState(VK_HOME) & 0x8000) != 0
        || (::GetKeyState(VK_END) & 0x8000) != 0;
#else
    return juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::upKey)
        || juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::rightKey)
        || juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::downKey)
        || juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::leftKey)
        || juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::pageUpKey)
        || juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::pageDownKey)
        || juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::homeKey)
        || juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::endKey);
#endif
}

} // namespace OpenTune::a11y
