#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
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
    SplitClip,
    MergeClips,
    DuplicateClip,
    NudgeLeft,
    NudgeRight,
    ToggleSnap,
    // Tool switching
    ToolDrawNote,
    ToolSelect,
    ToolLineAnchor,
    ToolHandDraw,
    ToolAutoTune,
    ToolTimeTool,
    // Cancel / Deselect
    CancelSelection,
    // OpenDyne tools (Melodyne-style)
    ToolODSelect,
    ToolODPitch,
    ToolODPitchModulation,
    ToolODPitchDrift,
    ToolODVolumeEnvelope,
    ToolODScissors,
    Eq,
    ScrollHorizontalLeft,
    ScrollHorizontalRight,
    ZoomHorizontalIn,
    ZoomHorizontalOut,
    ZoomVerticalIn,
    ZoomVerticalOut,
    FitSelectedNotes,
    FitAllNotes,
    Count
};

enum class ShortcutScope : std::uint8_t {
    PluginEditorGeneral = 1 << 0,
    StandaloneArrangement = 1 << 1,
    PianoRollOpenTune = 1 << 2,
    PianoRollOpenDyne = 1 << 3
};

using ShortcutScopeMask = std::uint8_t;

constexpr ShortcutScopeMask scopeMask(ShortcutScope scope)
{
    return static_cast<ShortcutScopeMask>(scope);
}

inline constexpr ShortcutScopeMask kPluginEditorGeneralScope = scopeMask(ShortcutScope::PluginEditorGeneral);
inline constexpr ShortcutScopeMask kStandaloneArrangementScope = scopeMask(ShortcutScope::StandaloneArrangement);
inline constexpr ShortcutScopeMask kPianoRollOpenTuneScope = scopeMask(ShortcutScope::PianoRollOpenTune);
inline constexpr ShortcutScopeMask kPianoRollOpenDyneScope = scopeMask(ShortcutScope::PianoRollOpenDyne);
inline constexpr ShortcutScopeMask kStandaloneShortcutScopes =
    kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope;

constexpr bool scopesIntersect(ShortcutScopeMask lhs, ShortcutScopeMask rhs)
{
    return (lhs & rhs) != 0;
}

enum class BindingType {
    Keyboard,
    WheelUp,
    WheelDown,
    WheelLeft,
    WheelRight,
    MiddleMouseButton,
    MouseBack,
    MouseForward
};

inline bool getWheelBindingType(const juce::MouseWheelDetails& wheel,
                                juce::ModifierKeys modifiers,
                                BindingType& type)
{
    // Shortcut labels follow physical wheel motion; native handlers continue using raw deltas.
    const float direction = wheel.isReversed ? -1.0f : 1.0f;
    float deltaX = wheel.deltaX * direction;
    float deltaY = wheel.deltaY * direction;

#if JUCE_MAC || JUCE_IOS
    if (modifiers.isShiftDown() && deltaY == 0.0f && deltaX != 0.0f) {
        deltaY = deltaX;
        deltaX = 0.0f;
    }
#else
    juce::ignoreUnused(modifiers);
#endif

    if (deltaY != 0.0f && std::abs(deltaY) >= std::abs(deltaX))
        type = deltaY > 0.0f ? BindingType::WheelUp : BindingType::WheelDown;
    else if (deltaX != 0.0f)
        type = deltaX > 0.0f ? BindingType::WheelLeft : BindingType::WheelRight;
    else
        return false;

    return true;
}

struct KeyBinding {
    BindingType type = BindingType::Keyboard;
    int keyCode = 0;
    juce::ModifierKeys modifiers;
    
    KeyBinding() = default;
    KeyBinding(int code, juce::ModifierKeys mods) : keyCode(code), modifiers(mods) {}
    KeyBinding(BindingType bindingType, juce::ModifierKeys mods = {})
        : type(bindingType), modifiers(mods) {}

    bool isValid() const
    {
        switch (type) {
            case BindingType::Keyboard: return keyCode > 0;
            case BindingType::WheelUp:
            case BindingType::WheelDown:
            case BindingType::WheelLeft:
            case BindingType::WheelRight:
            case BindingType::MiddleMouseButton:
            case BindingType::MouseBack:
            case BindingType::MouseForward: return true;
        }
        return false;
    }
    
