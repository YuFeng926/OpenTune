// SPDX-License-Identifier: AGPL-3.0-or-later
// The dialogs and the first-run onboarding of the editor's side of ControlUiBridge: which modal windows are open,
// closing them as Escape does, the onboarding overlay and the scale of the UI (Control/Ops/UiOps.cpp). It reads the
// windows through JUCE's ModalComponentManager and calls only what the dialogs and the overlay offer publicly, so it
// needs no hook in their classes.
#include "ControlEditorBridge.h"

#include "PluginEditor.h"

#include "ControlEditorBridgeColour.h"
#include "StandaloneArrangementHelpers.h"

#include "Editor/ConfirmDialogContent.h"
#include "Editor/OnboardingOverlayComponent.h"
#include "Editor/PitchShiftDialogContent.h"
#include "Editor/Preferences/TabbedPreferencesDialog.h"
#include "Editor/ShortcutCaptureDialogContent.h"
#include "Utils/LocalizationManager.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <string>
#include <typeinfo>

namespace OpenTune {

namespace {

using DialogInfo = ControlUiBridge::DialogInfo;

/**
 * A file chooser, which is not a dialog of OpenTune: juce::FileChooser shows the dialog of the operating system from a
 * component of its own that it makes modal while the dialog is open (FileChooser::Native, a class inside JUCE that
 * only its type name tells apart), and JUCE's own file chooser window is a FileChooserDialogBox. Only the user can
 * close either.
 */
bool isFileChooser(const juce::Component& component)
{
    if (dynamic_cast<const juce::FileChooserDialogBox*>(&component) != nullptr)
        return true;
    const std::string type = typeid(component).name();
    return type.find("FileChooser") != std::string::npos;
}

/** The ColourSelector inside a component, or nullptr. */
juce::ColourSelector* findColourSelector(juce::Component& component)
{
    for (auto* child : component.getChildren()) {
        if (auto* selector = dynamic_cast<juce::ColourSelector*>(child))
            return selector;
        if (auto* nested = findColourSelector(*child))
            return nested;
    }
    return nullptr;
}

bool isTrackId(int trackId)
{
    return trackId >= 0 && trackId < OpenTuneAudioProcessor::MAX_TRACKS;
}

/** The colour picker of a track (OpenTuneAudioProcessorEditor::trackColorChangeRequested): a call-out box around a
    ColourSelector, which carries no title and no track. */
juce::CallOutBox* colourPickerOf(juce::Component* modal)
{
    auto* callOut = dynamic_cast<juce::CallOutBox*>(modal);
    return callOut != nullptr && findColourSelector(*callOut) != nullptr ? callOut : nullptr;
}

/** The kind of a ConfirmDialogContent from its buttons: message for ConfirmDialogContent::showMessage (one button) and
    showDiagnostic (Copy, then OK, in the current language), confirm for every other use. */
const char* confirmDialogKind(const juce::Component& content)
{
    juce::StringArray texts;
    for (const auto* child : content.getChildren())
        if (const auto* button = dynamic_cast<const juce::TextButton*>(child))
            texts.add(button->getButtonText());
    if (texts.size() == 1 || (texts.size() == 2 && texts[0] == LOC(kCopy) && texts[1] == LOC(kOK)))
        return "message";
    return "confirm";
}

/**
 * What a modal window is (Source/Control/README.md, "Dialogs and onboarding"). A window the classification does not
 * know stays unknown with closeCommits true, as the protocol asks for a dialog whose behaviour the server cannot tell.
 */
DialogInfo classify(juce::Component& modal, int colourTrack)
{
    DialogInfo info;
    if (isFileChooser(modal)) {
        info.kind = "fileChooser";
        info.systemFileChooser = true;
        return info;
    }
    if (colourPickerOf(&modal) != nullptr) {
        // The content of the call-out commits the colour of its ColourSelector to the track in its destructor, however
        // the call-out closes. The track is the one of the last request for the picker (ControlTrackColourWatch); a
        // picker that was not asked for through the track panel stays unknown, with closeCommits true.
        if (isTrackId(colourTrack)) {
            info.kind = "trackColour";
            info.trackId = colourTrack;
        }
        return info;
    }
    auto* window = dynamic_cast<juce::DialogWindow*>(&modal);
    if (window == nullptr)
        return info; // A popup menu or another modal component.
    info.title = window->getName().toStdString();
    auto* content = window->getContentComponent();
    if (content == nullptr || dynamic_cast<ShortcutCaptureDialogContent*>(content) != nullptr)
        return info; // The shortcut dialog takes Escape as the new shortcut: unknown, it commits on Escape.
    if (dynamic_cast<ConfirmDialogContent*>(content) != nullptr) {
        // Escape is its dismiss path: the callback of a dismissal, never the action of a button.
        info.kind = confirmDialogKind(*content);
        info.closeCommits = false;
    } else if (dynamic_cast<PitchShiftDialogContent*>(content) != nullptr) {
        // Escape hides the window; only its confirm and reset buttons commit a shift.
        info.kind = "pitchShift";
        info.closeCommits = false;
    } else if (dynamic_cast<TabbedPreferencesDialog*>(content) != nullptr) {
        // The pages apply each setting when it changes; closing applies nothing.
        info.kind = "preferences";
        info.closeCommits = false;
    }
    return info;
}

juce::ModalComponentManager* modalManager()
{
    return juce::ModalComponentManager::getInstanceWithoutCreating();
}

bool stillModal(const juce::Component::SafePointer<juce::Component>& component)
{
    auto* manager = modalManager();
    return component != nullptr && manager != nullptr && manager->isModal(component.getComponent());
}

} // namespace

std::optional<std::vector<ControlUiBridge::DialogInfo>> ControlEditorBridge::openDialogs() const
{
    // ModalComponentManager::getModalComponent(0) is the window in front; the protocol lists the stack from the bottom.
    std::vector<DialogInfo> dialogs;
    auto* manager = modalManager();
    const int count = manager != nullptr ? manager->getNumModalComponents() : 0;
    for (int index = count - 1; index >= 0; --index)
        if (auto* component = manager->getModalComponent(index))
            dialogs.push_back(classify(*component, colourWatch_ != nullptr ? colourWatch_->lastRequestedTrack() : -1));
    return dialogs;
}

bool ControlEditorBridge::closeTopDialog()
{
    auto* manager = modalManager();
    if (manager == nullptr || manager->getNumModalComponents() == 0)
        return false;
    juce::Component::SafePointer<juce::Component> top(manager->getModalComponent(0));
    if (top == nullptr || isFileChooser(*top))
        return false;
    // The colour picker commits the colour it shows when it closes, with the destructor of its content, which the modal
    // manager runs later when it deletes the call-out box. The result of ui.dismiss reports that commit, so the same
    // steps run now (UiOps.cpp closeDialogs reads the track's colour after this returns); the destructor then sets the
    // colour it shows once more, which is the same.
    if (auto* picker = colourPickerOf(top.getComponent()); picker != nullptr && colourWatch_ != nullptr) {
        const int trackId = colourWatch_->lastRequestedTrack();
        if (auto* selector = findColourSelector(*picker); selector != nullptr && isTrackId(trackId)) {
            const auto selected = selector->getCurrentColour();
            setStandaloneTrackColour(editor_.processorRef_, trackId, selected);
            editor_.trackPanel_.setTrackColour(trackId, selected);
            editor_.arrangementView_.requestContentRedraw();
            editor_.projectSession_.markDirty();
        }
    }
    const juce::KeyPress escape(juce::KeyPress::escapeKey);
    auto* window = dynamic_cast<juce::DialogWindow*>(top.getComponent());
    juce::Component::SafePointer<juce::Component> content(window != nullptr ? window->getContentComponent() : nullptr);
    const bool shortcutDialog = dynamic_cast<ShortcutCaptureDialogContent*>(content.getComponent()) != nullptr;
    if (!shortcutDialog) {
        // Escape as the dialog gets it while it has the keyboard focus: the content first (ConfirmDialogContent and
        // PitchShiftDialogContent handle it there), then the window (DialogWindow hides itself where
        // escapeKeyTriggersCloseButton is set, which leaves the modal state). keyPressed is public in Component.
        if (content != nullptr)
            static_cast<juce::Component*>(content.getComponent())->keyPressed(escape);
        if (stillModal(top))
            static_cast<juce::Component*>(top.getComponent())->keyPressed(escape);
    }
    // A dialog that ignores Escape, and the shortcut dialog, whose Escape would become the new shortcut, close as with
    // their window's close button (the shortcut dialog then cancels); any other modal component leaves its modal state.
    if (stillModal(top) && window != nullptr)
        static_cast<juce::DocumentWindow*>(window)->closeButtonPressed();
    if (stillModal(top))
        top->exitModalState(0);
    if (stillModal(top))
        return false;
    // The window is deleted later, by the modal manager. Its content goes now, while the window is whole, so that what
    // the dialog does when it closes has happened when this returns: the track-colour dialog commits its colour in its
    // destructor. It also lets the content stop listening to its window before the window is deleted.
    if (auto* closed = dynamic_cast<juce::ResizableWindow*>(top.getComponent()))
        closed->clearContentComponent();
    return true;
}

std::optional<bool> ControlEditorBridge::onboardingShowing() const
{
    return editor_.onboardingOverlay_ != nullptr && editor_.onboardingOverlay_->isActive();
}

bool ControlEditorBridge::closeOnboarding()
{
    auto* overlay = editor_.onboardingOverlay_.get();
    if (overlay == nullptr || !overlay->isActive())
        return false;
    // OnboardingOverlayComponent::keyPressed(): Escape finishes the overlay (its private finish(), which its exit
    // button calls as well, and which restores the editor's view through the editor's callback).
    overlay->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    return !overlay->isActive();
}

std::optional<ControlUiBridge::UiScale> ControlEditorBridge::uiScale() const
{
    // The scale the editor last applied to the transport bar and the piano roll for the selected clip
    // (applyResolvedScaleForPlacementContent), which AUTO, SNAP and the pitch tool use.
    UiScale scale;
    scale.rootNote = editor_.lastResolvedScaleRootNote_;
    scale.scaleType = editor_.lastResolvedScaleType_;
    return scale;
}

} // namespace OpenTune
