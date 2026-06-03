#include "AudioSource.h"

namespace OpenTune {

double AraSourceShape::durationSeconds() const noexcept
{
    return sourceSampleRate > 0.0
        ? static_cast<double>(numSamples) / sourceSampleRate
        : 0.0;
}

bool AraSourceShape::isValid() const noexcept
{
    return sourceSampleRate > 0.0 && numChannels > 0 && numSamples > 0;
}

void AudioSource::updateFrom(juce::ARAAudioSource* audioSource)
{
    identity_.audioSource = audioSource;
    identity_.persistentId = audioSource != nullptr
        ? juce::String(audioSource->getPersistentID())
        : juce::String();

    shape_ = {};
    if (audioSource == nullptr)
        return;

    shape_.sourceName = audioSource->getName() != nullptr
        ? juce::String::fromUTF8(audioSource->getName())
        : juce::String("ARA Source");
    shape_.sourceSampleRate = audioSource->getSampleRate();
    shape_.numChannels = static_cast<int>(audioSource->getChannelCount());
    shape_.numSamples = static_cast<int64_t>(audioSource->getSampleCount());
}

void AudioSource::setSampleAccessEnabled(bool enabled) noexcept
{
    sampleAccessEnabled_ = enabled;
    if (!sampleAccessEnabled_)
        clearReaderLease();
}

void AudioSource::clearReaderLease() noexcept
{
    readerLease_.reset();
}

bool AudioSource::matches(juce::ARAAudioSource* audioSource) const noexcept
{
    return identity_.audioSource == audioSource;
}

bool AudioSource::hasSampleAccess() const noexcept
{
    return sampleAccessEnabled_;
}

bool AudioSource::hasReaderLease() const noexcept
{
    return readerLease_ != nullptr;
}

bool AudioSource::canReadSamples() const noexcept
{
    return sampleAccessEnabled_ && readerLease_ != nullptr && shape_.isValid();
}

bool AudioSource::createReaderLease()
{
    if (!sampleAccessEnabled_ || identity_.audioSource == nullptr)
        return false;

    readerLease_ = std::make_shared<ARA::PlugIn::HostAudioReader>(identity_.audioSource);
    return readerLease_ != nullptr;
}

bool AudioSource::readAudioSamples(int64_t sourceSamplePosition,
                                   int samplesPerChannel,
                                   void* const buffers[]) const noexcept
{
    return readerLease_ != nullptr
        && readerLease_->readAudioSamples(sourceSamplePosition, samplesPerChannel, buffers);
}

} // namespace OpenTune
