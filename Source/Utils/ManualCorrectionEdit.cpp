#include "ManualCorrectionEdit.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace OpenTune {

std::vector<ManualCorrectionOp> planManualCorrectionOps(
    AudioEditingScheme::Scheme scheme,
    const std::vector<float>& originalF0,
    AudioEditingScheme::FrameRange requestedRange,
    const ManualCorrectionFrameQuery& valueForFrame,
    PitchCorrectionSegment::Source source,
    PitchCorrectionSegment::ParameterSnapshot parameterSnapshot)
{
    std::vector<ManualCorrectionOp> ops;
    const auto trimmedRange = AudioEditingScheme::trimFrameRangeToEditableBounds(
        scheme, originalF0, requestedRange);
    if (!trimmedRange.isValid())
        return ops;

    int currentOpStart = -1;
    std::vector<float> currentOpData;

    auto flushCurrentOp = [&](int endFrameExclusive) {
        if (currentOpStart < 0 || currentOpData.empty()) {
            currentOpStart = -1;
            currentOpData.clear();
            return;
        }

        ManualCorrectionOp op;
        op.startFrame = currentOpStart;
        op.endFrameExclusive = endFrameExclusive;
        op.f0Data = std::move(currentOpData);
        op.source = source;
        op.parameterSnapshot = parameterSnapshot;
        ops.push_back(std::move(op));

        currentOpStart = -1;
        currentOpData.clear();
    };

    for (int frame = trimmedRange.startFrame; frame < trimmedRange.endFrameExclusive; ++frame) {
        if (!AudioEditingScheme::canEditFrame(scheme, originalF0, frame)) {
            flushCurrentOp(frame);
            continue;
        }

        const float frameValue = valueForFrame(frame);
        if (frameValue <= 0.0f) {
            flushCurrentOp(frame);
            continue;
        }

        if (currentOpStart < 0)
            currentOpStart = frame;
        currentOpData.push_back(frameValue);
    }

    flushCurrentOp(trimmedRange.endFrameExclusive);
    return ops;
}

ManualCorrectionStrokePlan planManualCorrectionStroke(
    AudioEditingScheme::Scheme scheme,
    const std::vector<float>& originalF0,
    ManualCorrectionStrokePoint first,
    ManualCorrectionStrokePoint second,
    PitchCorrectionSegment::Source source,
    PitchCorrectionSegment::ParameterSnapshot parameterSnapshot)
{
    ManualCorrectionStrokePlan plan;
    if (originalF0.empty())
        return plan;

    const bool firstIsLeft = first.frame <= second.frame;
    const int leftFrame = firstIsLeft ? first.frame : second.frame;
    const int rightFrame = firstIsLeft ? second.frame : first.frame;
    const float leftF0 = firstIsLeft ? first.f0 : second.f0;
    const float rightF0 = firstIsLeft ? second.f0 : first.f0;

    const int frameCount = static_cast<int>(originalF0.size());
    const int startFrame = std::clamp(leftFrame, 0, frameCount);
    const int endFrameExclusive = rightFrame < 0
        ? 0
        : (rightFrame >= frameCount - 1 ? frameCount : rightFrame + 1);
    if (endFrameExclusive <= startFrame)
        return plan;

    plan.ops = planManualCorrectionOps(
        scheme,
        originalF0,
        { startFrame, endFrameExclusive },
        [leftFrame, rightFrame, leftF0, rightF0](int frame) {
            if (leftFrame == rightFrame)
                return rightF0;
            const double frameOffset = static_cast<double>(frame) - static_cast<double>(leftFrame);
            const double frameSpan = static_cast<double>(rightFrame) - static_cast<double>(leftFrame);
            const float t = static_cast<float>(frameOffset / frameSpan);
            return interpolateLogF0(leftF0, rightF0, t);
        },
        source,
        parameterSnapshot);

    if (!plan.ops.empty()) {
        plan.actualRange = { plan.ops.front().startFrame, plan.ops.back().endFrameExclusive };
    }
    return plan;
}

float interpolateLogF0(float leftF0, float rightF0, float t)
{
    const float logLeft = std::log2(std::max(leftF0, 1.0f));
    const float logRight = std::log2(std::max(rightF0, 1.0f));
    return std::pow(2.0f, logLeft + (logRight - logLeft) * t);
}

ManualCorrectionLogLineFit fitOriginalF0LogTrend(const std::vector<float>& originalF0,
                                                int startFrame,
                                                int endFrameExclusive)
{
    double sumX = 0.0;
    double sumY = 0.0;
    double sumXX = 0.0;
    double sumXY = 0.0;
    int count = 0;

    for (int frame = startFrame; frame < endFrameExclusive; ++frame) {
        const float sourceF0 = originalF0[static_cast<std::size_t>(frame)];
        if (sourceF0 <= 0.0f) continue;

        const double x = static_cast<double>(frame - startFrame);
        const double y = std::log2(sourceF0);
        sumX += x;
        sumY += y;
        sumXX += x * x;
        sumXY += x * y;
        ++count;
    }

    if (count == 0)
        return {};

    const double n = static_cast<double>(count);
    const double denom = n * sumXX - sumX * sumX;
    ManualCorrectionLogLineFit fit;
    fit.slope = denom != 0.0
        ? static_cast<float>((n * sumXY - sumX * sumY) / denom)
        : 0.0f;
    fit.intercept = static_cast<float>((sumY - static_cast<double>(fit.slope) * sumX) / n);
    return fit;
}

float lineAnchorF0WithSourceShape(const std::vector<float>& originalF0,
                                  int frame,
                                  int startFrame,
                                  int endFrameExclusive,
                                  float leftTargetF0,
                                  float rightTargetF0,
                                  float retuneSpeed,
                                  ManualCorrectionLogLineFit sourceTrend)
{
    const int spanFrames = std::max(1, endFrameExclusive - startFrame);
    const float t = std::clamp(static_cast<float>(frame - startFrame) / static_cast<float>(spanFrames),
                               0.0f,
                               1.0f);
    const float targetLineF0 = interpolateLogF0(leftTargetF0, rightTargetF0, t);
    const float shapeAmount = 1.0f - std::clamp(retuneSpeed, 0.0f, 1.0f);
    if (shapeAmount <= 0.0f)
        return targetLineF0;

    const float sourceF0 = originalF0[static_cast<std::size_t>(frame)];
    if (sourceF0 <= 0.0f)
        return targetLineF0;

    const float logShape = std::log2(sourceF0)
        - (sourceTrend.intercept + sourceTrend.slope * static_cast<float>(frame - startFrame));
    return targetLineF0 * std::pow(2.0f, logShape * shapeAmount);
}

} // namespace OpenTune
