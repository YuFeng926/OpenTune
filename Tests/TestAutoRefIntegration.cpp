/**
 * Tests/TestAutoRefIntegration.cpp - Reference alignment domain applier tests.
 *
 * Covers:
 *   - ReferenceBinding -> ReferenceFeatureSet -> ReferenceAutoAlign request/patch
 *   - Processor/domain transactional apply into notes, corrected segments, TimeGrid
 *   - One composite undo/redo restoring pitch and time edits together
 *   - Processor-owned preheat / TimeTool seed feature paths
 */

#include "TestSupport.h"
#include "Utils/TimeCoordinate.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace {

constexpr double kDurationSeconds = 2.0;
constexpr int kF0Frames = 200;

OpenTuneAudioProcessor::PreparedImport makeAutoRefPreparedImport(const char* name)
{
    OpenTuneAudioProcessor::PreparedImport prepared;
    prepared.displayName = name;
    prepared.sourceFilePath = {};
    const int numSamples = static_cast<int>(OpenTune::TimeCoordinate::secondsToSamples(
        kDurationSeconds, OpenTune::TimeCoordinate::kRenderSampleRate));
    prepared.storedAudioBuffer.setSize(1, numSamples);
    prepared.storedAudioBuffer.clear();
    return prepared;
}

OpenTuneAudioProcessor::CommittedPlacement addPlacement(OpenTuneAudioProcessor& processor,
                                                        const char* name,
                                                        double timelineStartSeconds)
{
    OpenTuneAudioProcessor::ImportPlacement placement;
    placement.trackId = 0;
    placement.timelineStartSeconds = timelineStartSeconds;
    return processor.commitPreparedImportAsPlacement(makeAutoRefPreparedImport(name), placement);
}

Note makeNote(double startSeconds, double endSeconds, float pitchHz)
{
    Note note;
    note.startTime = startSeconds;
    note.endTime = endSeconds;
    note.pitch = pitchHz;
    note.originalPitch = pitchHz;
    note.isVoiced = true;
    return note;
}

std::vector<float> makeTwoPhraseF0(float firstPitchHz, float secondPitchHz)
{
    std::vector<float> f0(kF0Frames, 0.0f);
    std::fill(f0.begin() + 20, f0.begin() + 70, firstPitchHz);
    std::fill(f0.begin() + 100, f0.begin() + 150, secondPitchHz);
    return f0;
}

std::shared_ptr<PitchCurve> makeCurve(std::vector<float> f0)
{
    auto curve = std::make_shared<PitchCurve>();
    curve->setHopSize(160);
    curve->setSampleRate(16000.0);
    curve->setOriginalF0(f0);
    curve->setOriginalEnergy(std::vector<float>(f0.size(), 1.0f));
    return curve;
}

bool seedMaterializationFeatures(OpenTuneAudioProcessor& processor,
                                 uint64_t materializationId,
                                 const std::vector<float>& f0,
                                 const std::vector<Note>& notes)
{
    return processor.setMaterializationPitchCurveById(materializationId, makeCurve(f0))
        && processor.setMaterializationNotesById(materializationId, notes);
}

ReferenceFeatureSet makeReadyReferenceFeatures(const std::vector<Note>& notes,
                                               std::initializer_list<double> anchorTimesSeconds,
                                               int64_t inputFingerprint,
                                               double sourceDurationSeconds,
                                               ReferenceFeatureProducer producer = ReferenceFeatureProducer::Game)
{
    ReferenceFeatureSet features;
    features.status = ReferenceFeatureStatus::Ready;
    features.producer = producer;
    features.analysisRevision = 1;
    features.inputFingerprint = inputFingerprint;
    features.sourceDurationSeconds = sourceDurationSeconds;
    features.pitch.notes = notes;

    uint64_t nextAnchorId = 1;
    for (double anchorTime : anchorTimesSeconds) {
        ReferenceTimingAnchor anchor;
        anchor.anchorId = nextAnchorId++;
        anchor.sourceSeconds = anchorTime;
        anchor.strength = 1.0f;
        anchor.kind = ReferenceTimingAnchorKind::Onset;
        anchor.confidence = 1.0f;
        features.timing.anchors.push_back(anchor);
    }

    return features;
}

