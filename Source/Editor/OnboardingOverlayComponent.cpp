#include "OnboardingOverlayComponent.h"

#include <algorithm>

namespace OpenTune {
namespace {
const char* kTitle = "Choose your editing workflow";
const char* kMessage = "OpenTune edits notes; OpenDyne edits waveform blobs. The choice really switches the editor, and you can change it later in Options > Editing.";
const char* kOpenTune = "OpenTune";
const char* kOpenDyne = "OpenDyne";
const char* kLater = "Not now";
const char* kPrevious = "Previous";
const char* kNext = "Next";
const char* kFinish = "Finish";
const char* kExit = "Exit guide";

juce::String loc(const char* key) { return LOC_RAW(key); }
}

juce::String OnboardingOverlayComponent::text(const char* key) { return loc(key); }

OnboardingOverlayComponent::OnboardingOverlayComponent(
    AppPreferences& appPreferences, juce::Component& contentRoot, TopBarComponent& topBar,
    TransportBarComponent& transportBar, ParameterPanel& parameterPanel, PianoRollComponent& pianoRoll,
    TimelineOverviewComponent& overviewStrip, Environment environment,
    std::function<void(AudioEditingScheme::Scheme)> applyScheme, std::function<void(bool workspaceView)> showEditingView,
    std::function<void()> onFinished)
    : appPreferences_(appPreferences), contentRoot_(contentRoot), topBar_(topBar), transportBar_(transportBar),
      parameterPanel_(parameterPanel), pianoRoll_(pianoRoll), overviewStrip_(overviewStrip), environment_(environment),
      applyScheme_(std::move(applyScheme)), showEditingView_(std::move(showEditingView)), onFinished_(std::move(onFinished))
{
    setWantsKeyboardFocus(true);
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    setInterceptsMouseClicks(true, true);
    for (auto* button : { &previousButton_, &nextButton_, &exitButton_ })
    {
        button->setColour(juce::TextButton::buttonColourId, UIColors::backgroundLight);
        button->setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        button->setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);
        addAndMakeVisible(button);
    }
    previousButton_.onClick = [this] { previousStep(); };
    nextButton_.onClick = [this] { nextStep(); };
    exitButton_.onClick = [this] { finish(); };
    setVisible(false);
}

OnboardingOverlayComponent::~OnboardingOverlayComponent()
{
    active_ = false;
    previousButton_.onClick = {};
    nextButton_.onClick = {};
    exitButton_.onClick = {};
}

bool OnboardingOverlayComponent::isOpenDyne() const noexcept { return scheme_ == AudioEditingScheme::Scheme::NotesPrimary; }

bool OnboardingOverlayComponent::isStepApplicable(Step step) const noexcept
{
    if (step == Step::TrackView)
        return environment_ == Environment::Standalone;
    const auto value = static_cast<int>(step);
    if (value >= static_cast<int>(Step::OtDrawNote) && value <= static_cast<int>(Step::OtHandDraw))
        return !isOpenDyne();
    if (value >= static_cast<int>(Step::OdPitch) && value <= static_cast<int>(Step::OdScissors))
        return isOpenDyne();
    if (value >= static_cast<int>(Step::OtRetuneSpeed) && value <= static_cast<int>(Step::OtVibratoRate))
        return !isOpenDyne();
    if (step == Step::OdPitchGrid)
        return isOpenDyne();
    return true;
}

void OnboardingOverlayComponent::start()
{
    if (active_) return;
    scheme_ = appPreferences_.getState().shared.audioEditingScheme;
    step_ = Step::File;
    choiceResolved_ = false;
    savedParameterScrollY_ = parameterPanel_.getContentScrollY();
    active_ = true;
    setVisible(true);
    for (auto* button : { &previousButton_, &nextButton_, &exitButton_ })
        button->setVisible(false);
    updateLayout();
    toFront(false);
    grabKeyboardFocus();
    showWorkflowChoice();
}

