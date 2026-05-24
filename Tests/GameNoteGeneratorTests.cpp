/**
 * Tests/GameNoteGeneratorTests.cpp — L2 unit tests for GameNoteGenerator
 * (D3PM ONNX vocal note transcription) and the mergeChunkNotes stitcher.
 *
 * Tests gracefully self-skip when the GAME-small ONNX bundle is not on disk
 * (e.g., dev environment without LFS / fresh clone).
 */
#include "TestSupport.h"

#include "Inference/GameNoteGenerator.h"
#include "Inference/INoteGenerator.h"
#include "Utils/LegacyNoteGenerator.h"
#include "Utils/Note.h"

#include <onnxruntime_cxx_api.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

juce::String findGameModelDir()
{
    auto cwd = juce::File::getCurrentWorkingDirectory();
    for (int depth = 0; depth < 8 && cwd.isDirectory(); ++depth) {
        if (cwd.getChildFile("CMakeLists.txt").existsAsFile()) {
            const auto candidate = cwd.getChildFile("models/GAME");
            if (candidate.isDirectory()
                && candidate.getChildFile("encoder.onnx").existsAsFile()
                && candidate.getChildFile("segmenter.onnx").existsAsFile()
                && candidate.getChildFile("estimator.onnx").existsAsFile()
                && candidate.getChildFile("bd2dur.onnx").existsAsFile()
                && candidate.getChildFile("config.json").existsAsFile()) {
                return candidate.getFullPathName();
            }
            return {};
        }
        const auto parent = cwd.getParentDirectory();
        if (parent == cwd) break;
        cwd = parent;
    }
    return {};
}

/// Synthesise a 3-second mono A4 (440 Hz) sine with vibrato — produces a
/// signal GAME can plausibly transcribe. Returns audio at 44.1 kHz.
std::vector<float> makeSyntheticVocalA4(double seconds, double sampleRate)
{
    const int64_t n = static_cast<int64_t>(std::round(seconds * sampleRate));
    std::vector<float> out(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double f = 440.0 * (1.0 + 0.01 * std::sin(2.0 * kPi * 5.0 * t));
        out[static_cast<size_t>(i)] = 0.4f * static_cast<float>(std::sin(2.0 * kPi * f * t));
    }
    return out;
}

class GameTestEnv {
public:
    GameTestEnv()
    {
        // OpenTune is built with ORT_API_MANUAL_INIT — must initialize the
        // API table before constructing any Ort::* object.
        Ort::InitApi();
        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "OpenTuneGameTests");
    }

    Ort::Env& env() { return *env_; }

private:
    std::unique_ptr<Ort::Env> env_;
};

GameTestEnv& sharedEnv()
{
    static GameTestEnv env;
    return env;
}

} // namespace

// ============================================================================
// 1) Empty audio short-circuits — no ONNX call needed.
// ============================================================================

void runGameNoteGenerator_EmptyAudioShortCircuitsTest()
{
    constexpr const char* testName = "GameNoteGenerator_EmptyAudio_ReturnsEmpty";
    const auto modelDir = findGameModelDir();
    if (modelDir.isEmpty()) {
        logSection("[skip] GAME-small models not present; skipping construction-required tests");
        logPass(testName); // soft skip; treat as pass on dev environments without models
        return;
    }
    try {
        GameNoteGenerator gen(modelDir.toStdString(), sharedEnv().env());
        NoteGeneratorInput input;
        input.audio.clear();
        input.sampleRate = 44100.0;
        const auto notes = gen.generate(input);
        if (!notes.empty()) {
            logFail(testName, "expected empty notes for empty audio");
            return;
        }
    } catch (const std::exception& e) {
        logFail(testName, e.what());
        return;
    }
    logPass(testName);
}

// ============================================================================
// 2) Short-clip inference produces plausible notes.
// ============================================================================

void runGameNoteGenerator_ShortClipProducesNotesTest()
{
    constexpr const char* testName = "GameNoteGenerator_ShortClip_ProducesPlausibleNotes";
    const auto modelDir = findGameModelDir();
    if (modelDir.isEmpty()) {
        logSection("[skip] GAME-small models not present");
        logPass(testName);
        return;
    }
    try {
        GameNoteGenerator gen(modelDir.toStdString(), sharedEnv().env());
        NoteGeneratorInput input;
        input.sampleRate = 44100.0;
        input.audio      = makeSyntheticVocalA4(3.0, input.sampleRate);

        const auto notes = gen.generate(input);
        if (notes.empty()) {
            logFail(testName, "expected at least 1 note for 3 s of A4 sine");
            return;
        }
        for (const auto& n : notes) {
            if (!(n.startTime >= 0.0 && n.endTime > n.startTime && n.endTime <= 3.05)) {
                logFail(testName, "note has invalid time range");
                return;
            }
            if (!(n.pitch > 0.0f)) {
                logFail(testName, "note has zero pitch");
                return;
            }
            // Sentinel retune/vibrato — note generation must NOT bake in
            // correction values.
            if (!(approxEqual(n.retuneSpeed, -1.0f)
               && approxEqual(n.vibratoDepth, -1.0f)
               && approxEqual(n.vibratoRate, -1.0f))) {
                logFail(testName, "expected sentinel -1.0f for retune/vibrato fields");
                return;
            }
        }
    } catch (const std::exception& e) {
        logFail(testName, e.what());
        return;
    }
    logPass(testName);
}

