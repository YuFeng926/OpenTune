// CaptureSession explicit-recording tests. Drives the real CaptureSession through
// arm -> processBlock -> tick and inspects the finalized segments' content.

#include "../Source/Plugin/Capture/CaptureSession.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using OpenTune::ContentKey;
using OpenTune::DomainKind;
using OpenTune::Capture::CaptureSession;
using OpenTune::Capture::CaptureSegment;
using OpenTune::Capture::ProcessorBindings;
using OpenTune::Capture::SegmentState;

struct Counters
{
    int publish = 0;
    int refresh = 0;
    int fullRender = 0;
    int retire = 0;
    std::map<uint64_t, int> publishPerId;
};

ProcessorBindings countingBindings(Counters& c)
{
    ProcessorBindings b;
    b.publishPlaybackSource =
        [&c](const ContentKey& key,
             const std::shared_ptr<const OpenTune::EditableContentSnapshot>&,
             const std::shared_ptr<const juce::AudioBuffer<float>>&,
             double) {
            ++c.publish;
            ++c.publishPerId[key.objectId];
        };
    b.refreshSegment = [&c](ContentKey) { ++c.refresh; };
    b.requestFullRender = [&c](ContentKey) { ++c.fullRender; };
    b.retireSegment = [&c](ContentKey) { ++c.retire; };
    return b;
}

struct TestBindings
{
    int publishCount = 0;

    ProcessorBindings make()
    {
        ProcessorBindings b;
        b.publishPlaybackSource =
            [this](const ContentKey&,
                   const std::shared_ptr<const OpenTune::EditableContentSnapshot>&,
                   const std::shared_ptr<const juce::AudioBuffer<float>>&,
                   double) {
                ++publishCount;
            };
        return b;
    }
};

// Feed one block whose two channels hold distinguishable values.
void feedBlock(CaptureSession& session,
               int numSamples,
               int64_t hostSample,
               bool playing,
               float leftValue = 0.25f,
               float rightValue = -0.5f,
               double sampleRate = 48000.0)
{
    juce::AudioBuffer<float> buffer(2, numSamples);
    for (int s = 0; s < numSamples; ++s) {
        buffer.setSample(0, s, leftValue);
        buffer.setSample(1, s, rightValue);
    }
    session.processBlock(buffer, hostSample, sampleRate, playing);
}

CaptureSegment* findCapturing(const CaptureSession& session)
{
    for (const auto& seg : session.testSegments()) {
        if (seg->getState() == SegmentState::Capturing)
            return seg.get();
    }
    return nullptr;
}

CaptureSegment* findById(const CaptureSession& session, uint64_t id)
{
    for (const auto& seg : session.testSegments()) {
        if (seg->contentKey.objectId == id)
            return seg.get();
    }
    return nullptr;
}

const juce::AudioBuffer<float>* publishedAudio(const CaptureSegment& segment)
{
    if (!segment.content)
        return nullptr;
    return segment.content->content().audioBuffer.get();
}

std::shared_ptr<juce::AudioBuffer<float>> makeAudio(int samples)
{
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, samples);
    audio->clear();
    return audio;
}

// Test stand-in for CRS: stores the published PlaybackReadSource so the view can
// freeze it together with the replacement geometry.
struct FrozenSourceStore
{
    std::map<ContentKey, OpenTune::PlaybackReadSource> sources;

    void publish(const ContentKey& key,
                 const std::shared_ptr<const OpenTune::EditableContentSnapshot>& snapshot,
                 const std::shared_ptr<const juce::AudioBuffer<float>>&,
                 double)
    {
        OpenTune::PlaybackReadSource source;
        source.contentKey = key;
        source.contentSnapshot = snapshot;
        source.nativeSource = snapshot->nativeSource;
        source.canonicalBuffer = snapshot->audioBuffer;
        source.canonicalSampleRate = snapshot->audioSampleRate;
        source.firstCanonicalSample = snapshot->firstCanonicalSample;
        sources[key] = std::move(source);
    }

    bool get(ContentKey key, OpenTune::PlaybackReadSource& out) const
    {
        const auto it = sources.find(key);
        if (it == sources.end())
            return false;
        out = it->second;
        return true;
    }
};

void publishOwnerSource(FrozenSourceStore& store, const CaptureSession& session, uint64_t id)
{
    for (const auto& seg : session.testSegments()) {
        if (seg->contentKey.objectId != id || !seg->content)
            continue;
        const auto snap = seg->content->snapshotContent();
        store.publish(seg->contentKey, snap, snap->audioBuffer, snap->audioSampleRate);
        return;
    }
}

using ReplaceProbe = std::function<void(juce::AudioBuffer<float>&, int, int,
                                        const OpenTune::PlaybackReadSource&,
                                        int64_t, double, double)>;

ProcessorBindings sourceFrozenBindings(FrozenSourceStore& store, ReplaceProbe onReplace = {})
{
    ProcessorBindings b;
    b.publishPlaybackSource =
        [&store](const ContentKey& key,
                 const std::shared_ptr<const OpenTune::EditableContentSnapshot>& snapshot,
                 const std::shared_ptr<const juce::AudioBuffer<float>>& audio,
                 double sampleRate) {
            store.publish(key, snapshot, audio, sampleRate);
        };
    b.getPlaybackReadSource = [&store](ContentKey key, OpenTune::PlaybackReadSource& out) {
        return store.get(key, out);
    };
    b.replaceWithRendered = std::move(onReplace);
    return b;
}

bool testSpanBudgetExhaustionDropsWholeTake()
{
    TestBindings bindings;
    CaptureSession session(bindings.make());
    session.prepareToPlay(48000.0, 512, 2);

    // Tiny span capacity: 2 spans per take. Discontinuities come from gaps in
    // host sample positions (simulated by jumping the host position between blocks).
    session.testSetSpanCapacityOverride(2);

    if (!session.armNewCapture())
        return false;

    feedBlock(session, 512, 0, true);        // span 0
    feedBlock(session, 512, 100000, true);   // span 1
    feedBlock(session, 512, 200000, true);   // budget spent -> latch

    CaptureSegment* capturing = findCapturing(session);
    if (capturing == nullptr)
        return false;
    if (!capturing->spanBudgetExhausted.load(std::memory_order_acquire))
        return false;
    const int spansAtExhaustion = capturing->numSpans.load(std::memory_order_acquire);
    const int64_t maxAtExhaustion = capturing->maxHostSampleExclusive;
    feedBlock(session, 512, 200512, true);
    if (capturing->numSpans.load(std::memory_order_acquire) != spansAtExhaustion
        || capturing->maxHostSampleExclusive != maxAtExhaustion) {
        return false;
    }

    // User stops: an exhausted take must not publish partial data.
    session.stopCapture();

    bool sawPending = false;
    for (auto& seg : session.testSegments()) {
        if (seg->getState() == SegmentState::Pending)
            sawPending = true;
    }
    if (sawPending)
        return false;

    // Model the race where stopCapture changed the state to Pending just before
    // the audio thread published its exhaustion latch.
    capturing->setState(SegmentState::Pending);

    for (int i = 0; i < 8; ++i)
        session.tick();

    for (auto& seg : session.testSegments()) {
        const auto state = seg->getState();
        if (state == SegmentState::Capturing || state == SegmentState::Pending)
            return false;
    }
    if (bindings.publishCount != 0)
        return false;  // no partial success result was published

    // Session is idle again; a new arm is possible.
    return session.armNewCapture();
}

