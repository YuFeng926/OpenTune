#include "Standalone/UI/PianoRollComponent.h"
#include "Standalone/UI/PianoRoll/PianoRollTimeMap.h"
#include "Utils/PlayHeadState.h"
#include "Utils/TimeCoordinate.h"
#include "Utils/UndoManager.h"

#include <cmath>
#include <cstdio>

namespace OpenTune {

struct PianoRollComponentTestProbe
{
    static void zoom(PianoRollComponent& roll, int x, double factor)
    {
        roll.beginZoomPreviewAt(x, factor);
    }

    static void endGesture(PianoRollComponent& roll)
    {
        roll.zoomDeadlineTicks_ = 0;
        roll.probeReportWindowStart_ = juce::Time::getMillisecondCounterHiRes();
    }

    static juce::Image capture(PianoRollComponent& roll, float scale)
    {
        juce::Image frame(juce::Image::ARGB, static_cast<int>(roll.getWidth() * scale),
                          static_cast<int>(roll.getHeight() * scale), true);
        juce::Graphics g(frame);
        g.addTransform(juce::AffineTransform::scale(scale));
        roll.paint(g);
        return frame;
    }

    static bool visibleInk(const juce::Image& layer, juce::Rectangle<int> region, float scale)
    {
        if (!layer.isValid())
            return false;
        const auto physical = (region.toFloat() * scale).getSmallestIntegerContainer()
            .getIntersection(layer.getBounds());
        juce::Image::BitmapData pixels(layer, juce::Image::BitmapData::readOnly);
        for (int y = physical.getY(); y < physical.getBottom(); ++y)
            for (int x = physical.getX(); x < physical.getRight(); ++x)
                if (pixels.getPixelColour(x, y).getAlpha() != 0)
                    return true;
        return false;
    }

    static bool markersVisible(const PianoRollComponent& roll, const EditableContentSnapshot& snapshot,
                               float scale, bool notesPrimary)
    {
        const auto view = roll.makeViewMapper();
        const auto projection = roll.activeContentProjection();
        const PianoRollTimeMap map(projection, *snapshot.timeGrid, view);
        const auto at = [&](double source, float frequency) {
            return juce::Rectangle<int>(map.sourceToX(source) - 3,
                roll.rulerHeight_ + juce::roundToInt(view.freqToY(frequency)) - 3, 7, 7);
        };
        return visibleInk(roll.contentSurface_, at(32.0, 220.0f), scale)
            && visibleInk(roll.contentSurface_, at(32.0, 440.0f), scale)
            && (notesPrimary || visibleInk(roll.waveformSurface_,
                    {map.sourceToX(32.0) - 3, roll.rulerHeight_ + 8, 7, 90}, scale));
    }

    static int staticRasters(const PianoRollComponent& roll) { return roll.staticRasterProbe_.count; }
    static int contentRasters(const PianoRollComponent& roll) { return roll.contentRasterProbe_.count; }
};

} // namespace OpenTune

namespace {

using namespace OpenTune;
using Probe = PianoRollComponentTestProbe;

bool samePixels(const juce::Image& a, const juce::Image& b)
{
    const juce::Image::BitmapData lhs(a, juce::Image::BitmapData::readOnly);
    const juce::Image::BitmapData rhs(b, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < a.getHeight(); ++y)
        for (int x = 0; x < a.getWidth(); ++x)
            if (lhs.getPixelColour(x, y) != rhs.getPixelColour(x, y))
                return false;
    return true;
}

std::shared_ptr<const EditableContentSnapshot> makeSnapshot()
{
    constexpr int seconds = 48;
    constexpr int rate = 16000;
    constexpr int hop = 160;
    constexpr int framesPerSecond = rate / hop;
    auto snapshot = std::make_shared<EditableContentSnapshot>();
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, seconds * rate);
    audio->clear();
    std::vector<float> f0(seconds * framesPerSecond);
    std::vector<float> energy(f0.size());
    for (int start : {14, 30}) {
        Note note;
        note.startTime = start;
        note.endTime = start + 4;
        note.pitch = note.originalPitch = 220.0f;
        snapshot->notes.push_back(note);
        for (int i = start * framesPerSecond; i < (start + 4) * framesPerSecond; ++i) {
            f0[i] = 440.0f;
            energy[i] = 0.75f;
        }
        for (int i = start * rate; i < (start + 4) * rate; ++i)
            audio->setSample(0, i, i % 16 < 8 ? 0.85f : -0.85f);
    }
    PitchCurve pitch;
    pitch.replaceOriginalAnalysis(std::move(f0), std::move(energy), hop, rate);
    snapshot->audioBuffer = std::move(audio);
    snapshot->audioSampleRate = rate;
    snapshot->pitchCurve = pitch.getSnapshot();
    snapshot->timeGrid = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 24.0, 25.5, HandleKind::UserAdded},
        {3, 48.0, 48.0, HandleKind::ClipEnd},
    });
    snapshot->originalF0State = OriginalF0State::Ready;
    snapshot->noteTopologyInitialized = true;
    return snapshot;
}

