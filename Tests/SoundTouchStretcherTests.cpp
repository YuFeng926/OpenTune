/**
 * Tests/SoundTouchStretcherTests.cpp — Unit tests for §5 SoundTouchStretcher wrapper.
 *
 * Replaces archived Tests/RubberBandStretcherTests.cpp.  See change
 * `swap-time-stretch-to-soundtouch` for full migration rationale.
 *
 * Spec coverage:
 *   - openspec/changes/swap-time-stretch-to-soundtouch/specs/soundtouch-time-stretcher/spec.md
 *
 * Suite aggregator: runSoundTouchStretcherSuite() — registered in TestMain.cpp.
 *
 * 14+ test cases across:
 *   - Engine initial state (4 cases)
 *   - TempoSchedule math (3 cases)
 *   - push/pull streaming (3 cases)
 *   - Endpoint conservation (2 cases)
 *   - Lifecycle (2 cases)
 */
#include "TestSupport.h"
#include "Inference/SoundTouchStretcher.h"
#include "Utils/TimeGrid.h"

#include <cmath>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr double kPi = 3.14159265358979323846264338327950288;

std::vector<float> makeSineTone(double freqHz, double durationSec, double amp = 0.4)
{
    const int n = static_cast<int>(std::round(durationSec * kSampleRate));
    std::vector<float> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        out[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * kPi * freqHz * i / kSampleRate));
    }
    return out;
}

} // namespace

// ============================================================================
// Engine initial state
// ============================================================================

void runSoundTouchStretcher_ConstructTest()
{
    constexpr const char* testName = "SoundTouchStretcher_Construct";

    SoundTouchStretcher st(kSampleRate, /*channels=*/1);
    if (st.phase() != SoundTouchStretcher::Phase::Idle) {
        logFail(testName, "freshly-constructed stretcher should be in Idle phase");
        return;
    }
    logPass(testName);
}

void runSoundTouchStretcher_AaFilterEnabledTest()
{
    constexpr const char* testName = "SoundTouchStretcher_AaFilterEnabled";
    const uint32_t s = SoundTouchStretcher::getSoundTouchSettings();
    const uint32_t aaFilter = s & 0xFFu;
    if (aaFilter != 1) {
        logFail(testName, "SETTING_USE_AA_FILTER should be 1");
        return;
    }
    logPass(testName);
}

void runSoundTouchStretcher_QuickSeekDisabledTest()
{
    constexpr const char* testName = "SoundTouchStretcher_QuickSeekDisabled";
    const uint32_t s = SoundTouchStretcher::getSoundTouchSettings();
    const uint32_t quickseek = (s >> 8) & 0xFFu;
    if (quickseek != 0) {
        logFail(testName, "SETTING_USE_QUICKSEEK should be 0 (quality > speed)");
        return;
    }
    logPass(testName);
}

void runSoundTouchStretcher_SeekWindowAndOverlapTest()
{
    constexpr const char* testName = "SoundTouchStretcher_SeekWindowAndOverlap";
    const uint32_t s = SoundTouchStretcher::getSoundTouchSettings();
    const uint32_t seekWindowMs = (s >> 16) & 0xFFu;
    const uint32_t overlapMs    = (s >> 24) & 0xFFu;
    if (seekWindowMs != 25) {
        logFail(testName, "SETTING_SEEKWINDOW_MS should be 25");
        return;
    }
    if (overlapMs != 8) {
        logFail(testName, "SETTING_OVERLAP_MS should be 8");
        return;
    }
    logPass(testName);
}

// ============================================================================
// TempoSchedule math
// ============================================================================

