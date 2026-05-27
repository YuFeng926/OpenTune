/**
 * Tests/TestProjectSessionReference.cpp — ProjectSession reference binding roundtrip + corruption tests
 *
 * Coverage:
 *   - Roundtrip: captureSnapshot → applySnapshot preserves reference bindings
 *   - Corrupt binding: non-existent reference placement id marks session dirty
 *   - Cyclic binding: rejected by setPlacementReferencePlacement, marks session dirty
 *
 * Suite aggregator: runProjectSessionReferenceSuite() — registered in TestMain.cpp.
 */

#include "TestSupport.h"
#include "../Source/Utils/ProjectSession.h"
#include "../Source/Utils/AppPreferences.h"
#include "../Source/Utils/ProjectModel.h"
#include "../Source/StandaloneArrangement.h"
#include "../Source/SourceStore.h"
#include "../Source/MaterializationStore.h"

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>

// ============================================================================
// Helpers
// ============================================================================

namespace {

/** Write a mono float buffer to a temporary WAV file and return the File.
    Caller is responsible for the returned file‘s lifecycle. */
juce::File writeTempWav(const juce::String& tag, const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    auto tempDir = juce::File::createTempFile(".dummy");
    auto parent = tempDir.getParentDirectory();
    tempDir.deleteFile();
    auto wavDir = parent.getChildFile("OpenTuneTestWav_" + tag);
    wavDir.createDirectory();

    auto wavFile = wavDir.getChildFile(tag + ".wav");
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(new juce::FileOutputStream(wavFile),
                                   sampleRate,
                                   static_cast<unsigned int>(buffer.getNumChannels()),
                                   16, {}, 0));
    if (writer) {
        writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
    }
    wavFile.deleteFile(); // schedule cleanup on destruct? actually keep it for the test duration
    return wavFile;
}

/** Create minimal source / materialization / placement data in the processor. */
void seedMinimalArrangement(OpenTuneAudioProcessor& processor,
                            uint64_t matIdA, uint64_t matIdB,
                            uint64_t sourceIdA, uint64_t sourceIdB,
                            uint64_t& outPlacementIdA, uint64_t& outPlacementIdB)
{
    auto* sourceStore = processor.getSourceStore();
    auto* matStore = processor.getMaterializationStore();
    auto* arrangement = processor.getStandaloneArrangement();

    // Source A
    {
        juce::AudioBuffer<float> audio(1, 1024);
        audio.clear();
        audio.setSample(0, 0, 0.25f);
        auto wavFile = writeTempWav("test-source-a", audio, 44100.0);

        auto sharedAudio = std::make_shared<juce::AudioBuffer<float>>(audio);

        SourceStore::CreateSourceRequest req;
        req.displayName = "test-source-a";
        req.sourceFilePath = wavFile.getFullPathName();
        req.audioBuffer = sharedAudio;
        req.sampleRate = 44100.0;
        sourceStore->createSource(req, sourceIdA);
    }

    // Source B
    {
        juce::AudioBuffer<float> audio(1, 1024);
        audio.clear();
        audio.setSample(0, 0, 0.5f);
        auto wavFile = writeTempWav("test-source-b", audio, 44100.0);

        auto sharedAudio = std::make_shared<juce::AudioBuffer<float>>(audio);

        SourceStore::CreateSourceRequest req;
        req.displayName = "test-source-b";
        req.sourceFilePath = wavFile.getFullPathName();
        req.audioBuffer = sharedAudio;
        req.sampleRate = 44100.0;
        sourceStore->createSource(req, sourceIdB);
    }

    // Get source audio buffers for materialization creation
    std::shared_ptr<const juce::AudioBuffer<float>> sourceBufA;
    std::shared_ptr<const juce::AudioBuffer<float>> sourceBufB;
    sourceStore->getAudioBuffer(sourceIdA, sourceBufA);
    sourceStore->getAudioBuffer(sourceIdB, sourceBufB);

    // Materialization A
    {
        MaterializationStore::CreateMaterializationRequest req;
        req.sourceId = sourceIdA;
        req.sourceWindow = SourceWindow{sourceIdA, 0.0, 1024.0 / 44100.0};
        req.audioBuffer = sourceBufA;
        req.pitchCurve = std::make_shared<PitchCurve>();
        matStore->createMaterialization(req, matIdA);
    }

    // Materialization B
    {
        MaterializationStore::CreateMaterializationRequest req;
        req.sourceId = sourceIdB;
        req.sourceWindow = SourceWindow{sourceIdB, 0.0, 1024.0 / 44100.0};
        req.audioBuffer = sourceBufB;
        req.pitchCurve = std::make_shared<PitchCurve>();
        matStore->createMaterialization(req, matIdB);
    }

    // Placement A on track 0
    {
        StandaloneArrangement::Placement p;
        p.materializationId = matIdA;
        p.timelineStartSeconds = 0.0;
        p.durationSeconds = 1.0;
        if (!arrangement->insertPlacement(0, p)) {
            std::cerr << "seedMinimalArrangement: insertPlacement A failed" << std::endl;
            return;
        }
        outPlacementIdA = p.placementId;
    }

    // Placement B on track 0
    {
        StandaloneArrangement::Placement p;
        p.materializationId = matIdB;
        p.timelineStartSeconds = 0.5;
        p.durationSeconds = 1.0;
        if (!arrangement->insertPlacement(0, p)) {
            std::cerr << "seedMinimalArrangement: insertPlacement B failed" << std::endl;
            return;
        }
        outPlacementIdB = p.placementId;
    }
}

