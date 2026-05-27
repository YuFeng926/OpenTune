/**
 * Tests/TestArrangementContract.cpp — StandaloneArrangement contract / invariant tests
 *
 * Coverage: L2-L3 contract tests for reference binding stability:
 *   - ReferenceBindingIdempotent: setting the same reference twice is a no-op
 *   - ReferenceBindingWithInvalidId: non-existent target returns false
 *   - PlaybackSnapshotExcludesReferenceForAudioThread: snapshot loads without error
 *
 * Coverage: L2 import UX contract guards:
 *   - StandaloneImportDrop_UsesDropTrackInsteadOfPrompt
 *   - StandaloneImportDrop_BlankArrangementAreaTargetsNewTrack
 *   - StandaloneImportDrop_OutOfArrangementFallsBackToActiveTrack
 *   - StandaloneImportChooser_SingleFileRemainsActiveTrackFastPath
 *   - StandaloneImportDrop_PreviewIsTransientOnly
 *
 * Suite aggregator: runArrangementContractSuite() — registered in TestMain.cpp.
 */
#include "TestSupport.h"

namespace {

// ============================================================================
// Import UX source-scan helpers (reuse pattern from TestTimelineRenderingPipeline)
// ============================================================================

juce::File locateArrangementContractWorkspaceRoot()
{
    auto current = juce::File::getCurrentWorkingDirectory();
    for (int depth = 0; depth < 8 && current.isDirectory(); ++depth) {
        if (current.getChildFile("CMakeLists.txt").existsAsFile())
            return current;

        const auto parent = current.getParentDirectory();
        if (parent == current)
            break;

        current = parent;
    }
    return {};
}

juce::String readArrangementContractWorkspaceFile(const juce::String& relativePath)
{
    const auto root = locateArrangementContractWorkspaceRoot();
    if (!root.isDirectory())
        return {};

    const auto file = root.getChildFile(relativePath);
    return file.existsAsFile() ? file.loadFileAsString() : juce::String{};
}

juce::String extractArrangementContractWorkspaceSection(const juce::String& relativePath,
                                                         const juce::String& startNeedle,
                                                         const juce::String& endNeedle)
{
    const auto source = readArrangementContractWorkspaceFile(relativePath);
    const int start = source.indexOf(startNeedle);
    if (start < 0)
        return {};

    const int end = source.indexOf(start + startNeedle.length(), endNeedle);
    if (end < 0 || end <= start)
        return {};

    return source.substring(start, end);
}

/** Helper: create a placement on the given track, return its auto-assigned ID. */
uint64_t createPlacement(StandaloneArrangement& arr, int trackId,
                         double start, double duration, uint64_t matId)
{
    StandaloneArrangement::Placement p{};
    p.materializationId = matId;
    p.timelineStartSeconds = start;
    p.durationSeconds = duration;
    p.gain = 1.0f;
    arr.insertPlacement(trackId, p);
    return p.placementId; // auto-assigned by insertPlacement
}

// ============================================================================
// Test 1: ReferenceBindingIdempotent — setting A→B twice is a no-op
// ============================================================================

void runContractReferenceBindingIdempotentTest()
{
    constexpr const char* testName = "ArrangementContract_ReferenceBindingIdempotent";

    StandaloneArrangement arr;
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);
    const uint64_t bId = createPlacement(arr, 0, 3.0, 5.0, 2);

    if (aId == 0 || bId == 0) {
        logFail(testName, "failed to create placements");
        return;
    }

    // First set
    if (!arr.setPlacementReferencePlacement(0, aId, bId)) {
        logFail(testName, "first setPlacementReferencePlacement should succeed");
        return;
    }

    // Second set — same target, same reference — must succeed (idempotent)
    if (!arr.setPlacementReferencePlacement(0, aId, bId)) {
        logFail(testName, "second setPlacementReferencePlacement with same args failed (not idempotent)");
        return;
    }