bool testLoopRewindKeepsLastWritePerHostSample()
{
    TestBindings bindings;
    CaptureSession session(bindings.make());
    session.prepareToPlay(48000.0, 256, 2);
    if (!session.armNewCapture())
        return false;

    // Two passes over the same 256-sample host range: the later pass must win.
    feedBlock(session, 256, 0, true, 0.25f, -0.5f);
    feedBlock(session, 256, 0, true, 0.75f, 0.125f);

    session.stopCapture();
    session.tick();

    for (auto& seg : session.testSegments()) {
        if (seg->getState() != SegmentState::Processing)
            continue;
        const auto* audio = publishedAudio(*seg);
        if (audio == nullptr || audio->getNumSamples() != 256)
            return false;
        for (int s = 0; s < audio->getNumSamples(); ++s) {
            if (audio->getSample(0, s) != 0.75f || audio->getSample(1, s) != 0.125f)
                return false;
        }
        if (seg->hostStartSample.load(std::memory_order_acquire) != 0
            || seg->hostSampleCount.load(std::memory_order_acquire) != 256)
            return false;
        return bindings.publishCount == 1;
    }
    return false;
}

bool testWindowWrapAroundReadWrite()
{
    TestBindings bindings;
    CaptureSession session(bindings.make());
    session.prepareToPlay(48000.0, 256, 2);
    if (!session.armNewCapture())
        return false;

    CaptureSegment* capturing = findCapturing(session);
    if (capturing == nullptr)
        return false;
    const int64_t capacity = static_cast<int64_t>(capturing->capacitySamples);

    // Anchor at host 100, then rewind to host 0 with a 256-sample block. The window
    // offset of host 0 is capacity - 100, so the write wraps 100 tail + 156 head,
    // and the later pass overwrites host 100..199.
    feedBlock(session, 100, 100, true, 0.25f, -0.5f);
    feedBlock(session, 256, 0, true, 0.5f, -0.25f);
    if (capturing->windowOffset(0) != static_cast<int>(capacity - 100))
        return false;

    session.stopCapture();
    session.tick();

    for (auto& seg : session.testSegments()) {
        if (seg->getState() != SegmentState::Processing)
            continue;
        const auto* audio = publishedAudio(*seg);
        if (audio == nullptr || audio->getNumSamples() != 256)
            return false;
        for (int s = 0; s < audio->getNumSamples(); ++s) {
            if (audio->getSample(0, s) != 0.5f || audio->getSample(1, s) != -0.25f)
                return false;
        }
        return seg->hostStartSample.load(std::memory_order_acquire) == 0
            && seg->hostSampleCount.load(std::memory_order_acquire) == 256;
    }
    return false;
}

bool testWindowOverflowEndsTakeAndKeepsWrittenContent()
{
    TestBindings bindings;
    CaptureSession session(bindings.make());
    session.prepareToPlay(1000.0, 1000, 2);
    if (!session.armNewCapture())
        return false;

    CaptureSegment* capturing = findCapturing(session);
    if (capturing == nullptr)
        return false;
    const int64_t capacity = static_cast<int64_t>(capturing->capacitySamples);

    // First block fills part of the window; the second jumps beyond the capacity.
    feedBlock(session, 1000, 0, true, 0.5f, 0.5f, 1000.0);
    feedBlock(session, 1000, capacity + 1, true, 0.9f, 0.9f, 1000.0);
    if (!capturing->windowExhausted.load(std::memory_order_acquire))
        return false;
    if (capturing->numSpans.load(std::memory_order_acquire) != 1)
        return false;

    session.tick();  // ends the take
    session.tick();  // finalizes it

    for (auto& seg : session.testSegments()) {
        if (seg->getState() != SegmentState::Processing)
            continue;
        const auto* audio = publishedAudio(*seg);
        if (audio == nullptr || audio->getNumSamples() != 1000)
            return false;
        if (audio->getSample(0, 0) != 0.5f || audio->getSample(0, 999) != 0.5f)
            return false;
        return seg->hostStartSample.load(std::memory_order_acquire) == 0
            && seg->hostSampleCount.load(std::memory_order_acquire) == 1000;
    }
    return false;
}

bool testDiscontinuityAppendsSpanAndContiguousExtends()
{
    TestBindings bindings;
    CaptureSession session(bindings.make());
    session.prepareToPlay(1000.0, 100, 2);
    session.testSetSpanCapacityOverride(4);
    if (!session.armNewCapture())
        return false;

    feedBlock(session, 100, 0, true, 0.1f, 0.1f, 1000.0);
    feedBlock(session, 100, 100, true, 0.2f, 0.2f, 1000.0);      // contiguous -> extend
    feedBlock(session, 100, 500, true, 0.3f, 0.3f, 1000.0);      // jump -> new span

    CaptureSegment* capturing = findCapturing(session);
    if (capturing == nullptr)
        return false;
    if (capturing->numSpans.load(std::memory_order_acquire) != 2)
        return false;
    if (capturing->spans[0].hostStartSample != 0 || capturing->spans[0].sampleCount != 200)
        return false;
    if (capturing->spans[1].hostStartSample != 500 || capturing->spans[1].sampleCount != 100)
        return false;

    session.stopCapture();
    session.tick();

    int published = 0;
    for (auto& seg : session.testSegments()) {
        if (seg->getState() != SegmentState::Processing)
            continue;
        ++published;
        const auto* audio = publishedAudio(*seg);
        if (audio == nullptr)
            return false;
        const int64_t start = seg->hostStartSample.load(std::memory_order_acquire);
        if (start == 0 && (audio->getNumSamples() != 200 || audio->getSample(0, 199) != 0.2f))
            return false;
        if (start == 500 && (audio->getNumSamples() != 100 || audio->getSample(0, 0) != 0.3f))
            return false;
    }
    return published == 2;
}

bool testExplicitRecordingIgnoresExistingContent()
{
    TestBindings bindings;
    CaptureSession session(bindings.make());
    session.prepareToPlay(48000.0, 256, 2);

    // Existing Edited content covering the recording range.
    auto pcm = std::make_shared<juce::AudioBuffer<float>>(2, 256);
    pcm->clear();
    session.testInjectEditedSegment(0.0, 256.0 / 48000.0, 1, pcm, 48000.0);

    if (!session.armNewCapture())
        return false;

    feedBlock(session, 256, 0, true, 0.4f, 0.4f);
    CaptureSegment* capturing = findCapturing(session);
    if (capturing == nullptr)
        return false;
    if (capturing->numSpans.load(std::memory_order_acquire) != 1)
        return false;
    if (capturing->spans[0].hostStartSample != 0 || capturing->spans[0].sampleCount != 256)
        return false;

    session.stopCapture();
    session.tick();

    for (auto& seg : session.testSegments()) {
        if (seg->hostStartSample.load(std::memory_order_acquire) != 0)
            continue;
        if (seg->contentKey.objectId == 1)
            continue;
        const auto* audio = publishedAudio(*seg);
        if (audio != nullptr && audio->getNumSamples() == 256 && audio->getSample(0, 0) == 0.4f)
            return true;
    }
    return false;
}