bool seedReadyReferenceFeatures(OpenTuneAudioProcessor& processor,
                                uint64_t materializationId,
                                const std::vector<Note>& notes,
                                std::initializer_list<double> anchorTimesSeconds,
                                ReferenceFeatureProducer producer = ReferenceFeatureProducer::Game)
{
    auto* store = processor.getMaterializationStore();
    if (store == nullptr) {
        return false;
    }

    MaterializationStore::MaterializationSnapshot snapshot;
    if (!store->getSnapshot(materializationId, snapshot)) {
        return false;
    }

    const double durationSeconds = snapshot.audioBuffer != nullptr
        ? TimeCoordinate::samplesToSeconds(snapshot.audioBuffer->getNumSamples(),
                                           TimeCoordinate::kRenderSampleRate)
        : 0.0;
    auto features = makeReadyReferenceFeatures(notes,
                                               anchorTimesSeconds,
                                               static_cast<int64_t>(snapshot.renderRevision),
                                               durationSeconds,
                                               producer);
    return store->setReferenceFeatures(materializationId, features);
}

bool isExpectedGamePreheatUnavailable(const juce::String& errorMessage)
{
    return errorMessage == "AUTO Ref GAME analysis requires GAME backend and cannot fall back to Basic"
        || errorMessage == "AUTO Ref GAME analysis requires GAME note generator";
}

std::shared_ptr<const TimeGridSnapshot> makeWarpedReferenceGrid()
{
    std::vector<TimeHandle> handles;
    handles.push_back({ 1, 0.0, 0.0, HandleKind::ClipStart, true, Confidence::Default });
    handles.push_back({ 2, 1.0, 1.2, HandleKind::UserAdded, false, Confidence::Default });
    handles.push_back({ 3, kDurationSeconds, kDurationSeconds, HandleKind::ClipEnd, true, Confidence::Default });
    return TimeGridSnapshot::makeFromHandles(std::move(handles), 1);
}

bool hasReferenceAutoHandle(const std::shared_ptr<const TimeGridSnapshot>& grid)
{
    if (grid == nullptr) {
        return false;
    }

    const auto& handles = grid->handles();
    return std::any_of(handles.begin(), handles.end(), [](const TimeHandle& handle) {
        return handle.kind == HandleKind::ReferenceAuto;
    });
}

bool hasIdentityInternalOnsetHandle(const std::shared_ptr<const TimeGridSnapshot>& grid)
{
    if (grid == nullptr) {
        return false;
    }

    return std::any_of(grid->handles().begin(), grid->handles().end(), [](const TimeHandle& handle) {
        return handle.kind == HandleKind::InternalOnset
            && !handle.locked
            && approxEqual(static_cast<float>(handle.source_seconds),
                           static_cast<float>(handle.output_seconds),
                           1.0e-6f);
    });
}

bool readTargetState(OpenTuneAudioProcessor& processor,
                     uint64_t materializationId,
                     std::vector<Note>& notes,
                     std::vector<CorrectedSegment>& segments,
                     std::shared_ptr<const TimeGridSnapshot>& timeGrid)
{
    auto* store = processor.getMaterializationStore();
    if (store == nullptr) {
        return false;
    }

    notes = store->getNotes(materializationId);
    std::shared_ptr<PitchCurve> curve;
    if (!store->getPitchCurve(materializationId, curve) || curve == nullptr) {
        return false;
    }

    segments = curve->getSnapshot()->getCorrectedSegments();
    return store->getTimeGrid(materializationId, timeGrid) && timeGrid != nullptr;
}

bool readReferenceFeatures(OpenTuneAudioProcessor& processor,
                           uint64_t materializationId,
                           ReferenceFeatureSet& out)
{
    auto* store = processor.getMaterializationStore();
    return store != nullptr && store->getReferenceFeatures(materializationId, out);
}

bool approxNotePitch(const std::vector<Note>& notes, size_t index, float expectedPitchHz)
{
    return notes.size() > index && approxEqual(notes[index].pitch, expectedPitchHz, 1.0e-3f);
}