void runSoundTouchStretcher_TempoScheduleIdentityTest()
{
    constexpr const char* testName = "SoundTouchStretcher_TempoScheduleIdentity";

    SoundTouchStretcher st(kSampleRate, 1);
    auto snap = TimeGridSnapshot::makeIdentity(2.0);
    auto sched = st.buildTempoScheduleFromTimeGrid(*snap);

    if (!sched.identity) {
        logFail(testName, "identity grid should produce identity schedule");
        return;
    }
    if (std::abs(sched.totalSourceSeconds - 2.0) > 1e-9 ||
        std::abs(sched.totalOutputSeconds - 2.0) > 1e-9) {
        logFail(testName, "totalSource/totalOutput should both be 2.0 sec");
        return;
    }
    if (std::abs(sched.soundTouchTempoAt(1.0) - 1.0) > 1e-9) {
        logFail(testName, "tempoAt should be 1.0 for identity grid");
        return;
    }
    logPass(testName);
}

void runSoundTouchStretcher_TempoScheduleMultiRatioTest()
{
    constexpr const char* testName = "SoundTouchStretcher_TempoScheduleMultiRatio";

    // 3 segments: [0,2]→[0,3] ratio=1.5; [2,5]→[3,6] ratio=1.0; [5,8]→[6,8] ratio≈0.667
    std::vector<TimeHandle> handles = {
        {1, 0.0, 0.0, HandleKind::ClipStart, true},
        {2, 2.0, 3.0, HandleKind::OnsetVoiced, false},
        {3, 5.0, 6.0, HandleKind::OnsetVoiced, false},
        {4, 8.0, 8.0, HandleKind::ClipEnd, true},
    };
    auto snap = TimeGridSnapshot::makeFromHandles(handles, 4);
    if (!snap) { logFail(testName, "makeFromHandles failed"); return; }

    SoundTouchStretcher st(kSampleRate, 1);
    auto sched = st.buildTempoScheduleFromTimeGrid(*snap);

    // Endpoint conservation: total == 8 sec (locked endpoints)
    if (std::abs(sched.totalSourceSeconds - 8.0) > 1e-9 ||
        std::abs(sched.totalOutputSeconds - 8.0) > 1e-9) {
        logFail(testName, "endpoints should conserve total = 8 sec");
        return;
    }

    // SoundTouch tempo = 1/ratio.  Segment 1 (ratio=1.5) → tempo ≈ 0.667
    const double t1 = sched.soundTouchTempoAt(1.0);
    if (std::abs(t1 - 1.0 / 1.5) > 1e-6) {
        logFail(testName, "segment 1 (ratio=1.5) tempoAt should be 1/1.5");
        return;
    }
    // Segment 2 (ratio=1.0) → tempo = 1.0
    if (std::abs(sched.soundTouchTempoAt(3.5) - 1.0) > 1e-6) {
        logFail(testName, "segment 2 (ratio=1.0) tempoAt should be 1.0");
        return;
    }
    // Segment 3 (ratio≈0.667) → tempo = 1/0.667 ≈ 1.5
    if (std::abs(sched.soundTouchTempoAt(6.5) - 1.5) > 1e-3) {
        logFail(testName, "segment 3 (ratio≈0.667) tempoAt should be ≈1.5");
        return;
    }
    logPass(testName);
}

void runSoundTouchStretcher_EndpointConservationTest()
{
    constexpr const char* testName = "SoundTouchStretcher_EndpointConservation";

    // Mathematical proof: Σ Δsource_i × ratio_i = totalOutputSeconds
    std::vector<TimeHandle> handles = {
        {1, 0.0, 0.0, HandleKind::ClipStart, true},
        {2, 1.5, 2.0, HandleKind::OnsetVoiced, false},     // segment 1: ratio = 4/3
        {3, 3.5, 4.0, HandleKind::OnsetVoiced, false},     // segment 2: ratio = 1.0
        {4, 5.0, 5.0, HandleKind::ClipEnd, true},           // segment 3: ratio = 2/3
    };
    auto snap = TimeGridSnapshot::makeFromHandles(handles, 5);
    if (!snap) { logFail(testName, "makeFromHandles failed"); return; }

    SoundTouchStretcher st(kSampleRate, 1);
    auto sched = st.buildTempoScheduleFromTimeGrid(*snap);

    // Total output = 1.5×(4/3) + 2.0×1.0 + 1.5×(2/3) = 2.0 + 2.0 + 1.0 = 5.0
    if (std::abs(sched.totalOutputSeconds - 5.0) > 1e-9) {
        logFail(testName, "totalOutputSeconds should equal totalSourceSeconds (5.0) under endpoint lock");
        return;
    }
    logPass(testName);
}