    bool operator==(const KeyBinding& other) const
    {
        if (type != other.type)
            return false;
        if (keyCode != other.keyCode)
            return false;
        const bool ctrlMatch = type == BindingType::Keyboard
            ? (modifiers.isCtrlDown() || modifiers.isCommandDown())
                == (other.modifiers.isCtrlDown() || other.modifiers.isCommandDown())
            : modifiers.isCtrlDown() == other.modifiers.isCtrlDown()
                && modifiers.isCommandDown() == other.modifiers.isCommandDown();
        bool shiftMatch = modifiers.isShiftDown() == other.modifiers.isShiftDown();
        bool altMatch = modifiers.isAltDown() == other.modifiers.isAltDown();
        return ctrlMatch && shiftMatch && altMatch;
    }
    
    bool operator!=(const KeyBinding& other) const { return !(*this == other); }
};

struct ShortcutInfo {
    ShortcutId id;
    const char* storageKey;
    const char* displayNameKey;
    ShortcutScopeMask scopes;
    std::vector<KeyBinding> defaultBindings;

    template <size_t StorageKeySize>
    ShortcutInfo(ShortcutId shortcutId,
                 const char (&key)[StorageKeySize],
                 const char* displayName,
                 ShortcutScopeMask shortcutScopes,
                 std::initializer_list<KeyBinding> defaults)
        : id(shortcutId)
        , storageKey(key)
        , displayNameKey(displayName)
        , scopes(shortcutScopes)
        , defaultBindings(defaults)
    {
        static_assert(StorageKeySize > 1, "Shortcut storage key must not be empty");
    }
};

struct ShortcutBinding {
    std::vector<KeyBinding> bindings;
    
    void addBinding(const KeyBinding& binding)
    {
        if (!binding.isValid())
            return;
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
            names.add(getDisplayName(b));
        return names.joinIntoString(", ");
    }

private:
    static juce::String getDisplayName(const KeyBinding& binding)
    {
        if (binding.type != BindingType::Keyboard) {
            juce::String text;
#if JUCE_MAC || JUCE_IOS
            if (binding.modifiers.isCtrlDown()) text << "Ctrl+";
            if (binding.modifiers.isCommandDown()) text << "Cmd+";
#else
            if (binding.modifiers.isCtrlDown() || binding.modifiers.isCommandDown()) text << "Ctrl+";
#endif
            if (binding.modifiers.isShiftDown()) text << "Shift+";
            if (binding.modifiers.isAltDown()) text << "Alt+";
            switch (binding.type) {
                case BindingType::WheelUp: text << "Wheel Up"; break;
                case BindingType::WheelDown: text << "Wheel Down"; break;
                case BindingType::WheelLeft: text << "Wheel Left"; break;
                case BindingType::WheelRight: text << "Wheel Right"; break;
                case BindingType::MiddleMouseButton: text << "Middle Mouse Button"; break;
                case BindingType::MouseBack: text << "Mouse Back"; break;
                case BindingType::MouseForward: text << "Mouse Forward"; break;
                case BindingType::Keyboard: break;
            }
            return text;
        }

        const int keyCode = binding.keyCode;
        const auto modifiers = binding.modifiers;
        juce::String name;
        
#if JUCE_MAC || JUCE_IOS
        // macOS convention: symbol modifiers without + separators, order: ⌃⌥⇧⌘
        if (modifiers.isCtrlDown() && !modifiers.isCommandDown())
            name << juce::String::fromUTF8("\xe2\x8c\x83"); // ⌃ (Control)
        if (modifiers.isAltDown())
            name << juce::String::fromUTF8("\xe2\x8c\xa5"); // ⌥ (Option)
        if (modifiers.isShiftDown())
            name << juce::String::fromUTF8("\xe2\x87\xa7"); // ⇧ (Shift)
        if (modifiers.isCommandDown())
            name << juce::String::fromUTF8("\xe2\x8c\x98"); // ⌘ (Command)
#else
        if (modifiers.isCtrlDown() || modifiers.isCommandDown())
            name << "Ctrl+";
        if (modifiers.isShiftDown())
            name << "Shift+";
        if (modifiers.isAltDown())
            name << "Alt+";
#endif
        
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
        else if (keyCode >= juce::KeyPress::F1Key && keyCode <= juce::KeyPress::F12Key)
            name << "F" << (keyCode - juce::KeyPress::F1Key + 1);
        else if (keyCode >= 32 && keyCode < 127)
            name << juce::String::charToString(static_cast<juce::juce_wchar>(keyCode)).toUpperCase();
        else
            name << "Key" << keyCode;
        
        return name;
    }
};