void OnboardingOverlayComponent::showWorkflowChoice()
{
    const auto current = scheme_;
    const auto other = isOpenDyne() ? AudioEditingScheme::Scheme::CorrectedF0Primary : AudioEditingScheme::Scheme::NotesPrimary;
    juce::Component::SafePointer<OnboardingOverlayComponent> safeThis(this);
    auto* content = new ConfirmDialogContent(
        text(kTitle), text(kMessage),
        { { text(isOpenDyne() ? kOpenDyne : kOpenTune), [safeThis, current] { if (safeThis != nullptr) safeThis->chooseScheme(current); }, true },
          { text(isOpenDyne() ? kOpenTune : kOpenDyne), [safeThis, other] { if (safeThis != nullptr) safeThis->chooseScheme(other); } },
          { text(kLater), [safeThis] { if (safeThis != nullptr) safeThis->finish(); } } },
        [safeThis] { if (safeThis != nullptr && !safeThis->choiceResolved_) safeThis->finish(); },
        /*centreButtons=*/ true);
    ConfirmDialogContent::launch(content, &contentRoot_);
    appPreferences_.setOnboardingShown(true);
}

void OnboardingOverlayComponent::chooseScheme(AudioEditingScheme::Scheme scheme)
{
    if (!active_) return;
    choiceResolved_ = true;
    scheme_ = scheme;
    if (applyScheme_) applyScheme_(scheme);
    for (auto* button : { &previousButton_, &nextButton_, &exitButton_ })
        button->setVisible(true);
    juce::Component::SafePointer<OnboardingOverlayComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis]
    {
        if (safeThis != nullptr && safeThis->active_ && safeThis->isShowing())
            safeThis->grabKeyboardFocus();
    });
    beginTour();
}

void OnboardingOverlayComponent::beginTour()
{
    step_ = Step::File;
    updateStepGeometry();
    grabKeyboardFocus();
}

int OnboardingOverlayComponent::stepIndex() const noexcept
{
    int index = 0;
    for (int value = 0; value < static_cast<int>(step_); ++value)
        if (isStepApplicable(static_cast<Step>(value))) ++index;
    return index;
}

int OnboardingOverlayComponent::stepCount() const noexcept
{
    int count = 0;
    for (int value = 0; value < static_cast<int>(Step::Count); ++value)
        if (isStepApplicable(static_cast<Step>(value))) ++count;
    return count;
}

void OnboardingOverlayComponent::finish()
{
    if (!active_) return;
    active_ = false;
    choiceResolved_ = true;
    setVisible(false);
    parameterPanel_.setContentScrollY(savedParameterScrollY_);
    if (onFinished_) onFinished_();
}

void OnboardingOverlayComponent::setStep(Step step)
{
    step_ = step;
    if (showEditingView_ && (step_ == Step::TrackView || step_ == Step::PianoRoll))
    {
        showEditingView_(step_ == Step::TrackView);
    }
    updateStepGeometry();
    grabKeyboardFocus();
}

void OnboardingOverlayComponent::nextStep()
{
    if (!active_ || !choiceResolved_) return;
    auto next = static_cast<int>(step_) + 1;
    while (next < static_cast<int>(Step::Count) && !isStepApplicable(static_cast<Step>(next))) ++next;
    if (next >= static_cast<int>(Step::Count)) finish();
    else setStep(static_cast<Step>(next));
}

void OnboardingOverlayComponent::previousStep()
{
    if (!active_ || !choiceResolved_ || step_ == Step::File) return;
    auto previous = static_cast<int>(step_) - 1;
    while (previous >= 0 && !isStepApplicable(static_cast<Step>(previous))) --previous;
    if (previous >= 0) setStep(static_cast<Step>(previous));
}