bool testReclaimWaitsForPublishedViewReaders()
{
    std::mutex gateMutex;
    std::condition_variable gate;
    bool callbackEntered = false;
    bool releaseCallback = false;
    std::atomic<int> retiredCount { 0 };

    FrozenSourceStore store;
    ProcessorBindings bindings;
    bindings.replaceWithRendered = [&](juce::AudioBuffer<float>&, int, int,
                                       const OpenTune::PlaybackReadSource&,
                                       int64_t, double, double) {
        std::unique_lock<std::mutex> lock(gateMutex);
        if (callbackEntered)
            return;
        callbackEntered = true;
        gate.notify_all();
        gate.wait(lock, [&] { return releaseCallback; });
    };
    bindings.retireSegment = [&](ContentKey) {
        retiredCount.fetch_add(1, std::memory_order_relaxed);
    };
    bindings.publishPlaybackSource =
        [&store](const ContentKey& key,
                 const std::shared_ptr<const OpenTune::EditableContentSnapshot>& snapshot,
                 const std::shared_ptr<const juce::AudioBuffer<float>>& audio,
                 double sampleRate) {
            store.publish(key, snapshot, audio, sampleRate);
        };
    bindings.getPlaybackReadSource = [&store](ContentKey key,
                                              OpenTune::PlaybackReadSource& out) {
        return store.get(key, out);
    };

    CaptureSession session(std::move(bindings));
    session.prepareToPlay(48000.0, 512, 2);
    session.testInjectEditedSegment(0.0, 1.0, 1, makeAudio(512), 48000.0);
    publishOwnerSource(store, session, 1);
    session.refreshPublishedReplacementGeometry();

    // An empty take is dropped at finalize and parked for reclaim.
    session.armNewCapture();
    session.stopCapture();

    std::thread audioThread([&] {
        juce::AudioBuffer<float> buffer(2, 512);
        buffer.clear();
        session.processBlock(buffer, 0, 48000.0, true);
    });

    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        entered = gate.wait_for(lock, std::chrono::seconds(2), [&] { return callbackEntered; });
    }
    if (!entered) {
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            releaseCallback = true;
        }
        gate.notify_all();
        audioThread.join();
        return false;
    }

    for (int i = 0; i < 8; ++i)
        session.tick();
    const bool heldWhileReaderActive = retiredCount.load(std::memory_order_relaxed) == 0;

    {
        std::lock_guard<std::mutex> lock(gateMutex);
        releaseCallback = true;
    }
    gate.notify_all();
    audioThread.join();

    for (int i = 0; i < 8; ++i)
        session.tick();
    return heldWhileReaderActive && retiredCount.load(std::memory_order_relaxed) == 1;
}

bool testEditedStillReplacesPlaybackWhileProcessingDoesNot()
{
    int replaceCalls = 0;
    FrozenSourceStore store;
    CaptureSession session(sourceFrozenBindings(store,
        [&](juce::AudioBuffer<float>&, int, int, const OpenTune::PlaybackReadSource&,
            int64_t, double, double) {
            ++replaceCalls;
        }));
    session.prepareToPlay(48000.0, 512, 2);

    // Edited segment → replacement playback still happens.
    session.testInjectEditedSegment(0.0, 1.0, 1, makeAudio(512), 48000.0);
    publishOwnerSource(store, session, 1);
    session.refreshPublishedReplacementGeometry();
    juce::AudioBuffer<float> buffer(2, 512);
    buffer.clear();
    session.processBlock(buffer, 0, 48000.0, true);
    if (replaceCalls == 0)
        return false;

    // Processing segment → no replacement.
    session.testInjectProcessingSegment(2.0, 1.0, 2, nullptr, 48000.0);
    const int before = replaceCalls;
    session.processBlock(buffer, 48000 * 2, 48000.0, true);
    return replaceCalls == before;
}

bool testStopRequestSurvivesActiveWriter()
{
    TestBindings bindings;
    CaptureSession session(bindings.make());
    session.prepareToPlay(1000.0, 256, 1);

    // A: stop latched while the writer bit is held, retried by tick().
    if (!session.armNewCapture())
        return false;
    feedBlock(session, 256, 0, true, 0.5f, 0.5f, 1000.0);
    CaptureSegment* capturing = findCapturing(session);
    if (capturing == nullptr)
        return false;
    if (!capturing->tryAcquireWriter())
        return false;
    session.stopCapture();
    if (capturing->getState() != SegmentState::Capturing
        || !capturing->stopRequested.load(std::memory_order_acquire))
        return false;
    capturing->releaseWriter();
    session.tick();
    if (capturing->getState() != SegmentState::Processing)
        return false;
    if (capturing->stopRequested.load(std::memory_order_acquire))
        return false;
    const auto* audio = publishedAudio(*capturing);
    if (audio == nullptr || audio->getNumSamples() != 256)
        return false;

    // B: the latch is cleared when a second stop succeeds directly after the
    // writer released, before tick() retries.
    if (!session.armNewCapture())
        return false;
    feedBlock(session, 256, 1000, true, 0.25f, 0.25f, 1000.0);
    capturing = findCapturing(session);
    if (capturing == nullptr)
        return false;
    if (!capturing->tryAcquireWriter())
        return false;
    session.stopCapture();
    if (!capturing->stopRequested.load(std::memory_order_acquire))
        return false;
    capturing->releaseWriter();
    session.stopCapture();
    if (capturing->getState() != SegmentState::Pending
        || capturing->stopRequested.load(std::memory_order_acquire))
        return false;
    session.tick();
    return capturing->getState() == SegmentState::Processing;
}

bool testConcurrentArmStopProcessStress()
{
    CaptureSession session({});
    session.prepareToPlay(48000.0, 256, 2);

    std::atomic<bool> running { true };
    std::atomic<bool> violation { false };

    std::thread audioThread([&] {
        juce::AudioBuffer<float> buffer(2, 256);
        buffer.clear();
        int64_t hostSample = 0;
        while (running.load(std::memory_order_relaxed)) {
            session.processBlock(buffer, hostSample, 48000.0, true);
            hostSample += 256;
        }
    });

    auto checkSnapshot = [&] {
        for (const auto& [state, writer] : session.testStateSnapshot()) {
            if (state == SegmentState::Pending && writer)
                violation.store(true, std::memory_order_relaxed);
        }
    };

    for (int i = 0; i < 200; ++i) {
        session.armNewCapture();
        session.stopCapture();
        session.tick();
        checkSnapshot();
    }

    running.store(false, std::memory_order_relaxed);
    audioThread.join();

    for (int i = 0; i < 8; ++i)
        session.tick();

    if (violation.load(std::memory_order_relaxed))
        return false;
    // After all activity stopped, no Pending segment may hold the writer bit.
    for (const auto& [state, writer] : session.testStateSnapshot()) {
        if (state == SegmentState::Pending && writer)
            return false;
    }
    return true;
}

