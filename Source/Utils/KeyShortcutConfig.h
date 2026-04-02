#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <vector>
#include "LocalizationManager.h"

namespace OpenTune::KeyShortcutConfig {

enum class ShortcutId {
    PlayPause,
    Stop,
    PlayFromStart,
    Undo,
    Redo,
    Cut,
    Copy,
    Paste,
    SelectAll,
    Delete,
    Count
};

struct KeyBinding {
    int keyCode = 0;
    juce::ModifierKeys modifiers;
    
    KeyBinding() = default;
    KeyBinding(int code, juce::ModifierKeys mods) : keyCode(code), modifiers(mods) {}
    
    bool operator==(const KeyBinding& other) const
    {
        if (keyCode != other.keyCode)
            return false;
        bool ctrlMatch = (modifiers.isCtrlDown() || modifiers.isCommandDown()) ==
                         (other.modifiers.isCtrlDown() || other.modifiers.isCommandDown());
        bool shiftMatch = modifiers.isShiftDown() == other.modifiers.isShiftDown();
        bool altMatch = modifiers.isAltDown() == other.modifiers.isAltDown();
        return ctrlMatch && shiftMatch && altMatch;
    }
    
    bool operator!=(const KeyBinding& other) const { return !(*this == other); }
};

struct ShortcutInfo {
    ShortcutId id;
    const char* displayNameKey;
    std::vector<KeyBinding> defaultBindings;
};

struct ShortcutBinding {
    std::vector<KeyBinding> bindings;
    
    void addBinding(const KeyBinding& binding)
    {
        for (const auto& existing : bindings)
        {
            if (existing == binding)
                return;
        }
        bindings.push_back(binding);
    }
    
    void removeBinding(const KeyBinding& binding)
    {
        bindings.erase(
            std::remove(bindings.begin(), bindings.end(), binding),
            bindings.end()
        );
    }
    
    bool hasBinding(const KeyBinding& binding) const
    {
        for (const auto& b : bindings)
        {
            if (b == binding)
                return true;
        }
        return false;
    }
    
