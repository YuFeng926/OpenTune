#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <memory>

namespace OpenTune {

struct AraSourceIdentity
{
    juce::ARAAudioSource* audioSource{nullptr};
    juce::String persistentId;
};

struct AraSourceShape
{
    double sourceSampleRate{0.0};
    int numChannels{0};
    int64_t numSamples{0};

    double durationSeconds() const noexcept;
    bool isValid() const noexcept;
};

class AudioSource
{
public:
    void updateFrom(juce::ARAAudioSource* audioSource);

    bool matches(juce::ARAAudioSource* audioSource) const noexcept;
    bool canReadSamples() const noexcept;

    // Message-thread only. The DC creates and clears the lease in ARA source management callbacks.
    void createReaderLease();
    void clearReaderLease() noexcept;
    bool readAudioSamples(int64_t sourceSamplePosition,
                          int samplesPerChannel,
                          void* const buffers[]) const noexcept;

    const AraSourceIdentity& getIdentity() const noexcept { return identity_; }
    const AraSourceShape& getShape() const noexcept { return shape_; }
    juce::ARAAudioSource* getAraAudioSource() const noexcept { return identity_.audioSource; }
    uint64_t getSampleContentGeneration() const noexcept { return sampleContentGeneration_; }

    void incrementSampleContentGeneration() noexcept { ++sampleContentGeneration_; }

private:
    AraSourceIdentity identity_;
    AraSourceShape shape_;
    std::unique_ptr<ARA::PlugIn::HostAudioReader> readerLease_;
    uint64_t sampleContentGeneration_{0};
};

} // namespace OpenTune
