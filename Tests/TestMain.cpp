#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef OPENTUNE_SOURCE_DIR
#error "OPENTUNE_SOURCE_DIR must be defined by CMake"
#endif

namespace {

int failures = 0;

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        ++failures;
        std::cout << "[FAIL] " << message << "\n";
    }
}

std::filesystem::path sourcePath(std::string_view relative)
{
    return std::filesystem::path(OPENTUNE_SOURCE_DIR) / std::filesystem::path(relative);
}

std::string readText(std::string_view relative)
{
    const auto path = sourcePath(relative);
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + path.string());

    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool contains(std::string_view text, std::string_view token)
{
    return text.find(token) != std::string_view::npos;
}

size_t countOf(std::string_view text, std::string_view token)
{
    size_t count = 0;
    size_t pos = 0;
    while ((pos = text.find(token, pos)) != std::string_view::npos) {
        ++count;
        pos += token.size();
    }
    return count;
}

std::string extractBracedBlock(std::string_view text, size_t markerPos)
{
    if (markerPos == std::string_view::npos)
        return {};

    const size_t bracePos = text.find('{', markerPos);
    if (bracePos == std::string_view::npos)
        return {};

    int depth = 0;
    for (size_t i = bracePos; i < text.size(); ++i) {
        if (text[i] == '{') {
            ++depth;
        } else if (text[i] == '}') {
            --depth;
            if (depth == 0)
                return std::string(text.substr(markerPos, i - markerPos + 1));
        }
    }
    return {};
}

std::string extractFunctionBlock(std::string_view text, std::string_view signature)
{
    return extractBracedBlock(text, text.find(signature));
}

std::string extractBlockByMarker(std::string_view text, std::string_view marker)
{
    return extractBracedBlock(text, text.find(marker));
}

std::string textBetween(std::string_view text, std::string_view startMarker, std::string_view endMarker)
{
    const size_t start = text.find(startMarker);
    if (start == std::string_view::npos)
        return {};

    const size_t end = text.find(endMarker, start + startMarker.size());
    if (end == std::string_view::npos)
        return {};

    return std::string(text.substr(start, end - start + endMarker.size()));
}

bool inOrder(std::string_view text, std::initializer_list<std::string_view> tokens)
{
    size_t pos = 0;
    for (const auto token : tokens) {
        const size_t found = text.find(token, pos);
        if (found == std::string_view::npos)
            return false;
        pos = found + token.size();
    }
    return true;
}

void expectTokens(std::string_view blockName,
                  std::string_view text,
                  std::initializer_list<std::string_view> tokens)
{
    for (const auto token : tokens)
        expect(contains(text, token), std::string(blockName) + " missing token: " + std::string(token));
}

void expectNoTokens(std::string_view blockName,
                    std::string_view text,
                    std::initializer_list<std::string_view> tokens,
                    std::string_view detail = {})
{
    for (const auto token : tokens) {
        std::string msg = std::string(blockName) + " contains forbidden token: " + std::string(token);
        if (!detail.empty())
            msg += " — " + std::string(detail);
        expect(!contains(text, token), msg);
    }
}

void contentSlotNotesIsRestored()
{
    const auto component = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto slotsFunc = extractFunctionBlock(
        component, "std::vector<ContentSlot> PianoRollComponent::visibleContentSlots");
    const auto revisionFunc = extractFunctionBlock(
        component, "uint64_t PianoRollComponent::revisionForContentSlot");

    expect(!slotsFunc.empty(), "visibleContentSlots must be found");
    expect(contains(slotsFunc, "ContentSlot::Notes"),
           "visibleContentSlots must include ContentSlot::Notes");
    expect(contains(slotsFunc, "ContentSlot::Waveform"),
           "visibleContentSlots must include ContentSlot::Waveform");

    expect(!revisionFunc.empty(), "revisionForContentSlot must be found");
    expect(contains(revisionFunc, "ContentSlot::Notes"),
           "revisionForContentSlot must handle ContentSlot::Notes");
}