    juce::String getDisplayNames() const
    {
        juce::StringArray names;
        for (const auto& b : bindings)
            names.add(getKeyDisplayName(b.keyCode, b.modifiers));
        return names.joinIntoString(", ");
    }
    
private:
    static juce::String getKeyDisplayName(int keyCode, juce::ModifierKeys modifiers)
    {
        juce::String name;
        
        if (modifiers.isCtrlDown() || modifiers.isCommandDown())
            name << "Ctrl+";
        if (modifiers.isShiftDown())
            name << "Shift+";
        if (modifiers.isAltDown())
            name << "Alt+";
        
        if (keyCode == juce::KeyPress::spaceKey)
            name << "Space";
        else if (keyCode == juce::KeyPress::returnKey)
            name << "Enter";
        else if (keyCode == juce::KeyPress::escapeKey)
            name << "Esc";
        else if (keyCode == juce::KeyPress::backspaceKey)
            name << "Backspace";
        else if (keyCode == juce::KeyPress::deleteKey)
            name << "Delete";
        else if (keyCode == juce::KeyPress::tabKey)
            name << "Tab";
        else if (keyCode == juce::KeyPress::leftKey)
            name << "Left";
        else if (keyCode == juce::KeyPress::rightKey)
            name << "Right";
        else if (keyCode == juce::KeyPress::upKey)
            name << "Up";
        else if (keyCode == juce::KeyPress::downKey)
            name << "Down";
        else if (keyCode == juce::KeyPress::homeKey)
            name << "Home";
        else if (keyCode == juce::KeyPress::endKey)
            name << "End";
        else if (keyCode == juce::KeyPress::insertKey)
            name << "Insert";
        else if (keyCode == juce::KeyPress::pageDownKey)
            name << "PageDown";
        else if (keyCode == juce::KeyPress::pageUpKey)
            name << "PageUp";
        else if (keyCode >= 32 && keyCode < 127)
            name << juce::String::charToString(static_cast<juce::juce_wchar>(keyCode)).toUpperCase();
        else
            name << "Key" << keyCode;
        
        return name;
    }
};

inline const ShortcutInfo kShortcutInfos[] = {
    { ShortcutId::PlayPause, Loc::Keys::kPlayPause, { KeyBinding(juce::KeyPress::spaceKey, {}) } },
    { ShortcutId::Stop, Loc::Keys::kStop, { KeyBinding(juce::KeyPress::returnKey, {}) } },
    { ShortcutId::PlayFromStart, Loc::Keys::kPlayFromStart, { KeyBinding('A', {}) } },
    { ShortcutId::Undo, Loc::Keys::kUndo, { KeyBinding('Z', juce::ModifierKeys::commandModifier) } },
    { ShortcutId::Redo, Loc::Keys::kRedo, { 
        KeyBinding('Y', juce::ModifierKeys::commandModifier),
        KeyBinding('Z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier)
    } },
    { ShortcutId::Cut, Loc::Keys::kCut, { KeyBinding('X', juce::ModifierKeys::commandModifier) } },
    { ShortcutId::Copy, Loc::Keys::kCopy, { KeyBinding('C', juce::ModifierKeys::commandModifier) } },
    { ShortcutId::Paste, Loc::Keys::kPaste, { KeyBinding('V', juce::ModifierKeys::commandModifier) } },
    { ShortcutId::SelectAll, Loc::Keys::kSelectAll, { KeyBinding('A', juce::ModifierKeys::commandModifier) } },
    { ShortcutId::Delete, Loc::Keys::kDelete, { 
        KeyBinding(juce::KeyPress::deleteKey, {}),
        KeyBinding(juce::KeyPress::backspaceKey, {})
    } }
};

inline const size_t kShortcutCount = sizeof(kShortcutInfos) / sizeof(kShortcutInfos[0]);

inline juce::String getShortcutDisplayName(ShortcutId id)
{
    return Loc::get(kShortcutInfos[static_cast<size_t>(id)].displayNameKey);
}

struct KeyShortcutSettings {
    std::array<ShortcutBinding, static_cast<size_t>(ShortcutId::Count)> bindings;
    
    KeyShortcutSettings()
    {
        for (size_t i = 0; i < kShortcutCount; ++i)
        {
            const auto& info = kShortcutInfos[i];
            auto idx = static_cast<size_t>(info.id);
            for (const auto& binding : info.defaultBindings)
                bindings[idx].addBinding(binding);
        }
    }
    
    static KeyShortcutSettings getDefault()
    {
        return KeyShortcutSettings{};
    }
};

inline KeyShortcutSettings& getMutableSettings()
{
    static KeyShortcutSettings instance;
    return instance;
}

inline const KeyShortcutSettings& getSettings()
{
    return getMutableSettings();
}

inline void setSettings(const KeyShortcutSettings& settings)
{
    getMutableSettings() = settings;
}

inline const ShortcutInfo& getShortcutInfo(ShortcutId id)
{
    return kShortcutInfos[static_cast<size_t>(id)];
}

inline ShortcutBinding getShortcutBinding(ShortcutId id)
{
    return getSettings().bindings[static_cast<size_t>(id)];
}

inline void addShortcutBinding(ShortcutId id, const KeyBinding& binding)
{
    getMutableSettings().bindings[static_cast<size_t>(id)].addBinding(binding);
}

inline void removeShortcutBinding(ShortcutId id, const KeyBinding& binding)
{
    getMutableSettings().bindings[static_cast<size_t>(id)].removeBinding(binding);
}

inline void setShortcutBinding(ShortcutId id, const KeyBinding& binding)
{
    auto& settings = getMutableSettings();
    auto idx = static_cast<size_t>(id);
    settings.bindings[idx].bindings.clear();
    settings.bindings[idx].addBinding(binding);
}

inline void resetShortcutBinding(ShortcutId id)
{
    const auto& info = getShortcutInfo(id);
    auto idx = static_cast<size_t>(id);
    getMutableSettings().bindings[idx].bindings.clear();
    for (const auto& binding : info.defaultBindings)
        getMutableSettings().bindings[idx].addBinding(binding);
}

inline void resetAllShortcutBindings()
{
    getMutableSettings() = KeyShortcutSettings::getDefault();
}

inline std::vector<juce::KeyPress> getKeyPresses(ShortcutId id)
{
    std::vector<juce::KeyPress> result;
    const auto& binding = getShortcutBinding(id);
    for (const auto& b : binding.bindings)
        result.push_back(juce::KeyPress(b.keyCode, b.modifiers, 0));
    return result;
}

inline bool matchesShortcut(ShortcutId id, const juce::KeyPress& key)
{
    const auto& binding = getShortcutBinding(id);
    KeyBinding testBinding(key.getKeyCode(), key.getModifiers());
    return binding.hasBinding(testBinding);
}

inline ShortcutId findConflictingShortcut(ShortcutId excludeId, const KeyBinding& binding)
{
    for (size_t i = 0; i < kShortcutCount; ++i)
    {
        auto id = static_cast<ShortcutId>(i);
        if (id == excludeId)
            continue;
        
        const auto& shortcutBinding = getShortcutBinding(id);
        if (shortcutBinding.hasBinding(binding))
            return id;
    }
    return ShortcutId::Count;
}

inline bool parseKeyBinding(const juce::String& text, KeyBinding& outBinding)
{
    juce::StringArray parts;
    parts.addTokens(text.trim(), "+", "");
    
    if (parts.isEmpty())
        return false;
    
    juce::ModifierKeys modifiers;
    
    for (int i = 0; i < parts.size() - 1; ++i)
    {
        auto mod = parts[i].trim().toLowerCase();
        if (mod == "ctrl" || mod == "control")
            modifiers = modifiers.withFlags(juce::ModifierKeys::ctrlModifier);
        else if (mod == "shift")
            modifiers = modifiers.withFlags(juce::ModifierKeys::shiftModifier);
        else if (mod == "alt")
            modifiers = modifiers.withFlags(juce::ModifierKeys::altModifier);
        else
            return false;
    }
    
    auto keyPart = parts[parts.size() - 1].trim().toLowerCase();
    int keyCode = 0;
    
    if (keyPart == "space")
        keyCode = juce::KeyPress::spaceKey;
    else if (keyPart == "enter" || keyPart == "return")
        keyCode = juce::KeyPress::returnKey;
    else if (keyPart == "esc" || keyPart == "escape")
        keyCode = juce::KeyPress::escapeKey;
    else if (keyPart == "backspace" || keyPart == "back")
        keyCode = juce::KeyPress::backspaceKey;
    else if (keyPart == "delete" || keyPart == "del")
        keyCode = juce::KeyPress::deleteKey;
    else if (keyPart == "tab")
        keyCode = juce::KeyPress::tabKey;
    else if (keyPart == "left")
        keyCode = juce::KeyPress::leftKey;
    else if (keyPart == "right")
        keyCode = juce::KeyPress::rightKey;
    else if (keyPart == "up")
        keyCode = juce::KeyPress::upKey;
    else if (keyPart == "down")
        keyCode = juce::KeyPress::downKey;
    else if (keyPart == "home")
        keyCode = juce::KeyPress::homeKey;
    else if (keyPart == "end")
        keyCode = juce::KeyPress::endKey;
    else if (keyPart == "insert" || keyPart == "ins")
        keyCode = juce::KeyPress::insertKey;
    else if (keyPart == "pagedown" || keyPart == "pgdn")
        keyCode = juce::KeyPress::pageDownKey;
    else if (keyPart == "pageup" || keyPart == "pgup")
        keyCode = juce::KeyPress::pageUpKey;
    else if (keyPart.length() == 1)
    {
        auto c = keyPart[0];
        if (c >= 'a' && c <= 'z')
            keyCode = static_cast<int>(c);
        else if (c >= '0' && c <= '9')
            keyCode = static_cast<int>(c);
        else
            return false;
    }
    else
        return false;
    
    outBinding = KeyBinding(keyCode, modifiers);
    return true;
}

} // namespace OpenTune::KeyShortcutConfig
