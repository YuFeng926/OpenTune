#include "PhaseVocoderPitchShifter.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace OpenTune {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

double wrapPhase(double phase)
{
    return std::remainder(phase, kTwoPi);
}

int64_t floorDiv(int64_t value, int64_t divisor)
{
    int64_t quotient = value / divisor;
    if (value % divisor < 0)
        --quotient;
    return quotient;
}

int64_t ceilDiv(int64_t value, int64_t divisor)
{
    return -floorDiv(-value, divisor);
}

struct PeakState
{
    int bin = 0;
    double synthesisPhase = 0.0;
};

} // namespace

PhaseVocoderPitchShifter::PhaseVocoderPitchShifter(double sampleRate)
    : sampleRate_(sampleRate)
{
    assert(sampleRate_ > 0.0);
}

std::vector<float> PhaseVocoderPitchShifter::render(
    const float* source, int sourceSampleCount,
    int outputStartSample, int outputSampleCount,
    const float* originalF0, const float* correctedF0,
    int numF0Frames, double f0FrameRate) const
{
    assert(sampleRate_ > 0.0);
    assert(sourceSampleCount >= 0);
    assert(source != nullptr || sourceSampleCount == 0);
    assert(outputStartSample >= 0);
    assert(outputSampleCount >= 0);
    assert(static_cast<int64_t>(outputStartSample) + outputSampleCount
           <= static_cast<int64_t>(sourceSampleCount));
    assert(numF0Frames >= 0);
    assert(numF0Frames == 0 || (originalF0 != nullptr && correctedF0 != nullptr));
    assert(f0FrameRate > 0.0);

    if (outputSampleCount == 0)
        return {};

    const int64_t outputStart = outputStartSample;
    const int64_t outputEnd = outputStart + outputSampleCount;

    std::vector<double> logRatios(static_cast<size_t>(numF0Frames), 0.0);
    bool allNeutral = true;
    for (int i = 0; i < numF0Frames; ++i)
    {
        if (originalF0[i] > 0.0f && correctedF0[i] > 0.0f)
        {
            const double logRatio = std::log(
                static_cast<double>(correctedF0[i])
                / static_cast<double>(originalF0[i]));
            logRatios[(size_t) i] = logRatio;
            if (logRatio != 0.0)
                allNeutral = false;
        }
    }

    // Whole-control identity bypasses the STFT and is sample-exact.
    if (allNeutral)
        return std::vector<float>(source + outputStart, source + outputEnd);

    assert(numF0Frames > 0);

    const auto logRatioAtSample = [&](double samplePosition)
    {
        const double f0Position = samplePosition * f0FrameRate / sampleRate_;
        if (f0Position <= 0.0)
            return logRatios.front();
        if (f0Position >= static_cast<double>(numF0Frames - 1))
            return logRatios.back();
        const int index = static_cast<int>(std::floor(f0Position));
        const double fraction = f0Position - static_cast<double>(index);
        return logRatios[(size_t) index]
             + (logRatios[(size_t) index + 1] - logRatios[(size_t) index]) * fraction;
    };

    constexpr int fftSize = kFftSize;
    constexpr int hopSize = kHopSize;
    constexpr int halfFft = kFftSize / 2;
    constexpr int numBins = kFftSize / 2 + 1;

    thread_local const juce::dsp::FFT fft(kFftOrder);

    std::vector<float> window((size_t) fftSize);
    constexpr float twoPi = 6.2831853071795864769f;
    for (int n = 0; n < fftSize; ++n)
        window[(size_t) n] = 0.5f * (1.0f - std::cos(
            twoPi * (float) n / (float) fftSize));

    std::vector<std::complex<float>> fftInput((size_t) fftSize);
    std::vector<std::complex<float>> fftOutput((size_t) fftSize);
    std::vector<std::complex<float>> synthesisSpectrum((size_t) fftSize);
    std::vector<std::complex<float>> synthesisTime((size_t) fftSize);

    std::vector<float> magnitude((size_t) numBins);
    std::vector<float> analysisPhase((size_t) numBins);
    std::vector<float> previousAnalysisPhase((size_t) numBins);
    std::vector<double> deviation((size_t) numBins);

    std::vector<PeakState> peaks;
    std::vector<PeakState> previousPeaks;
    peaks.reserve((size_t) numBins);
    previousPeaks.reserve((size_t) numBins);

    std::vector<float> accumulated((size_t) outputSampleCount, 0.0f);
    std::vector<float> normalization((size_t) outputSampleCount, 0.0f);

    // A frame with centre k * hop covers samples
    // [k * hop - N / 2, k * hop + N / 2). Negative centres read zero padding.
    const int64_t firstOutputFrame = ceilDiv(outputStart - halfFft + 1, hopSize);
    const int64_t lastOutputFrame = floorDiv(outputEnd - 1 + halfFft, hopSize);
    const int64_t firstContentFrame = -static_cast<int64_t>(halfFft / hopSize) + 1;
    assert(firstOutputFrame >= firstContentFrame);
    assert(firstOutputFrame <= lastOutputFrame);

    // The nearest neutral analysis frame at or before the range supplies the
    // explicit phase reset point. Without one, replay starts at the fixed first
    // frame whose window overlaps the content origin.
    int64_t anchorFrame = firstContentFrame;
    for (int64_t frame = firstOutputFrame; frame >= firstContentFrame; --frame)
    {
        if (std::exp(logRatioAtSample(static_cast<double>(frame) * hopSize)) == 1.0)
        {
            anchorFrame = frame;
            break;
        }
    }

    const auto analyzeFrame = [&](int64_t frame)
    {
        const int64_t firstSample = frame * hopSize - halfFft;
        for (int n = 0; n < fftSize; ++n)
        {
            const int64_t sampleIndex = firstSample + n;
            const float sample =
                (sampleIndex >= 0 && sampleIndex < (int64_t) sourceSampleCount)
                    ? source[(size_t) sampleIndex]
                    : 0.0f;
            fftInput[(size_t) n] = std::complex<float>(
                sample * window[(size_t) n], 0.0f);
        }
        fft.perform(fftInput.data(), fftOutput.data(), false);
        for (int b = 0; b < numBins; ++b)
        {
            magnitude[(size_t) b] = std::abs(fftOutput[(size_t) b]);
            analysisPhase[(size_t) b] = std::arg(fftOutput[(size_t) b]);
        }
    };

    const auto computeDeviations = [&]()
    {
        for (int b = 0; b < numBins; ++b)
        {
            if (b == 0 || b == numBins - 1)
            {
                deviation[(size_t) b] = 0.0;
                continue;
            }
            const double expected = kTwoPi * (double) b * hopSize / fftSize;
            const double delta = wrapPhase(
                (double) analysisPhase[(size_t) b]
                - (double) previousAnalysisPhase[(size_t) b]
                - expected);
            deviation[(size_t) b] =
                delta * (double) fftSize / (kTwoPi * hopSize);
        }
    };

    const auto detectPeaks = [&]()
    {
        peaks.clear();
        for (int b = 1; b + 1 < numBins; ++b)
        {
            if (magnitude[(size_t) b] > magnitude[(size_t) b - 1]
                && magnitude[(size_t) b] >= magnitude[(size_t) b + 1])
            {
                peaks.push_back({b, 0.0});
            }
        }

        // A region partition must cover every bin. Flat spectra have no local
        // maximum, so the largest interior bin becomes the single region centre.
        if (peaks.empty())
        {
            int centre = 1;
            for (int b = 2; b + 1 < numBins; ++b)
                if (magnitude[(size_t) b] > magnitude[(size_t) centre])
                    centre = b;
            peaks.push_back({centre, 0.0});
        }
    };

    const auto advancePeakPhases = [&](double ratio, bool reset)
    {
        if (reset)
        {
            for (auto& peak : peaks)
                peak.synthesisPhase = analysisPhase[(size_t) peak.bin];
            return;
        }

        assert(!previousPeaks.empty());
        for (auto& peak : peaks)
        {
            const auto found = std::lower_bound(
                previousPeaks.begin(), previousPeaks.end(), peak.bin,
                [](const PeakState& candidate, int bin)
                {
                    return candidate.bin < bin;
                });

            auto inherited = found;
            if (found == previousPeaks.end())
            {
                inherited = found - 1;
            }
            else if (found != previousPeaks.begin())
            {
                const auto lower = found - 1;
                if (peak.bin - lower->bin <= found->bin - peak.bin)
                    inherited = lower;
            }

            const double peakFrequency =
                (double) peak.bin + deviation[(size_t) peak.bin];
            const double advance =
                kTwoPi * peakFrequency * ratio * hopSize / fftSize;
            peak.synthesisPhase = wrapPhase(
                inherited->synthesisPhase + advance);
        }
    };

    const auto synthesizeFrame = [&](int64_t frame, double ratio)
    {
        std::fill(synthesisSpectrum.begin(), synthesisSpectrum.end(),
                  std::complex<float>(0.0f, 0.0f));

        int regionStart = 0;
        for (size_t peakIndex = 0; peakIndex < peaks.size(); ++peakIndex)
        {
            const PeakState& peak = peaks[peakIndex];
            const int regionEnd =
                peakIndex + 1 < peaks.size()
                    ? (peak.bin + peaks[peakIndex + 1].bin) / 2
                    : numBins - 1;

            // The block around the peak is translated by the peak's fractional
            // pitch shift. Its relative analysis phase pattern is retained.
            const double peakFrequency =
                (double) peak.bin + deviation[(size_t) peak.bin];
            const double shift = (ratio - 1.0) * peakFrequency;

            for (int b = regionStart; b <= regionEnd; ++b)
            {
                const double target = (double) b + shift;
                if (target < 0.0 || target > (double) (numBins - 1))
                    continue;

                const double synthesisPhase = wrapPhase(
                    peak.synthesisPhase
                    + ((double) analysisPhase[(size_t) b]
                       - (double) analysisPhase[(size_t) peak.bin]));
                const float binMagnitude = magnitude[(size_t) b];

                const int targetBin = (int) std::floor(target);
                const double fraction = target - (double) targetBin;

                synthesisSpectrum[(size_t) targetBin] += std::polar(
                    binMagnitude * (float) (1.0 - fraction),
                    (float) synthesisPhase);

                if (fraction > 0.0)
                {
                    synthesisSpectrum[(size_t) targetBin + 1] += std::polar(
                        binMagnitude * (float) fraction,
                        (float) synthesisPhase);
                }
            }

            regionStart = regionEnd + 1;
        }

        // The real-signal spectrum requires real DC and Nyquist bins and a
        // complete conjugate mirror.
        synthesisSpectrum[0] =
            std::complex<float>(synthesisSpectrum[0].real(), 0.0f);
        synthesisSpectrum[(size_t) numBins - 1] = std::complex<float>(
            synthesisSpectrum[(size_t) numBins - 1].real(), 0.0f);
        for (int b = 1; b < numBins - 1; ++b)
        {
            synthesisSpectrum[(size_t) (fftSize - b)] =
                std::conj(synthesisSpectrum[(size_t) b]);
        }

        fft.perform(synthesisSpectrum.data(), synthesisTime.data(), true);

        const int64_t firstSample = frame * hopSize - halfFft;
        for (int n = 0; n < fftSize; ++n)
        {
            const int64_t sampleIndex = firstSample + n;
            if (sampleIndex < outputStart || sampleIndex >= outputEnd)
                continue;

            const size_t outputIndex = (size_t) (sampleIndex - outputStart);
            const float synthesisWindow = window[(size_t) n];
            accumulated[outputIndex] +=
                synthesisTime[(size_t) n].real() * synthesisWindow;
            normalization[outputIndex] += synthesisWindow * synthesisWindow;
        }
    };

    analyzeFrame(anchorFrame - 1);
    previousAnalysisPhase = analysisPhase;

    const bool initializeFromContentStart = (anchorFrame == firstContentFrame);

    for (int64_t frame = anchorFrame; frame <= lastOutputFrame; ++frame)
    {
        analyzeFrame(frame);
        computeDeviations();

        // The frame before the content start is zero padding. The first frame
        // of the replay therefore starts from bin-centre frequency estimates.
        if (initializeFromContentStart && frame == anchorFrame)
            std::fill(deviation.begin(), deviation.end(), 0.0);

        const double ratio = std::exp(
            logRatioAtSample(static_cast<double>(frame) * hopSize));
        const bool neutral = (ratio == 1.0);
        const bool reset = neutral || frame == anchorFrame;

        detectPeaks();
        advancePeakPhases(ratio, reset);

        if (frame >= firstOutputFrame)
            synthesizeFrame(frame, ratio);

        previousAnalysisPhase = analysisPhase;
        previousPeaks = peaks;
    }

    for (size_t i = 0; i < accumulated.size(); ++i)
    {
        assert(normalization[i] > 0.0f);
        accumulated[i] /= normalization[i];
    }

    return accumulated;
}

} // namespace OpenTune