void cachedContentTilesOwnNotes()
{
    const auto component = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto prepareTiles = extractFunctionBlock(
        component, "void PianoRollComponent::prepareCoverageContentTiles");

    expect(!prepareTiles.empty(), "prepareCoverageContentTiles must be found");

    expect(contains(prepareTiles, "ContentSlot::Notes"),
           "prepareCoverageContentTiles must handle ContentSlot::Notes");
    expect(contains(prepareTiles, "renderer_->drawNotes"),
           "prepareCoverageContentTiles must call renderer_->drawNotes for Notes slot");
    expect(contains(prepareTiles, "getCommittedNotes()"),
           "prepareCoverageContentTiles must pass committed notes to renderer");
    expectNoTokens("prepareCoverageContentTiles", prepareTiles,
                   {"item.displayNotes = {}", "getDisplayedNotes()"},
                   "Must not assign empty displayNotes or use draft notes in cache");
}

void noteAndPitchRevisionPollingIsIndependent()
{
    const auto standalone = readText("Source/Standalone/PluginEditor.cpp");
    const auto plugin = readText("Source/Plugin/PluginEditor.cpp");

    const auto standalonePoll = textBetween(
        standalone,
        "const bool contentChanged =",
        "lastPianoRollPitchRevision_ = currentPitchRevision;");
    const auto pluginPoll = extractBlockByMarker(plugin, "if (activeKey.isValid() && !contentJustSwitched)");

    expectTokens("Standalone revision poll",
                 standalonePoll,
                 {"pianoRoll_.onNotesRevisionChanged();", "pianoRoll_.onTimeGridRevisionChanged();", "pianoRoll_.onPitchRevisionChanged();"});
    expectNoTokens("Standalone revision poll",
                   standalonePoll,
                   {"} else if (currentNotesRevision != lastPianoRollNotesRevision_)"});
    expect(inOrder(standalonePoll, {"pianoRoll_.onNotesRevisionChanged();", "if (currentTimeGridRevision", "if (currentPitchRevision"}),
           "Standalone polling must continue from notes refresh to time-grid and pitch refresh");

    expectTokens("Plugin revision poll",
                 pluginPoll,
                 {"pianoRoll_.onNotesRevisionChanged();", "pianoRoll_.onTimeGridRevisionChanged();", "pianoRoll_.onPitchRevisionChanged();"});
    expect(inOrder(pluginPoll, {"pianoRoll_.onNotesRevisionChanged();", "if (currentTimeGridRevision", "if (currentPitchRevision"}),
           "Plugin polling must continue from notes refresh to time-grid and pitch refresh");
}

void notePatchCommitReturnsAuthoritativeSnapshot()
{
    const auto commandsHeader = readText("Source/Content/ContentEditCommands.h");
    const auto processorHeader = readText("Source/PluginProcessor.h");
    const auto processor = readText("Source/PluginProcessor.cpp");
    const auto component = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    expectNoTokens("ContentEditCommands.h",
                   commandsHeader,
                   {"virtual bool commitNotePatch"});
    expectTokens("ContentEditCommands.h",
                 commandsHeader,
                 {"virtual ContentCommitSnapshot commitNotePatch"});

    expectNoTokens("PluginProcessor.h",
                   processorHeader,
                   {"bool commitContentNotePatch"});
    expectTokens("PluginProcessor.h",
                 processorHeader,
                 {"ContentCommitSnapshot commitContentNotePatch"});

    expectNoTokens("PluginProcessor.cpp",
                   processor,
                   {"bool OpenTuneAudioProcessor::commitContentNotePatch"});
    expectTokens("PluginProcessor.cpp",
                 processor,
                 {"ContentCommitSnapshot OpenTuneAudioProcessor::commitContentNotePatch",
                  "auto committedSnap = getContentSnapshot(key);",
                  "return committedSnap;"});

    const auto commitNoteDraft = extractFunctionBlock(component, "bool PianoRollComponent::commitNoteDraft");
    const auto noteOnlyFallback = textBetween(
        component,
        "ContentNoteRangePatch afterPatch;",
        "return true;");

    expectTokens("commitNoteDraft",
                 commitNoteDraft,
                 {"const auto committedSnap =", "cachedNotes_ = committedSnap->notes"});
    expectNoTokens("commitNoteDraft",
                   commitNoteDraft,
                   {"refreshEditedContentNotes();"});

    expectTokens("note-only parameter path",
                 noteOnlyFallback,
                 {"const auto committedSnap =", "cachedNotes_ = committedSnap->notes"});
    expectNoTokens("note-only parameter path",
                   noteOnlyFallback,
                   {"refreshEditedContentNotes();"});
}

