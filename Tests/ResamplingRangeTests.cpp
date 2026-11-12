#include "../Source/DSP/ResamplingManager.h"
#include "CDSPResampler.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
using OpenTune::PlaybackSampleGeometry::projectToSample;

std::vector<float> signal(int size)
{
    std::vector<float> pcm(static_cast<size_t>(size));
    uint32_t state = 0x29451u;
    for (int i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        pcm[static_cast<size_t>(i)] = static_cast<float>(
            0.28 * (static_cast<double>(state) / 4294967296.0 - 0.5)
            + 0.24 * std::sin(0.421 * 6.283185307179586 * i)
            + 0.18 * std::sin(0.137 * 6.283185307179586 * i));
    }
    for (int i : {0, 1, 511, 512, 513, 10000, 10001, size - 1})
        pcm[static_cast<size_t>(i)] = 0.8f;
    return pcm;
}

std::vector<float> originalReference(const std::vector<float>& input, double src, double dst)
{
    const int length = static_cast<int>(projectToSample(input.size() * dst / src));
    std::vector<float> output(static_cast<size_t>(length));
    // This is the original zero-origin r8brain path: no range initialization.
    r8b::CDSPResampler24 original(src, dst, 4096);
    original.oneshot(input.data(), static_cast<int>(input.size()), output.data(), length);
    return output;
}

double error(const std::vector<float>& actual, const std::vector<float>& reference, int64_t first)
{
    double largest = 0.0;
    for (size_t i = 0; i < actual.size(); ++i) {
        const int64_t index = first + static_cast<int64_t>(i);
        const float expected = index >= 0 && index < static_cast<int64_t>(reference.size())
            ? reference[static_cast<size_t>(index)] : 0.0f;
        largest = std::max(largest, std::abs(static_cast<double>(actual[i]) - expected));
    }
    return largest;
}

bool rangesMatchOriginal(double src, double dst)
{
    auto input = signal(100000);
    const auto reference = originalReference(input, src, dst);
    OpenTune::ResamplingManager converter;
    double largest = 0.0;
    const int64_t starts[] = {-37, 0, 1, 512,
        static_cast<int64_t>(reference.size() / 2),
        static_cast<int64_t>(reference.size() - std::min<size_t>(2000, reference.size()))};
    for (const int64_t first : starts) {
        const int count = static_cast<int>(std::min<int64_t>(4096, reference.size() - first));
        const auto actual = converter.resampleRange(input.data(), input.size(), src, dst, 0, first, count);
        if (actual.size() != static_cast<size_t>(count))
            return false;
        largest = std::max(largest, error(actual, reference, first));

        // Give the converter exactly the queried input support. This catches
        // artificial crop boundaries and incomplete filter-history queries.
        const auto support = converter.requiredInputRange(src, dst, first, first + count);
        const int64_t a = std::max<int64_t>(0, support.begin);
        const int64_t b = std::min<int64_t>(input.size(), support.end);
        const auto cropped = converter.resampleRange(input.data() + a,
            static_cast<size_t>(b - a), src, dst, a, first, count);
        if (cropped.size() != static_cast<size_t>(count))
            return false;
        largest = std::max(largest, error(cropped, reference, first));
    }
    std::printf("range %.17g -> %.17g max_error=%.12g\n", src, dst, largest);
    return largest <= 1e-6;
}

bool partitionIndependent(double src, double dst)
{
    const auto input = signal(65536);
    const auto reference = originalReference(input, src, dst);
    constexpr int64_t start = 15001;
    constexpr int count = 4096;
    const int blocks[] = {1, 64, 127, 128, 256, 512, 1024};
    OpenTune::ResamplingManager converter;
    const auto whole = converter.resampleRange(input.data(), input.size(), src, dst, 0, start, count);
    std::vector<float> joined;
    int part = 0;
    while (joined.size() < count) {
        const int n = std::min(blocks[part++ % 7], count - static_cast<int>(joined.size()));
        const auto output = converter.resampleRange(input.data(), input.size(), src, dst, 0,
            start + static_cast<int64_t>(joined.size()), n);
        joined.insert(joined.end(), output.begin(), output.end());
    }
    double largest = 0.0;
    for (int i = 0; i < count; ++i)
        largest = std::max(largest, std::abs(static_cast<double>(joined[i]) - whole[i]));
    return largest <= 1e-6 && error(joined, reference, start) <= 1e-6;
}

bool nonzeroStorageOrigin()
{
    auto input = signal(65536);
    constexpr int64_t firstSource = 512;
    std::fill(input.begin(), input.begin() + firstSource, 0.0f);
    OpenTune::ResamplingManager converter;
    for (const auto rates : {std::pair{44100.0, 48000.0}, std::pair{48000.1, 44100.0}}) {
        const auto reference = originalReference(input, rates.first, rates.second);
        const int64_t first = projectToSample(firstSource * rates.second / rates.first) - 20;
        const auto output = converter.resampleRange(input.data() + firstSource, input.size() - firstSource,
            rates.first, rates.second, firstSource, first, 2048);
        if (error(output, reference, first) > 1e-6)
            return false;
    }
    return true;
}

