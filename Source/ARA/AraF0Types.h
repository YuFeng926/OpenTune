#pragma once

#include "../Content/ContentKey.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace OpenTune {

struct F0TaskId
{
    uint64_t value{0};

    bool operator==(const F0TaskId& rhs) const noexcept { return value == rhs.value; }
    bool operator!=(const F0TaskId& rhs) const noexcept { return !(*this == rhs); }
    bool operator<(const F0TaskId& rhs) const noexcept { return value < rhs.value; }
    explicit operator bool() const noexcept { return value != 0; }
};

struct F0TaskIdHash
{
    size_t operator()(F0TaskId id) const noexcept { return static_cast<size_t>(id.value); }
};

struct OriginalF0InputStamp
{
    juce::String sourcePersistentId;
    uint64_t sourceSampleGeneration{0};
    double sourceStartSeconds{0.0};
    double sourceEndSeconds{0.0};
    double sourceSampleRate{0.0};
    int64_t numSamples{0};
    int numChannels{0};

    bool operator==(const OriginalF0InputStamp& rhs) const noexcept
    {
        return sourcePersistentId == rhs.sourcePersistentId
            && sourceSampleGeneration == rhs.sourceSampleGeneration
            && std::abs(sourceStartSeconds - rhs.sourceStartSeconds) < 1e-9
            && std::abs(sourceEndSeconds - rhs.sourceEndSeconds) < 1e-9
            && std::abs(sourceSampleRate - rhs.sourceSampleRate) < 1e-9
            && numSamples == rhs.numSamples
            && numChannels == rhs.numChannels;
    }

    bool operator!=(const OriginalF0InputStamp& rhs) const noexcept { return !(*this == rhs); }
};

struct AraF0ConfigTag
{
    uint64_t selectionRevision{0};
    std::string absoluteModelPath;
    int64_t modelBytes{0};
    juce::Time modelWriteTime;

    bool operator==(const AraF0ConfigTag& rhs) const noexcept
    {
        return selectionRevision == rhs.selectionRevision
            && absoluteModelPath == rhs.absoluteModelPath
            && modelBytes == rhs.modelBytes
            && modelWriteTime == rhs.modelWriteTime;
    }
};

struct AraF0LocalKey
{
    OriginalF0InputStamp input;
    AraF0ConfigTag config;

    bool operator<(const AraF0LocalKey& rhs) const noexcept
    {
        const auto compareString = [](const juce::String& lhs, const juce::String& rhsValue) {
            return lhs.compare(rhsValue);
        };
        if (const int c = compareString(input.sourcePersistentId, rhs.input.sourcePersistentId); c != 0)
            return c < 0;
        if (input.sourceSampleGeneration != rhs.input.sourceSampleGeneration)
            return input.sourceSampleGeneration < rhs.input.sourceSampleGeneration;
        if (input.sourceStartSeconds != rhs.input.sourceStartSeconds)
            return input.sourceStartSeconds < rhs.input.sourceStartSeconds;
        if (input.sourceEndSeconds != rhs.input.sourceEndSeconds)
            return input.sourceEndSeconds < rhs.input.sourceEndSeconds;
        if (input.sourceSampleRate != rhs.input.sourceSampleRate)
            return input.sourceSampleRate < rhs.input.sourceSampleRate;
        if (input.numSamples != rhs.input.numSamples)
            return input.numSamples < rhs.input.numSamples;
        if (input.numChannels != rhs.input.numChannels)
            return input.numChannels < rhs.input.numChannels;
        if (config.selectionRevision != rhs.config.selectionRevision)
            return config.selectionRevision < rhs.config.selectionRevision;
        if (config.absoluteModelPath != rhs.config.absoluteModelPath)
            return config.absoluteModelPath < rhs.config.absoluteModelPath;
        if (config.modelBytes != rhs.config.modelBytes)
            return config.modelBytes < rhs.config.modelBytes;
        return config.modelWriteTime < rhs.config.modelWriteTime;
    }
};

struct AraF0Target
{
    ContentKey key;
    uint64_t wrapperGeneration{0};
    uint64_t birthRevision{0};
    juce::String modificationPersistentId;
    OriginalF0InputStamp stamp;
};

struct AraF0CompletionSink;

struct AraF0Receiver
{
    AraF0Target target;
    F0TaskId task;
    std::weak_ptr<AraF0CompletionSink> sink;
};

struct AraF0Pcm
{
    std::vector<float> channel0;
    double sourceSampleRate{0.0};
    int sourceChannelCount{0};
};

struct AraF0DocumentFlights
{
    std::map<AraF0LocalKey, F0TaskId> localTasks;
    std::map<F0TaskId, std::vector<AraF0Receiver>> receivers;
};

enum class AraF0Admission : uint8_t
{
    Accepted,
    Attached,
    QueueFull,
    InvalidInput,
    Closing
};

struct AraF0Submission
{
    AraF0Admission admission{AraF0Admission::InvalidInput};
    F0TaskId task;
};

enum class AraF0Abort : uint8_t
{
    SamplesDisabled,
    SourceUndo,
    SamplesChanged
};

} // namespace OpenTune