void noLegacyNoteInteractionState()
{
    const auto componentHeader = readText("Source/Standalone/UI/PianoRollComponent.h");
    const auto component = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    expectNoTokens("PianoRollComponent.h", componentHeader, {"interactionRevision_", "notesEpoch_"});
    expectNoTokens("PianoRollComponent.cpp", component, {"interactionRevision_", "notesEpoch_"});
}

// ============================================================================
// Arrangement selection publication contract tests
// ============================================================================

void selectedFieldsOnlyWrittenInCommitHelpers()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto commitBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitPlacementSelection");
    const auto emptyCommitBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitEmptyPlacementSelection");

    expect(!commitBody.empty(), "commitPlacementSelection must be found");
    expect(!emptyCommitBody.empty(), "commitEmptyPlacementSelection must be found");

    // selectedTrack_
    {
        const auto total = countOf(src, "selectedTrack_ =");
        const auto inCommit = countOf(commitBody, "selectedTrack_ =");
        const auto inEmpty = countOf(emptyCommitBody, "selectedTrack_ =");
        expect(total == inCommit + inEmpty,
               "All selectedTrack_ writes must be in commit helpers (total=" + std::to_string(total)
               + " commit=" + std::to_string(inCommit) + " empty=" + std::to_string(inEmpty) + ")");
    }

    // selectedPlacementId_
    {
        const auto total = countOf(src, "selectedPlacementId_ =");
        const auto inCommit = countOf(commitBody, "selectedPlacementId_ =");
        const auto inEmpty = countOf(emptyCommitBody, "selectedPlacementId_ =");
        expect(total == inCommit + inEmpty,
               "All selectedPlacementId_ writes must be in commit helpers (total=" + std::to_string(total)
               + " commit=" + std::to_string(inCommit) + " empty=" + std::to_string(inEmpty) + ")");
    }

    // selectedPlacementIndex_
    {
        const auto total = countOf(src, "selectedPlacementIndex_ =");
        const auto inCommit = countOf(commitBody, "selectedPlacementIndex_ =");
        const auto inEmpty = countOf(emptyCommitBody, "selectedPlacementIndex_ =");
        expect(total == inCommit + inEmpty,
               "All selectedPlacementIndex_ writes must be in commit helpers (total=" + std::to_string(total)
               + " commit=" + std::to_string(inCommit) + " empty=" + std::to_string(inEmpty) + ")");
    }
}

void placementSelectionChangedOnlyInCommitHelpers()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto header = readText("Source/Standalone/UI/ArrangementViewComponent.h");

    const auto commitBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitPlacementSelection");
    const auto emptyCommitBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitEmptyPlacementSelection");

    expect(!commitBody.empty(), "commitPlacementSelection must be found");
    expect(!emptyCommitBody.empty(), "commitEmptyPlacementSelection must be found");

    const auto total = countOf(src, "placementSelectionChanged(");
    const auto inCommit = countOf(commitBody, "placementSelectionChanged(");
    const auto inEmpty = countOf(emptyCommitBody, "placementSelectionChanged(");

    expect(total == inCommit + inEmpty,
           "All placementSelectionChanged calls must be in commit helpers (total=" + std::to_string(total)
           + " commit=" + std::to_string(inCommit) + " empty=" + std::to_string(inEmpty) + ")");

    // Header: placementSelectionChanged should appear only as a virtual declaration in Listener
    const auto headerCount = countOf(header, "placementSelectionChanged");
    expect(headerCount == 1,
           "placementSelectionChanged should appear exactly once in header (as Listener virtual declaration, count=" + std::to_string(headerCount) + ")");
}