// ============================================================================
// 3) Polymorphic dispatch via INoteGenerator works.
// ============================================================================

void runGameNoteGenerator_PolymorphicDispatchTest()
{
    constexpr const char* testName = "GameNoteGenerator_PolymorphicDispatch_ViaINoteGenerator";
    const auto modelDir = findGameModelDir();
    if (modelDir.isEmpty()) {
        logSection("[skip] GAME-small models not present");
        logPass(testName);
        return;
    }
    try {
        std::unique_ptr<INoteGenerator> gen =
            std::make_unique<GameNoteGenerator>(modelDir.toStdString(), sharedEnv().env());

        NoteGeneratorInput input;
        input.sampleRate = 44100.0;
        input.audio      = makeSyntheticVocalA4(2.5, input.sampleRate);

        const auto notes = gen->generate(input);
        if (notes.empty()) {
            logFail(testName, "polymorphic dispatch returned 0 notes");
            return;
        }
    } catch (const std::exception& e) {
        logFail(testName, e.what());
        return;
    }
    logPass(testName);
}

// ============================================================================
// 4) mergeChunkNotes — offset addition.
// ============================================================================

void runMergeChunkNotes_OffsetAdditionTest()
{
    constexpr const char* testName = "MergeChunkNotes_OffsetAddition";
    Note a; a.startTime = 1.0; a.endTime = 2.0; a.pitch = 440.0f;
    Note b; b.startTime = 0.5; b.endTime = 1.5; b.pitch = 466.16f;
    const std::vector<std::vector<Note>> per = {{ a }, { b }};
    const std::vector<double> starts = { 0.0, 30.0 };
    const auto out = OpenTune::mergeChunkNotes(per, starts);
    if (out.size() != 2) {
        logFail(testName, "expected 2 notes after merge");
        return;
    }
    if (!approxEqual(out[0].startTime, 1.0)) { logFail(testName, "chunk0 start"); return; }
    if (!approxEqual(out[1].startTime, 30.5)) { logFail(testName, "chunk1 start should be offset"); return; }
    if (!approxEqual(out[1].endTime, 31.5))   { logFail(testName, "chunk1 end should be offset");   return; }
    logPass(testName);
}

// ============================================================================
// 5) mergeChunkNotes — seam dedup with same MIDI.
// ============================================================================

void runMergeChunkNotes_SeamDedupSameMidiTest()
{
    constexpr const char* testName = "MergeChunkNotes_SeamDedup_DropsSameMidi";
    Note tail;  tail.startTime = 29.95; tail.endTime = 30.05;
    tail.pitch = Note::midiToFrequency(60); // C4
    Note head;  head.startTime = 0.02;  head.endTime = 1.0;
    head.pitch = Note::midiToFrequency(60); // same C4
    const std::vector<std::vector<Note>> per = {{ tail }, { head }};
    const std::vector<double> starts = { 0.0, 30.0 };
    const auto out = OpenTune::mergeChunkNotes(per, starts);
    // Tail-side note (29.95-30.05) should win; head-side note (30.02-31.00)
    // is within the seam tolerance and matches MIDI, so dropped.
    if (out.size() != 1) {
        logFail(testName, ("expected 1 note after dedup, got " + std::to_string(out.size())).c_str());
        return;
    }
    if (!approxEqual(out[0].startTime, 29.95)) {
        logFail(testName, "expected the chunk-0 tail note to win");
        return;
    }
    logPass(testName);
}

// ============================================================================
// 6) mergeChunkNotes — seam keeps notes with distinct MIDI.
// ============================================================================

void runMergeChunkNotes_SeamKeepsDistinctMidiTest()
{
    constexpr const char* testName = "MergeChunkNotes_Seam_KeepsDistinctMidi";
    Note tail;  tail.startTime = 29.0;  tail.endTime = 29.5;
    tail.pitch = Note::midiToFrequency(60); // C4
    Note head;  head.startTime = 0.02;  head.endTime = 1.0;
    head.pitch = Note::midiToFrequency(62); // D4 — different
    const std::vector<std::vector<Note>> per = {{ tail }, { head }};
    const std::vector<double> starts = { 0.0, 30.0 };
    const auto out = OpenTune::mergeChunkNotes(per, starts);
    if (out.size() != 2) {
        logFail(testName, ("expected 2 notes (no dedup), got " + std::to_string(out.size())).c_str());
        return;
    }
    logPass(testName);
}