/** Create an AppPreferences that writes to a temporary file. */
AppPreferences makeTestAppPreferences()
{
    auto tempDir = juce::File::createTempFile(".xml");
    // createTempFile actually creates a file; we want the parent directory + a filename
    auto parent = tempDir.getParentDirectory();
    tempDir.deleteFile();
    auto settingsDir = parent.getChildFile("OpenTuneTestSession_" + juce::Uuid().toString().substring(0, 8));
    settingsDir.createDirectory();

    AppPreferences::StorageOptions opts;
    opts.applicationName = "OpenTuneTests";
    opts.settingsDirectory = settingsDir;
    opts.fileName = "app-preferences.settings";
    return AppPreferences(opts);
}

} // anonymous namespace

// ============================================================================
// Test 1: Roundtrip preserves binding
// ============================================================================

void runProjectSessionRoundtripPreservesBinding()
{
    constexpr const char* testName = "ProjectSession_RoundtripPreservesBinding";

    OpenTuneAudioProcessor processor;
    auto appPrefs = makeTestAppPreferences();

    // Seed: two placements with a reference binding A→B
    constexpr uint64_t kSourceA = 100;
    constexpr uint64_t kSourceB = 101;
    constexpr uint64_t kMatA = 200;
    constexpr uint64_t kMatB = 201;
    uint64_t placementIdA = 0;
    uint64_t placementIdB = 0;

    seedMinimalArrangement(processor, kMatA, kMatB, kSourceA, kSourceB, placementIdA, placementIdB);

    if (placementIdA == 0 || placementIdB == 0) {
        logFail(testName, "failed to seed minimal arrangement");
        return;
    }

    auto* arrangement = processor.getStandaloneArrangement();
    if (!arrangement->setPlacementReferencePlacement(0, placementIdA, placementIdB)) {
        logFail(testName, "setPlacementReferencePlacement rejected valid binding");
        return;
    }

    // Roundtrip
    ProjectSession session(processor, appPrefs);
    auto snap = session.captureSnapshot();

    if (snap.referenceBindings.empty()) {
        logFail(testName, "captured snapshot has no reference bindings");
        return;
    }

    if (snap.referenceBindings[0].targetPlacementId != placementIdA
        || snap.referenceBindings[0].referencePlacementId != placementIdB) {
        logFail(testName, "captured reference binding IDs do not match");
        return;
    }

    auto applyResult = session.applySnapshot(snap);
    if (!applyResult.ok()) {
        logFail(testName, "applySnapshot failed");
        return;
    }

    const auto restoredRef = arrangement->getPlacementReferencePlacement(0, placementIdA);
    if (restoredRef != placementIdB) {
        logFail(testName, "reference binding not preserved after roundtrip");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 2: Corrupt binding marks dirty
// ============================================================================

void runProjectSessionCorruptBindingMarksDirty()
{
    constexpr const char* testName = "ProjectSession_CorruptBindingMarksDirty";

    OpenTuneAudioProcessor processor;
    auto appPrefs = makeTestAppPreferences();

    // Seed: two placements with a valid binding
    constexpr uint64_t kSourceA = 110;
    constexpr uint64_t kSourceB = 111;
    constexpr uint64_t kMatA = 210;
    constexpr uint64_t kMatB = 211;
    uint64_t placementIdA = 0;
    uint64_t placementIdB = 0;

    seedMinimalArrangement(processor, kMatA, kMatB, kSourceA, kSourceB, placementIdA, placementIdB);

    if (placementIdA == 0 || placementIdB == 0) {
        logFail(testName, "failed to seed minimal arrangement");
        return;
    }

    auto* arrangement = processor.getStandaloneArrangement();
    arrangement->setPlacementReferencePlacement(0, placementIdA, placementIdB);

    // Capture valid snapshot, then corrupt its reference binding
    ProjectSession session(processor, appPrefs);
    auto snap = session.captureSnapshot();

    ProjectReferenceBinding corrupt;
    corrupt.targetPlacementId = placementIdA;
    corrupt.referencePlacementId = 99999;  // non-existent placement
    corrupt.bindingRevision = 1;
    snap.referenceBindings.clear();
    snap.referenceBindings.push_back(corrupt);

    auto applyResult = session.applySnapshot(snap);
    if (!applyResult.ok()) {
        logFail(testName, "applySnapshot failed");
        return;
    }

    if (!session.isDirty()) {
        logFail(testName, "corrupt binding should mark session dirty");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 3: Cyclic binding rejected, marks dirty
// ============================================================================

void runProjectSessionCyclicBindingRejectedMarksDirty()
{
    constexpr const char* testName = "ProjectSession_CyclicBindingRejectedMarksDirty";

    OpenTuneAudioProcessor processor;
    auto appPrefs = makeTestAppPreferences();

    // Seed: two placements
    constexpr uint64_t kSourceA = 120;
    constexpr uint64_t kSourceB = 121;
    constexpr uint64_t kMatA = 220;
    constexpr uint64_t kMatB = 221;
    uint64_t placementIdA = 0;
    uint64_t placementIdB = 0;

    seedMinimalArrangement(processor, kMatA, kMatB, kSourceA, kSourceB, placementIdA, placementIdB);

    if (placementIdA == 0 || placementIdB == 0) {
        logFail(testName, "failed to seed minimal arrangement");
        return;
    }

    // Capture snapshot, then inject cyclic A→B and B→A bindings
    ProjectSession session(processor, appPrefs);
    auto snap = session.captureSnapshot();

    ProjectReferenceBinding aToB;
    aToB.targetPlacementId = placementIdA;
    aToB.referencePlacementId = placementIdB;
    aToB.bindingRevision = 1;

    ProjectReferenceBinding bToA;
    bToA.targetPlacementId = placementIdB;
    bToA.referencePlacementId = placementIdA;
    bToA.bindingRevision = 1;

    snap.referenceBindings.clear();
    snap.referenceBindings.push_back(aToB);
    snap.referenceBindings.push_back(bToA);

    auto applyResult = session.applySnapshot(snap);
    if (!applyResult.ok()) {
        logFail(testName, "applySnapshot failed");
        return;
    }

    // Cyclic binding should have been rejected → session becomes dirty
    if (!session.isDirty()) {
        logFail(testName, "cyclic binding should mark session dirty");
        return;
    }

    // At least one of the cyclic bindings should have failed
    auto* arrangement = processor.getStandaloneArrangement();
    const uint64_t refA = arrangement->getPlacementReferencePlacement(0, placementIdA);
    const uint64_t refB = arrangement->getPlacementReferencePlacement(0, placementIdB);

    // Both A→B and B→A cannot simultaneously exist (cyclic)
    if (refA == placementIdB && refB == placementIdA) {
        logFail(testName, "both cyclic bindings were restored — cycle should have been rejected");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

void runProjectSessionReferenceSuite()
{
    logSection("ProjectSession Reference");
    runProjectSessionRoundtripPreservesBinding();
    runProjectSessionCorruptBindingMarksDirty();
    runProjectSessionCyclicBindingRejectedMarksDirty();
}