bool verifyZoom(const std::shared_ptr<const EditableContentSnapshot>& snapshot,
                AudioEditingScheme::Scheme scheme, float scale)
{
    const bool notesPrimary = scheme == AudioEditingScheme::Scheme::NotesPrimary;
    const char* name = notesPrimary ? "OpenDyne" : "OpenTune";
    const auto check = [=](bool ok, const char* message) {
        if (!ok)
            std::fprintf(stderr, "%s scale %.2f: %s\n", name, scale, message);
        return ok;
    };

    PlayHeadState playhead;
    UndoManager undo;
    PianoRollComponent live(playhead, undo, {}, false);
    PianoRollComponent reference(playhead, undo, {}, false);
    const ContentKey key{DomainKind::StandaloneClip, 1};
    constexpr ContentTimelineProjection placement{2.0, 48.0, 0.0, 48.0};
    for (auto* roll : {&live, &reference}) {
        roll->setSize(1000, 600);
        roll->setAudioEditingScheme(scheme);
        roll->setShowWaveform(true);
        roll->setShowPianoKeyboard(!notesPrimary);
        roll->setReadContentSnapshot([=](ContentKey requested) { return requested == key ? snapshot : nullptr; });
        roll->setEditedContentAndProjection(key, snapshot->pitchCurve, snapshot->audioBuffer,
                                            snapshot->audioSampleRate, placement);
        auto& cache = roll->getWaveformMipmapCache();
        while (!cache.isComplete())
            cache.buildIncremental(100.0);
    }

    auto initial = live.viewportState();
    initial.camera.visibleStartSeconds = 15.0;
    initial.camera.pixelsPerSecond = 300.0;
    initial.pixelsPerSemitone = 18.0f;
    initial.verticalScrollOffset = 520.0f;
    live.restoreViewportState(initial);
    Probe::capture(live, scale);
    if (!check(!Probe::markersVisible(live, *snapshot, scale, notesPrimary), "far markers should start offscreen"))
        return false;

    const int x = live.timelinePolicyViewportWidth() / 2;
    constexpr double factors[]{0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 1.25, 0.65, 0.65, 0.65, 0.65,
                               1.35, 1.35, 1.35, 1.35, 1.35, 1.35, 1.35, 1.35, 1.35, 1.35};
    bool sawGlobal = false;
    juce::Image last;
    for (double factor : factors) {
        const auto before = live.timelineCamera();
        Probe::zoom(live, x, factor);
        const auto after = live.timelineCamera();
        if (after.visibleStartSeconds > 0.0) {
            const double startAnchor = before.visibleStartSeconds + x / before.pixelsPerSecond;
            const double endAnchor = after.visibleStartSeconds + x / after.pixelsPerSecond;
            if (!check(std::abs(startAnchor - endAnchor) < 1.0e-9, "zoom anchor drifted"))
                return false;
        }
        last = Probe::capture(live, scale);
        reference.restoreViewportState(live.viewportState());
        if (!check(samePixels(last, Probe::capture(reference, scale)), "gesture frame differs from fresh current camera"))
            return false;

        if (after.visibleStartSeconds == 0.0 && x * 2 / after.pixelsPerSecond > placement.timelineEndSeconds()) {
            sawGlobal = true;
            if (!check(Probe::markersVisible(live, *snapshot, scale, notesPrimary), "newly exposed waveform, F0 or note is missing"))
                return false;
        }
    }

    Probe::endGesture(live);
    const int staticRasters = Probe::staticRasters(live);
    const int contentRasters = Probe::contentRasters(live);
    const auto settled = Probe::capture(live, scale);
    return check(sawGlobal, "zoom trajectory never exposed the entire clip")
        && check(samePixels(last, settled), "first settled frame differs from the last gesture frame")
        && check(Probe::staticRasters(live) == staticRasters && Probe::contentRasters(live) == contentRasters,
                 "ending a presented gesture rebuilt retained surfaces");
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    const auto snapshot = makeSnapshot();
    for (auto scheme : {AudioEditingScheme::Scheme::CorrectedF0Primary, AudioEditingScheme::Scheme::NotesPrimary})
        for (float scale : {1.0f, 1.25f})
            if (!verifyZoom(snapshot, scheme, scale))
                return 1;
    std::puts("PianoRollZoomRenderTests passed (OpenTune and OpenDyne, 1x and 1.25x).");
    return 0;
}