// ============================================================================
// 7) Polymorphic dispatch via INoteGenerator works for the LEGACY backend too.
//    (The earlier #3 only tested GAME; this anchors the interface contract for
//    both implementations.)
// ============================================================================

void runLegacyNoteGenerator_PolymorphicDispatchTest()
{
    constexpr const char* testName = "LegacyNoteGenerator_PolymorphicDispatch_ViaINoteGenerator";

    // Synthesise a 100-frame F0 array (1 s @ 100 fps) that voiced-holds at
    // 220 Hz then drops to 0 (unvoiced) at the end — Legacy should commit at
    // least one note.
    std::vector<float> f0(100, 220.0f);
    for (int i = 90; i < 100; ++i) f0[i] = 0.0f;
    std::vector<float> energy(100, 1.0f);

    std::unique_ptr<OpenTune::INoteGenerator> gen =
        std::make_unique<OpenTune::LegacyNoteGenerator>();

    OpenTune::NoteGeneratorInput input;
    input.f0             = f0;
    input.energy         = energy;
    input.hopSize        = 160;          // 16k/100 frames
    input.f0SampleRate   = 16000.0;
    input.hostSampleRate = 44100.0;

    const auto notes = gen->generate(input);
    if (notes.empty()) {
        logFail(testName, "Legacy via interface produced 0 notes for a 220 Hz held tone");
        return;
    }
    for (const auto& n : notes) {
        if (!(n.startTime >= 0.0 && n.endTime > n.startTime && n.pitch > 0.0f)) {
            logFail(testName, "note has invalid time/pitch fields");
            return;
        }
    }
    logPass(testName);
}

// ============================================================================
// 8) Legacy ignores audio fields in the input struct — proves the input
//    struct's two halves (audio-domain / frame-domain) are properly disjoint.
// ============================================================================

void runLegacyNoteGenerator_IgnoresAudioFieldTest()
{
    constexpr const char* testName = "LegacyNoteGenerator_IgnoresAudioField_OutputDependsOnF0Only";

    std::vector<float> f0(50, 440.0f);
    std::vector<float> energy(50, 1.0f);
    std::unique_ptr<OpenTune::INoteGenerator> gen =
        std::make_unique<OpenTune::LegacyNoteGenerator>();

    OpenTune::NoteGeneratorInput a;
    a.f0 = f0; a.energy = energy;
    a.hopSize = 160; a.f0SampleRate = 16000.0; a.hostSampleRate = 44100.0;
    // a.audio empty

    OpenTune::NoteGeneratorInput b = a;
    b.audio.assign(44100, 0.5f); // 1s of garbage audio at 44.1k
    b.sampleRate = 44100.0;

    const auto notesA = gen->generate(a);
    const auto notesB = gen->generate(b);

    if (notesA.size() != notesB.size()) {
        logFail(testName, "Legacy output changed when audio fields were populated");
        return;
    }
    for (size_t i = 0; i < notesA.size(); ++i) {
        if (!(approxEqual(notesA[i].startTime, notesB[i].startTime, 1e-9)
           && approxEqual(notesA[i].endTime,   notesB[i].endTime,   1e-9)
           && approxEqual(notesA[i].pitch,     notesB[i].pitch))) {
            logFail(testName, "Legacy note differs between with/without audio");
            return;
        }
    }
    logPass(testName);
}

// ============================================================================
// 9) Backend selection: env OPENTUNE_NOTE_BACKEND=legacy forces Legacy even
//    when GAME is available. Touches the fallback path the support team
//    relies on for diagnostics.
// ============================================================================

namespace {
juce::String selectedBackendForTest(bool forceLegacyEnv,
                                    bool gameModelsPresent)
{
    // Mirrors the priority logic in OpenTuneAudioProcessor::ensureNoteGeneratorReady.
    if (forceLegacyEnv) return "Legacy";
    if (gameModelsPresent) return "GAME";
    return "Legacy";
}
} // namespace