void setMutatingHelpersArePure()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");

    const auto toggleBody = extractFunctionBlock(src, "void ArrangementViewComponent::togglePlacementSelection");
    const auto clearBody = extractFunctionBlock(src, "void ArrangementViewComponent::clearPlacementSelection");
    const auto rangeBody = extractFunctionBlock(src, "void ArrangementViewComponent::selectPlacementsInRange");
    const auto trackBody = extractFunctionBlock(src, "void ArrangementViewComponent::selectAllPlacementsInTrack");

    expect(!toggleBody.empty(), "togglePlacementSelection must be found");
    expect(!clearBody.empty(), "clearPlacementSelection must be found");
    expect(!rangeBody.empty(), "selectPlacementsInRange must be found");
    expect(!trackBody.empty(), "selectAllPlacementsInTrack must be found");

    expectNoTokens("togglePlacementSelection", toggleBody,
                   {"listeners_.", "refreshVisualState", "repaint()", "requestInvalidate",
                    "contentCache_", "patternCache_", "prepareCoverageContentTiles",
                    "requestContentInvalidation"});
    expectNoTokens("clearPlacementSelection", clearBody,
                   {"listeners_.", "refreshVisualState", "repaint()", "requestInvalidate",
                    "contentCache_", "patternCache_", "prepareCoverageContentTiles",
                    "requestContentInvalidation"});
    expectNoTokens("selectPlacementsInRange", rangeBody,
                   {"listeners_.", "refreshVisualState", "repaint()", "FrameScheduler", "requestInvalidate",
                    "contentCache_", "patternCache_", "prepareCoverageContentTiles",
                    "requestContentInvalidation"});
    expectNoTokens("selectAllPlacementsInTrack", trackBody,
                   {"listeners_.", "refreshVisualState", "repaint()", "FrameScheduler", "requestInvalidate",
                    "contentCache_", "patternCache_", "prepareCoverageContentTiles",
                    "requestContentInvalidation"});
}

void moveBranchHasNoBareRepaint()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto mouseDownBody = extractFunctionBlock(src, "void ArrangementViewComponent::mouseDown");

    expect(!mouseDownBody.empty(), "mouseDown must be found");

    // Find the Move branch within mouseDown: the code block after commitPlacementSelection
    // The Move branch starts with: currentDragOp_ = hit.isTopEdge ? DragOperation::Gain : DragOperation::Move;
    // The repaint() we want to forbid is NOT in clearMoveDragOverlay (which is unrelated to selection)
    //
    // Strategy: extract the section inside "if (currentDragOp_ == DragOperation::Move)"
    // and verify it does NOT contain "repaint();"
    const auto moveBranch = extractBlockByMarker(mouseDownBody, "if (currentDragOp_ == DragOperation::Move)");

    if (!moveBranch.empty()) {
        expectNoTokens("Move branch in mouseDown", moveBranch, {"repaint()"});
    }

    // Also verify mouseDown doesn't call placementSelectionChanged directly
    const auto pscCount = countOf(mouseDownBody, "placementSelectionChanged(");
    expect(pscCount == 0,
           "mouseDown must not call placementSelectionChanged directly (count=" + std::to_string(pscCount) + ")");
}

void togglePlacementSelectionAllowsLastItemToggle()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto toggleBody = extractFunctionBlock(src, "void ArrangementViewComponent::togglePlacementSelection");

    expect(!toggleBody.empty(), "togglePlacementSelection must be found");
    expect(!contains(toggleBody, "size() > 1"),
           "togglePlacementSelection must allow toggling the last selected item (no size() > 1 guard)");
    expect(!contains(toggleBody, "juce::ignoreUnused"),
           "togglePlacementSelection must not have unused-param ceremony");
}

void commitEmptyClearsModelSelection()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto emptyBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitEmptyPlacementSelection");

    expect(!emptyBody.empty(), "commitEmptyPlacementSelection must be found");
    expectTokens("commitEmptyPlacementSelection", emptyBody,
                 {"clearAllSelections()"});
    expect(!contains(emptyBody, "selectPlacement(previousTrack"),
           "commitEmptyPlacementSelection must use clearAllSelections() not per-track selectPlacement");
}

void noSelectPlacementOutsideCommitHelpers()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto commitBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitPlacementSelection");
    const auto emptyBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitEmptyPlacementSelection");

    expect(!commitBody.empty(), "commitPlacementSelection must be found");
    expect(!emptyBody.empty(), "commitEmptyPlacementSelection must be found");

    // Count total selectPlacement calls in the file
    const auto total = countOf(src, "selectPlacement(");
    const auto inCommit = countOf(commitBody, "selectPlacement(");
    const auto inEmpty = countOf(emptyBody, "selectPlacement(");

    expect(total == inCommit + inEmpty,
           "All selectPlacement calls must be in commit helpers (total=" + std::to_string(total)
           + " commit=" + std::to_string(inCommit) + " empty=" + std::to_string(inEmpty) + ")");
}

