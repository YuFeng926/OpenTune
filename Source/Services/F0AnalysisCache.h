#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

namespace OpenTune {

class F0AnalysisCache
{
public:
    using Digest = std::array<uint8_t, 32>;

    static constexpr int kSchemaVersion = 1;
    static constexpr uint64_t kDefaultCapacityBytes = 256ULL * 1024ULL * 1024ULL;
    static constexpr int kChannelZeroPolicyVersion = 1;

    struct KeyConfig
    {
        int schemaVersion{kSchemaVersion};
        int algorithmVersion{1};
        int resamplingAlgorithmVersion{1};
        int f0AlgorithmVersion{1};
        int energyAlgorithmVersion{1};
        int modelType{0};
        Digest modelWeightDigest{};
        float confidence{0.0f};
        float f0Min{0.0f};
        float f0Max{0.0f};
        std::string backend;
        double sourceSampleRate{0.0};
        int sourceChannelCount{0};
        int channelPolicyVersion{kChannelZeroPolicyVersion};
        uint64_t windowSampleCount{0};
    };

    struct Record
    {
        int hopSize{0};
        double f0SampleRate{0.0};
        std::vector<float> originalF0;
        std::vector<float> energy;
    };

    static Digest digestPcm(const float* pcm, size_t sampleCount);
    static Digest buildKey(const KeyConfig& config, const float* pcm, size_t sampleCount);
    static Digest buildKey(const KeyConfig& config, const Digest& pcmDigest);
    static std::string keyToHex(const Digest& key);
    static juce::File cacheDirectory();
    static juce::File fileForKey(const juce::File& directory, const Digest& key);

    static std::optional<Record> tryLoad(const juce::File& directory, const Digest& key);
    static bool publish(const juce::File& directory, const Digest& key, const Record& record);
    static void touch(const juce::File& directory, const Digest& key);
    static void enforceCapacity(const juce::File& directory,
                               uint64_t maxBytes = kDefaultCapacityBytes);
};

} // namespace OpenTune