bool sameRateAndClipping()
{
    const auto input = signal(12000);
    OpenTune::ResamplingManager converter;
    const auto output = converter.resampleRange(input.data(), input.size(), 48000.25, 48000.25,
        512, 509, 20);
    if (output.size() != 20 || output[0] != 0 || output[1] != 0 || output[2] != 0)
        return false;
    return std::equal(output.begin() + 3, output.end(), input.begin());
}

bool costIndependentOfPosition()
{
    OpenTune::ResamplingManager converter;
    for (const auto rates : {std::pair{44100.0, 48000.0}, std::pair{48000.0, 44100.0},
                            std::pair{192000.0, 8000.0}, std::pair{48000.1, 44100.0},
                            std::pair{8000.125, 192000.75}}) {
        int64_t minSize = std::numeric_limits<int64_t>::max();
        int64_t maxSize = 0;
        for (const int64_t first : {1000000LL, 1000000000LL, 1000000000000LL}) {
            const auto range = converter.requiredInputRange(rates.first, rates.second, first, first + 4096);
            minSize = std::min(minSize, range.end - range.begin);
            maxSize = std::max(maxSize, range.end - range.begin);
        }
        std::printf("support %.17g -> %.17g frames=[%lld,%lld]\n", rates.first, rates.second,
            static_cast<long long>(minSize), static_cast<long long>(maxSize));
        if (minSize <= 0 || maxSize - minSize > 1024 || maxSize > 200000)
            return false;
    }
    return true;
}

bool adjacentRateAtLargeIntegerAddress()
{
    const double target = std::nextafter(48000.0, std::numeric_limits<double>::infinity());
    // Exact binary64 ratio is 6597069766656000 / 6597069766656001.
    // The expected quotient/remainders below were calculated with integers;
    // converting the global address to double loses a whole input sample here.
    struct Position { int64_t output; int64_t inputFloor; double fraction; };
    for (const Position p : {
            Position{4503599627370495LL, 4503599627370494LL, 0.3173333333333336},
            Position{9007199254740993LL, 9007199254740991LL, 0.6346666666666667},
            Position{4611686018427387905LL, 4611686018427387205LL, 0.9493333333334392},
            Position{9223372036854743039LL, 9223372036854741640LL, 0.8986666666718458}}) {
        const int64_t firstInput = p.inputFloor - 16384;
        std::vector<float> input(32768);
        for (size_t i = 0; i < input.size(); ++i)
            input[i] = static_cast<float>(0.5 * std::sin(
                6.283185307179586 * ((firstInput + static_cast<int64_t>(i)) % 16) / 16.0));
        OpenTune::ResamplingManager converter;
        const auto output = converter.resampleRange(input.data(), input.size(), 48000.0, target,
            firstInput, p.output, 1024);
        if (output.size() != 1024)
            return false;
        double largest = 0.0;
        for (size_t i = 0; i < output.size(); ++i) {
            const double phase = ((p.inputFloor + static_cast<int64_t>(i)) % 16) + p.fraction;
            const double expected = 0.5 * std::sin(6.283185307179586 * phase / 16.0);
            largest = std::max(largest, std::abs(output[i] - expected));
        }
        std::printf("large address %lld max_error=%.12g\n", static_cast<long long>(p.output), largest);
        if (largest > 1e-6)
            return false;
    }
    return true;
}
} // namespace

int main()
{
    bool ok = true;
    const double next48 = std::nextafter(48000.0, std::numeric_limits<double>::infinity());
    for (const auto rates : {std::pair{44100.0, 48000.0}, std::pair{48000.0, 44100.0},
                            std::pair{96000.0, 48000.0}, std::pair{48000.0, 96000.0},
                            std::pair{192000.0, 8000.0}, std::pair{8000.0, 192000.0},
                            std::pair{48000.5, 96001.0}, std::pair{44100.125, 48000.75},
                            std::pair{48000.0, next48}, std::pair{48000.1, 44100.0},
                            std::pair{192000.0, 44100.125}, std::pair{8000.125, 192000.75}})
        ok = rangesMatchOriginal(rates.first, rates.second) && ok;
    for (const auto rates : {std::pair{44100.0, 48000.0}, std::pair{48000.0, 44100.0},
                            std::pair{48000.1, 44100.0}})
        ok = partitionIndependent(rates.first, rates.second) && ok;
    ok = nonzeroStorageOrigin() && ok;
    ok = sameRateAndClipping() && ok;
    ok = costIndependentOfPosition() && ok;
    ok = adjacentRateAtLargeIntegerAddress() && ok;
    std::printf("ResamplingRangeTests: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