bool testRetireHappensAfterAudioReadersExitOnce()
{
    std::atomic<int> retiredCount { 0 };

    ProcessorBindings bindings;
    bindings.retireSegment = [&](ContentKey) {
        retiredCount.fetch_add(1, std::memory_order_relaxed);
    };

    CaptureSession session(std::move(bindings));
    session.prepareToPlay(48000.0, 512, 2);
    session.testInjectEditedSegment(0.0, 1.0, 1, nullptr, 48000.0);

    // Empty take → dropped at finalize → its key becomes a retire candidate.
    session.armNewCapture();
    session.stopCapture();

    std::thread audioThread([&] {
        juce::AudioBuffer<float> buffer(2, 512);
        buffer.clear();
        for (int i = 0; i < 20; ++i)
            session.processBlock(buffer, i * 512, 48000.0, true);
    });
    audioThread.join();

    for (int i = 0; i < 8; ++i)
        session.tick();

    if (retiredCount.load(std::memory_order_relaxed) != 1)
        return false;

    // Repeated ticks must not retire again; further processBlock must not crash.
    for (int i = 0; i < 8; ++i)
        session.tick();
    if (retiredCount.load(std::memory_order_relaxed) != 1)
        return false;

    juce::AudioBuffer<float> buffer(2, 512);
    buffer.clear();
    session.processBlock(buffer, 0, 48000.0, true);
    return true;
}

bool testPublishCountTracksStructuralChanges()
{
    CaptureSession session({});
    session.prepareToPlay(48000.0, 256, 2);

    const uint64_t base = session.testPublishedViewCount();

    // No structural change: stopCapture / onRenderComplete must not publish.
    session.stopCapture();
    session.onRenderComplete(ContentKey{DomainKind::RegularVST3Capture, 999, 0});
    session.onRenderFailed(ContentKey{DomainKind::RegularVST3Capture, 999, 0}, "no such segment");
    if (session.testPublishedViewCount() != base)
        return false;

    // armNewCapture adds a segment → exactly one publication.
    if (!session.armNewCapture())
        return false;
    if (session.testPublishedViewCount() != base + 1)
        return false;

    // Finalize of the pending take → exactly one more publication (via tick).
    session.stopCapture();
    if (session.testPublishedViewCount() != base + 1)
        return false;
    session.tick();
    session.tick();
    if (session.testPublishedViewCount() != base + 2)
        return false;

    return true;
}

// ── 阶段 2：新 take 覆盖旧段的裁剪/切分测试 ─────────────────────────────

// 构造一份带内容的小 ContentState（48000 Hz）。
OpenTune::ContentState makeContentState(int64_t numSamples,
                                        double sampleRate,
                                        std::function<float(int64_t)> sampleAt,
                                        std::vector<OpenTune::Note> notes = {})
{
    OpenTune::ContentState state;
    auto pcm = std::make_shared<juce::AudioBuffer<float>>(1, static_cast<int>(numSamples));
    for (int64_t s = 0; s < numSamples; ++s)
        pcm->setSample(0, static_cast<int>(s), sampleAt(s));
    state.audioBuffer = pcm;
    state.sampleRate = sampleRate;
    state.notes = std::move(notes);
    return state;
}

// 运行一次 arm → feed → stop → tick(twice)，返回 finalize 后 Processing 新段指针集合。
void runTake(CaptureSession& session, int64_t hostStart, int numSamples, float value, double sr)
{
    if (!session.armNewCapture())
        return;
    juce::AudioBuffer<float> buffer(1, numSamples);
    for (int s = 0; s < numSamples; ++s)
        buffer.setSample(0, s, value);
    session.processBlock(buffer, hostStart, sr, true);
    session.stopCapture();
    session.tick();
    session.tick();
}

bool testNewTakeFullyCoversOldEditedSegment()
{
    Counters counters;
    CaptureSession session(countingBindings(counters));
    session.prepareToPlay(48000.0, 512, 1);

    auto state = makeContentState(64, 48000.0, [](int64_t s) { return 0.25f + static_cast<float>(s) * 0.001f; });
    const uint64_t oldId = session.testInjectSegment(std::move(state), SegmentState::Edited,
                                                     0.0, 64.0 / 48000.0, 1, 48000.0);

    runTake(session, 0, 64, 0.9f, 48000.0);

    for (const auto& seg : session.testSegments()) {
        if (seg->contentKey.objectId == oldId)
            return false;  // 旧段必须被移除
    }
    bool sawNew = false;
    for (const auto& seg : session.testSegments()) {
        if (seg->getState() == SegmentState::Processing
            && seg->hostStartSample.load(std::memory_order_acquire) == 0
            && seg->hostSampleCount.load(std::memory_order_acquire) == 64)
            sawNew = true;
    }
    return sawNew;
}

bool testNewTakeCoversOldSegmentHead()
{
    Counters counters;
    CaptureSession session(countingBindings(counters));
    session.prepareToPlay(48000.0, 512, 1);

    // 旧段 [0, 128)，覆盖头部 [0, 64) → 保留尾部 [64, 128)。
    auto note = OpenTune::Note{};
    note.startTime = 0.0;
    note.endTime = 128.0 / 48000.0;
    auto state = makeContentState(128, 48000.0,
                                  [](int64_t s) { return static_cast<float>(s) * 0.001f; },
                                  {note});
    const uint64_t oldId = session.testInjectSegment(std::move(state), SegmentState::Edited,
                                                     0.0, 128.0 / 48000.0, 1, 48000.0);

    runTake(session, 0, 64, 0.9f, 48000.0);

    for (const auto& seg : session.testSegments()) {
        if (seg->contentKey.objectId == oldId)
            return false;
        if (seg->getState() != SegmentState::Edited)
            continue;
        const int64_t start = seg->hostStartSample.load(std::memory_order_acquire);
        const int64_t count = seg->hostSampleCount.load(std::memory_order_acquire);
        if (start != 64 || count != 64)
            continue;
        const auto* audio = publishedAudio(*seg);
        if (audio == nullptr || audio->getNumSamples() != 64)
            return false;
        // 音频样本值：host 样本 64 → 原 buffer offset 64。
        if (std::abs(audio->getSample(0, 0) - 0.064f) > 1e-4f
            || std::abs(audio->getSample(0, 63) - 0.127f) > 1e-4f)
            return false;
        // notes 时间平移到 0 起点。
        for (const auto& n : seg->content->content().notes) {
            if (n.startTime < -1e-6 || n.endTime > 64.0 / 48000.0 + 1e-6)
                return false;
        }
        if (seg->content->content().notes.empty())
            return false;
        return true;
    }
    return false;
}

