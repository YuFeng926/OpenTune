#include "PianoKeyAudition.h"
#include "BinaryData.h"
#include "AppLogger.h"

namespace OpenTune {

// ---------------------------------------------------------------------------
PianoKeyAudition::PianoKeyAudition() = default;

// ---------------------------------------------------------------------------
void PianoKeyAudition::loadSamples()
{
    if (samplesLoaded_)
        return;

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    for (int midi = kMidiMin; midi <= kMidiMax; ++midi)
    {
        const juce::String name = juce::String("piano_") + juce::String(midi) + "_mp3";

        int dataSize = 0;
        const char* data = BinaryData::getNamedResource(name.toRawUTF8(), dataSize);
        jassert(data != nullptr && dataSize > 0);

        if (data == nullptr || dataSize <= 0)
            continue;

        auto* stream = new juce::MemoryInputStream(data, static_cast<size_t>(dataSize), false);
        auto reader = std::unique_ptr<juce::AudioFormatReader>(
            formatManager.createReaderFor(std::unique_ptr<juce::InputStream>(stream)));

        jassert(reader != nullptr);
        if (reader == nullptr)
            continue;

        const int idx = midi - kMidiMin;
        auto& smp = samples_[static_cast<size_t>(idx)];
        smp.pcm.setSize(1, static_cast<int>(reader->lengthInSamples));
        reader->read(&smp.pcm, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);

        // 计算 onset：首个振幅超过阈值的位置，跳过静音前导
        smp.onsetSample = 0;
        const float* d = smp.pcm.getReadPointer(0);
        const int len = smp.pcm.getNumSamples();
        for (int s = 0; s < len; ++s)
        {
            if (std::fabs(d[s]) > 0.005f)
            {
                smp.onsetSample = s;
                break;
            }
        }
    }

    samplesLoaded_ = true;
}

// ---------------------------------------------------------------------------
void PianoKeyAudition::noteOn(int midiNote)
{
    const int pos = eventWritePos_.load(std::memory_order_relaxed);
    const int next = (pos + 1) % kEventBufferSize;

    // 缓冲区满则丢弃（极不可能发生，但避免阻塞消息线程）
    if (next == eventReadPos_.load(std::memory_order_acquire))
        return;

    eventBuffer_[static_cast<size_t>(pos)] = { midiNote, true };
    eventWritePos_.store(next, std::memory_order_release);

    pressedNote_.store(midiNote, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
void PianoKeyAudition::noteOff(int midiNote)
{
    const int pos = eventWritePos_.load(std::memory_order_relaxed);
    const int next = (pos + 1) % kEventBufferSize;

    if (next == eventReadPos_.load(std::memory_order_acquire))
        return;

    eventBuffer_[static_cast<size_t>(pos)] = { midiNote, false };
    eventWritePos_.store(next, std::memory_order_release);

    int expected = midiNote;
    pressedNote_.compare_exchange_strong(expected, -1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
void PianoKeyAudition::startVoice(int midiNote, double sampleRate)
{
    if (midiNote < kMidiMin || midiNote > kMidiMax)
        return;

    const int sampleIdx = midiNote - kMidiMin;
    const auto& smp = samples_[static_cast<size_t>(sampleIdx)];

    // 先查找空闲 Voice
    Voice* target = nullptr;
    for (auto& v : voices_)
    {
        if (!v.active) { target = &v; break; }
    }

    // 无空闲则抢占第一个活跃 Voice
    if (target == nullptr)
        target = &voices_.front();

    target->active = true;
    target->midiNote = midiNote;
    target->sampleIndex = sampleIdx;
    target->position = static_cast<double>(smp.onsetSample);
    target->playbackRate = kSampleRate / sampleRate;
    target->gain = 0.7f;
    target->releasing = false;
    target->releaseGain = 1.0f;
    target->releaseDecrement = 0.0f;
}

// ---------------------------------------------------------------------------
void PianoKeyAudition::stopVoice(int midiNote, double sampleRate)
{
    const float dec = 1.0f / static_cast<float>(kReleaseTimeSeconds * sampleRate);

    for (auto& v : voices_)
    {
        if (v.active && v.midiNote == midiNote && !v.releasing)
        {
            v.releasing = true;
            v.releaseGain = 1.0f;
            v.releaseDecrement = dec;
        }
    }
}

// ---------------------------------------------------------------------------
void PianoKeyAudition::processEvents(double sampleRate)
{
    int pos = eventReadPos_.load(std::memory_order_relaxed);

    while (pos != eventWritePos_.load(std::memory_order_acquire))
    {
        const auto& e = eventBuffer_[static_cast<size_t>(pos)];

        if (e.isOn)
            startVoice(e.midiNote, sampleRate);
        else
            stopVoice(e.midiNote, sampleRate);

        pos = (pos + 1) % kEventBufferSize;
        eventReadPos_.store(pos, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
void PianoKeyAudition::mixIntoBuffer(juce::AudioBuffer<float>& output, int numSamples, double sampleRate)
{
    if (!samplesLoaded_)
        return;

    processEvents(sampleRate);

    const int numChannels = output.getNumChannels();

    for (auto& voice : voices_)
    {
        if (!voice.active)
            continue;

        const auto& sample = samples_[static_cast<size_t>(voice.sampleIndex)];
        const float* src = sample.pcm.getReadPointer(0);
        const int srcLen = sample.pcm.getNumSamples();

        for (int i = 0; i < numSamples; ++i)
        {
            const int idx = static_cast<int>(voice.position);
            const float frac = static_cast<float>(voice.position - static_cast<double>(idx));

            float s = 0.0f;
            if (idx + 1 < srcLen)
                s = src[idx] * (1.0f - frac) + src[idx + 1] * frac;
            else if (idx < srcLen)
                s = src[idx];

            s *= voice.gain;

            if (voice.releasing)
            {
                s *= voice.releaseGain;
                voice.releaseGain -= voice.releaseDecrement;
            }

            for (int ch = 0; ch < numChannels; ++ch)
                output.addSample(ch, i, s);

            voice.position += voice.playbackRate;

            if (voice.position >= static_cast<double>(srcLen)
                || (voice.releasing && voice.releaseGain <= 0.0f))
            {
                voice.active = false;
                break;
            }
        }
    }
}

} // namespace OpenTune