inline const ShortcutInfo kShortcutInfos[] = {
    // Transport（PlayPause / Stop / PlayFromStart）：VST3/ARA 插件内 transport 归宿主所有，
    // 插件运行时不消费这些快捷键，也不发送任何 transport 请求；绑定只对 Standalone 生效。
    { ShortcutId::PlayPause, "shared.shortcuts.playPause", Loc::Keys::kPlayPause,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding(juce::KeyPress::spaceKey, {}) } },
#if JUCE_MAC || JUCE_IOS
    // Stop: 只对 Standalone 生效（插件内 transport 归宿主）。
    { ShortcutId::Stop, "shared.shortcuts.stop", Loc::Keys::kStop,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding('.', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier
                                | juce::ModifierKeys::shiftModifier) } },
    // PlayFromStart: 只对 Standalone 生效（插件内 transport 归宿主）。
    { ShortcutId::PlayFromStart, "shared.shortcuts.playFromStart", Loc::Keys::kPlayFromStart,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding(juce::KeyPress::returnKey,
                   juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier) } },
#else
    // Stop: 只对 Standalone 生效（插件内 transport 归宿主）。
    { ShortcutId::Stop, "shared.shortcuts.stop", Loc::Keys::kStop,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding(juce::KeyPress::returnKey, {}) } },
    // PlayFromStart: 只对 Standalone 生效（插件内 transport 归宿主）。
    { ShortcutId::PlayFromStart, "shared.shortcuts.playFromStart", Loc::Keys::kPlayFromStart,
      kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding('A', {}) } },
#endif
    { ShortcutId::Undo, "shared.shortcuts.undo", Loc::Keys::kUndo,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding('Z', juce::ModifierKeys::commandModifier) } },
    { ShortcutId::Redo, "shared.shortcuts.redo", Loc::Keys::kRedo,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope, {
        KeyBinding('Y', juce::ModifierKeys::commandModifier),
        KeyBinding('Z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier)
    } },
    { ShortcutId::Cut, "shared.shortcuts.cut", Loc::Keys::kCut,
      kPluginEditorGeneralScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding('X', juce::ModifierKeys::commandModifier) } },
    { ShortcutId::Copy, "shared.shortcuts.copy", Loc::Keys::kCopy,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding('C', juce::ModifierKeys::commandModifier) } },
    { ShortcutId::Paste, "shared.shortcuts.paste", Loc::Keys::kPaste,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding('V', juce::ModifierKeys::commandModifier) } },
    { ShortcutId::SelectAll, "shared.shortcuts.selectAll", Loc::Keys::kSelectAll,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding('A', juce::ModifierKeys::commandModifier) } },
#if JUCE_MAC || JUCE_IOS
    { ShortcutId::Delete, "shared.shortcuts.delete", Loc::Keys::kDelete,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope, {
        KeyBinding(juce::KeyPress::deleteKey, {}),
        KeyBinding(juce::KeyPress::backspaceKey, {})
    } },
#else
    { ShortcutId::Delete, "shared.shortcuts.delete", Loc::Keys::kDelete,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope, {
        KeyBinding(juce::KeyPress::deleteKey, {}),
        KeyBinding(juce::KeyPress::backspaceKey, {}),
        KeyBinding('1', {})
    } },
#endif
#if JUCE_MAC || JUCE_IOS
    { ShortcutId::SplitClip, "shared.shortcuts.splitClip", Loc::Keys::kSplitClip,
      kStandaloneArrangementScope, { KeyBinding('E', juce::ModifierKeys::commandModifier) } },
    { ShortcutId::MergeClips, "shared.shortcuts.mergeClips", Loc::Keys::kMergeClips,
      kStandaloneArrangementScope, { KeyBinding('J', juce::ModifierKeys::commandModifier) } },
