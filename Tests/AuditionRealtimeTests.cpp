#include "../Source/Audio/NotePitchAudition.h"
#include "../Source/Utils/NoteAudition.h"
#include "../Source/Utils/PianoKeyAudition.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#if defined(OPENTUNE_REALTIME_AUDIT)
#include "RealtimeAudit.h"
#endif

namespace OpenTune {

struct AuditionRealtimeTestAccess {
    static int stagePitch(NotePitchAudition& audition, uint64_t generation,
                          float value, double sampleRate)
    {
        const int index = audition.claimBuildSlot();
        if (index < 0)
            return -1;

        audition.discardPendingSlots();
        auto& slot = audition.slots_[static_cast<size_t>(index)];
        slot.snippet.mono.assign(2048, value);
        slot.snippet.sampleRate = sampleRate;
        slot.generation = generation;
        audition.controlGeneration_.store(generation, std::memory_order_release);
        slot.state.store(static_cast<uint8_t>(NotePitchAudition::SlotState::Pending),
                         std::memory_order_release);
        return index;
    }

    static uint8_t pitchSlotState(const NotePitchAudition& audition, int index)
    {
        return audition.slots_[static_cast<size_t>(index)].state.load(
            std::memory_order_acquire);
    }

    static int pitchPendingSlot(const NotePitchAudition& audition)
    {
        for (int i = 0; i < NotePitchAudition::kNumSlots; ++i)
            if (pitchSlotState(audition, i)
                == static_cast<uint8_t>(NotePitchAudition::SlotState::Pending))
                return i;
        return -1;
    }

    static bool pitchSlotIsAudioOwned(const NotePitchAudition& audition, int index)
    {
        return pitchSlotState(audition, index)
            == static_cast<uint8_t>(NotePitchAudition::SlotState::AudioOwned);
    }

    static int pitchVoiceCount(const NotePitchAudition& audition)
    {
        int count = 0;
        for (const auto& voice : audition.voices_)
            count += voice.slotIndex >= 0 ? 1 : 0;
        return count;
    }

    static bool pitchVoiceFading(const NotePitchAudition& audition)
    {
        for (const auto& voice : audition.voices_)
            if (voice.slotIndex >= 0 && voice.targetGain == 0.0f)
                return true;
        return false;
    }

    static int pitchFadingVoiceSlot(const NotePitchAudition& audition)
    {
        for (const auto& voice : audition.voices_)
            if (voice.slotIndex >= 0 && voice.targetGain == 0.0f)
                return voice.slotIndex;
        return -1;
    }

    static uint64_t pitchStopGeneration(const NotePitchAudition& audition)
    {
        return audition.stopGeneration_.load(std::memory_order_acquire);
    }

    static bool pitchWasRejected(const NotePitchAudition& audition, uint64_t generation)
    {
        return audition.rejectedGeneration_.load(std::memory_order_acquire) == generation;
    }

    static bool pitchSlotIsReturned(const NotePitchAudition& audition, int index)
    {
        return pitchSlotState(audition, index)
            == static_cast<uint8_t>(NotePitchAudition::SlotState::Returned);
    }

    static bool pitchSlotIsFreeAndEmpty(const NotePitchAudition& audition, int index)
    {
        const auto& slot = audition.slots_[static_cast<size_t>(index)];
        return slot.state.load(std::memory_order_acquire)
                == static_cast<uint8_t>(NotePitchAudition::SlotState::Free)
            && slot.snippet.mono.empty();
    }

    static void setPitchPendingGeneration(NotePitchAudition& audition, uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(audition.requestMutex_);
        audition.pending_.generation = generation;
        audition.hasPending_ = true;
    }

    static bool pitchHasPendingGeneration(NotePitchAudition& audition,
                                          uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(audition.requestMutex_);
        return audition.hasPending_ && audition.pending_.generation == generation;
    }

    static void processPitchWorkerReturns(NotePitchAudition& audition)
    {
        audition.processRejectedResult();
        audition.reclaimReturnedSlots();
    }

