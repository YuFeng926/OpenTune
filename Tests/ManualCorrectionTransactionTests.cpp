#include "../Source/PluginProcessor.h"

#include <cmath>
#include <iostream>

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

bool runTransactions()
{
    OpenTuneAudioProcessor processor;
    processor.prepareToPlay(44100.0, 512);
    auto& repository = *processor.getStandaloneContentRepository();
    auto& history = processor.getUndoManager();
    const auto key = repository.createClip();
    const std::vector<float> original{100.0f, 100.0f, 0.0f, 100.0f, 100.0f, 100.0f, 100.0f};
    auto curve = std::make_shared<PitchCurve>();
    curve->setHopSize(256);
    curve->setSampleRate(44100.0);
    curve->setOriginalF0(original);
    curve->setManualCorrectionRange(0, 1, {150.0f}, PitchCorrectionSegment::Source::HandDraw);
    repository.findClip(key)->applyPitchCurve(std::move(curve));

    const auto commitStroke = [&](ManualCorrectionStrokePoint first,
                                  ManualCorrectionStrokePoint second) {
        const auto plan = planManualCorrectionStroke(
            AudioEditingScheme::Scheme::NotesPrimary, original, first, second);
        return processor.applyManualCorrection(
            key, plan.ops, plan.actualRange.startFrame,
            plan.actualRange.endFrameExclusive - 1, "Hand-drawn curve");
    };

    auto result = commitStroke({4, 220.0f}, {4, 330.0f});
    if (!expect(result.status == ManualCorrectionEditStatus::Applied
                    && history.size() == 1
                    && near(correctionAt(*result.contentSnapshot, 4), 330.0f)
                    && near(correctionAt(*result.contentSnapshot, 0), 150.0f),
                "Single-frame stroke must commit the final sample and preserve other corrections"))
        return false;
    history.undo();
    if (!expect(correctionAt(*processor.getContentSnapshot(key), 4) < 0.0f
                    && near(correctionAt(*processor.getContentSnapshot(key), 0), 150.0f),
                "Single-frame undo must restore the original correction range"))
        return false;
    history.redo();

    result = commitStroke({5, 220.0f}, {6, 440.0f});
    if (!expect(result.status == ManualCorrectionEditStatus::Applied
                    && history.size() == 2
                    && near(correctionAt(*result.contentSnapshot, 5), 220.0f)
                    && near(correctionAt(*result.contentSnapshot, 6), 440.0f),
                "Terminal stroke must include the final frame"))
        return false;
    history.undo();
    if (!expect(correctionAt(*processor.getContentSnapshot(key), 6) < 0.0f
                    && near(correctionAt(*processor.getContentSnapshot(key), 4), 330.0f),
                "Terminal undo must preserve the previous single-frame edit"))
        return false;
    history.redo();

    const auto plan = planManualCorrectionStroke(
        AudioEditingScheme::Scheme::NotesPrimary, original, {3, 800.0f}, {1, 200.0f});
    result = processor.applyManualCorrection(
        key, plan.ops, plan.actualRange.startFrame, plan.actualRange.endFrameExclusive - 1,
        "Reverse stroke across gap");
    if (!expect(result.status == ManualCorrectionEditStatus::Applied
                    && history.size() == 3
                    && near(correctionAt(*result.contentSnapshot, 1), 200.0f)
                    && correctionAt(*result.contentSnapshot, 2) < 0.0f
                    && near(correctionAt(*result.contentSnapshot, 3), 800.0f),
                "Reverse stroke must preserve editable gaps through Processor commit"))
        return false;
    for (const auto& op : plan.ops) {
        for (std::size_t offset = 0; offset < op.f0Data.size(); ++offset) {
            if (!expect(near(correctionAt(*result.contentSnapshot, op.startFrame + static_cast<int>(offset)),
                             op.f0Data[offset]),
                        "Committed correction must match the preview sample"))
                return false;
        }
    }
    history.undo();
    if (!expect(correctionAt(*processor.getContentSnapshot(key), 1) < 0.0f
                    && correctionAt(*processor.getContentSnapshot(key), 3) < 0.0f
                    && near(correctionAt(*processor.getContentSnapshot(key), 0), 150.0f)
                    && near(correctionAt(*processor.getContentSnapshot(key), 6), 440.0f),
                "Gap stroke undo must preserve corrections outside its actual range"))
        return false;
    history.redo();
    return expect(near(correctionAt(*processor.getContentSnapshot(key), 1), 200.0f)
                      && near(correctionAt(*processor.getContentSnapshot(key), 3), 800.0f),
                  "Gap stroke redo must restore generated samples");
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    return runTransactions() ? 0 : 1;
}