#else
    { ShortcutId::SplitClip, "shared.shortcuts.splitClip", Loc::Keys::kSplitClip,
      kStandaloneArrangementScope, { KeyBinding('S', {}) } },
    { ShortcutId::MergeClips, "shared.shortcuts.mergeClips", Loc::Keys::kMergeClips,
      kStandaloneArrangementScope, { KeyBinding('M', {}) } },
#endif
    { ShortcutId::DuplicateClip, "shared.shortcuts.duplicateClip", Loc::Keys::kDuplicateClip,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding('D', juce::ModifierKeys::commandModifier) } },
    { ShortcutId::NudgeLeft, "shared.shortcuts.nudgeLeft", Loc::Keys::kNudgeLeft,
      kStandaloneArrangementScope, { KeyBinding(juce::KeyPress::leftKey, {}) } },
    { ShortcutId::NudgeRight, "shared.shortcuts.nudgeRight", Loc::Keys::kNudgeRight,
      kStandaloneArrangementScope, { KeyBinding(juce::KeyPress::rightKey, {}) } },
    { ShortcutId::ToggleSnap, "shared.shortcuts.toggleSnap", Loc::Keys::kToggleSnap,
      kStandaloneArrangementScope,
      { KeyBinding('S', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier) } },
    { ShortcutId::ToolDrawNote, "shared.shortcuts.toolDrawNote", Loc::Keys::kToolDrawNote,
      kPianoRollOpenTuneScope, { KeyBinding('2', {}) } },
    { ShortcutId::ToolSelect, "shared.shortcuts.toolSelect", Loc::Keys::kToolSelect,
      kPianoRollOpenTuneScope, { KeyBinding('3', {}) } },
    { ShortcutId::ToolLineAnchor, "shared.shortcuts.toolLineAnchor", Loc::Keys::kToolLineAnchor,
      kPianoRollOpenTuneScope, { KeyBinding('4', {}) } },
#if JUCE_MAC || JUCE_IOS
    { ShortcutId::ToolHandDraw, "shared.shortcuts.toolHandDraw", Loc::Keys::kToolHandDraw,
      kPianoRollOpenTuneScope | kPianoRollOpenDyneScope, { KeyBinding('H', {}) } },
    { ShortcutId::ToolAutoTune, "shared.shortcuts.toolAutoTune", Loc::Keys::kToolAutoTune,
      kPluginEditorGeneralScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding('A', {}) } },
#else
    { ShortcutId::ToolHandDraw, "shared.shortcuts.toolHandDraw", Loc::Keys::kToolHandDraw,
      kPianoRollOpenTuneScope | kPianoRollOpenDyneScope, { KeyBinding('5', {}) } },
    { ShortcutId::ToolAutoTune, "shared.shortcuts.toolAutoTune", Loc::Keys::kToolAutoTune,
      kPluginEditorGeneralScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding('6', {}) } },
#endif
    { ShortcutId::ToolTimeTool, "shared.shortcuts.toolTimeTool", Loc::Keys::kToolTimeTool,
      kPianoRollOpenTuneScope | kPianoRollOpenDyneScope, { KeyBinding('t', {}) } },
    { ShortcutId::CancelSelection, "shared.shortcuts.cancelSelection", Loc::Keys::kCancelSelection,
      kPluginEditorGeneralScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding(juce::KeyPress::escapeKey, {}) } },
#if JUCE_MAC || JUCE_IOS
    // OpenDyne工具按界面顺序分配数字键1-6
    { ShortcutId::ToolODSelect, "shared.shortcuts.toolODSelect", Loc::Keys::kToolODSelect,
      kPianoRollOpenDyneScope, { KeyBinding('1', {}) } },
    { ShortcutId::ToolODPitch, "shared.shortcuts.toolODPitch", Loc::Keys::kToolODPitch,
      kPianoRollOpenDyneScope, { KeyBinding('2', {}) } },
    { ShortcutId::ToolODPitchModulation, "shared.shortcuts.toolODPitchModulation", Loc::Keys::kToolODPitchModulation,
      kPianoRollOpenDyneScope, { KeyBinding('3', {}) } },
    { ShortcutId::ToolODPitchDrift, "shared.shortcuts.toolODPitchDrift", Loc::Keys::kToolODPitchDrift,
      kPianoRollOpenDyneScope, { KeyBinding('4', {}) } },
    { ShortcutId::ToolODVolumeEnvelope, "shared.shortcuts.toolODVolumeEnvelope", Loc::Keys::kToolODVolumeEnvelope,
      kPianoRollOpenDyneScope, { KeyBinding('5', {}) } },
    { ShortcutId::ToolODScissors, "shared.shortcuts.toolODScissors", Loc::Keys::kToolODScissors,
      kPianoRollOpenDyneScope, { KeyBinding('6', {}) } },
