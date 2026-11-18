#include "../Source/PluginProcessor.h"
#include "../Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h"
#include "../Source/Standalone/UI/ViewMapper.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

using namespace OpenTune;

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

float correctionAt(const EditableContentSnapshot& snapshot, int frame)
{
    for (const auto& segment : snapshot.pitchCurve->getCorrectionSegments()) {
        if (frame >= segment.startFrame && frame < segment.endFrame)
            return segment.f0Data[static_cast<std::size_t>(frame - segment.startFrame)];
    }
    return -1.0f;
}

bool near(float first, float second)
{
    return std::abs(first - second) < 0.001f;
}

juce::MouseEvent mouseEvent(juce::Component& surface,
                            juce::Point<int> position,
                            juce::Point<int> mouseDownPosition,
                            juce::Time eventTime,
                            bool dragged)
{
    return { juce::Desktop::getInstance().getMainMouseSource(), position.toFloat(),
             juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
             &surface, &surface, eventTime, mouseDownPosition.toFloat(), eventTime, 1, dragged };
}

bool runHandDrawEventTransaction()
{
    OpenTuneAudioProcessor processor;
    processor.prepareToPlay(44100.0, 512);
    auto& repository = *processor.getStandaloneContentRepository();
    auto& history = processor.getUndoManager();
    const auto key = repository.createClip();

    constexpr int hopSize = 256;
    constexpr double sampleRate = 44100.0;
    const std::vector<float> original(9, 100.0f);
    auto curve = std::make_shared<PitchCurve>();
    curve->setHopSize(hopSize);
    curve->setSampleRate(sampleRate);
    curve->setOriginalF0(original);
    curve->setManualCorrectionRange(0, 1, { 150.0f }, PitchCorrectionSegment::Source::HandDraw);

    const F0Timeline timeline(hopSize, sampleRate, static_cast<int>(original.size()));
    const double duration = timeline.timeAtFrame(timeline.endFrameExclusive());
    const ContentTimelineProjection projection{ 0.0, duration, 0.0, duration };
    const auto timeGrid = TimeGridSnapshot::makeIdentity(duration);
    ViewMapper mapper{ 0.0, sampleRate, 100, 2500, 720, 12.0f, 0.0f, 108.0f };
    constexpr int contentOriginY = 30;
    const auto pointAtFrameAndMidi = [&](int frame, int midi) {
        return juce::Point<int>{ mapper.timeToX(timeline.timeAtFrame(frame)),
                                 contentOriginY + static_cast<int>(mapper.midiToY(static_cast<float>(midi))) };
    };

    const auto mouseDownPosition = pointAtFrameAndMidi(7, 60);
    Note note;
    note.startTime = 0.0;
    note.endTime = duration;
    note.pitch = mapper.yToFreq(static_cast<float>(mouseDownPosition.y - contentOriginY));
    const std::vector<Note> notes{ note };
    auto* clip = repository.findClip(key);
    clip->applyPitchCurve(std::move(curve));
    clip->applyNotes(notes);

    InteractionState state;
    double dirtyStartTime = -1.0;
    double dirtyEndTime = -1.0;
    PianoRollToolHandler::Context context;
    context.getState = [&]() -> InteractionState& { return state; };
    context.clearNoteDraft = [&]() { state.noteDraft.clear(); };
    context.getViewMapper = [&]() { return mapper; };
    context.contentOriginY = contentOriginY;
    context.getCommittedNotes = [&]() -> const std::vector<Note>& { return notes; };
    context.getDisplayNotes = [&]() -> const std::vector<Note>& { return notes; };
    context.getPianoKeyWidth = []() { return 100; };
    context.getContentProjection = [&]() { return projection; };
    context.getActiveContentTimeGrid = [&]() { return timeGrid; };
    context.getEditableContentSnapshot = [&]() { return processor.getContentSnapshot(key); };
    context.getOriginalF0 = [&]() { return original; };
    context.getF0Timeline = [&]() { return timeline; };
    context.getAudioEditingScheme = []() { return AudioEditingScheme::Scheme::NotesPrimary; };
    context.getDirtyStartTime = [&]() { return dirtyStartTime; };
    context.setDirtyStartTime = [&](double time) { dirtyStartTime = time; };
    context.getDirtyEndTime = [&]() { return dirtyEndTime; };
    context.setDirtyEndTime = [&](double time) { dirtyEndTime = time; };
    context.getHandDrawPreviewBounds = []() { return juce::Rectangle<int>{}; };
    context.getDragThreshold = []() { return 2; };
    context.grabKeyboardFocus = []() {};
    context.notifyPitchCurveEdited = [](int, int) {};
    context.editManualCorrection = [&](std::vector<ManualCorrectionOp> ops,
                                       int startFrame,
                                       int endFrame,
                                       juce::String description) {
        return processor.applyManualCorrection(key, std::move(ops), startFrame, endFrame,
                                               std::move(description));
    };

    PianoRollToolHandler handler(std::move(context));
    handler.setTool(ToolId::HandDraw);
    juce::Component surface;
    surface.setSize(2500, 720);
    const auto eventTime = juce::Time::getCurrentTime();
    const auto firstDrag = pointAtFrameAndMidi(5, 64);
    const auto secondDrag = pointAtFrameAndMidi(3, 67);
    const auto release = pointAtFrameAndMidi(1, 72);
    const auto pitchAt = [&](juce::Point<int> point) {
        return mapper.yToFreq(static_cast<float>(point.y - contentOriginY));
    };

    handler.mouseDown(mouseEvent(surface, mouseDownPosition, mouseDownPosition, eventTime, false));
    if (!expect(state.handDrawPendingDrag && !state.emptySpaceIntent.active,
                "HandDraw must begin on the existing note"))
        return false;
    handler.mouseDrag(mouseEvent(surface, firstDrag, mouseDownPosition, eventTime, true));
    handler.mouseDrag(mouseEvent(surface, secondDrag, mouseDownPosition, eventTime, true));
    const auto& preview = state.drawing.handDrawBuffer;
    if (!expect(state.drawing.isDrawingF0
                    && near(preview[7], pitchAt(mouseDownPosition))
                    && near(preview[5], pitchAt(firstDrag))
                    && near(preview[3], pitchAt(secondDrag)),
                "HandDraw preview must contain the note press point and reverse drag samples"))
        return false;

    handler.mouseUp(mouseEvent(surface, release, mouseDownPosition, eventTime, true));
    auto snapshot = processor.getContentSnapshot(key);
    if (!expect(history.size() == 1
                    && near(correctionAt(*snapshot, 7), pitchAt(mouseDownPosition))
                    && near(correctionAt(*snapshot, 5), pitchAt(firstDrag))
                    && near(correctionAt(*snapshot, 3), pitchAt(secondDrag))
                    && near(correctionAt(*snapshot, 1), pitchAt(release)),
                "HandDraw commit must include the note press, drag, and distinct release samples"))
        return false;

    history.undo();
    snapshot = processor.getContentSnapshot(key);
    if (!expect(correctionAt(*snapshot, 1) < 0.0f && correctionAt(*snapshot, 7) < 0.0f
                    && near(correctionAt(*snapshot, 0), 150.0f),
                "HandDraw undo must restore its actual sample range and preserve the outside sample"))
        return false;

    history.redo();
    snapshot = processor.getContentSnapshot(key);
    return expect(near(correctionAt(*snapshot, 1), pitchAt(release))
                      && near(correctionAt(*snapshot, 7), pitchAt(mouseDownPosition)),
                  "HandDraw redo must restore the press and release samples");
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    return runHandDrawEventTransaction() ? 0 : 1;
}