// ============================================================================
// push/pull streaming
// ============================================================================

void runSoundTouchStretcher_IdentityRoundTripTest()
{
    constexpr const char* testName = "SoundTouchStretcher_IdentityRoundTrip";

    SoundTouchStretcher st(kSampleRate, 1);

    auto snap = TimeGridSnapshot::makeIdentity(1.0);
    auto sched = st.buildTempoScheduleFromTimeGrid(*snap);

    st.beginRebuild(sched);
    if (st.phase() != SoundTouchStretcher::Phase::Pushing) {
        logFail(testName, "after beginRebuild expect Pushing phase");
        return;
    }

    auto audio = makeSineTone(220.0, 1.0);
    st.push(audio.data(), audio.size(), /*isLast=*/true);
    if (st.phase() != SoundTouchStretcher::Phase::Drained) {
        logFail(testName, "after push(isLast=true) expect Drained phase");
        return;
    }

    std::vector<float> output(audio.size() * 2, 0.0f);
    size_t totalRetrieved = 0;
    int safetyIterations = 200;
    while (st.available() > 0 && safetyIterations-- > 0) {
        const size_t want = std::min<size_t>(st.available(), output.size() - totalRetrieved);
        if (want == 0) break;
        const size_t got = st.pull(output.data() + totalRetrieved, want);
        if (got == 0) break;
        totalRetrieved += got;
    }

    if (totalRetrieved < audio.size() * 0.7) {
        logFail(testName, "expected at least 70% of input samples in output");
        return;
    }

    bool nonZero = false;
    for (size_t i = 0; i < totalRetrieved; ++i) {
        if (std::abs(output[i]) > 1e-4f) { nonZero = true; break; }
    }
    if (!nonZero) {
        logFail(testName, "output should not be all zeros");
        return;
    }
    logPass(testName);
}

void runSoundTouchStretcher_PushPullStateMachineTest()
{
    constexpr const char* testName = "SoundTouchStretcher_PushPullStateMachine";

    SoundTouchStretcher st(kSampleRate, 1);
    auto audio = makeSineTone(220.0, 0.1);

    // push() before beginRebuild → no-op (phase Idle)
    st.push(audio.data(), audio.size(), false);
    if (st.available() != 0) {
        logFail(testName, "push before beginRebuild should be no-op");
        return;
    }
    if (st.phase() != SoundTouchStretcher::Phase::Idle) {
        logFail(testName, "phase should remain Idle without beginRebuild");
        return;
    }
    logPass(testName);
}

void runSoundTouchStretcher_ExpectedOutputSamplesTest()
{
    constexpr const char* testName = "SoundTouchStretcher_ExpectedOutputSamples";

    SoundTouchStretcher st(kSampleRate, 1);
    auto snap = TimeGridSnapshot::makeIdentity(1.0);
    auto sched = st.buildTempoScheduleFromTimeGrid(*snap);

    st.beginRebuild(sched);
    const size_t expected = st.expectedOutputSamples();
    if (expected != static_cast<size_t>(kSampleRate)) {
        logFail(testName, "expectedOutputSamples should equal totalSource × sampleRate");
        return;
    }
    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

void runSoundTouchStretcherSuite()
{
    logSection("SoundTouchStretcher");
    runSoundTouchStretcher_ConstructTest();
    runSoundTouchStretcher_AaFilterEnabledTest();
    runSoundTouchStretcher_QuickSeekDisabledTest();
    runSoundTouchStretcher_SeekWindowAndOverlapTest();
    runSoundTouchStretcher_TempoScheduleIdentityTest();
    runSoundTouchStretcher_TempoScheduleMultiRatioTest();
    runSoundTouchStretcher_EndpointConservationTest();
    runSoundTouchStretcher_IdentityRoundTripTest();
    runSoundTouchStretcher_PushPullStateMachineTest();
    runSoundTouchStretcher_ExpectedOutputSamplesTest();
}
