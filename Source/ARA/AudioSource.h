#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace OpenTune {

struct AraSourceIdentity
{
    juce::ARAAudioSource* audioSource{nullptr};
    juce::String persistentId;
};

struct AraSourceShape
{
    juce::String sourceName;
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
    void setSampleAccessEnabled(bool enabled) noexcept;
    void clearReaderLease() noexcept;

    bool matches(juce::ARAAudioSource* audioSource) const noexcept;
    bool hasSampleAccess() const noexcept;
    bool hasReaderLease() const noexcept;
    bool canReadSamples() const noexcept;
    bool createReaderLease();
    bool readAudioSamples(int64_t sourceSamplePosition,
                          int samplesPerChannel,
                          void* const buffers[]) const noexcept;

    const AraSourceIdentity& getIdentity() const noexcept { return identity_; }
    const AraSourceShape& getShape() const noexcept { return shape_; }
    juce::ARAAudioSource* getAraAudioSource() const noexcept { return identity_.audioSource; }
    std::shared_ptr<ARA::PlugIn::HostAudioReader> shareReaderLease() const noexcept { return readerLease_; }

private:
    AraSourceIdentity identity_;
    AraSourceShape shape_;
    bool sampleAccessEnabled_{false};
    std::shared_ptr<ARA::PlugIn::HostAudioReader> readerLease_;
};

} // namespace OpenTune