bool testNewTakeCoversOldSegmentTail()
{
    Counters counters;
    CaptureSession session(countingBindings(counters));
    session.prepareToPlay(48000.0, 512, 1);

    // 旧段 [0, 128)，覆盖尾部 [64, 128) → 保留头部 [0, 64)。
    auto state = makeContentState(128, 48000.0,
                                  [](int64_t s) { return static_cast<float>(s) * 0.001f; });
    const uint64_t oldId = session.testInjectSegment(std::move(state), SegmentState::Edited,
                                                     0.0, 128.0 / 48000.0, 1, 48000.0);

    runTake(session, 64, 64, 0.9f, 48000.0);

    for (const auto& seg : session.testSegments()) {
        if (seg->contentKey.objectId == oldId)
            return false;
        if (seg->getState() != SegmentState::Edited)
            continue;
        const int64_t start = seg->hostStartSample.load(std::memory_order_acquire);
        const int64_t count = seg->hostSampleCount.load(std::memory_order_acquire);
        if (start != 0 || count != 64)
            continue;
        const auto* audio = publishedAudio(*seg);
        if (audio == nullptr || audio->getNumSamples() != 64)
            return false;
        if (std::abs(audio->getSample(0, 0) - 0.0f) > 1e-6f
            || std::abs(audio->getSample(0, 63) - 0.063f) > 1e-4f)
            return false;
        return true;
    }
    return false;
}

bool testNewTakeSplitsOldSegmentInMiddle()
{
    Counters counters;
    CaptureSession session(countingBindings(counters));
    session.prepareToPlay(48000.0, 512, 1);

    // 旧段 [0, 192)，新 take 覆盖中间 [64, 128) → 左 [0,64) + 右 [128,192)。
    auto state = makeContentState(192, 48000.0,
                                  [](int64_t s) { return static_cast<float>(s) * 0.001f; });
    const uint64_t oldId = session.testInjectSegment(std::move(state), SegmentState::Edited,
                                                     0.0, 192.0 / 48000.0, 1, 48000.0);

    runTake(session, 64, 64, 0.9f, 48000.0);

    int pieces = 0;
    for (const auto& seg : session.testSegments()) {
        if (seg->contentKey.objectId == oldId)
            return false;
        if (seg->getState() != SegmentState::Edited)
            continue;
        const int64_t start = seg->hostStartSample.load(std::memory_order_acquire);
        const int64_t count = seg->hostSampleCount.load(std::memory_order_acquire);
        if (start == 0 && count == 64)
            ++pieces;
        else if (start == 128 && count == 64)
            ++pieces;
    }
    return pieces == 2;
}

bool testProcessingWithCurveCallbacksDiscardOldKey()
{
    Counters counters;
    CaptureSession session(countingBindings(counters));
    session.prepareToPlay(48000.0, 512, 1);

    // 旧 Processing 段 [0,128) 带切片曲线 → 覆盖头部后新片段保持 Processing 并 requestFullRender。
    auto curve = std::make_shared<OpenTune::PitchCurve>();
    curve->setHopSize(64);
    curve->setSampleRate(48000.0);
    curve->setOriginalF0(std::vector<float>(128, 220.0f));
    auto state = makeContentState(128, 48000.0,
                                  [](int64_t s) { return static_cast<float>(s) * 0.001f; });
    state.analysis.pitchCurve = curve;
    state.analysis.setOriginalF0State(OpenTune::OriginalF0State::Ready);
    const uint64_t oldId = session.testInjectSegment(std::move(state), SegmentState::Processing,
                                                     0.0, 128.0 / 48000.0, 1, 48000.0);

    runTake(session, 0, 64, 0.9f, 48000.0);

    // 找到保留尾部的 Processing 新片段。
    CaptureSegment* kept = nullptr;
    for (auto& seg : session.testSegments()) {
        if (seg->contentKey.objectId == oldId)
            return false;
        if (seg->getState() == SegmentState::Processing
            && seg->hostStartSample.load(std::memory_order_acquire) == 64)
            kept = seg.get();
    }
    if (kept == nullptr)
        return false;

    // 新 key 正常完成 F0 提交后 tick 推进。
    auto committed = std::make_shared<OpenTune::PitchCurve>();
    committed->setHopSize(64);
    committed->setSampleRate(48000.0);
    committed->setOriginalF0(std::vector<float>(64, 220.0f));
    if (!session.commitSegmentF0Result(kept->contentKey, committed, OpenTune::OriginalF0State::Ready))
        return false;
    session.tick();

    return counters.fullRender > 0 && kept->getState() != SegmentState::Failed;
}

bool testProcessingWithoutCurveRefreshesNewKey()
{
    Counters counters;
    CaptureSession session(countingBindings(counters));
    session.prepareToPlay(48000.0, 512, 1);

    // 旧 Processing 段 [0,128) 无曲线 → 新片段 refreshSegment。
    auto state = makeContentState(128, 48000.0,
                                  [](int64_t s) { return static_cast<float>(s) * 0.001f; });
    state.analysis.setOriginalF0State(OpenTune::OriginalF0State::Extracting);
    const uint64_t oldId = session.testInjectSegment(std::move(state), SegmentState::Processing,
                                                     0.0, 128.0 / 48000.0, 1, 48000.0);

    runTake(session, 0, 64, 0.9f, 48000.0);

    int refreshTargetIds = 0;
    for (auto& seg : session.testSegments()) {
        if (seg->contentKey.objectId == oldId)
            return false;
        if (seg->getState() == SegmentState::Processing
            && counters.publishPerId.count(seg->contentKey.objectId) > 0)
            ++refreshTargetIds;
    }
    return counters.refresh > 0 && refreshTargetIds > 0;
}

bool testTrimmedSegmentSerializeRoundTrip()
{
    Counters counters;
    CaptureSession session(countingBindings(counters));
    session.prepareToPlay(48000.0, 512, 1);

    // 旧段 [0,128)，覆盖头部 [0,64) → 尾片段 [64,128)。
    auto curve = std::make_shared<OpenTune::PitchCurve>();
    curve->setHopSize(1);
    curve->setSampleRate(48000.0);
    curve->setOriginalF0(std::vector<float>(128, 220.0f));
    OpenTune::PitchCorrectionSegment correction;
    correction.startFrame = 0;
    correction.endFrame = 128;
    correction.f0Data.resize(128);
    correction.baseF0Data.resize(128);
    for (int i = 0; i < 128; ++i) {
        correction.f0Data[static_cast<size_t>(i)] = 100.0f + static_cast<float>(i);
        correction.baseF0Data[static_cast<size_t>(i)] = 200.0f + static_cast<float>(i);
    }
    curve->replaceCorrectionSegments({correction});

    auto state = makeContentState(128, 48000.0,
                                  [](int64_t s) { return static_cast<float>(s) * 0.001f; });
    state.analysis.pitchCurve = curve;
    state.analysis.setOriginalF0State(OpenTune::OriginalF0State::Ready);
    session.testInjectSegment(std::move(state), SegmentState::Edited,
                              0.0, 128.0 / 48000.0, 1, 48000.0);
    runTake(session, 0, 64, 0.9f, 48000.0);

    const CaptureSegment* kept = nullptr;
    for (auto& seg : session.testSegments()) {
        if (seg->getState() == SegmentState::Edited
            && seg->hostStartSample.load(std::memory_order_acquire) == 64)
            kept = seg.get();
    }
    if (kept == nullptr)
        return false;

    const auto block = session.serialize();
    CaptureSession restored(countingBindings(counters));
    restored.prepareToPlay(48000.0, 512, 1);
    if (!restored.deserialize(block))
        return false;

    for (auto& seg : restored.testSegments()) {
        if (seg->hostStartSample.load(std::memory_order_acquire) != 64)
            continue;
        const auto* audio = publishedAudio(*seg);
        if (audio == nullptr || audio->getNumSamples() != 64)
            return false;
        if (seg->hostSampleCount.load(std::memory_order_acquire) != 64)
            return false;

        // 切片曲线与 baseF0Data 必须随 v14 归档往返一致。
        const auto& restoredCurve = seg->content->content().analysis.pitchCurve;
        if (restoredCurve == nullptr)
            return false;
        const auto segments = restoredCurve->getSnapshot()->getCorrectionSegments();
        if (segments.size() != 1)
            return false;
        const auto& restoredSegment = segments.front();
        if (restoredSegment.startFrame != 0 || restoredSegment.endFrame != 64)
            return false;
        if (restoredSegment.f0Data.size() != 64 || restoredSegment.baseF0Data.size() != 64)
            return false;
        if (restoredSegment.f0Data.front() != 164.0f
            || restoredSegment.f0Data.back() != 227.0f)
            return false;
        return restoredSegment.baseF0Data.front() == 264.0f
            && restoredSegment.baseF0Data.back() == 327.0f;
    }
    return false;
}

