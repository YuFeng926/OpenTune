#pragma once

#include <juce_core/juce_core.h>

namespace OpenTune {
namespace UiText {

// Piano Roll context menu text strings
// Using explicit UTF-8 encoding to prevent encoding issues

inline juce::String pianoRollToolSelect()
{
    return juce::String::fromUTF8(u8"鼠标选择工具\t[3]");
}

inline juce::String pianoRollToolDrawNote()
{
    return juce::String::fromUTF8(u8"绘制音符工具\t[2]");
}

inline juce::String pianoRollToolLineAnchor()
{
    return juce::String::fromUTF8(u8"锚点工具\t[4]");
}

inline juce::String pianoRollToolHandDraw()
{
    return juce::String::fromUTF8(u8"手绘工具\t[5]");
}

} // namespace UiText
} // namespace OpenTune