bool seedBoundReferencePair(OpenTuneAudioProcessor& processor,
                            OpenTuneAudioProcessor::CommittedPlacement& target,
                            OpenTuneAudioProcessor::CommittedPlacement& reference)
{
    target = addPlacement(processor, "AUTO Ref Target", 0.0);
    reference = addPlacement(processor, "AUTO Ref Reference", 0.0);
    if (!target.isValid() || !reference.isValid()) {
        return false;
    }

    const std::vector<Note> targetNotes = {
        makeNote(0.20, 0.70, 220.0f),
        makeNote(1.00, 1.50, 220.0f)
    };
    const std::vector<Note> referenceNotes = {
        makeNote(0.20, 0.70, 330.0f),
        makeNote(1.00, 1.50, 440.0f)
    };

    if (!seedMaterializationFeatures(processor,
                                     target.materializationId,
                                     makeTwoPhraseF0(220.0f, 220.0f),
                                     targetNotes)) {
        return false;
    }
    if (!seedMaterializationFeatures(processor,
                                     reference.materializationId,
                                     makeTwoPhraseF0(330.0f, 440.0f),
                                     referenceNotes)) {
        return false;
    }

    if (!seedReadyReferenceFeatures(processor,
                                    target.materializationId,
                                    targetNotes,
                                    { 0.20, 1.00 },
                                    ReferenceFeatureProducer::Game)) {
        return false;
    }
    if (!seedReadyReferenceFeatures(processor,
                                    reference.materializationId,
                                    referenceNotes,
                                    { 0.20, 1.00 },
                                    ReferenceFeatureProducer::Game)) {
        return false;
    }

    auto warpedGrid = makeWarpedReferenceGrid();
    if (warpedGrid == nullptr
        || !processor.setMaterializationTimeGridById(reference.materializationId,
                                                     warpedGrid,
                                                     0,
                                                     static_cast<int64_t>(kF0Frames))) {
        return false;
    }

    auto* arrangement = processor.getStandaloneArrangement();
    return arrangement != nullptr
        && arrangement->setPlacementReferencePlacement(0, target.placementId, reference.placementId);
}

void runAutoRefIntegrationTransactionalApplyAndCompositeUndoTest()
{
    constexpr const char* testName = "AutoRefIntegration_TransactionalApplyAndCompositeUndo";

    OpenTuneAudioProcessor processor;
    OpenTuneAudioProcessor::CommittedPlacement target;
    OpenTuneAudioProcessor::CommittedPlacement reference;
    if (!seedBoundReferencePair(processor, target, reference)) {
        logFail(testName, "failed to seed bound reference pair");
        return;
    }

    std::vector<Note> notesBefore;
    std::vector<CorrectedSegment> segmentsBefore;
    std::shared_ptr<const TimeGridSnapshot> gridBefore;
    if (!readTargetState(processor, target.materializationId, notesBefore, segmentsBefore, gridBefore)) {
        logFail(testName, "failed to read initial target state");
        return;
    }
    if (!segmentsBefore.empty() || hasReferenceAutoHandle(gridBefore)) {
        logFail(testName, "target should start without AUTO Ref edits");
        return;
    }

    const auto result = processor.executeReferenceAlignmentForPlacement(target.placementId);
    if (!result.succeeded()) {
        logFail(testName, result.message.toRawUTF8());
        return;
    }

    std::vector<Note> notesAfter;
    std::vector<CorrectedSegment> segmentsAfter;
    std::shared_ptr<const TimeGridSnapshot> gridAfter;
    if (!readTargetState(processor, target.materializationId, notesAfter, segmentsAfter, gridAfter)) {
        logFail(testName, "failed to read aligned target state");
        return;
    }
    if (!approxNotePitch(notesAfter, 0, 330.0f) || !approxNotePitch(notesAfter, 1, 440.0f)) {
        logFail(testName, "alignment did not commit reference pitches");
        return;
    }
    if (segmentsAfter.empty()) {
        logFail(testName, "alignment did not commit corrected segments");
        return;
    }
    if (!hasReferenceAutoHandle(gridAfter)) {
        logFail(testName, "alignment did not commit AUTO Ref TimeGrid handles");
        return;
    }
    if (!processor.getUndoManager().canUndo()) {
        logFail(testName, "alignment should create one undoable composite edit");
        return;
    }

    processor.getUndoManager().undo();
    std::vector<Note> notesUndo;
    std::vector<CorrectedSegment> segmentsUndo;
    std::shared_ptr<const TimeGridSnapshot> gridUndo;
    if (!readTargetState(processor, target.materializationId, notesUndo, segmentsUndo, gridUndo)) {
        logFail(testName, "failed to read undo target state");
        return;
    }
    if (!approxNotePitch(notesUndo, 0, 220.0f) || !approxNotePitch(notesUndo, 1, 220.0f)) {
        logFail(testName, "single undo did not restore target pitches");
        return;
    }
    if (!segmentsUndo.empty() || hasReferenceAutoHandle(gridUndo)) {
        logFail(testName, "single undo did not restore pitch and time state together");
        return;
    }

    processor.getUndoManager().redo();
    std::vector<Note> notesRedo;
    std::vector<CorrectedSegment> segmentsRedo;
    std::shared_ptr<const TimeGridSnapshot> gridRedo;
    if (!readTargetState(processor, target.materializationId, notesRedo, segmentsRedo, gridRedo)) {
        logFail(testName, "failed to read redo target state");
        return;
    }
    if (!approxNotePitch(notesRedo, 0, 330.0f)
        || !approxNotePitch(notesRedo, 1, 440.0f)
        || segmentsRedo.empty()
        || !hasReferenceAutoHandle(gridRedo)) {
        logFail(testName, "redo did not reapply full AUTO Ref patch");
        return;
    }

    logPass(testName);
}