juce::Component* OnboardingOverlayComponent::targetComponent() const
{
    switch (step_)
    {
        case Step::File: return &transportBar_.getFileButton();
        case Step::Edit: return &transportBar_.getEditButton();
        case Step::View: return &transportBar_.getViewButton();
        case Step::AudioEntry: return environment_ == Environment::Standalone ? nullptr : &transportBar_.getRecordButton();
        case Step::TrackView: return &transportBar_.getTrackViewButton();
        case Step::PianoRoll: return &pianoRoll_;
        case Step::Select: return parameterPanel_.getToolComponent(ToolId::Select);
        case Step::OtDrawNote: return parameterPanel_.getToolComponent(ToolId::DrawNote);
        case Step::OtLineAnchor: return parameterPanel_.getToolComponent(ToolId::LineAnchor);
        case Step::OtHandDraw: return parameterPanel_.getToolComponent(ToolId::HandDraw);
        case Step::OdPitch: return parameterPanel_.getToolComponent(ToolId::Pitch);
        case Step::OdModulation: return parameterPanel_.getToolComponent(ToolId::PitchModulation);
        case Step::OdDrift: return parameterPanel_.getToolComponent(ToolId::PitchDrift);
        case Step::OdVolumeEnvelope: return parameterPanel_.getToolComponent(ToolId::VolumeEnvelope);
        case Step::OdScissors: return parameterPanel_.getToolComponent(ToolId::Scissors);
        case Step::OtRetuneSpeed: return &parameterPanel_.getRetuneSpeedControl();
        case Step::OtVibratoDepth: return &parameterPanel_.getVibratoDepthControl();
        case Step::OtVibratoRate: return &parameterPanel_.getVibratoRateControl();
        case Step::AutoSnap: return parameterPanel_.getToolComponent(ToolId::AutoTune);
        case Step::OdPitchGrid: return &parameterPanel_.getPitchGridControl();
        case Step::Scale: return &transportBar_;
        case Step::Overview: return &overviewStrip_;
        case Step::Transport: return environment_ == Environment::Standalone ? nullptr : &transportBar_.getRecordButton();
        case Step::Help: return nullptr;
        case Step::Count: break;
    }
    return nullptr;
}

juce::Rectangle<int> OnboardingOverlayComponent::targetBoundsInOverlay() const
{
    if (step_ == Step::Transport && environment_ == Environment::Standalone)
        return contentRoot_.getLocalArea(&transportBar_, transportBar_.getPlaybackControlsBounds()).expanded(6);
    if (step_ == Step::Scale)
        return contentRoot_.getLocalArea(&transportBar_, transportBar_.getScaleControlsBounds()).expanded(6);
    if (step_ == Step::Help || (step_ == Step::AudioEntry && environment_ == Environment::Standalone))
        return {};
    auto* target = targetComponent();
    return contentRoot_.getLocalArea(target, target->getLocalBounds()).expanded(6);
}

juce::String OnboardingOverlayComponent::stepTitle() const
{
    static const char* const keys[] = {
        "File menu", "Edit menu", "View menu", "Audio entry", "Track view", "Piano roll", "Select", "Draw Note", "Line Anchor", "Hand Draw",
        "Pitch", "Modulation", "Drift", "Volume Envelope", "Scissors", "Retune Speed", "Vibrato Depth", "Vibrato Rate",
        "AUTO / SNAP", "Pitch Grid", "Scale", "Overview", "Audition", "Help and options" };
    return text(keys[static_cast<int>(step_)]);
}