#else
    { ShortcutId::ToolODSelect, "shared.shortcuts.toolODSelect", Loc::Keys::kToolODSelect,
      kPianoRollOpenDyneScope, { KeyBinding(juce::KeyPress::F1Key, {}) } },
    { ShortcutId::ToolODPitch, "shared.shortcuts.toolODPitch", Loc::Keys::kToolODPitch,
      kPianoRollOpenDyneScope, { KeyBinding(juce::KeyPress::F2Key, {}) } },
    // F3/F5: free keys next to F2 and F4/F6, so modulation and drift are reachable without the mouse.
    { ShortcutId::ToolODPitchModulation, "shared.shortcuts.toolODPitchModulation", Loc::Keys::kToolODPitchModulation,
      kPianoRollOpenDyneScope, { KeyBinding(juce::KeyPress::F3Key, {}) } },
    { ShortcutId::ToolODPitchDrift, "shared.shortcuts.toolODPitchDrift", Loc::Keys::kToolODPitchDrift,
      kPianoRollOpenDyneScope, { KeyBinding(juce::KeyPress::F5Key, {}) } },
    { ShortcutId::ToolODVolumeEnvelope, "shared.shortcuts.toolODVolumeEnvelope", Loc::Keys::kToolODVolumeEnvelope,
      kPianoRollOpenDyneScope, { KeyBinding(juce::KeyPress::F4Key, {}) } },
    { ShortcutId::ToolODScissors, "shared.shortcuts.toolODScissors", Loc::Keys::kToolODScissors,
      kPianoRollOpenDyneScope, { KeyBinding(juce::KeyPress::F6Key, {}) } },
#endif
    { ShortcutId::Eq, "shared.shortcuts.toolEq", Loc::Keys::kToolEq,
      kPluginEditorGeneralScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding('E', {}) } },
    { ShortcutId::ScrollHorizontalLeft, "shared.shortcuts.scrollHorizontalLeft", Loc::Keys::kScrollHorizontalLeft,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding(juce::KeyPress::leftKey, juce::ModifierKeys::altModifier),
        KeyBinding(BindingType::WheelUp, juce::ModifierKeys::altModifier) } },
    { ShortcutId::ScrollHorizontalRight, "shared.shortcuts.scrollHorizontalRight", Loc::Keys::kScrollHorizontalRight,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding(juce::KeyPress::rightKey, juce::ModifierKeys::altModifier),
        KeyBinding(BindingType::WheelDown, juce::ModifierKeys::altModifier) } },
#if JUCE_MAC || JUCE_IOS
    { ShortcutId::ZoomHorizontalIn, "shared.shortcuts.zoomHorizontalIn", Loc::Keys::kZoomHorizontalIn,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding(juce::KeyPress::rightKey,
                   juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier),
        KeyBinding(BindingType::WheelUp, juce::ModifierKeys::ctrlModifier) } },
    { ShortcutId::ZoomHorizontalOut, "shared.shortcuts.zoomHorizontalOut", Loc::Keys::kZoomHorizontalOut,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding(juce::KeyPress::leftKey,
                   juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier),
        KeyBinding(BindingType::WheelDown, juce::ModifierKeys::ctrlModifier) } },
    { ShortcutId::ZoomVerticalIn, "shared.shortcuts.zoomVerticalIn", Loc::Keys::kZoomVerticalIn,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding(juce::KeyPress::upKey,
                   juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier),
        KeyBinding(BindingType::WheelUp, juce::ModifierKeys::shiftModifier) } },
    { ShortcutId::ZoomVerticalOut, "shared.shortcuts.zoomVerticalOut", Loc::Keys::kZoomVerticalOut,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding(juce::KeyPress::downKey,
                   juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier),
        KeyBinding(BindingType::WheelDown, juce::ModifierKeys::shiftModifier) } },
