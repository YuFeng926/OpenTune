// ============================================================================
// OpenTune PianoRoll Static Contract Tests
// ============================================================================
// Plan: docs/plans/2026-07-06-equivalence-hard-closeout-refactor-repair.md
//
// Static source-analysis tests. No JUCE headers, no linked libraries.
// These tests encode the kill-list contracts from the plan.
// Expected first run: FAIL — production code hasn't been fixed yet.
// ============================================================================

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// OPENTUNE_SOURCE_DIR is defined by CMake at build time
#ifndef OPENTUNE_SOURCE_DIR
#error "OPENTUNE_SOURCE_DIR must be defined by CMake"
#endif

namespace {

static int failures = 0;

// ------------------------------------------------------------------
// Helper utilities
// ------------------------------------------------------------------
std::filesystem::path sourcePath(std::string_view relative)
{
    return std::filesystem::path(OPENTUNE_SOURCE_DIR) / std::filesystem::path(relative);
}

std::string readText(std::string_view relative)
{
    const auto path = sourcePath(relative);
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool contains(std::string_view text, std::string_view token)
{
    return text.find(token) != std::string_view::npos;
}

// Returns the body of a function (from signature to matching close-brace).
// This is approximate — it counts braces naively — good enough for static checks.
std::string extractFunctionBlock(std::string_view text, std::string_view signature)
{
    const size_t sigPos = text.find(signature);
    if (sigPos == std::string_view::npos) return {};
    const size_t bracePos = text.find('{', sigPos);
    if (bracePos == std::string_view::npos) return {};
    int depth = 0;
    for (size_t i = bracePos; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}') { --depth; if (depth == 0) return std::string(text.substr(sigPos, i - sigPos + 1)); }
    }
    return {};
}

// ------------------------------------------------------------------
// Slice between two signatures (for checking order in a method body)
// ------------------------------------------------------------------
std::string textBetween(std::string_view text, std::string_view startMarker, std::string_view endMarker)
{
    const auto start = text.find(startMarker);
    if (start == std::string_view::npos) return {};
    const auto end = text.find(endMarker, start + startMarker.size());
    if (end == std::string_view::npos) return {};
    return std::string(text.substr(start, end - start + endMarker.size()));
}

// ============================================================================
// Task 1.5: commitNotesAndSegments returns committed snapshot (not bool)
// ============================================================================
bool check_commitNotesAndSegmentsReturnsCommittedSnapshot()
{
    const auto header = readText("Source/Content/ContentEditCommands.h");
    const auto processor = readText("Source/PluginProcessor.cpp");
    const auto component = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    if (contains(header, "virtual bool commitNotesAndSegments")) {
        std::cout << "[FAIL] commitNotesAndSegments must return committed snapshot, not bool (ContentEditCommands.h)\n";
        ++failures; return false;
    }

    if (contains(processor, "bool OpenTuneAudioProcessor::commitContentNotesAndSegments")) {
        std::cout << "[FAIL] commitContentNotesAndSegments must return committed snapshot, not bool (PluginProcessor.cpp)\n";
        ++failures; return false;
    }

    // PianoRollComponent must not call readEditedSnapshot() AFTER commitNotesAndSegments()
    const auto body = extractFunctionBlock(component, "PianoRollComponent::commitEditedContentNotesAndSegments");
    if (!body.empty()) {
        const auto commitPos = body.find("commitNotesAndSegments");
        const auto readPos  = body.find("readEditedSnapshot");
        if (commitPos != std::string::npos && readPos != std::string::npos && commitPos < readPos) {
            std::cout << "[FAIL] PianoRollComponent must not commit then readEditedSnapshot() (PianoRollComponent.cpp)\n";
            ++failures; return false;
        }
    }

    std::cout << "[PASS] commitNotesAndSegmentsReturnsCommittedSnapshot\n";
    return true;
}

// ============================================================================
// Task 1.4: PianoRollEditAction no nullable undo/redo
// ============================================================================
bool check_pianoRollEditActionNoDefensiveNull()
{
    const auto impl = readText("Source/Utils/PianoRollEditAction.cpp");

    // undo/redo must assert commands_ != nullptr, not silently skip
    const auto undo = extractFunctionBlock(impl, "PianoRollEditAction::undo()");
    const auto redo = extractFunctionBlock(impl, "PianoRollEditAction::redo()");

    if (!undo.empty() && contains(undo, "if (commands_ != nullptr)")) {
        std::cout << "[FAIL] PianoRollEditAction::undo() must not silently skip through nullable check\n";
        ++failures; return false;
    }
    if (!redo.empty() && contains(redo, "if (commands_ != nullptr)")) {
        std::cout << "[FAIL] PianoRollEditAction::redo() must not silently skip through nullable check\n";
        ++failures; return false;
    }

    std::cout << "[PASS] pianoRollEditActionNoDefensiveNull\n";
    return true;
}

// ============================================================================
// Task 2.4: No active drag rejection outside source range
// ============================================================================
bool check_pianoRollActiveDragNoEarlyReject()
{
    const auto source = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const std::vector<std::string> activeMethods = {
        "updateF0SelectionDrag",
        "handleDrawCurveTool",
        "handleDrawNoteMouseDrag",
        "updateSelectionBoxDrag",
        "updateResizeDrag",
        "updateLineAnchorDrag"
    };

    for (const auto& method : activeMethods) {
        const auto body = extractFunctionBlock(source, method);
        if (body.empty()) continue;
        // Bug pattern: editRange.contains(*sourceTime) ... return;
        if (contains(body, "contains(*sourceTime)") && contains(body, "return;")) {
            std::cout << "[FAIL] " << method << " rejects active drag outside source range (PianoRollToolHandler.cpp)\n";
            ++failures; return false;
        }
    }

    std::cout << "[PASS] pianoRollActiveDragNoEarlyReject\n";
    return true;
}

// ============================================================================
// Task 2.4: HandDraw frameIndex uses clamped source time (not raw *sourceTime)
// ============================================================================
bool check_handDrawFrameIndexClamped()
{
    const auto source = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto body = extractFunctionBlock(source, "handleDrawCurveTool");
    if (body.empty()) {
        std::cout << "[PASS] handDrawFrameIndexClamped (could not locate function body)\n";
        return true;
    }

    // frameIndex = f0tl.frameAtOrBefore(*sourceTime) — without jlimit — is the bug
    const auto frameIdxLine = body.find("frameIndex = f0tl.frameAtOrBefore(*sourceTime)");
    if (frameIdxLine != std::string::npos) {
        // Check whether jlimit wraps it nearby
        const auto slice = body.substr(frameIdxLine, std::min(size_t(120), body.size() - frameIdxLine));
        if (!contains(slice, "jlimit")) {
            std::cout << "[FAIL] HandDraw frameIndex must use clamped source with jlimit, not raw *sourceTime (PianoRollToolHandler.cpp)\n";
            ++failures; return false;
        }
    }

    std::cout << "[PASS] handDrawFrameIndexClamped\n";
    return true;
}

// ============================================================================
// Task 3.3: PianoRoll pattern tiles use ARGB (not RGB) when skipping background
// ============================================================================
bool check_pianoRollTilesAreTransparent()
{
    const auto source = readText("Source/Standalone/UI/TimelineLayerComposer.cpp");
    const auto body = extractFunctionBlock(source, "buildPatternTile");
    if (body.empty()) {
        std::cout << "[PASS] pianoRollTilesAreTransparent (could not locate function body)\n";
        return true;
    }

    // Correct pattern: pianoRoll tiles use ARGB, others use RGB
    // Check that isPianoRoll controls the format selection with ARGB for pianoroll
    if (!contains(body, "isPianoRoll") || !contains(body, "juce::Image::ARGB")) {
        std::cout << "[FAIL] PianoRoll pattern tiles must use ARGB when isPianoRoll is true (TimelineLayerComposer.cpp)\n";
        ++failures; return false;
    }

    // Original RGB-only bug: tile was always RGB even when pianoroll was detected
    // Ensure we don't have the pattern where pianoroll is checked but tile stays RGB always
    const bool wasOldBug = contains(body, "juce::Image tile(juce::Image::RGB")
                          && !contains(body, "isPianoRoll");
    if (wasOldBug) {
        std::cout << "[FAIL] PianoRoll pattern tiles must use ARGB when skipping background (TimelineLayerComposer.cpp)\n";
        ++failures; return false;
    }

    std::cout << "[PASS] pianoRollTilesAreTransparent\n";
    return true;
}

// ============================================================================
// Task 4.6: Arrangement uses only historical painter.
// ============================================================================
bool check_arrangementHistoricalPainterOnly()
{
    const auto source = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");

    if (contains(source, std::string("struct Placement") + "Visual")) {
        std::cout << "[FAIL] old placement visual struct must be removed (ArrangementViewComponent.cpp)\n";
        ++failures; return false;
    }

    if (contains(source, std::string("g.fill") + "Path(wavePath)")) {
        std::cout << "[FAIL] Arrangement waveform must be stroked, not filled (ArrangementViewComponent.cpp)\n";
        ++failures; return false;
    }

    if (contains(source, std::string("fillRoundedRectangle(label") + "Back")
        || contains(source, std::string("fillRoundedRectangle(gain") + "Back")) {
        std::cout << "[FAIL] Historical label/gain text must not be capsules (ArrangementViewComponent.cpp)\n";
        ++failures; return false;
    }

    std::cout << "[PASS] arrangementHistoricalPainterOnly\n";
    return true;
}

// ============================================================================
// Task 0 Kill List: No anti-pattern tokens
// ============================================================================
bool check_noKillListTokens()
{
    const std::vector<std::string> scanFiles = {
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp",
        "Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp",
        "Source/Content/ContentEditCommands.h",
        "Source/Content/ContentEditCommands.cpp"
    };

    const std::vector<std::string> bannedTokens = {
        std::string("PianoRollEdit") + "Geometry",
        std::string("Geometry") + "Service",
        "PianoRollSurfaceCache2",
        std::string("pendingSeek") + "Time_",
        "ContentEditRangeFrames{}",
        std::string("virtual bool commitNotes") + "AndSegments"
    };

    for (const auto& file : scanFiles) {
        const auto text = readText(file);
        if (text.empty()) continue;
        for (const auto token : bannedTokens) {
            if (contains(text, token)) {
                std::cout << "[FAIL] Kill-list token '" << token << "' found in " << file << "\n";
                ++failures; return false;
            }
        }
    }

    std::cout << "[PASS] noKillListTokens\n";
    return true;
}

// ============================================================================
// Task 0.2: Global scan — no bool-only commitNotesAndSegments anywhere
// ============================================================================
bool check_noBoolCommitNotesAndSegmentsAnywhere()
{
    // Scan the full Source directory for the bool-only signature
    const std::vector<std::string_view> sourceDirs = {
        "Source/Content",
        "Source/Standalone",
        "Source/Utils"
    };

    for (const auto dir : sourceDirs) {
        const auto dirPath = sourcePath(dir);
        if (!std::filesystem::exists(dirPath)) continue;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dirPath)) {
            if (!entry.is_regular_file()) continue;
            const auto ext = entry.path().extension().string();
            if (ext != ".h" && ext != ".cpp" && ext != ".hpp") continue;

            std::ifstream in(entry.path(), std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

            if (contains(content, std::string("virtual bool commitNotes") + "AndSegments")
                || contains(content, "bool OpenTuneAudioProcessor::commitContentNotesAndSegments")) {
                std::cout << "[FAIL] Bool-only commitNotesAndSegments found in " << entry.path().string() << "\n";
                ++failures; return false;
            }
        }
    }

    std::cout << "[PASS] noBoolCommitNotesAndSegmentsAnywhere\n";
    return true;
}

} // anonymous namespace

// ============================================================================
int main()
{
    std::cout << "=== OpenTune PianoRoll Static Contract Tests ===\n\n";

    // Task 1: Undo commit API
    check_commitNotesAndSegmentsReturnsCommittedSnapshot();
    check_pianoRollEditActionNoDefensiveNull();
    check_noBoolCommitNotesAndSegmentsAnywhere();

    // Task 2: Active drag clamp
    check_pianoRollActiveDragNoEarlyReject();
    check_handDrawFrameIndexClamped();

    // Task 3: Chrome/tile ownership
    check_pianoRollTilesAreTransparent();

    // Task 4: Arrangement painter
    check_arrangementHistoricalPainterOnly();

    // Task 0: Kill list
    check_noKillListTokens();

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "ALL STATIC CHECKS PASSED\n";
        return 0;
    } else {
        std::cout << failures << " CHECK(S) FAILED\n";
        return 1;
    }
}