void runAutoRefIntegrationMissingBindingLeavesTargetUnchangedTest()
{
    constexpr const char* testName = "AutoRefIntegration_MissingBindingLeavesTargetUnchanged";

    OpenTuneAudioProcessor processor;
    const auto target = addPlacement(processor, "AUTO Ref Target", 0.0);
    if (!target.isValid()) {
        logFail(testName, "failed to seed target placement");
        return;
    }

    const std::vector<Note> targetNotes = {
        makeNote(0.20, 0.70, 220.0f),
        makeNote(1.00, 1.50, 220.0f)
    };
    if (!seedMaterializationFeatures(processor,
                                     target.materializationId,
                                     makeTwoPhraseF0(220.0f, 220.0f),
                                     targetNotes)) {
        logFail(testName, "failed to seed target analysis inputs");
        return;
    }

    std::vector<Note> notesBefore;
    std::vector<CorrectedSegment> segmentsBefore;
    std::shared_ptr<const TimeGridSnapshot> gridBefore;
    if (!readTargetState(processor, target.materializationId, notesBefore, segmentsBefore, gridBefore)) {
        logFail(testName, "failed to read initial target state");
        return;
    }

    const auto result = processor.executeReferenceAlignmentForPlacement(target.placementId);
    if (result.status != OpenTuneAudioProcessor::ReferenceAlignmentResult::Status::NoReferenceBinding) {
        logFail(testName, "missing binding should be reported before mutation");
        return;
    }

    std::vector<Note> notesAfter;
    std::vector<CorrectedSegment> segmentsAfter;
    std::shared_ptr<const TimeGridSnapshot> gridAfter;
    if (!readTargetState(processor, target.materializationId, notesAfter, segmentsAfter, gridAfter)) {
        logFail(testName, "failed to read final target state");
        return;
    }
    if (!approxNotePitch(notesAfter, 0, notesBefore[0].pitch)
        || !approxNotePitch(notesAfter, 1, notesBefore[1].pitch)
        || segmentsAfter.size() != segmentsBefore.size()
        || hasReferenceAutoHandle(gridAfter)
        || !gridAfter->isIdentity()) {
        logFail(testName, "failed AUTO Ref request mutated target state");
        return;
    }

    logPass(testName);
}

