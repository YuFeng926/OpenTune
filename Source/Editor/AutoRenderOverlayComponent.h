#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include <cmath>
#include "Inference/RenderCache.h"
#include "Standalone/UI/UIColors.h"
#include "Content/ContentKey.h"
#include "Utils/LocalizationManager.h"

namespace OpenTune {

enum class RenderStatus {
    Idle,
    Rendering,
    Ready
};

inline juce::String buildRenderingOverlayTitle(int completedTasks, int totalTasks, float progress)
{
    if (totalTasks <= 0)
        return LOC(kRendering);
    const int pct = static_cast<int>(std::round(progress * 100.0f));
    return LOC(kRendering) + " "
        + juce::String(pct) + "% ("
        + juce::String(completedTasks) + "/"
        + juce::String(totalTasks) + ")";
}

struct RenderStatusSnapshot {
    RenderStatus status{RenderStatus::Idle};
    RenderCache::ChunkStats chunkStats;
    ContentKey contentKey;
    uint64_t placementId{0};
    bool hasContent{false};
};

struct AutoRenderOverlayDecision {
    bool shouldClearTargetClip{false};
    bool shouldShowOverlay{false};
    bool shouldDisplayRenderStatus{false};
    RenderStatus displayStatus{RenderStatus::Idle};
};

inline RenderStatus evaluateRenderStatus(const RenderCache::StateSnapshot& cacheSnapshot)
{
    if (cacheSnapshot.chunkStats.hasActiveWork()) {
        return RenderStatus::Rendering;
    }

    if (cacheSnapshot.chunkStats.total() == 0 || !cacheSnapshot.hasNonBlankChunks) {
        return RenderStatus::Idle;
    }

    if (cacheSnapshot.hasPublishedAudio) {
        return RenderStatus::Ready;
    }

    return RenderStatus::Idle;
}

inline RenderStatusSnapshot makeRenderStatusSnapshot(ContentKey contentKey,
                                                     uint64_t placementId,
                                                     const RenderCache::StateSnapshot& cacheSnapshot)
{
    RenderStatusSnapshot snapshot;
    snapshot.status = evaluateRenderStatus(cacheSnapshot);
    snapshot.chunkStats = cacheSnapshot.chunkStats;
    snapshot.contentKey = contentKey;
    snapshot.placementId = placementId;
    snapshot.hasContent = cacheSnapshot.hasNonBlankChunks;
    return snapshot;
}

inline AutoRenderOverlayDecision evaluateAutoRenderOverlay(const RenderStatusSnapshot& snapshot,
                                                           bool hasAutoTargetClip,
                                                           bool autoTuneProcessing)
{
    AutoRenderOverlayDecision decision;

    if (hasAutoTargetClip)
    {
        if (!snapshot.contentKey.isValid())
        {
            decision.shouldClearTargetClip = true;
            return decision;
        }

        if (!autoTuneProcessing
            && (snapshot.status == RenderStatus::Ready || snapshot.status == RenderStatus::Idle))
        {
            decision.shouldClearTargetClip = true;
            return decision;
        }

        if (snapshot.status == RenderStatus::Rendering)
        {
            decision.shouldShowOverlay = true;
            decision.shouldDisplayRenderStatus = true;
            decision.displayStatus = snapshot.status;
            return decision;
        }

        return decision;
    }

    if (snapshot.status == RenderStatus::Rendering)
    {
        decision.shouldShowOverlay = true;
        decision.shouldDisplayRenderStatus = true;
        decision.displayStatus = snapshot.status;
    }

    return decision;
}

/**
 * Visual overlay used while content analysis or render work is in progress.
 * It covers PianoRoll and paints a spinner, but does not consume any input.
 */
class AutoRenderOverlayComponent : public juce::Component,
                                    private juce::Timer
{
public:
    AutoRenderOverlayComponent()
    {
        setInterceptsMouseClicks(false, false);
        setWantsKeyboardFocus(false);
        setAlwaysOnTop(true);
    }
    ~AutoRenderOverlayComponent() override = default;

    void setMessageText(const juce::String& text)
    {
        messageText_ = text;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto themeId = UIColors::currentThemeId();

        if (themeId == ThemeId::Overdose)
            g.fillAll(juce::Colour(Overdose::Colors::LoadingBackdrop).withAlpha(0.88f));
        else
            g.fillAll(juce::Colours::black.withAlpha(0.7f));

        auto bounds = getLocalBounds().toFloat();
        auto centerX = bounds.getCentreX();
        auto centerY = bounds.getCentreY();

        const float textHeight = 24.0f;
        const float spacing = 30.0f;
        const float spinnerSize = 60.0f;

        g.setColour(themeId == ThemeId::Overdose
                        ? juce::Colour(Overdose::Colors::TextOnDark)
                        : juce::Colours::white);
        g.setFont(UIColors::getUIFont(18.0f).boldened());

        float textStartY = centerY - textHeight - spacing;
        juce::Rectangle<float> textBounds(
            centerX - 200.0f,
            textStartY,
            400.0f,
            textHeight
        );
        g.drawText(messageText_, textBounds, juce::Justification::centred, false);

        const double time = juce::Time::getMillisecondCounterHiRes() * 0.001;
        const float phase = static_cast<float>(std::fmod(time * 1.5, 1.0));
        const float startAngle = phase * juce::MathConstants<float>::twoPi;
        const float endAngle = startAngle + juce::MathConstants<float>::pi * 1.5f;

        juce::Rectangle<float> spinnerBounds(
            centerX - spinnerSize / 2.0f,
            centerY + spacing / 2.0f,
            spinnerSize,
            spinnerSize
        );

        g.setColour(juce::Colours::white.withAlpha(0.2f));
        g.drawEllipse(spinnerBounds, 3.0f);

        g.setColour(themeId == ThemeId::Overdose
                        ? juce::Colour(Overdose::Colors::LoadingAccent)
                        : UIColors::accent);
        juce::Path arc;
        arc.addCentredArc(
            spinnerBounds.getCentreX(),
            spinnerBounds.getCentreY(),
            spinnerSize / 2.0f,
            spinnerSize / 2.0f,
            0.0f,
            startAngle,
            endAngle,
            true
        );
        g.strokePath(arc, juce::PathStrokeType(3.0f));
    }

    void resized() override {}

    void visibilityChanged() override
    {
        if (isVisible())
            startTimer(16);
        else
            stopTimer();
    }

private:
    void timerCallback() override { repaint(); }

    juce::String messageText_ = LOC(kRendering);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AutoRenderOverlayComponent)
};

} // namespace OpenTune