juce::String OnboardingOverlayComponent::stepBody() const
{
    const char* key = "File opens Standalone import. In a plugin, use the host audio region instead of local import or export.";
    switch (step_)
    {
        case Step::File: key = environment_ == Environment::Standalone ? "Import audio, save projects and export from File." : "In a plugin, do not import, save or export here; choose audio in the host."; break;
        case Step::Edit: key = "Edit contains the editing history and undo or redo actions."; break;
        case Step::View: key = "View controls the visible editor and display settings."; break;
        case Step::AudioEntry: key = environment_ == Environment::Standalone ? "Import a file, or double-click a clip. The audio appears in the track view." : (environment_ == Environment::Ara ? "Select a host region to read its audio. Playback remains controlled by the host." : "Click Read Audio, start host playback, then click again to end capture."); break;
        case Step::TrackView: key = "Track view shows imported audio clips. Double-click a clip to enter the piano roll editor."; break;
        case Step::PianoRoll: key = isOpenDyne() ? "Waveform blobs, pitch, curves and the time ruler are shown here. Empty content reads: Import or read audio first." : "Keys, notes, pitch curves and the time ruler are shown here. Empty content reads: Import or read audio first."; break;
        case Step::Select: key = "Select notes or waveform blobs before editing them."; break;
        case Step::OtDrawNote: key = "Draw Note creates and edits note objects."; break;
        case Step::OtLineAnchor: key = "Line Anchor places points that shape the pitch curve."; break;
        case Step::OtHandDraw: key = "Hand Draw directly edits the pitch curve."; break;
        case Step::OdPitch: key = "Pitch edits the selected waveform blob's pitch."; break;
        case Step::OdModulation: key = "Modulation edits expressive pitch movement in a blob."; break;
        case Step::OdDrift: key = "Drift corrects slower pitch movement in a blob."; break;
        case Step::OdVolumeEnvelope: key = "Volume Envelope changes level over time."; break;
        case Step::OdScissors: key = "Scissors splits waveform blobs into editable parts."; break;
        case Step::OtRetuneSpeed: key = "Retune Speed controls how quickly note correction follows the target."; break;
        case Step::OtVibratoDepth: key = "Vibrato Depth controls the amount of vibrato."; break;
        case Step::OtVibratoRate: key = "Vibrato Rate controls how quickly vibrato cycles."; break;
        case Step::AutoSnap: key = isOpenDyne() ? "SNAP constrains blob edits to the selected grid." : "AUTO applies the current note correction settings."; break;
        case Step::OdPitchGrid: key = "Pitch Grid chooses how OpenDyne pitch edits snap."; break;
        case Step::Scale: key = "Transport shows the current root and scale; More contains additional scale choices."; break;
        case Step::Overview: key = "Overview moves through the whole clip while keeping the current zoom."; break;
        case Step::Transport: key = environment_ == Environment::Standalone ? "Play, pause, stop and loop here." : "Audition with the plugin host transport."; break;
        case Step::Help: key = "Options can change the editing mode and enable EQ. Help opens the user guide."; break;
        case Step::Count: break;
    }
    return text(key);
}

void OnboardingOverlayComponent::updateStepGeometry()
{
    if (!active_ || !choiceResolved_) { targetBounds_ = {}; cardBounds_ = {}; resized(); repaint(); return; }
    auto* target = targetComponent();
    const bool targetsParameterPanel = step_ == Step::Select
        || step_ == Step::OtDrawNote || step_ == Step::OtLineAnchor || step_ == Step::OtHandDraw
        || step_ == Step::OdPitch || step_ == Step::OdModulation || step_ == Step::OdDrift
        || step_ == Step::OdVolumeEnvelope || step_ == Step::OdScissors
        || step_ == Step::OtRetuneSpeed || step_ == Step::OtVibratoDepth
        || step_ == Step::OtVibratoRate || step_ == Step::AutoSnap || step_ == Step::OdPitchGrid;
    if (targetsParameterPanel)
        parameterPanel_.revealControl(*target);
    targetBounds_ = targetBoundsInOverlay();
    const int width = juce::jmin(440, juce::jmax(360, getWidth() - 32));
    const int height = juce::jmin(210, juce::jmax(175, getHeight() - 32));
    cardBounds_ = { 0, 0, width, height };
    const int gap = 16;
    if (targetBounds_.isEmpty()) cardBounds_.setCentre(getLocalBounds().getCentre());
    else if (targetBounds_.getRight() + gap + width <= getWidth()) cardBounds_.setPosition(targetBounds_.getRight() + gap, targetBounds_.getY());
    else if (targetBounds_.getX() - gap - width >= 0) cardBounds_.setPosition(targetBounds_.getX() - gap - width, targetBounds_.getY());
    else if (targetBounds_.getBottom() + gap + height <= getHeight()) cardBounds_.setPosition(targetBounds_.getX(), targetBounds_.getBottom() + gap);
    else cardBounds_.setCentre(getLocalBounds().getCentre());
    cardBounds_ = cardBounds_.constrainedWithin(getLocalBounds().reduced(12));
    resized(); repaint();
}

void OnboardingOverlayComponent::updateLayout() { setBounds(contentRoot_.getLocalBounds()); updateStepGeometry(); }

