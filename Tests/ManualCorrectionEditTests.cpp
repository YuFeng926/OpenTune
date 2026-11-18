#include "Utils/ManualCorrectionEdit.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

using namespace OpenTune;

bool near(float actual, float expected, float tolerance = 0.001f)
{
    return std::abs(actual - expected) <= tolerance;
}

bool checkHandDrawStrokeDirectionAndNonMidpoints()
{
    const std::vector<float> originalF0(9, 100.0f);
    const auto forward = planManualCorrectionStroke(
        AudioEditingScheme::Scheme::NotesPrimary, originalF0, { 1, 100.0f }, { 7, 6400.0f });
    const auto reverse = planManualCorrectionStroke(
        AudioEditingScheme::Scheme::NotesPrimary, originalF0, { 7, 6400.0f }, { 1, 100.0f });
    if (forward.actualRange.startFrame != 1 || forward.actualRange.endFrameExclusive != 8
        || reverse.actualRange.startFrame != 1 || reverse.actualRange.endFrameExclusive != 8
        || forward.ops.size() != 1 || reverse.ops.size() != 1
        || forward.ops[0].f0Data.size() != 7 || reverse.ops[0].f0Data.size() != 7)
        return false;

    for (std::size_t i = 0; i < forward.ops[0].f0Data.size(); ++i) {
        const float expected = 100.0f * std::pow(2.0f, static_cast<float>(i));
        if (!near(forward.ops[0].f0Data[i], expected)
            || !near(reverse.ops[0].f0Data[i], expected))
            return false;
    }
    return near(forward.ops[0].f0Data[2], 400.0f);
}

bool checkHandDrawSingleAndTerminalFrames()
{
    const auto single = planManualCorrectionStroke(
        AudioEditingScheme::Scheme::CorrectedF0Primary,
        { 100.0f, 110.0f, 120.0f },
        { 1, 110.0f }, { 1, 220.0f });
    const auto terminal = planManualCorrectionStroke(
        AudioEditingScheme::Scheme::CorrectedF0Primary,
        { 100.0f, 110.0f, 120.0f },
        { 1, 120.0f }, { 2, 240.0f });
    return single.actualRange.startFrame == 1 && single.actualRange.endFrameExclusive == 2
        && single.ops.size() == 1 && single.ops[0].f0Data == std::vector<float>{ 220.0f }
        && terminal.actualRange.startFrame == 1 && terminal.actualRange.endFrameExclusive == 3
        && terminal.ops.size() == 1 && terminal.ops[0].endFrameExclusive == 3
        && near(terminal.ops[0].f0Data.back(), 240.0f);
}

bool checkHandDrawStrokeClipsToAvailableFrames()
{
    const auto clipped = planManualCorrectionStroke(
        AudioEditingScheme::Scheme::CorrectedF0Primary,
        { 100.0f, 100.0f, 100.0f },
        { -1, 100.0f }, { 3, 1600.0f });
    return clipped.actualRange.startFrame == 0 && clipped.actualRange.endFrameExclusive == 3
        && clipped.ops.size() == 1
        && near(clipped.ops[0].f0Data[0], 200.0f)
        && near(clipped.ops[0].f0Data[1], 400.0f)
        && near(clipped.ops[0].f0Data[2], 800.0f);
}

bool checkHandDrawStrokePreservesEditableGaps()
{
    const auto stroke = planManualCorrectionStroke(
        AudioEditingScheme::Scheme::NotesPrimary,
        { 100.0f, 110.0f, 0.0f, 130.0f, 140.0f },
        { 0, 100.0f }, { 4, 1600.0f });
    return stroke.actualRange.startFrame == 0 && stroke.actualRange.endFrameExclusive == 5
        && stroke.ops.size() == 2
        && stroke.ops[0].startFrame == 0 && stroke.ops[0].endFrameExclusive == 2
        && stroke.ops[1].startFrame == 3 && stroke.ops[1].endFrameExclusive == 5
        && near(stroke.ops[0].f0Data[0], 100.0f)
        && near(stroke.ops[0].f0Data[1], 200.0f)
        && near(stroke.ops[1].f0Data[0], 800.0f)
        && near(stroke.ops[1].f0Data[1], 1600.0f);
}