void rebuildContentMetricsUsesIsPlacementSelected()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto body = extractFunctionBlock(src, "void ArrangementViewComponent::rebuildContentMetrics");

    expect(!body.empty(), "rebuildContentMetrics must be found");
    expect(contains(body, "isPlacementSelected"),
           "rebuildContentMetrics must use isPlacementSelected");
}

void prepareCoverageContentTilesUsesArrangementClips()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto body = extractFunctionBlock(src, "void ArrangementViewComponent::prepareCoverageContentTiles");

    expect(!body.empty(), "prepareCoverageContentTiles must be found");
    expect(contains(body, "ContentSlot::ArrangementClips"),
           "prepareCoverageContentTiles must use ContentSlot::ArrangementClips");
    expect(contains(body, "contentMetrics_.revision"),
           "prepareCoverageContentTiles must use contentMetrics_.revision");
}

void selectPlacementClearsOtherTracks()
{
    const auto src = readText("Source/StandaloneArrangement.cpp");
    const auto selectBody = extractFunctionBlock(src, "bool StandaloneArrangement::selectPlacement");
    const auto setIndexBody = extractFunctionBlock(src, "bool StandaloneArrangement::setSelectedPlacementIndex");

    expect(!selectBody.empty(), "StandaloneArrangement::selectPlacement must be found");
    expect(!setIndexBody.empty(), "StandaloneArrangement::setSelectedPlacementIndex must be found");

    // Both write paths must clear other tracks' selectedPlacementId before setting
    for (const auto* body : { &selectBody, &setIndexBody }) {
        expect(contains(*body, "kTrackCount"),
               "Must iterate all tracks to clear other selections");
        expect(contains(*body, "!= trackId"),
               "Must skip the target track when clearing");
    }
}

void captureApplyAudioBufferContractIsReferenceWithIdentityTimeGrid()
{
    const auto header = readText("Source/Content/CaptureSegmentContent.h");
    const auto impl = readText("Source/Content/CaptureSegmentContent.cpp");
    const auto body = extractFunctionBlock(impl, "void CaptureSegmentContent::applyAudioBuffer");

    expect(!body.empty(), "applyAudioBuffer body must be found");

    // Signature: reference, not pointer
    expect(!contains(header, "applyAudioBuffer(const juce::AudioBuffer<float>*"),
           "applyAudioBuffer must take reference, not pointer");

    // No nullptr / empty early return
    expect(!contains(body, "nullptr"),
           "applyAudioBuffer must not have nullptr early return");
    expect(!contains(body, "getNumSamples() == 0"),
           "applyAudioBuffer must not have empty-buffer early return");

    // Identity TimeGrid birth
    expect(contains(body, "TimeGridSnapshot::makeIdentity"),
           "applyAudioBuffer must generate identity TimeGridSnapshot");
    expect(contains(body, "++editable_.timeGridRevision"),
           "applyAudioBuffer must bump timeGridRevision");
    expect(contains(body, "editable_.timeGrid = TimeGridSnapshot::makeIdentity"),
           "applyAudioBuffer must assign identity TimeGrid to editable_.timeGrid");
}

void captureCallSitesDereferenceNotNullSharedPtr()
{
    const auto session = readText("Source/Plugin/Capture/CaptureSession.cpp");
    const auto persistence = readText("Source/Plugin/Capture/CapturePersistence.cpp");

    // No .get() call in applyAudioBuffer call sites
    expect(!contains(session, "applyAudioBuffer(pcm.get()"),
           "CaptureSession must not pass pcm.get() to applyAudioBuffer");
    expect(!contains(persistence, "applyAudioBuffer(p.audio.get()"),
           "CapturePersistence must not pass p.audio.get() to applyAudioBuffer");

    // Dereference pattern present
    expect(contains(session, "applyAudioBuffer(*pcm"),
           "CaptureSession must dereference pcm with *pcm");
    expect(contains(persistence, "applyAudioBuffer(*p.audio"),
           "CapturePersistence must dereference p.audio with *p.audio");
}

// ============================================================================
// PianoRoll ruler control footprint — pattern cache identity 只描述像素
// Time/Cont 按钮覆盖效果由 child component z-order 产生，不属于 tile cache
// ============================================================================