bool testActiveDisplayFollowsTrimmedPiece()
{
    Counters counters;
    CaptureSession session(countingBindings(counters));
    session.prepareToPlay(48000.0, 512, 1);

    // 旧 Edited 段 [0,128) 是 activeDisplay；覆盖尾部后 activeDisplay 指向保留的头部片段。
    auto state = makeContentState(128, 48000.0,
                                  [](int64_t s) { return static_cast<float>(s) * 0.001f; });
    session.testInjectSegment(std::move(state), SegmentState::Edited,
                              0.0, 128.0 / 48000.0, 1, 48000.0);
    runTake(session, 64, 64, 0.9f, 48000.0);

    OpenTune::Capture::SegmentInfo info;
    // 回退分支：hostTimeSeconds 不命中任何 Edited 段（用远未来时刻）。
    return session.resolveDisplaySegment(1e9, info)
        && info.T_start == 0.0;
}

bool testApplyExternalEditCombinesAllOwners()
{
    Counters counters;
    CaptureSession session(countingBindings(counters));
    session.prepareToPlay(48000.0, 512, 1);

    const int64_t numSamples = 128;
    auto state = makeContentState(numSamples, 48000.0,
                                  [](int64_t s) { return static_cast<float>(s) * 0.001f; });
    state.analysis.setOriginalF0State(OpenTune::OriginalF0State::Extracting);
    const uint64_t id = session.testInjectSegment(std::move(state), SegmentState::Edited,
                                                  0.0, static_cast<double>(numSamples) / 48000.0,
                                                  1, 48000.0);
    CaptureSegment* seg = findById(session, id);
    if (seg == nullptr || !seg->content)
        return false;
    const ContentKey key = seg->contentKey;

    // External edit document payload: curve with usable Original F0 + correction segments,
    // notes, pitch-shift settings, volume envelope.
    auto curve = std::make_shared<OpenTune::PitchCurve>();
    curve->setHopSize(1);
    curve->setSampleRate(48000.0);
    curve->setOriginalF0(std::vector<float>(static_cast<size_t>(numSamples), 220.0f));
    OpenTune::PitchCorrectionSegment correction;
    correction.startFrame = 0;
    correction.endFrame = numSamples;
    correction.f0Data.assign(static_cast<size_t>(numSamples), 110.0f);
    correction.baseF0Data.assign(static_cast<size_t>(numSamples), 220.0f);
    curve->replaceCorrectionSegments({correction});

    std::vector<OpenTune::Note> notes;
    OpenTune::Note note;
    note.startTime = 0.0;
    note.endTime = static_cast<double>(numSamples) / 48000.0;
    note.pitch = 69.0f;
    notes.push_back(note);

    OpenTune::AutomationLane envelope;
    envelope = OpenTune::AutomationLane::fromSnapshot({OpenTune::AutomationPoint{0.0, -6.0f}});

    const OpenTune::PitchShiftSettings shiftSettings{2, 50};
    const auto preCurve = seg->content->content().analysis.pitchCurve;
    if (!session.applyExternalEdit(key, notes, curve, envelope, shiftSettings))
        return false;

    const auto& content = seg->content->content();
    // Notes replaced.
    if (content.notes.size() != 1 || std::abs(content.notes.front().pitch - 69.0f) > 1e-5f)
        return false;
    // Curve + Ready Original F0 applied (was Extracting before).
    if (content.analysis.pitchCurve == nullptr || preCurve == content.analysis.pitchCurve)
        return false;
    if (content.analysis.originalF0State != OpenTune::OriginalF0State::Ready)
        return false;
    // Correction segments from the curve are present.
    const auto segments = content.analysis.pitchCurve->getSnapshot()->getCorrectionSegments();
    if (segments.size() != 1 || segments.front().f0Data.front() != 110.0f)
        return false;
    // Pitch-shift settings written.
    if (!(content.pitchShiftSettings == shiftSettings))
        return false;
    // Envelope applied; note outputGainDb derived from the new envelope.
    if (std::abs(content.notes.front().outputGainDb - (-6.0f)) > 1e-4f)
        return false;

    // Invalid key: returns false and leaves existing content untouched.
    ContentKey bogus;
    bogus.domainKind = DomainKind::RegularVST3Capture;
    bogus.objectId = 0xdeadbeef;
    const auto revisionBefore = content.contentRevision;
    const auto notesRevisionBefore = content.notesRevision;
    if (session.applyExternalEdit(bogus, notes, curve, envelope, shiftSettings))
        return false;
    if (seg->content->content().contentRevision != revisionBefore
        || seg->content->content().notesRevision != notesRevisionBefore
        || seg->content->content().notes.size() != 1)
        return false;
    return true;
}

// ── 阶段 3：冻结来源 + 固定几何同版发布回归 ─────────────────────────────