bool checkHandDrawPreviewSamplesMatchFinalOps()
{
    const std::vector<float> originalF0(12, 100.0f);
    std::vector<float> preview(originalF0.size(), -1.0f);
    int actualStartFrame = -1;
    int actualEndFrameExclusive = -1;

    const auto applyStroke = [&](const ManualCorrectionStrokePlan& stroke) {
        if (!stroke.actualRange.isValid())
            return;
        actualStartFrame = actualStartFrame < 0
            ? stroke.actualRange.startFrame
            : std::min(actualStartFrame, stroke.actualRange.startFrame);
        actualEndFrameExclusive = std::max(actualEndFrameExclusive, stroke.actualRange.endFrameExclusive);
        for (const auto& op : stroke.ops) {
            for (std::size_t i = 0; i < op.f0Data.size(); ++i)
                preview[static_cast<std::size_t>(op.startFrame) + i] = op.f0Data[i];
        }
    };
    applyStroke(planManualCorrectionStroke(
        AudioEditingScheme::Scheme::NotesPrimary, originalF0, { 1, 100.0f }, { 4, 800.0f }));
    applyStroke(planManualCorrectionStroke(
        AudioEditingScheme::Scheme::NotesPrimary, originalF0, { 8, 1600.0f }, { 10, 6400.0f }));

    const auto finalOps = planManualCorrectionOps(
        AudioEditingScheme::Scheme::NotesPrimary,
        originalF0,
        { actualStartFrame, actualEndFrameExclusive },
        [&](int frame) { return preview[static_cast<std::size_t>(frame)]; },
        PitchCorrectionSegment::Source::HandDraw);
    if (actualStartFrame != 1 || actualEndFrameExclusive != 11 || finalOps.size() != 2
        || finalOps[0].startFrame != 1 || finalOps[0].endFrameExclusive != 5
        || finalOps[1].startFrame != 8 || finalOps[1].endFrameExclusive != 11)
        return false;

    for (const auto& op : finalOps) {
        for (std::size_t i = 0; i < op.f0Data.size(); ++i) {
            const auto frame = static_cast<std::size_t>(op.startFrame) + i;
            if (!near(op.f0Data[i], preview[frame]))
                return false;
        }
    }
    return preview[5] < 0.0f && preview[6] < 0.0f && preview[7] < 0.0f;
}

bool checkEditableGapsAndOperationMetadata()
{
    const std::vector<float> originalF0{ 0.0f, 100.0f, 110.0f, 0.0f, 120.0f,
                                         130.0f, 140.0f, 150.0f, 160.0f, 170.0f, 0.0f };
    const std::vector<float> drawnF0{ 90.0f, 201.0f, 202.0f, 203.0f, 0.0f,
                                      205.0f, -1.0f, 207.0f, 208.0f, 209.0f, 210.0f };
    std::vector<int> queriedFrames;
    PitchCorrectionSegment::ParameterSnapshot parameters;
    parameters.retuneSpeed = 0.25f;
    parameters.vibratoDepth = 0.4f;
    parameters.vibratoRate = 6.0f;
    parameters.pitchDriftScale = 0.7f;

    const auto ops = planManualCorrectionOps(
        AudioEditingScheme::Scheme::NotesPrimary,
        originalF0,
        { 0, 11 },
        [&](int frame) {
            queriedFrames.push_back(frame);
            return drawnF0[static_cast<std::size_t>(frame)];
        },
        PitchCorrectionSegment::Source::HandDraw,
        parameters);

    if (ops.size() != 3
        || ops[0].startFrame != 1 || ops[0].endFrameExclusive != 3
        || ops[0].f0Data != std::vector<float>{ 201.0f, 202.0f }
        || ops[1].startFrame != 5 || ops[1].endFrameExclusive != 6
        || ops[1].f0Data != std::vector<float>{ 205.0f }
        || ops[2].startFrame != 7 || ops[2].endFrameExclusive != 10
        || ops[2].f0Data != std::vector<float>{ 207.0f, 208.0f, 209.0f })
        return false;

    const std::vector<int> expectedQueries{ 1, 2, 4, 5, 6, 7, 8, 9 };
    if (queriedFrames != expectedQueries)
        return false;

    for (const auto& op : ops) {
        if (op.source != PitchCorrectionSegment::Source::HandDraw
            || !near(op.parameterSnapshot.retuneSpeed, parameters.retuneSpeed)
            || !near(op.parameterSnapshot.vibratoDepth, parameters.vibratoDepth)
            || !near(op.parameterSnapshot.vibratoRate, parameters.vibratoRate)
            || !near(op.parameterSnapshot.pitchDriftScale, parameters.pitchDriftScale))
            return false;
    }
    return true;
}

bool checkCorrectedF0SchemeKeepsInteriorGaps()
{
    const std::vector<float> originalF0{ 0.0f, 100.0f, 110.0f, 0.0f, 120.0f,
                                         130.0f, 140.0f, 150.0f, 160.0f, 170.0f, 0.0f };
    const auto ops = planManualCorrectionOps(
        AudioEditingScheme::Scheme::CorrectedF0Primary,
        originalF0,
        { -4, 20 },
        [](int frame) { return static_cast<float>(200 + frame); },
        PitchCorrectionSegment::Source::LineAnchor);
    return ops.size() == 2
        && ops[0].startFrame == 1 && ops[0].endFrameExclusive == 3
        && ops[1].startFrame == 4 && ops[1].endFrameExclusive == 10;
}