    static bool testNoteEventsAtCapacity(PianoKeyAudition& audition)
    {
        constexpr int eventCount = PianoKeyAudition::kEventBufferSize - 1;
        int position = 0;
        for (int i = 0; i < eventCount - 1; ++i) {
            audition.eventBuffer_[static_cast<size_t>(position)] = {60, true};
            position = (position + 1) % PianoKeyAudition::kEventBufferSize;
        }
        audition.eventBuffer_[static_cast<size_t>(position)] = {60, false};
        position = (position + 1) % PianoKeyAudition::kEventBufferSize;
        audition.eventReadPos_.store(0, std::memory_order_relaxed);
        audition.eventWritePos_.store(position, std::memory_order_release);

        const int processed = audition.processEvents(44100.0);

        if (processed != eventCount
            || audition.eventReadPos_.load(std::memory_order_acquire) != position)
            return false;
        for (const auto& voice : audition.voices_)
            if (!voice.active || !voice.releasing)
                return false;
        return true;
    }

    static bool hasReturnedNoteSlot(const NoteAudition& audition)
    {
        for (const auto& slot : audition.slots_)
            if (slot.state.load(std::memory_order_acquire)
                == static_cast<uint8_t>(NoteAudition::SlotState::Returned))
                return true;
        return false;
    }

    static bool testConcurrentPianoBoundary(PianoKeyAudition& audition)
    {
        for (int i = 0; i < PianoKeyAudition::kEventBufferSize - 1; ++i)
            audition.noteOn(60);

        std::atomic<bool> ready{false};
        std::atomic<bool> finished{false};
        std::thread producer([&] {
            ready.store(true, std::memory_order_release);
            while (!finished.load(std::memory_order_acquire)) {
                audition.noteOn(60);
                audition.noteOff(60);
            }
        });
        while (!ready.load(std::memory_order_acquire))
            std::this_thread::yield();

        int total = 0;
        bool bounded = true;
        for (int block = 0; block < 2048; ++block) {
            const int processed = audition.processEvents(44100.0);
            bounded = bounded && processed >= 0
                && processed <= PianoKeyAudition::kEventBufferSize - 1;
            total += processed;
        }
        finished.store(true, std::memory_order_release);
        producer.join();
        return bounded && total > 0;
    }
};

} // namespace OpenTune