#else
    { ShortcutId::ZoomHorizontalIn, "shared.shortcuts.zoomHorizontalIn", Loc::Keys::kZoomHorizontalIn,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding(juce::KeyPress::rightKey, juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier),
        KeyBinding(BindingType::WheelUp, juce::ModifierKeys::ctrlModifier) } },
    { ShortcutId::ZoomHorizontalOut, "shared.shortcuts.zoomHorizontalOut", Loc::Keys::kZoomHorizontalOut,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding(juce::KeyPress::leftKey, juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier),
        KeyBinding(BindingType::WheelDown, juce::ModifierKeys::ctrlModifier) } },
    { ShortcutId::ZoomVerticalIn, "shared.shortcuts.zoomVerticalIn", Loc::Keys::kZoomVerticalIn,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding(juce::KeyPress::upKey, juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier),
        KeyBinding(BindingType::WheelUp, juce::ModifierKeys::shiftModifier) } },
    { ShortcutId::ZoomVerticalOut, "shared.shortcuts.zoomVerticalOut", Loc::Keys::kZoomVerticalOut,
      kPluginEditorGeneralScope | kStandaloneArrangementScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding(juce::KeyPress::downKey, juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier),
        KeyBinding(BindingType::WheelDown, juce::ModifierKeys::shiftModifier) } },
#endif
    { ShortcutId::FitSelectedNotes, "shared.shortcuts.fitSelectedNotes", Loc::Keys::kFitSelectedNotes,
      kPluginEditorGeneralScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding('0', juce::ModifierKeys::commandModifier) } },
    { ShortcutId::FitAllNotes, "shared.shortcuts.fitAllNotes", Loc::Keys::kFitAllNotes,
      kPluginEditorGeneralScope | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope,
      { KeyBinding('0', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier) } },
};

inline constexpr size_t kShortcutCount = sizeof(kShortcutInfos) / sizeof(kShortcutInfos[0]);
static_assert(kShortcutCount == static_cast<size_t>(ShortcutId::Count),
              "Every ShortcutId must have exactly one ShortcutInfo entry");

inline juce::String getShortcutDisplayName(ShortcutId id)
{
    return Loc::get(kShortcutInfos[static_cast<size_t>(id)].displayNameKey);
}

inline bool parseKeyBinding(const juce::String& text, KeyBinding& outBinding);

struct KeyShortcutSettings {
    std::array<ShortcutBinding, static_cast<size_t>(ShortcutId::Count)> bindings;
    
    KeyShortcutSettings()
    {
        for (size_t i = 0; i < kShortcutCount; ++i)
        {
            const auto& info = kShortcutInfos[i];
            const auto idx = static_cast<size_t>(info.id);
            for (const auto& binding : info.defaultBindings)
                bindings[idx].addBinding(binding);
        }
    }
    
    static KeyShortcutSettings getDefault()
    {
        return KeyShortcutSettings{};
    }
};

inline const ShortcutInfo& getShortcutInfo(ShortcutId id)
{
    return kShortcutInfos[static_cast<size_t>(id)];
}

inline const ShortcutBinding& getShortcutBinding(const KeyShortcutSettings& settings, ShortcutId id)
{
    return settings.bindings[static_cast<size_t>(id)];
}

