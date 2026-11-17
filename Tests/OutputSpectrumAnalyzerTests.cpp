#include "../Source/DSP/OutputSpectrumAnalyzer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void fillSine(juce::AudioBuffer<float>& buffer, int startSample,
              float frequency, float sampleRate)
{
    constexpr float twoPi = 6.2831853071795864769f;
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        const float phase = twoPi * frequency * static_cast<float>(startSample + sample)
                          / sampleRate;
        buffer.setSample(0, sample, 0.4f * std::sin(phase));
    }
}

float maxValue(const OpenTune::SpectrumArray& values)
{
    return *std::max_element(values.begin(), values.end());
}

void testWindowIsPublishedAndAnalyzedOnRead()
{
    constexpr float sampleRate = 48000.0f;
    OpenTune::OutputSpectrumAnalyzer analyzer;
    analyzer.prepare(sampleRate);

    OpenTune::SpectrumArray spectrum{};
    OpenTune::SpectrumArray peaks{};
    analyzer.copySnapshot(spectrum, peaks);
    expect(maxValue(spectrum) == 0.0f, "reset output begins empty");

    juce::AudioBuffer<float> firstHalf(1, OpenTune::OutputSpectrumAnalyzer::kFftSize / 2);
    fillSine(firstHalf, 0, 1000.0f, sampleRate);
    analyzer.push(firstHalf);
    analyzer.copySnapshot(spectrum, peaks);
    expect(maxValue(spectrum) == 0.0f, "partial PCM does not publish a window");

    juce::AudioBuffer<float> secondHalf(1, OpenTune::OutputSpectrumAnalyzer::kFftSize / 2);
    fillSine(secondHalf, firstHalf.getNumSamples(), 1000.0f, sampleRate);
    analyzer.push(secondHalf);

    analyzer.copySnapshot(spectrum, peaks);
    expect(maxValue(spectrum) > 0.1f, "complete PCM window produces a visible spectrum");
    expect(maxValue(peaks) > 0.0f, "complete PCM window updates peak values");

    const auto firstRead = spectrum;
    const auto firstPeaks = peaks;
    analyzer.copySnapshot(spectrum, peaks);
    expect(spectrum == firstRead && peaks == firstPeaks,
           "re-reading a window does not advance smoothing");
}

void testLargeBlockPublishesAtMostOneWindow()
{
    constexpr float sampleRate = 48000.0f;
    OpenTune::OutputSpectrumAnalyzer analyzer;
    analyzer.prepare(sampleRate);

    juce::AudioBuffer<float> block(1, OpenTune::OutputSpectrumAnalyzer::kFftSize * 3);
    fillSine(block, 0, 1000.0f, sampleRate);
    analyzer.push(block);

    OpenTune::SpectrumArray spectrum{};
    OpenTune::SpectrumArray peaks{};
    analyzer.copySnapshot(spectrum, peaks);
    const float maximum = maxValue(spectrum);
    expect(maximum > 0.1f && maximum < 0.5f,
            "a block containing several windows causes one smoothing update");
}

void testConcurrentPublicationKeepsCompleteWindows()
{
    constexpr float sampleRate = 48000.0f;
    juce::AudioBuffer<float> block(1, OpenTune::OutputSpectrumAnalyzer::kFftSize);
    fillSine(block, 0, 1000.0f, sampleRate);

    OpenTune::OutputSpectrumAnalyzer reference;
    reference.prepare(sampleRate);
    reference.push(block);
    OpenTune::SpectrumArray expected{}, expectedPeaks{};
    reference.copySnapshot(expected, expectedPeaks);
    const auto expectedPeak = std::max_element(expected.begin(), expected.end()) - expected.begin();

    OpenTune::OutputSpectrumAnalyzer analyzer;
    analyzer.prepare(sampleRate);
    std::atomic<bool> start{false};
    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int window = 0; window < 4096; ++window)
            analyzer.push(block);
    });

    start.store(true, std::memory_order_release);
    OpenTune::SpectrumArray spectrum{}, peaks{};
    bool completeWindows = true;
    for (int read = 0; read < 256; ++read)
    {
        analyzer.copySnapshot(spectrum, peaks);
        for (size_t bin = 0; bin < spectrum.size(); ++bin)
            completeWindows = completeWindows
                && std::isfinite(spectrum[bin]) && spectrum[bin] >= 0.0f
                && spectrum[bin] <= 1.0f && peaks[bin] >= spectrum[bin]
                && peaks[bin] <= 1.0f;
        if (maxValue(spectrum) > 0.0f)
            completeWindows = completeWindows
                && std::max_element(spectrum.begin(), spectrum.end()) - spectrum.begin() == expectedPeak;
    }
    producer.join();
    analyzer.copySnapshot(spectrum, peaks);
    expect(completeWindows, "concurrent reads retain complete spectra and peak values");
    expect(maxValue(spectrum) > 0.1f
               && std::max_element(spectrum.begin(), spectrum.end()) - spectrum.begin() == expectedPeak,
           "the final published PCM window remains readable after the producer exits");
}

} // namespace

int main()
{
    testWindowIsPublishedAndAnalyzedOnRead();
    testLargeBlockPublishesAtMostOneWindow();
    testConcurrentPublicationKeepsCompleteWindows();
    return failures == 0 ? 0 : 1;
}