void OnboardingOverlayComponent::refreshLanguage()
{
    previousButton_.setButtonText(text(kPrevious));
    nextButton_.setButtonText(step_ == Step::Help ? text(kFinish) : text(kNext));
    exitButton_.setButtonText(text(kExit)); repaint();
}

void OnboardingOverlayComponent::resized()
{
    auto row = cardBounds_.reduced(16).removeFromBottom(kFooterHeight);
    row.removeFromRight(kStepCountWidth + kStepCountGap);
    exitButton_.setBounds(row.removeFromLeft(juce::jmin(100, row.getWidth() / 3))); row.removeFromLeft(8);
    previousButton_.setBounds(row.removeFromLeft(juce::jmin(100, row.getWidth() / 3))); row.removeFromLeft(8); nextButton_.setBounds(row);
    refreshLanguage();
}

void OnboardingOverlayComponent::paint(juce::Graphics& g)
{
    if (!active_) return;
    juce::Path dim; dim.setUsingNonZeroWinding(false); dim.addRectangle(getLocalBounds().toFloat());
    if (!targetBounds_.isEmpty()) dim.addRoundedRectangle(targetBounds_.toFloat(), 8.0f);
    g.setColour(juce::Colours::black.withAlpha(0.64f)); g.fillPath(dim);
    if (!targetBounds_.isEmpty()) { g.setColour(UIColors::accent); g.drawRoundedRectangle(targetBounds_.toFloat(), 8.0f, 2.0f); }
    if (cardBounds_.isEmpty()) return;
    UIColors::fillAuroraGlass(g, cardBounds_.toFloat(), 10.0f, 1.0f); g.setColour(UIColors::panelBorder); g.drawRoundedRectangle(cardBounds_.toFloat(), 10.0f, 1.0f);
    auto area = cardBounds_.reduced(16);
    auto titleArea = area.removeFromTop(25);
    auto bodyArea = area.withTrimmedBottom(kFooterHeight + kBodyGap).withTrimmedTop(8);
    juce::AttributedString titleText(stepTitle());
    titleText.setColour(UIColors::textPrimary);
    titleText.setFont(UIColors::getUIFont(17.0f).withStyle(juce::Font::bold));
    juce::TextLayout titleLayout; titleLayout.createLayout(titleText, (float) titleArea.getWidth());
    g.setColour(UIColors::textPrimary); titleLayout.draw(g, titleArea.toFloat());
    juce::AttributedString bodyText(stepBody());
    bodyText.setColour(UIColors::textSecondary);
    bodyText.setFont(UIColors::getUIFont(13.0f));
    juce::TextLayout bodyLayout; bodyLayout.createLayout(bodyText, (float) bodyArea.getWidth());
    g.setColour(UIColors::textSecondary); bodyLayout.draw(g, bodyArea.toFloat());
    g.setColour(UIColors::textDisabled); g.setFont(UIColors::getUIFont(11.0f));
    auto stepArea = cardBounds_.reduced(16).removeFromBottom(kFooterHeight)
                                             .removeFromRight(kStepCountWidth);
    g.drawText(juce::String(stepIndex() + 1) + " / " + juce::String(stepCount()),
               stepArea, juce::Justification::centredRight, false);
}

bool OnboardingOverlayComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey) { finish(); return true; }
    if (key.getKeyCode() == juce::KeyPress::tabKey)
    {
        if (auto* focused = juce::Component::getCurrentlyFocusedComponent()) focused->moveKeyboardFocusToSibling(key.getModifiers().isShiftDown() ? false : true);
        return true;
    }
    if (auto* focused = juce::Component::getCurrentlyFocusedComponent())
        if (focused == &previousButton_ || focused == &nextButton_ || focused == &exitButton_)
            if (key == juce::KeyPress::returnKey || key.getKeyCode() == ' ') { static_cast<juce::Button*>(focused)->triggerClick(); return true; }
    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::rightKey) { nextStep(); return true; }
    if (key == juce::KeyPress::leftKey) { previousStep(); return true; }
    return true;
}

void OnboardingOverlayComponent::visibilityChanged() { if (active_ && isShowing()) grabKeyboardFocus(); }

bool OnboardingOverlayComponent::keyStateChanged(bool) { return true; }
}
