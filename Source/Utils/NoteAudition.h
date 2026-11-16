#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <memory>

namespace OpenTune {

/**
 * Note audition: when moving between notes with the arrows, plays that note's vocal
 * fragment, as in Melodyne. The host transport is not touched: the audio is
 * mixed into the plugin output just like the piano key audition.
 *
 * play/stop are called on the message thread, mixIntoBuffer on the audio thread.
 * The audio thread never allocates or frees memory: it parks the old clip
 * in retired_, and the message thread frees it on its next call.
 */
class NoteAudition {
public:
    NoteAudition() = default;

    /// Play numSamples of source from startSample (replaces whatever is playing).
    void play(const juce::AudioBuffer<float>& source, double sourceSampleRate,
              int startSample, int numSamples);
    void stop();

    void mixIntoBuffer(juce::AudioBuffer<float>& output, int numSamples, double outputSampleRate);

    static constexpr double kMaxDurationSeconds = 4.0;

private:
    struct Clip {
        juce::AudioBuffer<float> mono;
        double sampleRate = 44100.0;
    };

    void takePendingClip();

    juce::SpinLock lock_;
    std::unique_ptr<Clip> pending_;   // guarded by lock_
    std::unique_ptr<Clip> retired_;   // guarded by lock_
    bool stopRequested_ = false;      // guarded by lock_

    // Audio thread only.
    std::unique_ptr<Clip> playing_;
    double position_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoteAudition)
};

} // namespace OpenTune