// Deleted: patternTileKeyExcludesRulerControlFootprint - tests removed TimelinePatternCache
// Deleted: renderParamsExcludesRulerControlFootprint - tests removed TimelinePatternCache
// Deleted: prepareCoveragePatternTilesNoFootprintOrTileViewportX - tests removed prepareCoveragePatternTiles

void buildPatternTileNoFootprintToRenderParams()
{
    const auto composer = readText("Source/Standalone/UI/TimelineLayerComposer.cpp");
    const auto buildBlock = extractFunctionBlock(
        composer, "juce::Image TimelineLayerComposer::buildPatternTile");

    expect(!buildBlock.empty(), "buildPatternTile must be found");
    expectNoTokens("buildPatternTile", buildBlock,
                   {"rulerControlFootprintX", "rulerControlFootprintY",
                    "rulerControlFootprintW", "rulerControlFootprintH"});
}

void drawTimeRulerHasRulerPaintBounds()
{
    const auto composer = readText("Source/Standalone/UI/TimelineLayerComposer.cpp");
    const auto drawBlock = extractFunctionBlock(
        composer, "void TimelineLayerComposer::drawTimeRuler");

    expect(!drawBlock.empty(), "drawTimeRuler must be found");
    expectTokens("drawTimeRuler uses rulerPaintBounds",
                 drawBlock,
                 {"rulerPaintBounds", "getWidth()"});
    expectNoTokens("drawTimeRuler no footprint",
                   drawBlock,
                   {"rulerControlFootprint"});
}

void noArrangementClipExclusionLeakedToPianoRoll()
{
    const auto component = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto header = readText("Source/Standalone/UI/PianoRollComponent.h");

    expectNoTokens("PianoRollComponent.cpp", component,
                   {"rulerClipExclusion", "buttonClipExclusion", "rulerButtonClip",
                    "exclusionRect", "clipExclusionRect"});
    expectNoTokens("PianoRollComponent.h", header,
                   {"rulerClipExclusion", "buttonClipExclusion", "rulerButtonClip",
                    "exclusionRect", "clipExclusionRect"});
}

} // namespace

int main()
{
    std::cout << "=== OpenTune Architecture Tests ===\n\n";

    try {
        contentSlotNotesIsRestored();
        cachedContentTilesOwnNotes();
        noteAndPitchRevisionPollingIsIndependent();
        notePatchCommitReturnsAuthoritativeSnapshot();
        noLegacyNoteInteractionState();

        // Arrangement selection publication contract
        selectedFieldsOnlyWrittenInCommitHelpers();
        placementSelectionChangedOnlyInCommitHelpers();
        setMutatingHelpersArePure();
        moveBranchHasNoBareRepaint();
        togglePlacementSelectionAllowsLastItemToggle();
        commitEmptyClearsModelSelection();
        noSelectPlacementOutsideCommitHelpers();
        selectPlacementClearsOtherTracks();
        rebuildContentMetricsUsesIsPlacementSelected();
        prepareCoverageContentTilesUsesArrangementClips();

        // Capture audio buffer → identity TimeGrid contract (VST3 sync with Standalone fix eaf3bf7)
        captureApplyAudioBufferContractIsReferenceWithIdentityTimeGrid();
        captureCallSitesDereferenceNotNullSharedPtr();

        // PianoRoll ruler control footprint — tile cache identity 只描述像素
        // Deleted: patternTileKeyExcludesRulerControlFootprint - TimelinePatternCache removed
        // Deleted: renderParamsExcludesRulerControlFootprint - TimelinePatternCache removed
        // Deleted: prepareCoveragePatternTilesNoFootprintOrTileViewportX - prepareCoveragePatternTiles removed
        buildPatternTileNoFootprintToRenderParams();
        drawTimeRulerHasRulerPaintBounds();
        noArrangementClipExclusionLeakedToPianoRoll();
    } catch (const std::exception& e) {
        ++failures;
        std::cout << "[FAIL] uncaught exception: " << e.what() << "\n";
    }

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "ALL ARCHITECTURE TESTS PASSED\n";
        return 0;
    }

    std::cout << failures << " ARCHITECTURE TEST(S) FAILED\n";
    return 1;
}
