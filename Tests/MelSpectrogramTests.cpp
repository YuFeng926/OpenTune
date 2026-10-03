#include "DSP/MelSpectrogram.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

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

OpenTune::MelSpectrogramConfig makeConfig()
{
    OpenTune::MelSpectrogramConfig config;
    config.sampleRate = 44100;
    config.nFft = 2048;
    config.winLength = 2048;
    config.hopLength = 512;
    config.nMels = 140;
    config.fMin = 40.0f;
    config.fMax = 16000.0f;
    config.logEps = 1.0e-9f;
    config.melFilterbank.type = OpenTune::MelFilterbankSpec::Type::CustomHighFrequency;
    config.melFilterbank.baseNumBins = 128;
    config.melFilterbank.regions = {
        {8000.0f, 12000.0f, 21},
        {12000.0f, 16000.0f, 14},
    };
    return config;
}

std::vector<float> makeAudio()
{
    constexpr int sampleRate = 44100;
    constexpr int sampleCount = 4096;
    std::vector<float> audio(sampleCount);
    for (int i = 0; i < sampleCount; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(sampleRate);
        audio[static_cast<size_t>(i)] =
            0.4f * std::sin(2.0f * 3.14159265358979323846f * 350.0f * t)
            + 0.2f * std::sin(2.0f * 3.14159265358979323846f * 10000.0f * t);
    }
    return audio;
}

void testCustomFilterbank()
{
    auto config = makeConfig();
    OpenTune::MelSpectrogramProcessor processor;
    const auto configured = processor.configure(config);
    expect(configured.ok(), "custom high-frequency mel filterbank should configure");

    const auto audio = makeAudio();
    const auto result = processor.compute(audio.data(), static_cast<int>(audio.size()), 8);
    expect(result.ok(), "custom high-frequency mel filterbank should compute");
    if (!result.ok())
        return;

    const auto& values = result.value();
    expect(values.size() == 140u * 8u, "custom filterbank output shape should be 140 x frames");
    for (const auto value : values)
        expect(std::isfinite(value), "custom filterbank output should be finite");
}

void testCustomAndLegacyDiffer()
{
    const auto audio = makeAudio();
    auto customConfig = makeConfig();
    auto legacyConfig = customConfig;
    legacyConfig.melFilterbank = {};

    OpenTune::MelSpectrogramProcessor custom;
    OpenTune::MelSpectrogramProcessor legacy;
    expect(custom.configure(customConfig).ok(), "custom config should configure");
    expect(legacy.configure(legacyConfig).ok(), "legacy config should configure");

    const auto customResult = custom.compute(audio.data(), static_cast<int>(audio.size()), 8);
    const auto legacyResult = legacy.compute(audio.data(), static_cast<int>(audio.size()), 8);
    expect(customResult.ok() && legacyResult.ok(), "both filterbanks should compute");
    if (!customResult.ok() || !legacyResult.ok())
        return;

    float difference = 0.0f;
    for (size_t i = 0; i < customResult.value().size(); ++i)
        difference = std::max(difference,
                              std::abs(customResult.value()[i] - legacyResult.value()[i]));
    expect(difference > 1.0e-4f, "custom and legacy filterbanks should not be identical");
}

void testInvalidBinCountRejected()
{
    auto config = makeConfig();
    config.nMels = 139;
    OpenTune::MelSpectrogramProcessor processor;
    expect(!processor.configure(config).ok(), "custom filterbank bin mismatch should be rejected");
}

} // namespace

int main()
{
    testCustomFilterbank();
    testCustomAndLegacyDiffer();
    testInvalidBinCountRejected();
    return failures == 0 ? 0 : 1;
}
