#include "../Source/Utils/PitchCurve.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cout << "[FAIL] " << message << "\n";
        return false;
    }
    return true;
}

bool hasSegmentSourceOverlap(const std::vector<OpenTune::PitchCorrectionSegment>& segments,
                             OpenTune::PitchCorrectionSegment::Source source,
                             int startFrame,
                             int endFrameExclusive)
{
    for (const auto& segment : segments) {
        if (segment.source != source)
            continue;

        if (segment.endFrame > startFrame && segment.startFrame < endFrameExclusive)
            return true;
    }

    return false;
}

float renderedF0At(const OpenTune::PitchCurve& curve, int frame)
{
    float value = 0.0f;
    curve.renderFinalF0Range(frame, frame + 1, [&](int, const float* data, int count) {
        if (count > 0)
            value = data[0];
    });
    return value;
}

constexpr int kHopSize = 512;
constexpr double kF0SampleRate = 44100.0;
constexpr int kEditStartFrame = 20;
constexpr int kEditEndFrame = 40;

double secondsAtFrame(int frame)
{
    return static_cast<double>(frame) * static_cast<double>(kHopSize) / kF0SampleRate;
}

OpenTune::Note makeNoteForEditFrames()
{
    OpenTune::Note note;
    note.startTime = secondsAtFrame(kEditStartFrame);
    note.endTime = secondsAtFrame(kEditEndFrame);
    note.pitch = 440.0f;
    note.originalPitch = 220.0f;
    return note;
}

void initialiseCurve(OpenTune::PitchCurve& curve)
{
    curve.setHopSize(kHopSize);
    curve.setSampleRate(kF0SampleRate);
    curve.setOriginalF0(std::vector<float>(100, 220.0f));
}

bool f0FrameTimeUsesHopSizeAndSampleRate()
{
    if (!expect(std::abs(secondsAtFrame(kEditStartFrame) - 0.20) > 0.01,
                "f0FrameTimeUsesHopSizeAndSampleRate: frame 20 must not be treated as 0.20 seconds"))
        return false;

    if (!expect(std::abs(secondsAtFrame(kEditStartFrame) - 10240.0 / 44100.0) < 1.0e-12,
                "f0FrameTimeUsesHopSizeAndSampleRate: frame seconds must equal frame * hopSize / f0SampleRate"))
        return false;

    return true;
}

bool noteBasedEditReplacesPriorLineAnchorResult()
{
    OpenTune::PitchCurve curve;
    initialiseCurve(curve);

    curve.setManualCorrectionRange(kEditStartFrame,
                                   kEditEndFrame,
                                   std::vector<float>(20, 330.0f),
                                   OpenTune::PitchCorrectionSegment::Source::LineAnchor);

    const OpenTune::Note note = makeNoteForEditFrames();

    curve.applyCorrectionToRange({ note }, kEditStartFrame, kEditEndFrame, 1.0f, 0.0f, 7.5f);

    const auto snapshot = curve.getSnapshot();
    const auto& segments = snapshot->getCorrectionSegments();
    const auto affectedRange = OpenTune::PitchCurve::expandNoteBasedCorrectionRange(kEditStartFrame, kEditEndFrame, 100);

    if (!expect(!hasSegmentSourceOverlap(segments,
                                         OpenTune::PitchCorrectionSegment::Source::LineAnchor,
                                         affectedRange.startFrame,
                                         affectedRange.endFrameExclusive),
                "noteBasedEditReplacesPriorLineAnchorResult: LineAnchor segment must not survive inside the note edit range"))
        return false;

    if (!expect(hasSegmentSourceOverlap(segments,
                                        OpenTune::PitchCorrectionSegment::Source::NoteBased,
                                        kEditStartFrame,
                                        kEditEndFrame),
                "noteBasedEditReplacesPriorLineAnchorResult: note edit must publish a NoteBased corrected segment"))
        return false;

    const float editedF0 = renderedF0At(curve, 25);
    if (!expect(std::abs(editedF0 - 440.0f) < 1.0f,
                "noteBasedEditReplacesPriorLineAnchorResult: rendered CorrectedF0 must follow the note edit, not the old anchor"))
        return false;

    return true;
}

bool noteBasedEditReplacesPriorHandDrawResult()
{
    OpenTune::PitchCurve curve;
    initialiseCurve(curve);

    curve.setManualCorrectionRange(25,
                                   35,
                                   std::vector<float>(10, 310.0f),
                                   OpenTune::PitchCorrectionSegment::Source::HandDraw);

    const OpenTune::Note note = makeNoteForEditFrames();

    curve.applyCorrectionToRange({ note }, kEditStartFrame, kEditEndFrame, 1.0f, 0.0f, 7.5f);

    const auto snapshot = curve.getSnapshot();
    const auto& segments = snapshot->getCorrectionSegments();
    const auto affectedRange = OpenTune::PitchCurve::expandNoteBasedCorrectionRange(kEditStartFrame, kEditEndFrame, 100);

    if (!expect(!hasSegmentSourceOverlap(segments,
                                         OpenTune::PitchCorrectionSegment::Source::HandDraw,
                                         affectedRange.startFrame,
                                         affectedRange.endFrameExclusive),
                "noteBasedEditReplacesPriorHandDrawResult: HandDraw segment must not survive inside the note edit range"))
        return false;

    const float editedF0 = renderedF0At(curve, 30);
    if (!expect(std::abs(editedF0 - 440.0f) < 1.0f,
                "noteBasedEditReplacesPriorHandDrawResult: rendered CorrectedF0 must follow the note edit, not the old hand draw"))
        return false;

    return true;
}

} // namespace

int main()
{
    if (!f0FrameTimeUsesHopSizeAndSampleRate())
        return 1;

    if (!noteBasedEditReplacesPriorLineAnchorResult())
        return 1;

    if (!noteBasedEditReplacesPriorHandDrawResult())
        return 1;

    std::cout << "[PASS] PitchCurve editing contract\n";
    return 0;
}