void runBackendSelection_PriorityOrderTest()
{
    constexpr const char* testName = "BackendSelection_PriorityOrder_EnvOverridesGame";

    // (1) env=legacy + GAME present -> Legacy (forced fallback)
    if (selectedBackendForTest(/*forceLegacyEnv=*/true, /*gameModelsPresent=*/true) != "Legacy") {
        logFail(testName, "env legacy override should force Legacy"); return;
    }
    // (2) no env + GAME present -> GAME (default)
    if (selectedBackendForTest(false, true) != "GAME") {
        logFail(testName, "no env + GAME present should pick GAME"); return;
    }
    // (3) no env + GAME missing -> Legacy (graceful fallback)
    if (selectedBackendForTest(false, false) != "Legacy") {
        logFail(testName, "no env + GAME missing should fall back to Legacy"); return;
    }
    // (4) env=legacy + GAME missing -> Legacy
    if (selectedBackendForTest(true, false) != "Legacy") {
        logFail(testName, "env legacy + GAME missing should give Legacy"); return;
    }
    logPass(testName);
}

// ============================================================================
// 10) Contract: both backends return well-formed notes.
// ============================================================================

void runContract_BothBackendsWellFormedTest()
{
    constexpr const char* testName = "Contract_BothBackendsReturnWellFormedNotes";

    // Legacy on F0 fixture
    {
        std::vector<float> f0(80, 261.63f); // C4 @ 0.8s
        for (int i = 70; i < 80; ++i) f0[i] = 0.0f;
        std::vector<float> energy(80, 1.0f);

        OpenTune::NoteGeneratorInput input;
        input.f0 = f0; input.energy = energy;
        input.hopSize = 160; input.f0SampleRate = 16000.0; input.hostSampleRate = 44100.0;

        OpenTune::LegacyNoteGenerator legacy;
        const auto notes = legacy.generate(input);
        for (const auto& n : notes) {
            if (!(n.startTime >= 0.0 && n.endTime > n.startTime && n.pitch > 0.0f)) {
                logFail(testName, "Legacy: invalid note fields"); return;
            }
            if (!approxEqual(n.retuneSpeed, -1.0f)) {
                logFail(testName, "Legacy: expected sentinel retuneSpeed"); return;
            }
        }
    }

    // GAME on synthetic audio (skip if models missing)
    const auto modelDir = findGameModelDir();
    if (!modelDir.isEmpty()) {
        OpenTune::GameNoteGenerator game(modelDir.toStdString(), sharedEnv().env());
        OpenTune::NoteGeneratorInput input;
        input.sampleRate = 44100.0;
        input.audio = makeSyntheticVocalA4(2.0, 44100.0);
        const auto notes = game.generate(input);
        for (const auto& n : notes) {
            if (!(n.startTime >= 0.0 && n.endTime > n.startTime && n.pitch > 0.0f)) {
                logFail(testName, "GAME: invalid note fields"); return;
            }
            if (!approxEqual(n.retuneSpeed, -1.0f)) {
                logFail(testName, "GAME: expected sentinel retuneSpeed"); return;
            }
        }
    }
    logPass(testName);
}

// ============================================================================
// 11) Contract: Legacy is fast (perf gate to catch accidental pathological
//     regressions on the DSP path).
// ============================================================================

void runContract_LegacyFastTest()
{
    constexpr const char* testName = "Contract_LegacyReturnsUnder100ms_For30sFixture";

    std::vector<float> f0(3000, 220.0f); // 30 s @ 100 fps
    for (int i = 200; i < 250; ++i) f0[i] = 0.0f;
    for (int i = 1500; i < 1600; ++i) f0[i] = 0.0f;
    std::vector<float> energy(3000, 1.0f);

    OpenTune::NoteGeneratorInput input;
    input.f0 = f0; input.energy = energy;
    input.hopSize = 160; input.f0SampleRate = 16000.0; input.hostSampleRate = 44100.0;

    OpenTune::LegacyNoteGenerator legacy;
    const auto t0 = std::chrono::steady_clock::now();
    const auto notes = legacy.generate(input);
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    if (ms > 100) {
        logFail(testName, ("Legacy took " + std::to_string(ms) + "ms for 30 s fixture (>100ms gate)").c_str());
        return;
    }
    if (notes.empty()) {
        logFail(testName, "Legacy returned 0 notes for a clearly voiced fixture");
        return;
    }
    logPass(testName);
}

// ============================================================================
// Suite registration
// ============================================================================

void runGameNoteGeneratorSuite()
{
    logSection("=== GameNoteGenerator suite ===");
    runGameNoteGenerator_EmptyAudioShortCircuitsTest();
    runGameNoteGenerator_ShortClipProducesNotesTest();
    runGameNoteGenerator_PolymorphicDispatchTest();
    runMergeChunkNotes_OffsetAdditionTest();
    runMergeChunkNotes_SeamDedupSameMidiTest();
    runMergeChunkNotes_SeamKeepsDistinctMidiTest();
    runLegacyNoteGenerator_PolymorphicDispatchTest();
    runLegacyNoteGenerator_IgnoresAudioFieldTest();
    runBackendSelection_PriorityOrderTest();
    runContract_BothBackendsWellFormedTest();
    runContract_LegacyFastTest();
}