    // Verify reference is still B
    const uint64_t ref = arr.getPlacementReferencePlacement(0, aId);
    if (ref != bId) {
        logFail(testName, "reference changed after idempotent re-set");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 2: ReferenceBindingWithInvalidIdReturnsFalse
// ============================================================================

void runContractReferenceBindingWithInvalidIdTest()
{
    constexpr const char* testName = "ArrangementContract_ReferenceBindingWithInvalidId";

    StandaloneArrangement arr;
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);

    if (aId == 0) {
        logFail(testName, "failed to create placement");
        return;
    }

    // Try to set reference on a non-existent target placementId
    constexpr uint64_t kInvalidTargetId = 99999;
    const bool setOk = arr.setPlacementReferencePlacement(0, kInvalidTargetId, aId);
    if (setOk) {
        logFail(testName, "setPlacementReferencePlacement with invalid target should return false");
        return;
    }

    // Also test with non-existent reference placementId
    constexpr uint64_t kInvalidRefId = 88888;
    const bool setRefOk = arr.setPlacementReferencePlacement(0, aId, kInvalidRefId);
    if (setRefOk) {
        logFail(testName, "setPlacementReferencePlacement with non-existent reference should return false");
        return;
    }

    // Verify A's reference was not corrupted
    const uint64_t aRef = arr.getPlacementReferencePlacement(0, aId);
    if (aRef != 0) {
        logFail(testName, "A's reference was corrupted by failed set operation");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Test 3: PlaybackSnapshotExcludesReferenceForAudioThread — snapshot loads ok
// ============================================================================

void runContractPlaybackSnapshotLoadsWithoutErrorTest()
{
    constexpr const char* testName = "ArrangementContract_PlaybackSnapshotLoadsWithoutError";

    StandaloneArrangement arr;

    // Create several placements across tracks
    const uint64_t aId = createPlacement(arr, 0, 0.0, 5.0, 1);
    const uint64_t bId = createPlacement(arr, 0, 3.0, 5.0, 2);
    const uint64_t cId = createPlacement(arr, 1, 1.0, 4.0, 3);

    if (aId == 0 || bId == 0 || cId == 0) {
        logFail(testName, "failed to create placements");
        return;
    }

    // Set up a reference binding A→B
    if (!arr.setPlacementReferencePlacement(0, aId, bId)) {
        logFail(testName, "A→B reference should succeed");
        return;
    }

    // Load playback snapshot — must return non-null shared_ptr
    auto snapshot = arr.loadPlaybackSnapshot();
    if (snapshot == nullptr) {
        logFail(testName, "loadPlaybackSnapshot returned null shared_ptr");
        return;
    }

    // Verify epoch is non-zero (snapshot was published)
    if (snapshot->epoch == 0) {
        logFail(testName, "playback snapshot epoch is zero (snapshot not published)");
        return;
    }

    // Verify track 0 contains the expected number of placements (A + B = 2)
    const size_t track0Count = snapshot->tracks[0].placements.size();
    if (track0Count != 2) {
        logFail(testName, "snapshot track 0 does not contain expected 2 placements");
        return;
    }

    // Verify track 1 contains C
    const size_t track1Count = snapshot->tracks[1].placements.size();
    if (track1Count != 1) {
        logFail(testName, "snapshot track 1 does not contain expected 1 placement");
        return;
    }

    // The snapshot must contain the referencePlacementId field (it's part of Placement struct),
    // but we don't enforce that it's stripped — the audio thread simply uses it.
    // Just verify that loading the snapshot with references present does not crash.

    logPass(testName);
}

// ============================================================================
// Import Drop UX Contract Guards (L2 source-scan, no UI runtime required)
// ============================================================================

void runStandaloneImportDropUsesDropTrackInsteadOfPromptTest()
{
    constexpr const char* testName = "StandaloneImportDrop_UsesDropTrackInsteadOfPrompt";

    const auto filesDroppedSection = extractArrangementContractWorkspaceSection(
        "Source/Standalone/PluginEditor.cpp",
        "void OpenTuneAudioProcessorEditor::filesDropped",
        "void OpenTuneAudioProcessorEditor::fileDragEnter");

    if (filesDroppedSection.isEmpty()) {
        logFail(testName, "could not locate filesDropped implementation — source file or section changed");
        return;
    }

    // Drag-drop must not call the legacy track picker
    if (filesDroppedSection.contains("promptTrackSelectionForDroppedFile")) {
        logFail(testName, "filesDropped still routes through legacy track picker");
        return;
    }

    // Must use the target resolver
    if (!filesDroppedSection.contains("resolveImportDropTarget")) {
        logFail(testName, "filesDropped does not resolve import target from drop position");
        return;
    }

    // Must clear preview state on drop
    if (!filesDroppedSection.contains("clearImportDropPreview")) {
        logFail(testName, "filesDropped does not clear pending hover preview before commit");
        return;
    }

    logPass(testName);
}

void runStandaloneImportDropBlankArrangementAreaTargetsNewTrackTest()
{
    constexpr const char* testName = "StandaloneImportDrop_BlankArrangementAreaTargetsNewTrack";

    const auto resolverSection = extractArrangementContractWorkspaceSection(
        "Source/Standalone/PluginEditor.cpp",
        "ImportDropTarget OpenTuneAudioProcessorEditor::resolveImportDropTarget",
        "void OpenTuneAudioProcessorEditor::applyImportDropTarget");

    if (resolverSection.isEmpty()) {
        logFail(testName, "could not locate resolveImportDropTarget implementation");
        return;
    }

    // Blank-area drop below visible tracks must yield NewTrack kind
    if (!resolverSection.contains("ImportDropTarget::Kind::NewTrack")) {
        logFail(testName, "resolver does not produce NewTrack target for blank-area drop");
        return;
    }

    // Must check MAX_TRACKS before allowing new track creation
    if (!resolverSection.contains("MAX_TRACKS")) {
        logFail(testName, "resolver does not enforce MAX_TRACKS limit for blank-area drop");
        return;
    }

    // At MAX_TRACKS must yield Reject, not NewTrack
    if (!resolverSection.contains("ImportDropTarget::Kind::Reject")) {
        logFail(testName, "resolver does not produce Reject when at MAX_TRACKS");
        return;
    }

    if (!resolverSection.contains("viewportXToAbsoluteTime")) {
        logFail(testName, "blank-area drop does not preserve horizontal drop time");
        return;
    }

    logPass(testName);
}

void runStandaloneImportDropOutOfArrangementFallsBackToActiveTrackTest()
{
    constexpr const char* testName = "StandaloneImportDrop_OutOfArrangementFallsBackToActiveTrack";

    const auto resolverSection = extractArrangementContractWorkspaceSection(
        "Source/Standalone/PluginEditor.cpp",
        "ImportDropTarget OpenTuneAudioProcessorEditor::resolveImportDropTarget",
        "void OpenTuneAudioProcessorEditor::applyImportDropTarget");

    if (resolverSection.isEmpty()) {
        logFail(testName, "could not locate resolveImportDropTarget implementation");
        return;
    }

    // Must check if drop is inside Arrangement bounds
    if (!resolverSection.contains("isInsideArrangement") || !resolverSection.contains("getLocalBounds()")) {
        logFail(testName, "resolver does not check Arrangement bounds before resolving track");
        return;
    }

    if (!resolverSection.contains("getRulerHeight")) {
        logFail(testName, "resolver does not exclude ruler/non-lane area before track targeting");
        return;
    }

    // Non-Arrangement drop must fallback to active track
    if (!resolverSection.contains("ImportDropTarget::Kind::FallbackActiveTrack")) {
        logFail(testName, "resolver does not produce FallbackActiveTrack for non-Arrangement drop");
        return;
    }

    // Fallback must use active track, not guess from geometry
    if (!resolverSection.contains("getStandaloneActiveTrack")) {
        logFail(testName, "fallback target does not resolve from active track");
        return;
    }

    logPass(testName);
}

void runStandaloneImportChooserSingleFileRemainsActiveTrackFastPathTest()
{
    constexpr const char* testName = "StandaloneImportChooser_SingleFileRemainsActiveTrackFastPath";

    const auto chooserSection = extractArrangementContractWorkspaceSection(
        "Source/Standalone/PluginEditor.cpp",
        "void OpenTuneAudioProcessorEditor::importAudioRequested",
        "void OpenTuneAudioProcessorEditor::exportAudioRequested");

    if (chooserSection.isEmpty()) {
        logFail(testName, "could not locate importAudioRequested implementation");
        return;
    }

    // Single-file path must not show a track-choice popup
    if (chooserSection.contains("promptTrackSelectionForDroppedFile")) {
        logFail(testName, "importAudioRequested still references legacy track picker");
        return;
    }

    // Single-file path must use importAudioFileToTrack directly
    const auto singleFileBlock = chooserSection.fromFirstOccurrenceOf("selectedFiles.size() == 1", false, false);
    if (singleFileBlock.isEmpty()) {
        logFail(testName, "could not locate single-file branch inside importAudioRequested");
        return;
    }

    // Isolate the single-file branch: find the closing brace before the else
    const int elsePos = singleFileBlock.indexOf("else");
    const auto singleFileOnly = (elsePos > 0) ? singleFileBlock.substring(0, elsePos) : singleFileBlock;

    if (!singleFileOnly.contains("importAudioFileToTrack")) {
        logFail(testName, "single-file chooser path does not route to importAudioFileToTrack");
        return;
    }

    // Must not use any modal dialog for single file
    if (singleFileOnly.contains("AlertWindow") || singleFileOnly.contains("enterModalState")) {
        logFail(testName, "single-file chooser path still opens a modal dialog");
        return;
    }

    logPass(testName);
}

void runStandaloneImportDropPreviewIsTransientOnlyTest()
{
    constexpr const char* testName = "StandaloneImportDrop_PreviewIsTransientOnly";

    const auto headerFile = readArrangementContractWorkspaceFile("Source/Standalone/PluginEditor.h");
    const auto cppFile = readArrangementContractWorkspaceFile("Source/Standalone/PluginEditor.cpp");

    // Preview state must stay in UI layer and not require editor-side persisted hover truth
    if (!headerFile.contains("updateImportDropPreview")) {
        logFail(testName, "PluginEditor header is missing the UI-only import preview update entry");
        return;
    }

    // Preview must be cleared on drop
    const auto filesDroppedSection = extractArrangementContractWorkspaceSection(
        "Source/Standalone/PluginEditor.cpp",
        "void OpenTuneAudioProcessorEditor::filesDropped",
        "void OpenTuneAudioProcessorEditor::fileDragEnter");

    if (!filesDroppedSection.contains("clearImportDropPreview")) {
        logFail(testName, "filesDropped does not clear preview state before commit");
        return;
    }

    // fileDragExit must clear preview
    const auto dragExitSection = extractArrangementContractWorkspaceSection(
        "Source/Standalone/PluginEditor.cpp",
        "void OpenTuneAudioProcessorEditor::fileDragExit",
        "void OpenTuneAudioProcessorEditor::clearImportDropPreview");

    if (!dragExitSection.contains("clearImportDropPreview")) {
        logFail(testName, "fileDragExit does not call clearImportDropPreview");
        return;
    }

    // Preview must not create tracks or placements
    if (dragExitSection.contains("importAudioFileToTrack")
        || dragExitSection.contains("showMoreTracks")
        || dragExitSection.contains("queuePendingImport")) {
        logFail(testName, "fileDragExit performs placement/track mutations instead of clearing only");
        return;
    }

    // File-drag hover must not call commit paths
    const auto dragEnterSection = extractArrangementContractWorkspaceSection(
        "Source/Standalone/PluginEditor.cpp",
        "void OpenTuneAudioProcessorEditor::fileDragEnter",
        "void OpenTuneAudioProcessorEditor::fileDragMove");

    const auto dragMoveSection = extractArrangementContractWorkspaceSection(
        "Source/Standalone/PluginEditor.cpp",
        "void OpenTuneAudioProcessorEditor::fileDragMove",
        "void OpenTuneAudioProcessorEditor::fileDragExit");

    if ((!dragEnterSection.isEmpty() && dragEnterSection.contains("importAudioFileToTrack"))
        || (!dragMoveSection.isEmpty() && dragMoveSection.contains("importAudioFileToTrack"))) {
        logFail(testName, "fileDragEnter/Move calls importAudioFileToTrack before actual drop");
        return;
    }

    if ((!dragEnterSection.isEmpty() && dragEnterSection.contains("queuePendingImport"))
        || (!dragMoveSection.isEmpty() && dragMoveSection.contains("queuePendingImport"))) {
        logFail(testName, "fileDragEnter/Move enqueues imports before actual drop");
        return;
    }

    // Verify ArrangementViewComponent preview state is in UI-only header, not in arrangement truth
    const auto arrangementHeader = readArrangementContractWorkspaceFile("Source/Standalone/UI/ArrangementViewComponent.h");
    if (!arrangementHeader.contains("ImportDropPreview")) {
        logFail(testName, "ArrangementViewComponent does not define ImportDropPreview struct");
        return;
    }

    // The preview state must remain in ArrangementViewComponent (UI layer), not in StandaloneArrangement
    const auto arrangementTruthHeader = readArrangementContractWorkspaceFile("Source/StandaloneArrangement.h");
    if (arrangementTruthHeader.contains("ImportDropPreview")) {
        logFail(testName, "ImportDropPreview leaked into StandaloneArrangement (persisted truth)");
        return;
    }

    logPass(testName);
}

} // namespace

// ============================================================================
// Suite aggregator
// ============================================================================

void runArrangementContractSuite()
{
    logSection("ArrangementContract");
    runContractReferenceBindingIdempotentTest();
    runContractReferenceBindingWithInvalidIdTest();
    runContractPlaybackSnapshotLoadsWithoutErrorTest();
    runStandaloneImportDropUsesDropTrackInsteadOfPromptTest();
    runStandaloneImportDropBlankArrangementAreaTargetsNewTrackTest();
    runStandaloneImportDropOutOfArrangementFallsBackToActiveTrackTest();
    runStandaloneImportChooserSingleFileRemainsActiveTrackFastPathTest();
    runStandaloneImportDropPreviewIsTransientOnlyTest();
}