bool testRestoreBuildsReplacementGeometryAtFirstPrepare()
{
    FrozenSourceStore sourceStore;
    CaptureSession source(sourceFrozenBindings(sourceStore));
    source.prepareToPlay(48000.0, 512, 1);
    source.testInjectEditedSegment(0.0, 1.0, 1, makeAudio(48000), 48000.0);
    publishOwnerSource(sourceStore, source, 1);
    source.refreshPublishedReplacementGeometry();
    const auto archive = source.serialize();

    auto makeProbe = [](int& calls, int64_t& readStart, bool& hasSnapshot) {
        return [&calls, &readStart, &hasSnapshot](
                   juce::AudioBuffer<float>&, int, int,
                   const OpenTune::PlaybackReadSource& src,
                   int64_t start, double, double) {
            ++calls;
            readStart = start;
            hasSnapshot = src.contentSnapshot != nullptr;
        };
    };

    // Restore before the first prepare: no rate exists yet, so the first
    // prepare must publish the geometry.
    {
        FrozenSourceStore store;
        int calls = 0;
        int64_t readStart = -1;
        bool hasSnapshot = false;
        ProcessorBindings bindings =
            sourceFrozenBindings(store, makeProbe(calls, readStart, hasSnapshot));
        CaptureSession restored(std::move(bindings));
        if (!restored.deserialize(archive))
            return false;
        juce::AudioBuffer<float> buffer(1, 256);
        buffer.clear();
        restored.processBlock(buffer, 0, 48000.0, true);
        if (calls != 0)
            return false;
        restored.prepareToPlay(48000.0, 512, 1);
        restored.processBlock(buffer, 0, 48000.0, true);
        if (calls == 0 || readStart != 0 || !hasSnapshot)
            return false;
    }

    // Restore after prepare: deserialize publishes sources before the view.
    {
        FrozenSourceStore store;
        int calls = 0;
        int64_t readStart = -1;
        bool hasSnapshot = false;
        ProcessorBindings bindings =
            sourceFrozenBindings(store, makeProbe(calls, readStart, hasSnapshot));
        CaptureSession restored(std::move(bindings));
        restored.prepareToPlay(48000.0, 512, 1);
        if (!restored.deserialize(archive))
            return false;
        juce::AudioBuffer<float> buffer(1, 256);
        buffer.clear();
        restored.processBlock(buffer, 0, 48000.0, true);
        if (calls == 0 || readStart != 0 || !hasSnapshot)
            return false;
    }

    return true;
}

bool testNineSixKSuffixAndRepeatedSliceKeepAbsoluteSourceOrigin()
{
    FrozenSourceStore store;
    int64_t observedReadStart = -1;
    double observedWindowStart = 0.0;
    int64_t observedFirstCanonical = -1;
    CaptureSession session(sourceFrozenBindings(store,
        [&](juce::AudioBuffer<float>&, int, int,
            const OpenTune::PlaybackReadSource& src,
            int64_t start, double, double) {
            observedReadStart = start;
            if (src.contentSnapshot != nullptr) {
                observedWindowStart = src.contentSnapshot->sourceWindow.sourceStartSeconds;
                observedFirstCanonical = src.contentSnapshot->firstCanonicalSample;
            }
        }));
    // runTake feeds a 1024-sample block; declare it as the prepared max block so
    // processBlock's scratch window covers the take.
    session.prepareToPlay(96000.0, 1024, 1);

    // Native 1024-sample suffix at 96k: the canonical origin is 470, but the
    // source window carries the authoritative 1024 / 96000 seconds.
    session.testInjectEditedSegment(0.0, 2048.0 / 96000.0, 1, makeAudio(2048), 96000.0);
    publishOwnerSource(store, session, 1);
    session.refreshPublishedReplacementGeometry();

    runTake(session, 0, 1024, 0.9f, 96000.0);
    juce::AudioBuffer<float> buffer(1, 256);
    buffer.clear();
    observedReadStart = -1;
    session.processBlock(buffer, 1024, 96000.0, true);
    if (observedReadStart != 1024)
        return false;
    if (std::abs(observedWindowStart - 1024.0 / 96000.0) > 1e-15)
        return false;
    if (observedFirstCanonical != OpenTune::PlaybackSampleGeometry::projectToSample(
            1024.0 / 96000.0 * OpenTune::TimeCoordinate::kRenderSampleRate))
        return false;

    // Slice the suffix again: the window advances by absolute seconds, and the
    // canonical origin is projected once from the new absolute start.
    runTake(session, 1024, 512, 0.9f, 96000.0);
    observedReadStart = -1;
    session.processBlock(buffer, 1536, 96000.0, true);
    if (observedReadStart != 1536)
        return false;
    if (std::abs(observedWindowStart - 1536.0 / 96000.0) > 1e-15)
        return false;
    return observedFirstCanonical == 706;
}

bool testFortyFourOnePrefixCanonicalLengthIsExact()
{
    FrozenSourceStore store;
    int64_t observedReadStart = -1;
    CaptureSession session(sourceFrozenBindings(store,
        [&](juce::AudioBuffer<float>&, int, int, const OpenTune::PlaybackReadSource&,
            int64_t start, double, double) {
            observedReadStart = start;
        }));
    session.prepareToPlay(44100.0, 512, 1);
    session.testInjectEditedSegment(0.0, 200.0 / 44100.0, 1, makeAudio(200), 44100.0);
    publishOwnerSource(store, session, 1);
    session.refreshPublishedReplacementGeometry();

    runTake(session, 4, 196, 0.9f, 44100.0);

    for (const auto& seg : session.testSegments()) {
        if (seg->getState() != SegmentState::Edited || !seg->content)
            continue;
        const auto& state = seg->content->content();
        const int64_t canonicalBegin = OpenTune::PlaybackSampleGeometry::projectToSample(
            state.sourceWindow.sourceStartSeconds * OpenTune::TimeCoordinate::kRenderSampleRate);
        const int64_t canonicalEnd = OpenTune::PlaybackSampleGeometry::projectToSample(
            state.sourceWindow.sourceEndSeconds * OpenTune::TimeCoordinate::kRenderSampleRate);
        if (canonicalBegin != 0 || canonicalEnd - canonicalBegin != 4)
            return false;
        if (std::abs(state.sourceWindow.sourceEndSeconds - 4.0 / 44100.0) > 1e-15)
            return false;
        if (state.firstCanonicalSample != 0)
            return false;
        juce::AudioBuffer<float> buffer(1, 64);
        buffer.clear();
        observedReadStart = -1;
        session.processBlock(buffer, 0, 44100.0, true);
        return observedReadStart == 0;
    }
    return false;
}

