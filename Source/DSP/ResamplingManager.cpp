#include "ResamplingManager.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#endif

#include "CDSPResampler.h"
#include "SampleGridArithmetic.h"
#include "Utils/TimeCoordinate.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <vector>

namespace OpenTune {

namespace {

using namespace SampleGrid;

constexpr int kProcessBlock = 4096;

struct Ratio
{
    int64_t num{1};
    int64_t den{1};
};

void reduceRatio(Ratio& ratio)
{
    const int64_t divisor = std::gcd(ratio.num, ratio.den);
    ratio.num /= divisor;
    ratio.den /= divisor;
}

void multiplyRatio(Ratio& ratio, int64_t up, int64_t down)
{
    ratio.num *= up;
    ratio.den *= down;
    reduceRatio(ratio);
}

int64_t lcmPositive(int64_t lhs, int64_t rhs)
{
    return (lhs / std::gcd(lhs, rhs)) * rhs;
}

struct ExactRatio
{
    uint64_t num{1};
    uint64_t den{1};
};

struct BinaryValue
{
    uint64_t mantissa{0};
    int exponent{0};
};

BinaryValue decomposeBinary(double value)
{
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    const int exponentBits = static_cast<int>((bits >> 52) & 0x7ffu);
    uint64_t mantissa = bits & ((uint64_t{1} << 52) - 1);

    if (exponentBits == 0)
    {
        mantissa <<= 1;
        return { mantissa, -1075 };
    }

    mantissa |= uint64_t{1} << 52;
    return { mantissa, exponentBits - 1075 };
}

ExactRatio exactRateRatio(double sourceRate, double destRate)
{
    BinaryValue source = decomposeBinary(sourceRate);
    BinaryValue dest = decomposeBinary(destRate);

    const uint64_t common = std::gcd(source.mantissa, dest.mantissa);
    source.mantissa /= common;
    dest.mantissa /= common;

    int shift = source.exponent - dest.exponent;

    if (shift > 0)
    {
        while (shift > 0 && (dest.mantissa & 1u) == 0)
        {
            dest.mantissa >>= 1;
            --shift;
        }

        return { source.mantissa << shift, dest.mantissa };
    }

    if (shift < 0)
    {
        while (shift < 0 && (source.mantissa & 1u) == 0)
        {
            source.mantissa >>= 1;
            ++shift;
        }

        return { source.mantissa, dest.mantissa << (-shift) };
    }

    return { source.mantissa, dest.mantissa };
}

enum class StepKind
{
    Block,
    Fractional,
    HalfBandUp,
    HalfBandDown
};

struct ChainStep
{
    StepKind kind{StepKind::Block};
    int64_t up{1};
    int64_t down{1};
    int64_t left{0};
    int64_t right{0};
    int64_t latency{0};
    r8b::CDSPFracInterpolator* fractional{nullptr};
    bool whole{false};
    ExactRatio ratio;
};

struct RangePlan
{
    int64_t firstInput{0};
    int64_t supportEnd{0};
    int64_t firstOutput{0};
    r8b::CDSPFracInterpolator* fractional{nullptr};
    int fractionalLatency{0};
    int wholePhase{0};
    double fractionalPhase{0.0};
};

void inspectChain(r8b::CDSPResampler24& resampler, std::vector<ChainStep>& steps,
    int& fractionalIndex)
{
    steps.clear();
    fractionalIndex = -1;

    const int count = resampler.getStepCount();

    for (int i = 0; i < count; ++i)
    {
        r8b::CDSPProcessor* const processor = resampler.getProcessingStep(i);
        ChainStep step;

        if (auto* const block = dynamic_cast<r8b::CDSPBlockConvolver*>(processor))
        {
            step.kind = StepKind::Block;
            step.up = block->getUpFactor();
            step.down = block->getDownFactor();
            step.latency = block->getFilterLatency();
        }
        else if (auto* const fractional =
                     dynamic_cast<r8b::CDSPFracInterpolator*>(processor))
        {
            step.kind = StepKind::Fractional;
            step.fractional = fractional;
            step.left = fractional->getFilterLeftLen();
            step.right = fractional->getFilterRightLen();
            step.whole = fractional->isWholeStepping();

            if (step.whole)
            {
                step.ratio = { static_cast<uint64_t>(fractional->getInStep()),
                               static_cast<uint64_t>(fractional->getOutStep()) };
            }
            else
            {
                step.ratio = exactRateRatio(fractional->getSrcSampleRate(),
                    fractional->getDstSampleRate());
            }

            if (fractionalIndex < 0)
                fractionalIndex = i;
        }
        else if (auto* const halfBandUp =
                     dynamic_cast<r8b::CDSPHBUpsampler*>(processor))
        {
            step.kind = StepKind::HalfBandUp;
            step.up = 2;
            step.down = 1;
            step.left = halfBandUp->getFilterLeftLen();
            step.right = halfBandUp->getFilterRightLen();
        }
        else if (auto* const halfBandDown =
                     dynamic_cast<r8b::CDSPHBDownsampler*>(processor))
        {
            step.kind = StepKind::HalfBandDown;
            step.up = 1;
            step.down = 2;
            step.left = halfBandDown->getFilterLeftLen();
            step.right = halfBandDown->getFilterRightLen();
        }

        steps.push_back(step);
    }
}

// Reverse projections stay signed-wide: the fractional step can multiply a
// full int64 address past 2^63 before the leading integer steps divide back.
void projectStep(const ChainStep& step, Wide& begin, Wide& end)
{
    switch (step.kind)
    {
    case StepKind::Block:
        begin = multiplyDivideAddWide(begin, static_cast<uint64_t>(step.down),
            -step.latency, static_cast<uint64_t>(step.up), true);
        end = addWide(multiplyDivideAddWide(addWide(end, -1),
            static_cast<uint64_t>(step.down), step.latency,
            static_cast<uint64_t>(step.up), false), 1);
        break;

    case StepKind::Fractional:
    {
        const uint64_t sourceStep = static_cast<uint64_t>(step.ratio.num);
        const uint64_t destStep = static_cast<uint64_t>(step.ratio.den);
        begin = addWide(multiplyDivideFloorWide(begin, sourceStep, destStep),
            -step.left);
        end = addWide(addWide(multiplyDivideFloorWide(addWide(end, -1),
            sourceStep, destStep), step.right), 1);
        break;
    }

    case StepKind::HalfBandUp:
    {
        Wide projectedBegin = floorDivideWide(begin, 2);
        Wide projectedEnd = addWide(floorDivideWide(addWide(end, -1), 2), 1);
        const Wide endBefore = addWide(end, -1);
        const Wide firstOdd = (begin.low & 1u) != 0 ? begin : addWide(begin, 1);
        const Wide lastOdd = (endBefore.low & 1u) != 0
            ? endBefore : addWide(endBefore, -1);

        if (compareWide(firstOdd, end) < 0)
        {
            const Wide beginReach = addWide(floorDivideWide(firstOdd, 2),
                1 - step.right);

            if (compareWide(beginReach, projectedBegin) < 0)
                projectedBegin = beginReach;

            const Wide endReach = addWide(floorDivideWide(lastOdd, 2),
                step.right + 1);

            if (compareWide(endReach, projectedEnd) > 0)
                projectedEnd = endReach;
        }

        begin = projectedBegin;
        end = projectedEnd;
        break;
    }

    case StepKind::HalfBandDown:
    {
        const int64_t reach = step.left * 2 - 1;
        begin = addWide(scaleWide(begin, 2), -reach);
        end = addWide(scaleWide(addWide(end, -1), 2), reach + 1);
        break;
    }
    }
}

RangePlan buildRangePlan(r8b::CDSPResampler24& resampler, int64_t outputBegin,
    int64_t outputEnd)
{
    std::vector<ChainStep> steps;
    int fractionalIndex = -1;
    inspectChain(resampler, steps, fractionalIndex);

    Wide supportBegin = wideFromInt64(outputBegin);
    Wide supportEnd = wideFromInt64(outputEnd);

    for (size_t i = steps.size(); i > 0; --i)
        projectStep(steps[i - 1], supportBegin, supportEnd);

    Ratio inputToFraction;
    int64_t period = 1;

    const size_t preFractionEnd = fractionalIndex >= 0
        ? static_cast<size_t>(fractionalIndex)
        : steps.size();

    for (size_t i = 0; i < preFractionEnd; ++i)
    {
        const ChainStep& step = steps[i];
        multiplyRatio(inputToFraction, step.up, step.down);
        period = lcmPositive(period, inputToFraction.den);
    }

    Ratio outputAfterFraction;

    if (fractionalIndex >= 0)
    {
        for (size_t i = static_cast<size_t>(fractionalIndex) + 1;
             i < steps.size(); ++i)
        {
            const ChainStep& step = steps[i];
            multiplyRatio(outputAfterFraction, step.up, step.down);
        }
    }

    const Wide alignedBegin = scaleWide(
        floorDivideWide(supportBegin, static_cast<uint64_t>(period)),
        static_cast<uint64_t>(period));

    // Only the public source and output endpoints narrow to int64.
    RangePlan plan;
    plan.supportEnd = wideToInt64(supportEnd);
    plan.firstInput = wideToInt64(alignedBegin.negative ? Wide{} : alignedBegin);

    if (fractionalIndex >= 0)
    {
        const ChainStep& step = steps[static_cast<size_t>(fractionalIndex)];
        const Wide fractionInputSamples = multiplyDivideFloorWide(
            wideFromInt64(plan.firstInput),
            static_cast<uint64_t>(inputToFraction.num),
            static_cast<uint64_t>(inputToFraction.den));

        const Wide scaledInput = scaleWide(fractionInputSamples, step.ratio.den);
        uint64_t remainder = 0;
        const U128 quotient = divideU128Full(
            { scaledInput.high, scaledInput.low }, step.ratio.num, remainder);

        Wide outputIndex{ scaledInput.negative, quotient.high, quotient.low };

        if (remainder != 0)
            outputIndex = addWide(outputIndex, 1);

        const uint64_t endpoint = remainder == 0 ? 0 : step.ratio.num - remainder;
        const uint64_t latency = endpoint / step.ratio.den;
        const uint64_t phase = endpoint % step.ratio.den;

        plan.fractional = step.fractional;
        plan.fractionalLatency = static_cast<int>(latency);

        if (step.whole)
        {
            plan.wholePhase = static_cast<int>(phase);
        }
        else
        {
            plan.fractionalPhase = static_cast<double>(phase)
                / static_cast<double>(step.ratio.den);

            if (plan.fractionalPhase >= 1.0)
            {
                plan.fractionalPhase = 0.0;
                ++plan.fractionalLatency;
            }
        }

        plan.firstOutput = wideToInt64(multiplyDivideFloorWide(outputIndex,
            static_cast<uint64_t>(outputAfterFraction.num),
            static_cast<uint64_t>(outputAfterFraction.den)));
    }
    else
    {
        plan.firstOutput = wideToInt64(multiplyDivideFloorWide(
            wideFromInt64(plan.firstInput),
            static_cast<uint64_t>(inputToFraction.num),
            static_cast<uint64_t>(inputToFraction.den)));
    }

    return plan;
}

} // namespace

ResamplingManager::ResamplingManager() {
}

ResamplingManager::~ResamplingManager() {
}

std::vector<float> ResamplingManager::downsampleForInference(
    const float* input, size_t inputLength,
    double inputSR, double targetSR
) {
    return resample(input, inputLength, inputSR, targetSR);
}

std::vector<float> ResamplingManager::upsampleForHost(
    const float* input, size_t inputLength,
    double inputSR, double targetSR
) {
    return resample(input, inputLength, inputSR, targetSR);
}

std::vector<float> ResamplingManager::resampleRange(
    const float* input, size_t inputLength, double inputSR, double targetSR,
    int64_t firstInputSample, int64_t firstOutputSample, int outputLength)
{
    if (outputLength <= 0)
        return {};

    std::vector<float> output(static_cast<size_t>(outputLength), 0.0f);

    const int64_t outputEnd = firstOutputSample + static_cast<int64_t>(outputLength);

    if (inputSR == targetSR)
    {
        const int64_t sourceBegin = firstInputSample;
        const int64_t sourceEnd = sourceBegin + static_cast<int64_t>(inputLength);

        for (int i = 0; i < outputLength; ++i)
        {
            const int64_t globalOutput = firstOutputSample + i;

            if (globalOutput >= 0 && globalOutput >= sourceBegin &&
                globalOutput < sourceEnd)
            {
                output[static_cast<size_t>(i)] =
                    input[globalOutput - sourceBegin];
            }
        }

        return output;
    }

    const int64_t outputBegin = std::max<int64_t>(0, firstOutputSample);

    if (outputEnd <= outputBegin)
        return output;

    r8b::CDSPResampler24 resampler(inputSR, targetSR, kProcessBlock);
    const RangePlan plan = buildRangePlan(resampler, outputBegin, outputEnd);

    if (plan.fractional != nullptr)
    {
        plan.fractional->setInitialPhase(plan.fractionalLatency, plan.wholePhase,
            plan.fractionalPhase);
    }

    resampler.clear();

    const size_t producedCount =
        static_cast<size_t>(outputEnd - plan.firstOutput);
    std::vector<double> produced(producedCount);

    const int64_t sourceBegin = firstInputSample;
    const int64_t sourceEnd = sourceBegin + static_cast<int64_t>(inputLength);
    int64_t inputPosition = plan.firstInput;
    size_t producedWritten = 0;
    std::vector<double> chunk(static_cast<size_t>(kProcessBlock));

    while (producedWritten < producedCount)
    {
        int chunkLength = kProcessBlock;
        std::fill(chunk.begin(), chunk.end(), 0.0);

        if (inputPosition < plan.supportEnd)
        {
            chunkLength = static_cast<int>(std::min<int64_t>(
                kProcessBlock, plan.supportEnd - inputPosition));

            const int64_t realBegin = std::max(inputPosition, sourceBegin);
            const int64_t realEnd = std::min(inputPosition + chunkLength, sourceEnd);

            if (realEnd > realBegin)
            {
                const int64_t destination = realBegin - inputPosition;
                const int64_t source = realBegin - sourceBegin;
                const size_t count = static_cast<size_t>(realEnd - realBegin);
                std::copy_n(input + source, count, chunk.begin() + destination);
            }

            inputPosition += chunkLength;
        }

        double* processed = nullptr;
        const int available = resampler.process(chunk.data(), chunkLength, processed);

        if (available > 0)
        {
            const size_t count = std::min(producedCount - producedWritten,
                static_cast<size_t>(available));
            std::copy_n(processed, count, produced.begin() + producedWritten);
            producedWritten += count;
        }
    }

    const size_t prefix = static_cast<size_t>(outputBegin - plan.firstOutput);
    const size_t count = static_cast<size_t>(outputEnd - outputBegin);
    const size_t destination = static_cast<size_t>(outputBegin - firstOutputSample);

    for (size_t i = 0; i < count; ++i)
        output[destination + i] = static_cast<float>(produced[prefix + i]);

    return output;
}

PlaybackSampleGeometry::SampleSpan ResamplingManager::requiredInputRange(
    double inputSR, double targetSR, int64_t outputBegin, int64_t outputEnd)
{
    const int64_t requiredBegin = std::max<int64_t>(0, outputBegin);

    if (outputEnd <= requiredBegin)
        return { 0, 0 };

    if (inputSR == targetSR)
        return { requiredBegin, outputEnd };

    r8b::CDSPResampler24 resampler(inputSR, targetSR, kProcessBlock);
    const RangePlan plan = buildRangePlan(resampler, requiredBegin, outputEnd);

    return { plan.firstInput, plan.supportEnd };
}

std::vector<float> ResamplingManager::resampleExactLength(
    const float* input, size_t inputLength,
    double inputSR, double targetSR, int outputLength)
{
    if (outputLength <= 0)
        return {};

    return resampleRange(input, inputLength, inputSR, targetSR, 0, 0, outputLength);
}

std::vector<float> ResamplingManager::resample(
    const float* input, size_t inputLength,
    double inputSR, double targetSR
) {
    if (inputLength == 0)
        return {};

    int outputLength = 0;

    if (inputSR == targetSR)
    {
        outputLength = static_cast<int>(std::min<size_t>(inputLength,
            static_cast<size_t>(std::numeric_limits<int>::max())));
    }
    else
    {
        const int64_t projected = TimeCoordinate::sampleRateProject(
            static_cast<int64_t>(inputLength), inputSR, targetSR);
        outputLength = static_cast<int>(std::min<int64_t>(
            projected, static_cast<int64_t>(std::numeric_limits<int>::max())));

        if (outputLength <= 0)
            outputLength = 1;
    }

    return resampleRange(input, inputLength, inputSR, targetSR, 0, 0, outputLength);
}

} // namespace OpenTune