inline juce::String toCanonicalString(const KeyBinding& binding)
{
    if (!binding.isValid())
        return {};

    juce::String text;
    const auto modifierFlags = binding.modifiers.getRawFlags();

    if (binding.type == BindingType::Keyboard) {
#if JUCE_MAC || JUCE_IOS
        if ((modifierFlags & juce::ModifierKeys::ctrlModifier) != 0)
            text << "Ctrl+";
        if ((modifierFlags & juce::ModifierKeys::commandModifier) != 0)
            text << "Cmd+";
#else
        if ((modifierFlags & juce::ModifierKeys::ctrlModifier) != 0
            || (modifierFlags & juce::ModifierKeys::commandModifier) != 0)
            text << "Ctrl+";
#endif
    } else {
#if JUCE_MAC || JUCE_IOS
        if ((modifierFlags & juce::ModifierKeys::ctrlModifier) != 0)
            text << "Ctrl+";
        if ((modifierFlags & juce::ModifierKeys::commandModifier) != 0)
            text << "Cmd+";
#else
        if ((modifierFlags & juce::ModifierKeys::ctrlModifier) != 0
            || (modifierFlags & juce::ModifierKeys::commandModifier) != 0)
            text << "Ctrl+";
#endif
    }
    if ((modifierFlags & juce::ModifierKeys::shiftModifier) != 0) {
        text << "Shift+";
    }
    if ((modifierFlags & juce::ModifierKeys::altModifier) != 0) {
        text << "Alt+";
    }

    if (binding.type == BindingType::WheelUp) {
        text << "Wheel Up";
    } else if (binding.type == BindingType::WheelDown) {
        text << "Wheel Down";
    } else if (binding.type == BindingType::WheelLeft) {
        text << "Wheel Left";
    } else if (binding.type == BindingType::WheelRight) {
        text << "Wheel Right";
    } else if (binding.type == BindingType::MiddleMouseButton) {
        text << "Middle Mouse Button";
    } else if (binding.type == BindingType::MouseBack) {
        text << "Mouse Back";
    } else if (binding.type == BindingType::MouseForward) {
        text << "Mouse Forward";
    } else if (binding.keyCode == juce::KeyPress::spaceKey) {
        text << "Space";
    } else if (binding.keyCode == juce::KeyPress::returnKey) {
        text << "Enter";
    } else if (binding.keyCode == juce::KeyPress::escapeKey) {
        text << "Esc";
    } else if (binding.keyCode == juce::KeyPress::backspaceKey) {
        text << "Backspace";
    } else if (binding.keyCode == juce::KeyPress::deleteKey) {
        text << "Delete";
    } else if (binding.keyCode == juce::KeyPress::tabKey) {
        text << "Tab";
    } else if (binding.keyCode == juce::KeyPress::leftKey) {
        text << "Left";
    } else if (binding.keyCode == juce::KeyPress::rightKey) {
        text << "Right";
    } else if (binding.keyCode == juce::KeyPress::upKey) {
        text << "Up";
    } else if (binding.keyCode == juce::KeyPress::downKey) {
        text << "Down";
    } else if (binding.keyCode == juce::KeyPress::homeKey) {
        text << "Home";
    } else if (binding.keyCode == juce::KeyPress::endKey) {
        text << "End";
    } else if (binding.keyCode == juce::KeyPress::insertKey) {
        text << "Insert";
    } else if (binding.keyCode == juce::KeyPress::pageDownKey) {
        text << "PageDown";
    } else if (binding.keyCode == juce::KeyPress::pageUpKey) {
        text << "PageUp";
    } else if (binding.keyCode >= juce::KeyPress::F1Key && binding.keyCode <= juce::KeyPress::F12Key) {
        text << "F" << (binding.keyCode - juce::KeyPress::F1Key + 1);
    } else if (binding.keyCode >= 32 && binding.keyCode < 127) {
        text << juce::String::charToString(static_cast<juce::juce_wchar>(binding.keyCode)).toUpperCase();
    }

    return text;
}

inline juce::String toCanonicalString(const ShortcutBinding& binding)
{
    juce::StringArray parts;
    for (const auto& keyBinding : binding.bindings) {
        if (keyBinding.isValid())
            parts.add(toCanonicalString(keyBinding));
    }
    return parts.joinIntoString("|");
}

inline bool parseShortcutBinding(const juce::String& text, ShortcutBinding& outBinding)
{
    ShortcutBinding parsed;
    if (text.trim().isEmpty()) {
        outBinding = std::move(parsed);
        return true;
    }

    juce::StringArray parts;
    parts.addTokens(text, "|", "");

    for (const auto& part : parts) {
        KeyBinding keyBinding;
        if (!parseKeyBinding(part, keyBinding)) {
            return false;
        }

        if (keyBinding.keyCode >= 'a' && keyBinding.keyCode <= 'z') {
            keyBinding.keyCode = keyBinding.keyCode - ('a' - 'A');
        }

        parsed.addBinding(keyBinding);
    }

    outBinding = std::move(parsed);
    return true;
}

inline bool matchesShortcut(const KeyShortcutSettings& settings, ShortcutId id, const juce::KeyPress& key)
{
    const auto& binding = getShortcutBinding(settings, id);
    KeyBinding testBinding(key.getKeyCode(), key.getModifiers());
    return binding.hasBinding(testBinding);
}