bool checkLogFrequencyInterpolation()
{
    return near(interpolateLogF0(100.0f, 400.0f, 0.0f), 100.0f)
        && near(interpolateLogF0(100.0f, 400.0f, 0.5f), 200.0f)
        && near(interpolateLogF0(100.0f, 400.0f, 1.0f), 400.0f)
        && near(interpolateLogF0(0.0f, 16.0f, 0.5f), 4.0f);
}

bool checkSourceTrendFitIgnoresUnvoicedFrames()
{
    const std::vector<float> sourceF0{ 100.0f, 0.0f, 400.0f, 0.0f };
    const auto fit = fitOriginalF0LogTrend(sourceF0, 0, 4);
    if (!near(fit.slope, 1.0f)
        || !near(fit.intercept, std::log2(100.0f)))
        return false;

    const auto oneSample = fitOriginalF0LogTrend({ 0.0f, 256.0f, 0.0f }, 0, 3);
    const auto noSamples = fitOriginalF0LogTrend({ 0.0f, 0.0f }, 0, 2);
    return near(oneSample.intercept, 8.0f)
        && near(oneSample.slope, 0.0f)
        && near(noSamples.intercept, 0.0f)
        && near(noSamples.slope, 0.0f);
}

bool checkLineAnchorSourceShapeAtRetuneEndpoints()
{
    const std::vector<float> originalF0{ 0.0f, 4.0f, 1.0f };
    const ManualCorrectionLogLineFit trend{};
    const float fullyShaped = lineAnchorF0WithSourceShape(
        originalF0, 1, 0, 2, 100.0f, 100.0f, 0.0f, trend);
    const float fullyRetuned = lineAnchorF0WithSourceShape(
        originalF0, 1, 0, 2, 100.0f, 100.0f, 1.0f, trend);
    const float halfRetuned = lineAnchorF0WithSourceShape(
        originalF0, 1, 0, 2, 100.0f, 100.0f, 0.5f, trend);
    const float unvoiced = lineAnchorF0WithSourceShape(
        originalF0, 0, 0, 2, 100.0f, 100.0f, 0.0f, { 8.0f, 0.0f });
    return near(fullyShaped, 400.0f)
        && near(fullyRetuned, 100.0f)
        && near(halfRetuned, 200.0f)
        && near(unvoiced, 100.0f);
}

bool checkEmptyPlans()
{
    const auto emptySource = planManualCorrectionOps(
        AudioEditingScheme::Scheme::NotesPrimary, {}, { 0, 10 },
        [](int) { return 100.0f; }, PitchCorrectionSegment::Source::HandDraw);
    const auto emptyRange = planManualCorrectionOps(
        AudioEditingScheme::Scheme::CorrectedF0Primary, { 100.0f, 110.0f }, { 1, 1 },
        [](int) { return 100.0f; }, PitchCorrectionSegment::Source::HandDraw);
    return emptySource.empty() && emptyRange.empty();
}

} // namespace

int main()
{
    if (!checkHandDrawStrokeDirectionAndNonMidpoints()) {
        std::cerr << "Manual correction hand-draw direction check failed\n";
        return 1;
    }
    if (!checkHandDrawSingleAndTerminalFrames()) {
        std::cerr << "Manual correction hand-draw boundary check failed\n";
        return 1;
    }
    if (!checkHandDrawStrokeClipsToAvailableFrames()) {
        std::cerr << "Manual correction hand-draw clipping check failed\n";
        return 1;
    }
    if (!checkHandDrawStrokePreservesEditableGaps()) {
        std::cerr << "Manual correction hand-draw editable-gap check failed\n";
        return 1;
    }
    if (!checkHandDrawPreviewSamplesMatchFinalOps()) {
        std::cerr << "Manual correction hand-draw preview/commit check failed\n";
        return 1;
    }
    if (!checkEditableGapsAndOperationMetadata()) {
        std::cerr << "Manual correction editable-gap plan check failed\n";
        return 1;
    }
    if (!checkCorrectedF0SchemeKeepsInteriorGaps()) {
        std::cerr << "Manual correction CorrectedF0 gap plan check failed\n";
        return 1;
    }
    if (!checkLogFrequencyInterpolation()) {
        std::cerr << "Manual correction logarithmic interpolation check failed\n";
        return 1;
    }
    if (!checkSourceTrendFitIgnoresUnvoicedFrames()) {
        std::cerr << "Manual correction source trend fit check failed\n";
        return 1;
    }
    if (!checkLineAnchorSourceShapeAtRetuneEndpoints()) {
        std::cerr << "Manual correction line-anchor shape check failed\n";
        return 1;
    }
    if (!checkEmptyPlans()) {
        std::cerr << "Manual correction empty-plan check failed\n";
        return 1;
    }
    return 0;
}