namespace {

using OpenTune::AuditionRealtimeTestAccess;

bool realtimeAuditPassed = true;

template <typename Audition>
void mixAudited(Audition& audition, juce::AudioBuffer<float>& output,
                 int numSamples, double sampleRate,
                 [[maybe_unused]] const char* label = "audition_mix_and_return_slots")
{
#if defined(OPENTUNE_REALTIME_AUDIT)
    OpenTune::RealtimeAudit::Scope scope(label);
#endif
    audition.mixIntoBuffer(output, numSamples, sampleRate);
#if defined(OPENTUNE_REALTIME_AUDIT)
    const auto result = scope.finish();
    OpenTune::RealtimeAuditTest::reportResult(result);
    realtimeAuditPassed = OpenTune::RealtimeAuditTest::isClean(result) && realtimeAuditPassed;
#endif
}

bool check(bool condition, const char* message)
{
    if (!condition)
        std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

void fillSignal(juce::AudioBuffer<float>& buffer, float value)
{
    buffer.clear();
    for (int i = 0; i < buffer.getNumSamples(); ++i)
        buffer.setSample(0, i, value);
}

OpenTune::PlaybackReadSource makeWorkerPitchSource(OpenTune::TimeStretchCache& timeStretchCache)
{
    constexpr int sampleCount = 44100;
    constexpr double sampleRate = 44100.0;
    constexpr double frequency = 220.0;
    constexpr double twoPi = 6.2831853071795864769;

    auto pcm = std::make_shared<juce::AudioBuffer<float>>(1, sampleCount);
    for (int i = 0; i < sampleCount; ++i)
        pcm->setSample(0, i, static_cast<float>(0.4 * std::sin(
            twoPi * frequency * static_cast<double>(i) / sampleRate)));

    auto snapshot = std::make_shared<OpenTune::EditableContentSnapshot>();
    snapshot->audioBuffer = pcm;
    snapshot->audioSampleRate = sampleRate;
    snapshot->pitchCurve = std::make_shared<const OpenTune::PitchCurveSnapshot>(
        std::vector<float>(120, static_cast<float>(frequency)),
        std::vector<float>(120, 0.5f),
        std::vector<OpenTune::PitchCorrectionSegment>{}, 441, sampleRate);

    return OpenTune::makePlaybackReadSource(
        {OpenTune::DomainKind::StandaloneClip, 1, 0}, std::move(snapshot),
        {pcm, sampleRate, 0, 1}, pcm, sampleRate, 0,
        std::make_shared<OpenTune::RenderCache>(), timeStretchCache);
}

int waitForPitchPendingSlot(const OpenTune::NotePitchAudition& audition)
{
    int slot = -1;
    while (slot < 0) {
        slot = AuditionRealtimeTestAccess::pitchPendingSlot(audition);
        if (slot < 0)
            std::this_thread::yield();
    }
    return slot;
}

bool testNoteAuditionLatestWins()
{
    OpenTune::NoteAudition audition;
    juce::AudioBuffer<float> first(1, 1024);
    juce::AudioBuffer<float> second(1, 1024);
    juce::AudioBuffer<float> latest(1, 1024);
    fillSignal(first, 0.1f);
    fillSignal(second, 0.35f);
    fillSignal(latest, 0.8f);

    audition.play(first, 44100.0, 0, first.getNumSamples());
    audition.play(second, 44100.0, 0, second.getNumSamples());
    audition.play(latest, 44100.0, 0, latest.getNumSamples());

    juce::AudioBuffer<float> output(1, 220);
    output.clear();
    mixAudited(audition, output, output.getNumSamples(), 44100.0);
    return check(output.getSample(0, 219) > 0.7f,
                 "NoteAudition rapid requests play the latest PCM");
}

bool testNoteAuditionStopThenRestart()
{
    OpenTune::NoteAudition audition;
    juce::AudioBuffer<float> oldSignal(1, 1024);
    juce::AudioBuffer<float> newSignal(1, 1024);
    fillSignal(oldSignal, 0.2f);
    fillSignal(newSignal, 0.7f);

    audition.play(oldSignal, 44100.0, 0, oldSignal.getNumSamples());
    audition.stop();
    audition.play(newSignal, 44100.0, 0, newSignal.getNumSamples());

    juce::AudioBuffer<float> output(1, 220);
    output.clear();
    mixAudited(audition, output, output.getNumSamples(), 44100.0);
    return check(output.getSample(0, 219) > 0.6f,
                 "a stop generation does not cancel a later play generation");
}

bool testPitchRapidSwitchAndFade()
{
    OpenTune::NotePitchAudition audition;
    const int obsolete = AuditionRealtimeTestAccess::stagePitch(audition, 1, 0.1f, 44100.0);
    const int current = AuditionRealtimeTestAccess::stagePitch(audition, 2, 0.4f, 44100.0);
    if (obsolete < 0 || current < 0)
        return check(false, "pitch test slots are available");

    juce::AudioBuffer<float> output(1, 1);
    output.clear();
    mixAudited(audition, output, 1, 44100.0);
    if (!check(std::abs(output.getSample(0, 0) - 0.4f) < 1.0e-5f,
               "pitch audition replaces an unclaimed pending result"))
        return false;
    if (!check(AuditionRealtimeTestAccess::pitchSlotIsFreeAndEmpty(audition, obsolete),
               "the replaced pending PCM is cleared before publishing the newer result"))
        return false;

    const int next = AuditionRealtimeTestAccess::stagePitch(audition, 3, 0.3f, 44100.0);
    if (next < 0)
        return check(false, "pitch slot is returned for the next producer request");
    output.clear();
    mixAudited(audition, output, 1, 44100.0);
    if (!check(AuditionRealtimeTestAccess::pitchVoiceCount(audition) == 2
                   && AuditionRealtimeTestAccess::pitchVoiceFading(audition),
               "rapid pitch switching keeps the two-voice fade"))
        return false;

    const int fadingSlot = AuditionRealtimeTestAccess::pitchFadingVoiceSlot(audition);
    juce::AudioBuffer<float> fadeOut(1, 600);
    fadeOut.clear();
    mixAudited(audition, fadeOut, fadeOut.getNumSamples(), 44100.0);
    return check(fadingSlot >= 0
                     && AuditionRealtimeTestAccess::pitchSlotIsReturned(audition, fadingSlot)
                     && AuditionRealtimeTestAccess::pitchVoiceCount(audition) == 1,
                 "a faded voice returns its PCM without another incoming result");
}

bool testPitchStopRestartGeneration()
{
    OpenTune::NotePitchAudition audition;
    if (AuditionRealtimeTestAccess::stagePitch(audition, 1, 0.2f, 44100.0) < 0)
        return check(false, "initial pitch slot is available");
    juce::AudioBuffer<float> output(1, 1);
    output.clear();
    mixAudited(audition, output, 1, 44100.0);

    audition.stop();
    const uint64_t restartGeneration =
        AuditionRealtimeTestAccess::pitchStopGeneration(audition) + 1;
    if (AuditionRealtimeTestAccess::stagePitch(
            audition, restartGeneration, 0.8f, 44100.0) < 0)
        return check(false, "restart pitch slot is available");
    output.clear();
    mixAudited(audition, output, 1, 44100.0);
    if (!check(output.getSample(0, 0) > 0.99f,
               "stop fades older voices while a newer result starts"))
        return false;

    audition.stop();
    juce::AudioBuffer<float> fadeOut(1, 600);
    fadeOut.clear();
    mixAudited(audition, fadeOut, fadeOut.getNumSamples(), 44100.0);
    return check(!audition.isActive(), "the current stop generation fades both voices out");
}

bool testPitchRateRejectionAndProducerReclaim()
{
    OpenTune::NotePitchAudition audition;
    const int slot = AuditionRealtimeTestAccess::stagePitch(audition, 1, 0.5f, 48000.0);
    if (slot < 0)
        return check(false, "mismatched-rate pitch slot is available");

    juce::AudioBuffer<float> output(1, 16);
    output.clear();
    mixAudited(audition, output, output.getNumSamples(), 44100.0);
    const bool returned = AuditionRealtimeTestAccess::pitchSlotIsReturned(audition, slot)
        && AuditionRealtimeTestAccess::pitchWasRejected(audition, 1);
    AuditionRealtimeTestAccess::setPitchPendingGeneration(audition, 2);
    AuditionRealtimeTestAccess::processPitchWorkerReturns(audition);
    const bool reclaimed = AuditionRealtimeTestAccess::pitchSlotIsFreeAndEmpty(audition, slot);
    const bool newerPendingSurvives =
        AuditionRealtimeTestAccess::pitchHasPendingGeneration(audition, 2);
    return check(returned && reclaimed && newerPendingSurvives,
                 "rate rejection returns PCM and preserves a newer pending request");
}

bool testPitchPlaybackRateChangeReturnsVoice()
{
    OpenTune::NotePitchAudition audition;
    const int slot = AuditionRealtimeTestAccess::stagePitch(audition, 1, 0.5f, 48000.0);
    if (slot < 0)
        return check(false, "pitch slot is available at the device rate");

    juce::AudioBuffer<float> output(1, 16);
    output.clear();
    mixAudited(audition, output, output.getNumSamples(), 48000.0);
    const bool started = AuditionRealtimeTestAccess::pitchVoiceCount(audition) == 1;

    output.clear();
    mixAudited(audition, output, output.getNumSamples(), 44100.0);
    return check(started
                     && AuditionRealtimeTestAccess::pitchVoiceCount(audition) == 0
                     && AuditionRealtimeTestAccess::pitchSlotIsReturned(audition, slot),
                 "a playing voice returns its PCM when the device rate changes");
}

bool testPitchWorkerRequestPlaybackAndStopFade()
{
    OpenTune::TimeStretchCache timeStretchCache;
    OpenTune::NotePitchAudition audition;
    constexpr double sampleRate = 44100.0;
    static const int targetRegionToken = 0;
    audition.setTargetRegion(&targetRegionToken);
    audition.request(makeWorkerPitchSource(timeStretchCache),
                     0.1, 0.5, 246.94f, sampleRate);

    const int pendingSlot = waitForPitchPendingSlot(audition);

    juce::AudioBuffer<float> output(1, 512);
    output.clear();
    mixAudited(audition, output, output.getNumSamples(), sampleRate, "pitch_worker_first_block");
    if (!check(AuditionRealtimeTestAccess::pitchVoiceCount(audition) == 1
                   && AuditionRealtimeTestAccess::pitchSlotIsAudioOwned(audition, pendingSlot)
                   && output.getMagnitude(0, 0, output.getNumSamples()) > 1.0e-5f,
               "the first audited mix claims and plays worker-rendered PCM"))
        return false;

    output.clear();
    mixAudited(audition, output, output.getNumSamples(), sampleRate, "pitch_worker_next_block");
    if (!check(audition.isActive()
                    && output.getMagnitude(0, 0, output.getNumSamples()) > 1.0e-5f,
               "worker-rendered PCM continues across mix blocks"))
        return false;

    if (!check(audition.activeTargetRegion() == &targetRegionToken,
               "the active voice publishes its target region for playback suppression"))
        return false;

    const uint64_t previousStopGeneration =
        AuditionRealtimeTestAccess::pitchStopGeneration(audition);
    audition.setTargetRegion(nullptr);
    audition.stop();
    if (!check(AuditionRealtimeTestAccess::pitchStopGeneration(audition)
                   > previousStopGeneration,
               "stop publishes a newer stop generation"))
        return false;

    juce::AudioBuffer<float> fadeOut(1, 128);
    fadeOut.clear();
    mixAudited(audition, fadeOut, fadeOut.getNumSamples(), sampleRate, "pitch_worker_stop_fade");
    if (!check(fadeOut.getMagnitude(0, 0, fadeOut.getNumSamples()) > 1.0e-5f
                   && audition.isActive()
                   && audition.activeTargetRegion() == &targetRegionToken,
               "the target remains suppressed while its voice fades"))
        return false;

    juce::AudioBuffer<float> fadeRemainder(1, 512);
    fadeRemainder.clear();
    mixAudited(audition, fadeRemainder, fadeRemainder.getNumSamples(), sampleRate,
               "pitch_worker_stop_fade_remainder");
    return check(fadeRemainder.getMagnitude(0, 424, 88) == 0.0f
                     && !audition.isActive()
                     && audition.activeTargetRegion() == nullptr,
                 "the audio mix completes the stop fade and clears its target");
}

bool testReturnedNotePcmCanBeReusedAndDestroyed()
{
    OpenTune::NoteAudition audition;
    juce::AudioBuffer<float> first(1, 64);
    juce::AudioBuffer<float> next(1, 1024);
    fillSignal(first, 0.4f);
    fillSignal(next, 0.6f);

    audition.play(first, 44100.0, 0, first.getNumSamples());
    juce::AudioBuffer<float> output(1, 128);
    output.clear();
    mixAudited(audition, output, output.getNumSamples(), 44100.0);

    const bool foundReturned = AuditionRealtimeTestAccess::hasReturnedNoteSlot(audition);
    audition.play(next, 44100.0, 0, next.getNumSamples());

    return check(foundReturned,
                 "audio completion publishes a returned slot for producer reuse");
}

bool testPianoEventSnapshotBoundary()
{
    OpenTune::PianoKeyAudition audition;
    return check(AuditionRealtimeTestAccess::testNoteEventsAtCapacity(audition),
                 "one block processes exactly the 63-event SPSC capacity");
}

bool testPianoLoadNoteOnMixNoteOff()
{
    OpenTune::PianoKeyAudition audition;
    audition.loadSamples();
    audition.noteOn(60);

    juce::AudioBuffer<float> firstBlock(1, 512);
    firstBlock.clear();
    mixAudited(audition, firstBlock, firstBlock.getNumSamples(), 44100.0, "piano_note_on_first_block");
    if (!check(firstBlock.getMagnitude(0, 0, firstBlock.getNumSamples()) > 1.0e-5f,
               "Piano noteOn produces PCM in the first mixed block"))
        return false;

    audition.noteOff(60);
    juce::AudioBuffer<float> releaseBlock(1, 4096);
    releaseBlock.clear();
    mixAudited(audition, releaseBlock, releaseBlock.getNumSamples(), 44100.0, "piano_note_off_release");
    return check(releaseBlock.getMagnitude(0, 0, 128) > 1.0e-5f
                     && releaseBlock.getMagnitude(0, 3072, 1024) == 0.0f,
                 "Piano noteOff release reaches final silence");
}

bool testPianoConcurrentProducerBoundary()
{
    OpenTune::PianoKeyAudition audition;
    return check(AuditionRealtimeTestAccess::testConcurrentPianoBoundary(audition),
                 "continuous event publication keeps each block within 63 events");
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
#if defined(OPENTUNE_REALTIME_AUDIT)
    const auto hooks = OpenTune::RealtimeAuditTest::installWindowsHooks();
    OpenTune::RealtimeAuditTest::reportHookCoverage(hooks);
    if (!check(hooks.patchedImports > 0, "Windows realtime audit hooks are installed"))
        return 1;
#endif
    const bool passed = testNoteAuditionLatestWins()
        && testNoteAuditionStopThenRestart()
        && testPitchRapidSwitchAndFade()
        && testPitchStopRestartGeneration()
        && testPitchRateRejectionAndProducerReclaim()
        && testPitchPlaybackRateChangeReturnsVoice()
        && testPitchWorkerRequestPlaybackAndStopFade()
        && testReturnedNotePcmCanBeReusedAndDestroyed()
        && testPianoLoadNoteOnMixNoteOff()
        && testPianoEventSnapshotBoundary()
        && testPianoConcurrentProducerBoundary();
    if (!passed || !realtimeAuditPassed)
        return 1;
    std::puts("AuditionRealtimeTests passed");
    return 0;
}