inline bool matchesShortcut(const KeyShortcutSettings& settings, ShortcutId id, const KeyBinding& input)
{
    return getShortcutBinding(settings, id).hasBinding(input);
}

inline void setShortcutBinding(KeyShortcutSettings& settings, ShortcutId id, const KeyBinding& binding)
{
    if (!binding.isValid())
        return;
    auto& shortcutBinding = settings.bindings[static_cast<size_t>(id)];
    shortcutBinding.bindings.clear();
    shortcutBinding.addBinding(binding);
}

inline void resetShortcutBinding(KeyShortcutSettings& settings, ShortcutId id)
{
    const auto& info = getShortcutInfo(id);
    auto& shortcutBinding = settings.bindings[static_cast<size_t>(id)];
    shortcutBinding.bindings.clear();
    for (const auto& binding : info.defaultBindings) {
        shortcutBinding.addBinding(binding);
    }
}

inline void resetAllShortcutBindings(KeyShortcutSettings& settings)
{
    settings = KeyShortcutSettings::getDefault();
}

inline std::vector<ShortcutId> findConflictingShortcuts(
    const KeyShortcutSettings& settings,
    ShortcutId excludeId,
    const KeyBinding& binding,
    const std::vector<ShortcutId>& candidates,
    ShortcutScopeMask activeScopes)
{
    std::vector<ShortcutId> conflicts;
    const auto editableScopes = static_cast<ShortcutScopeMask>(
        getShortcutInfo(excludeId).scopes & activeScopes);
    for (const auto id : candidates) {
        if (id == excludeId || !scopesIntersect(editableScopes, getShortcutInfo(id).scopes))
            continue;

        const auto& shortcutBinding = getShortcutBinding(settings, id);
        if (shortcutBinding.hasBinding(binding))
            conflicts.push_back(id);
    }
    return conflicts;
}

inline ShortcutId findShortcutForBinding(const KeyShortcutSettings& settings,
                                         const KeyBinding& binding,
                                         ShortcutScopeMask activeScopes)
{
    for (size_t i = 0; i < kShortcutCount; ++i) {
        const auto id = static_cast<ShortcutId>(i);
        if (scopesIntersect(getShortcutInfo(id).scopes, activeScopes)
            && getShortcutBinding(settings, id).hasBinding(binding))
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
        else if (mod == "cmd" || mod == "command")
            modifiers = modifiers.withFlags(juce::ModifierKeys::commandModifier);
        else if (mod == "shift")
            modifiers = modifiers.withFlags(juce::ModifierKeys::shiftModifier);
        else if (mod == "alt" || mod == "opt" || mod == "option")
            modifiers = modifiers.withFlags(juce::ModifierKeys::altModifier);
        else
            return false;
    }
    
    auto keyPart = parts[parts.size() - 1].trim().toLowerCase();
    BindingType bindingType = BindingType::Keyboard;
    if (keyPart == "wheel up")
        bindingType = BindingType::WheelUp;
    else if (keyPart == "wheel down")
        bindingType = BindingType::WheelDown;
    else if (keyPart == "wheel left")
        bindingType = BindingType::WheelLeft;
    else if (keyPart == "wheel right")
        bindingType = BindingType::WheelRight;
    else if (keyPart == "middle mouse button")
        bindingType = BindingType::MiddleMouseButton;
    else if (keyPart == "mouse back")
        bindingType = BindingType::MouseBack;
    else if (keyPart == "mouse forward")
        bindingType = BindingType::MouseForward;

    if (bindingType != BindingType::Keyboard) {
        outBinding = KeyBinding(bindingType, modifiers);
        return true;
    }

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
    else if (keyPart.length() > 1 && keyPart[0] == 'f')
    {
        int num = keyPart.substring(1).getIntValue();
        if (num >= 1 && num <= 12)
            keyCode = juce::KeyPress::F1Key + num - 1;
        else
            return false;
    }
    else if (keyPart.length() == 1)
    {
        auto c = keyPart[0];
        if (c >= 'a' && c <= 'z')
            keyCode = static_cast<int>(c);
        else if ((c >= '0' && c <= '9') || (c >= 32 && c < 127 && c != '+'))
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