void runAutoRefIntegrationProcessorOwnsReferenceAnalysisPreheatTest()
{
    constexpr const char* testName = "AutoRefIntegration_ProcessorOwnsReferenceAnalysisPreheat";

    OpenTuneAudioProcessor processor;
    const auto placement = addPlacement(processor, "AUTO Ref Analysis", 0.0);
    if (!placement.isValid()) {
        logFail(testName, "failed to seed placement");
        return;
    }

    const std::vector<Note> notes = {
        makeNote(0.20, 0.70, 220.0f),
        makeNote(1.00, 1.50, 330.0f)
    };
    if (!seedMaterializationFeatures(processor,
                                     placement.materializationId,
                                     makeTwoPhraseF0(220.0f, 330.0f),
                                     notes)) {
        logFail(testName, "failed to seed source-derived inputs");
        return;
    }

    auto* store = processor.getMaterializationStore();
    if (store == nullptr) {
        logFail(testName, "materialization store unavailable");
        return;
    }

    MaterializationStore::MaterializationSnapshot snapshot;
    if (!store->getSnapshot(placement.materializationId, snapshot)) {
        logFail(testName, "failed to read materialization snapshot");
        return;
    }
    const auto expectedFingerprint = static_cast<int64_t>(snapshot.renderRevision);

    processor.setReferenceAnalysisNotificationDispatcherForTests([](std::function<void()> task) {
        task();
    });

    const auto status = processor.preheatReferenceAlignmentFeatures(placement.materializationId);
    if (status != OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus::Queued
        && status != OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus::AlreadyReady) {
        logFail(testName, "processor did not accept reference analysis preheat");
        return;
    }

    for (int attempt = 0; attempt < 40; ++attempt) {
        ReferenceFeatureSet features;
        if (store->getReferenceFeatures(placement.materializationId, features)) {
            if (features.status == ReferenceFeatureStatus::Failed
                && features.producer == ReferenceFeatureProducer::Game
                && features.inputFingerprint == expectedFingerprint) {
                if (isExpectedGamePreheatUnavailable(features.errorMessage)) {
                    logPass(testName);
                    return;
                }
                logFail(testName, features.errorMessage.isNotEmpty()
                    ? features.errorMessage.toRawUTF8()
                    : "processor-owned preheat published Failed Game features");
                return;
            }

            if (features.isReady()
                && features.producer == ReferenceFeatureProducer::Game
                && features.inputFingerprint == expectedFingerprint
                && !features.pitch.notes.empty()
                && features.timing.anchors.size() >= 2) {
                logPass(testName);
                return;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    logFail(testName, "processor-owned ReferenceAnalysisService did not publish a terminal Game-owned result");
}

void runAutoRefIntegrationEnsureTimeToolAnchorSeedBuildsIdentityInternalHandlesTest()
{
    constexpr const char* testName =
        "AutoRefIntegration_EnsureTimeToolAnchorSeed_BuildsIdentityInternalHandles";

    OpenTuneAudioProcessor processor;
    const auto placement = addPlacement(processor, "TimeTool Seed", 0.0);
    if (!placement.isValid()) {
        logFail(testName, "failed to seed placement");
        return;
    }

    const std::vector<Note> notes = {
        makeNote(0.20, 0.70, 220.0f),
        makeNote(1.00, 1.50, 330.0f)
    };
    if (!seedMaterializationFeatures(processor,
                                     placement.materializationId,
                                     makeTwoPhraseF0(220.0f, 330.0f),
                                     notes)) {
        logFail(testName, "failed to seed source-derived inputs");
        return;
    }

    auto* store = processor.getMaterializationStore();
    if (store == nullptr) {
        logFail(testName, "materialization store unavailable");
        return;
    }

    MaterializationStore::MaterializationSnapshot snapshot;
    if (!store->getSnapshot(placement.materializationId, snapshot)) {
        logFail(testName, "failed to read materialization snapshot");
        return;
    }

    if (!processor.ensureTimeToolAnchorSeed(placement.materializationId)) {
        logFail(testName, "ensureTimeToolAnchorSeed returned false");
        return;
    }

    const auto grid = processor.getMaterializationTimeGridById(placement.materializationId);
    if (grid == nullptr) {
        logFail(testName, "seed did not publish a TimeGrid");
        return;
    }
    if (grid->handles().size() < 3) {
        logFail(testName, "seeded TimeGrid must contain internal anchors");
        return;
    }
    if (!grid->isIdentity()) {
        logFail(testName, "seeded TimeGrid must remain identity");
        return;
    }
    if (!hasIdentityInternalOnsetHandle(grid)) {
        logFail(testName, "seeded TimeGrid must contain unlocked InternalOnset handles");
        return;
    }

    ReferenceFeatureSet features;
    if (!store->getReferenceFeatures(placement.materializationId, features)
        || !features.isReady()
        || features.inputFingerprint != static_cast<int64_t>(snapshot.renderRevision)
        || !features.hasTimingAnchors()) {
        logFail(testName, "ensureTimeToolAnchorSeed should preheat fallback ReferenceFeatureSet facts");
        return;
    }

    logPass(testName);
}

void runAutoRefIntegrationEnsureTimeToolAnchorSeedPreservesExistingWarpTest()
{
    constexpr const char* testName = "AutoRefIntegration_EnsureTimeToolAnchorSeed_PreservesExistingWarp";

    OpenTuneAudioProcessor processor;
    const auto placement = addPlacement(processor, "TimeTool Existing Warp", 0.0);
    if (!placement.isValid()) {
        logFail(testName, "failed to seed placement");
        return;
    }

    const std::vector<Note> notes = {
        makeNote(0.20, 0.70, 220.0f),
        makeNote(1.00, 1.50, 330.0f)
    };
    if (!seedMaterializationFeatures(processor,
                                     placement.materializationId,
                                     makeTwoPhraseF0(220.0f, 330.0f),
                                     notes)) {
        logFail(testName, "failed to seed source-derived inputs");
        return;
    }

    const auto warpedGrid = makeWarpedReferenceGrid();
    if (warpedGrid == nullptr
        || !processor.setMaterializationTimeGridById(placement.materializationId,
                                                     warpedGrid,
                                                     0,
                                                     static_cast<int64_t>(kF0Frames))) {
        logFail(testName, "failed to seed warped TimeGrid");
        return;
    }

    const auto before = processor.getMaterializationTimeGridById(placement.materializationId);
    if (before == nullptr || before->isIdentity()) {
        logFail(testName, "expected non-identity TimeGrid before seed");
        return;
    }

    if (!processor.ensureTimeToolAnchorSeed(placement.materializationId)) {
        logFail(testName, "ensureTimeToolAnchorSeed should no-op but still succeed");
        return;
    }

    const auto after = processor.getMaterializationTimeGridById(placement.materializationId);
    if (after == nullptr || after->isIdentity()) {
        logFail(testName, "existing warped TimeGrid must be preserved");
        return;
    }
    if (after->handles().size() != before->handles().size()) {
        logFail(testName, "existing warped TimeGrid handle count changed");
        return;
    }
    if (!approxEqual(static_cast<float>(after->handles()[1].output_seconds),
                     static_cast<float>(before->handles()[1].output_seconds),
                     1.0e-6f)) {
        logFail(testName, "existing warped TimeGrid handle positions changed");
        return;
    }
    if (after->revision() != before->revision()) {
        logFail(testName, "existing warped TimeGrid should not be rewritten by seed");
        return;
    }

    logPass(testName);
}

} // namespace

void runAutoRefIntegrationSuite()
{
    logSection("AutoRefIntegration");
    runAutoRefIntegrationTransactionalApplyAndCompositeUndoTest();
    runAutoRefIntegrationMissingBindingLeavesTargetUnchangedTest();
    runAutoRefIntegrationProcessorOwnsReferenceAnalysisPreheatTest();
    runAutoRefIntegrationEnsureTimeToolAnchorSeedBuildsIdentityInternalHandlesTest();
    runAutoRefIntegrationEnsureTimeToolAnchorSeedPreservesExistingWarpTest();
}
