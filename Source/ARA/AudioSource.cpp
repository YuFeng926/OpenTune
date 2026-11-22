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

    shape_.sourceSampleRate = audioSource->getSampleRate();
    shape_.numChannels = static_cast<int>(audioSource->getChannelCount());
    shape_.numSamples = static_cast<int64_t>(audioSource->getSampleCount());
}

bool AudioSource::matches(juce::ARAAudioSource* audioSource) const noexcept
{
    return identity_.audioSource == audioSource;
}

bool AudioSource::canReadSamples() const noexcept
{
    return readerLease_ != nullptr && shape_.isValid();
}

void AudioSource::createReaderLease()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    jassert(identity_.audioSource->isSampleAccessEnabled()
            && !identity_.audioSource->isDeactivatedForUndoHistory()
            && readerLease_ == nullptr);
    readerLease_ = std::make_unique<ARA::PlugIn::HostAudioReader>(identity_.audioSource);
}

void AudioSource::clearReaderLease() noexcept
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    readerLease_.reset();
}

bool AudioSource::readAudioSamples(int64_t sourceSamplePosition,
                                  int samplesPerChannel,
                                  void* const buffers[]) const noexcept
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    jassert(canReadSamples());
    return readerLease_->readAudioSamples(sourceSamplePosition, samplesPerChannel, buffers);
}

} // namespace OpenTune