bool testTimeGridRefreshFreezesSourceWithGeometry()
{
    FrozenSourceStore store;
    int sourceLookups = 0;
    OpenTune::PlaybackReadSource observed;
    int64_t observedReadStart = -1;
    double observedOutputStart = -1.0;
    ProcessorBindings bindings;
    bindings.publishPlaybackSource =
        [&store](const ContentKey& key,
                 const std::shared_ptr<const OpenTune::EditableContentSnapshot>& snapshot,
                 const std::shared_ptr<const juce::AudioBuffer<float>>& audio,
                 double sampleRate) {
            store.publish(key, snapshot, audio, sampleRate);
        };
    bindings.getPlaybackReadSource = [&](ContentKey key, OpenTune::PlaybackReadSource& out) {
        ++sourceLookups;
        return store.get(key, out);
    };
    bindings.replaceWithRendered = [&](juce::AudioBuffer<float>&, int, int,
                                        const OpenTune::PlaybackReadSource& src,
                                        int64_t readStart, double outputStart, double) {
        observed = src;
        observedReadStart = readStart;
        observedOutputStart = outputStart;
    };

    CaptureSession session(std::move(bindings));
    session.prepareToPlay(44100.0, 256, 1);
    session.testInjectEditedSegment(0.0, 1.0, 1, makeAudio(44100), 44100.0);
    auto* segment = findById(session, 1);
    auto state = segment->content->content();
    state.sourceWindow.sourceStartSeconds = 0.25;
    state.sourceWindow.sourceEndSeconds = 1.25;
    segment->content->applyContentState(std::move(state));
    publishOwnerSource(store, session, 1);
    session.refreshPublishedReplacementGeometry();

    std::vector<OpenTune::TimeHandle> handles = {
        {1, 0.0, 0.0, OpenTune::HandleKind::ClipStart, OpenTune::Confidence::Default},
        {2, 0.5, 0.4, OpenTune::HandleKind::OnsetVoiced, OpenTune::Confidence::Default},
        {3, 1.0, 1.0, OpenTune::HandleKind::ClipEnd, OpenTune::Confidence::Default}};
    const auto grid = OpenTune::TimeGridSnapshot::makeFromHandles(handles);
    if (grid == nullptr)
        return false;
    const ContentKey key{DomainKind::RegularVST3Capture, 1, 0};
    if (!session.applyTimeGrid(key, grid))
        return false;

    // An owner edit can precede its CRS publication. Rebuilding a view in that
    // interval must derive geometry from the still-published source snapshot.
    session.refreshPublishedReplacementGeometry();
    juce::AudioBuffer<float> buffer(1, 128);
    buffer.clear();
    session.processBlock(buffer, 0, 44100.0, true);
    if (observed.contentSnapshot == nullptr || observed.contentSnapshot->timeGrid == grid
        || observedReadStart != 11025 || observedOutputStart != 0.0)
        return false;

    // Production republish path: owner truth -> CRS, then rebuild the view once.
    publishOwnerSource(store, session, 1);
    session.refreshPublishedReplacementGeometry();

    const int lookupsBeforeAudio = sourceLookups;
    buffer.clear();
    session.processBlock(buffer, 0, 44100.0, true);
    if (observed.contentSnapshot == nullptr || observed.contentSnapshot->timeGrid != grid
        || observedReadStart != 0 || observedOutputStart != 0.0)
        return false;
    // The audio callback consumes the frozen source; it never re-resolves one.
    return sourceLookups == lookupsBeforeAudio;
}

} // namespace

int main()
{
    int failed = 0;
    if (!testSpanBudgetExhaustionDropsWholeTake()) {
        std::fputs("FAIL: span budget exhaustion drops the whole take\n", stderr);
        ++failed;
    }
    if (!testLoopRewindKeepsLastWritePerHostSample()) {
        std::fputs("FAIL: loop rewind keeps the last write per host sample\n", stderr);
        ++failed;
    }
    if (!testWindowWrapAroundReadWrite()) {
        std::fputs("FAIL: window wrap-around read/write\n", stderr);
        ++failed;
    }
    if (!testWindowOverflowEndsTakeAndKeepsWrittenContent()) {
        std::fputs("FAIL: window overflow ends the take and keeps written content\n", stderr);
        ++failed;
    }
    if (!testDiscontinuityAppendsSpanAndContiguousExtends()) {
        std::fputs("FAIL: contiguous blocks extend a span, jumps append one\n", stderr);
        ++failed;
    }
    if (!testExplicitRecordingIgnoresExistingContent()) {
        std::fputs("FAIL: explicit recording writes over existing Edited content\n", stderr);
        ++failed;
    }
    if (!testReclaimWaitsForPublishedViewReaders()) {
        std::fputs("FAIL: reclaim waits for published-view readers\n", stderr);
        ++failed;
    }
    if (!testEditedStillReplacesPlaybackWhileProcessingDoesNot()) {
        std::fputs("FAIL: Edited replaces playback, Processing does not\n", stderr);
        ++failed;
    }
    if (!testConcurrentArmStopProcessStress()) {
        std::fputs("FAIL: concurrent arm/stop/process stress\n", stderr);
        ++failed;
    }
    if (!testStopRequestSurvivesActiveWriter()) {
        std::fputs("FAIL: stop request survives an active writer\n", stderr);
        ++failed;
    }
    if (!testRetireHappensAfterAudioReadersExitOnce()) {
        std::fputs("FAIL: retire happens once after audio readers exit\n", stderr);
        ++failed;
    }
    if (!testPublishCountTracksStructuralChanges()) {
        std::fputs("FAIL: publish count tracks structural changes\n", stderr);
        ++failed;
    }
    if (!testNewTakeFullyCoversOldEditedSegment()) {
        std::fputs("FAIL: new take fully covers old Edited segment\n", stderr);
        ++failed;
    }
    if (!testNewTakeCoversOldSegmentHead()) {
        std::fputs("FAIL: new take covers old segment head keeps tail\n", stderr);
        ++failed;
    }
    if (!testNewTakeCoversOldSegmentTail()) {
        std::fputs("FAIL: new take covers old segment tail keeps head\n", stderr);
        ++failed;
    }
    if (!testNewTakeSplitsOldSegmentInMiddle()) {
        std::fputs("FAIL: new take splits old segment into two pieces\n", stderr);
        ++failed;
    }
    if (!testProcessingWithCurveCallbacksDiscardOldKey()) {
        std::fputs("FAIL: Processing with curve: full render on new key after trim\n", stderr);
        ++failed;
    }
    if (!testProcessingWithoutCurveRefreshesNewKey()) {
        std::fputs("FAIL: Processing without curve: refreshSegment on new key after trim\n", stderr);
        ++failed;
    }
    if (!testTrimmedSegmentSerializeRoundTrip()) {
        std::fputs("FAIL: trimmed segment serialize/deserialize round trip\n", stderr);
        ++failed;
    }
    if (!testActiveDisplayFollowsTrimmedPiece()) {
        std::fputs("FAIL: activeDisplaySegmentId follows trimmed piece\n", stderr);
        ++failed;
    }
    if (!testApplyExternalEditCombinesAllOwners()) {
        std::fputs("FAIL: applyExternalEdit combines all owners in one commit\n", stderr);
        ++failed;
    }
    if (!testRestoreBuildsReplacementGeometryAtFirstPrepare()) {
        std::fputs("FAIL: restore builds replacement geometry at first prepare\n", stderr);
        ++failed;
    }
    if (!testNineSixKSuffixAndRepeatedSliceKeepAbsoluteSourceOrigin()) {
        std::fputs("FAIL: 96k suffix and repeated slice keep absolute source origin\n", stderr);
        ++failed;
    }
    if (!testFortyFourOnePrefixCanonicalLengthIsExact()) {
        std::fputs("FAIL: 44.1k prefix canonical length is exact\n", stderr);
        ++failed;
    }
    if (!testTimeGridRefreshFreezesSourceWithGeometry()) {
        std::fputs("FAIL: TimeGrid refresh freezes source with geometry\n", stderr);
        ++failed;
    }
    if (failed == 0) {
        std::puts("All CaptureSession explicit-recording tests passed.");
        return 0;
    }
    std::fprintf(stderr, "%d test(s) FAILED.\n", failed);
    return 1;
}
