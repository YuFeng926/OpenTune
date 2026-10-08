#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

#include "ConfirmDialogContent.h"
#include "../Standalone/UI/ParameterPanel.h"
#include "../Standalone/UI/PianoRollComponent.h"
#include "../Standalone/UI/TimelineOverviewComponent.h"
#include "../Standalone/UI/TopBarComponent.h"
#include "../Standalone/UI/TransportBarComponent.h"
#include "../Utils/AppPreferences.h"

namespace OpenTune {

class OnboardingOverlayComponent final : public juce::Component
{
public:
    enum class Environment
    {
        Standalone,
        Ara,
        Capture
    };

    OnboardingOverlayComponent(
        AppPreferences& appPreferences,
        juce::Component& contentRoot,
        TopBarComponent& topBar,
        TransportBarComponent& transportBar,
        ParameterPanel& parameterPanel,
        PianoRollComponent& pianoRoll,
        TimelineOverviewComponent& overviewStrip,
        Environment environment,
        std::function<void(AudioEditingScheme::Scheme)> applyScheme,
        std::function<void(bool workspaceView)> showEditingView,
        std::function<void()> onFinished);

    ~OnboardingOverlayComponent() override;

    void start();
    bool isActive() const noexcept { return active_; }
    void updateLayout();
    void refreshLanguage();

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;
    void visibilityChanged() override;

private:
    enum class Step
    {
        File, Edit, View, AudioEntry, TrackView,
        PianoRoll,
        Select,
        OtDrawNote, OtLineAnchor, OtHandDraw,
        OdPitch, OdModulation, OdDrift, OdVolumeEnvelope, OdScissors,
        OtRetuneSpeed, OtVibratoDepth, OtVibratoRate,
        AutoSnap, OdPitchGrid, Scale,
        Overview,
        Transport,
        Help,
        Count
    };

    static juce::String text(const char* key);

    void showWorkflowChoice();
    void chooseScheme(AudioEditingScheme::Scheme scheme);
    void beginTour();
    void finish();
    void setStep(Step step);
    void nextStep();
    void previousStep();
    void updateStepGeometry();

    juce::Component* targetComponent() const;
    juce::Rectangle<int> targetBoundsInOverlay() const;
    juce::String stepTitle() const;
    juce::String stepBody() const;
    int stepIndex() const noexcept;
    int stepCount() const noexcept;
    bool isOpenDyne() const noexcept;
    bool isStepApplicable(Step step) const noexcept;

    AppPreferences& appPreferences_;
    juce::Component& contentRoot_;
    TopBarComponent& topBar_;
    TransportBarComponent& transportBar_;
    ParameterPanel& parameterPanel_;
    PianoRollComponent& pianoRoll_;
    TimelineOverviewComponent& overviewStrip_;
    Environment environment_;
    std::function<void(AudioEditingScheme::Scheme)> applyScheme_;
    std::function<void(bool workspaceView)> showEditingView_;
    std::function<void()> onFinished_;

    AudioEditingScheme::Scheme scheme_ = AudioEditingScheme::Scheme::CorrectedF0Primary;
    Step step_ = Step::File;
    bool active_ = false;
    bool choiceResolved_ = false;
    int savedParameterScrollY_ = 0;
    juce::Rectangle<int> targetBounds_;
    juce::Rectangle<int> cardBounds_;
    static constexpr int kFooterHeight = 32;
    static constexpr int kStepCountWidth = 48;
    static constexpr int kStepCountGap = 8;
    static constexpr int kBodyGap = 10;
    juce::TextButton previousButton_ { "" };
    juce::TextButton nextButton_ { "" };
    juce::TextButton exitButton_ { "" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OnboardingOverlayComponent)
};

} // namespace OpenTune
