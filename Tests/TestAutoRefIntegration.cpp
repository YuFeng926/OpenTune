/**
 * Tests/TestAutoRefIntegration.cpp - Reference alignment domain applier tests.
 *
 * Covers:
 *   - ReferenceBinding -> AlignmentFeatures -> ReferenceAutoAlign request/patch
 *   - Processor/domain transactional apply into notes, corrected segments, TimeGrid
 *   - One composite undo/redo restoring pitch and time edits together
 */

#include "TestSupport.h"
#include "Utils/TimeCoordinate.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>

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

bool seedReadyTimeToolAnalysis(OpenTuneAudioProcessor& processor,
                               uint64_t materializationId,
                               std::initializer_list<double> eventTimesSeconds)
{
    auto* store = processor.getMaterializationStore();
    if (store == nullptr) {
        return false;
    }

    MaterializationStore::MaterializationSnapshot snapshot;
    if (!store->getSnapshot(materializationId, snapshot)) {
        return false;
    }

    MaterializationStore::DerivedAnalysis analysis;
    analysis.state = F0ExtractionState::Ready;
    analysis.backendMode = 2;
    analysis.inputFingerprint = static_cast<int64_t>(snapshot.renderRevision);
    analysis.sourceDurationSeconds = snapshot.audioBuffer != nullptr
        ? static_cast<double>(snapshot.audioBuffer->getNumSamples()) / TimeCoordinate::kRenderSampleRate
        : 0.0;

    uint64_t eventId = 1;
    for (double eventSeconds : eventTimesSeconds) {
        MaterializationStore::DerivedAnalysis::TemporalEvent event;
        event.eventId = eventId++;
        event.sourceSeconds = eventSeconds;
        event.kind = MaterializationStore::DerivedAnalysis::TemporalEventKind::Onset;
        event.strength = 1.0f;
        event.confidence = 1.0f;
        analysis.temporalEvents.push_back(event);
    }

    return store->setDerivedAnalysis(materializationId, analysis);
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

    processor.setReferenceAnalysisNotificationDispatcherForTests([](std::function<void()> task) {
        task();
    });

    const auto status = processor.preheatReferenceAlignmentFeatures(placement.materializationId);
    if (status != OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus::Queued
        && status != OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus::AlreadyReady) {
        logFail(testName, "processor did not accept reference analysis preheat");
        return;
    }

    auto* store = processor.getMaterializationStore();
    if (store == nullptr) {
        logFail(testName, "materialization store unavailable");
        return;
    }

    for (int attempt = 0; attempt < 40; ++attempt) {
        MaterializationStore::DerivedAnalysis analysis;
        if (store->getDerivedAnalysis(placement.materializationId, analysis)
            && analysis.state == F0ExtractionState::Ready
            && !analysis.basicDerivedNotes.empty()
            && analysis.temporalEvents.size() >= 2) {
            logPass(testName);
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    logFail(testName, "processor-owned ReferenceAnalysisService did not publish Ready features");
}

void runAutoRefIntegrationEnsureTimeToolAnchorSeedBuildsIdentityInternalHandlesTest()
{
    constexpr const char* testName = "AutoRefIntegration_EnsureTimeToolAnchorSeed_BuildsIdentityInternalHandles";

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

    if (!seedReadyTimeToolAnalysis(processor, placement.materializationId, { 0.45, 1.20, 1.55 })) {
        logFail(testName, "failed to seed ready derived analysis");
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

    const auto& handles = grid->handles();
    if (handles.size() < 3) {
        logFail(testName, "seeded TimeGrid must contain internal anchors");
        return;
    }
    if (!grid->isIdentity()) {
        logFail(testName, "seeded TimeGrid must remain identity");
        return;
    }

    const bool hasInternalOnset = std::any_of(handles.begin(), handles.end(), [](const TimeHandle& handle) {
        return handle.kind == HandleKind::InternalOnset
            && !handle.locked
            && approxEqual(static_cast<float>(handle.source_seconds),
                           static_cast<float>(handle.output_seconds),
                           1.0e-6f);
    });
    if (!hasInternalOnset) {
        logFail(testName, "seeded TimeGrid must contain unlocked InternalOnset handles");
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

// ============================================================================
// Suite aggregator
// ============================================================================

void runAutoRefIntegrationSuite()
{
    logSection("AutoRefIntegration");
    runAutoRefIntegrationTransactionalApplyAndCompositeUndoTest();
    runAutoRefIntegrationMissingBindingLeavesTargetUnchangedTest();
    runAutoRefIntegrationProcessorOwnsReferenceAnalysisPreheatTest();
    runAutoRefIntegrationEnsureTimeToolAnchorSeedBuildsIdentityInternalHandlesTest();
    runAutoRefIntegrationEnsureTimeToolAnchorSeedPreservesExistingWarpTest();
}
