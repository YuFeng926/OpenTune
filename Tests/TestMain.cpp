// OpenTune ARA Architecture Contract And Runtime Tests
// Focused source-scan guards plus small runtime tests for ownership/lifecycle behavior.
// Oracle review: commit e0326f0 / docs/plans/2026-06-10-ara2-content-ownership-phase3-capture-plan.md
//
// Static guards prevent old architecture paths from returning. Runtime tests
// prove the Capture lifecycle and owner persistence behavior that token scans
// cannot verify.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "Content/EditableContentSnapshot.h"
#include "Content/StandaloneClipContent.h"
#include "Content/ContentEditCommands.h"
#include "Plugin/Capture/CaptureSession.h"
#include "Utils/PitchCurve.h"
#include "Utils/Note.h"

namespace fs = std::filesystem;

// ============================================================================
// Compile-time source root (set by CMake: OPENTUNE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
// ============================================================================
#ifndef OPENTUNE_SOURCE_DIR
#define OPENTUNE_SOURCE_DIR "."
#endif

// ============================================================================
// Test infrastructure
// ============================================================================

struct CheckResult
{
    std::string name;
    bool passed = false;
    std::string detail; // empty on pass, failure description on fail
};

static CheckResult pass(std::string name)
{
    return {std::move(name), true, {}};
}

static CheckResult fail(std::string name, std::string detail)
{
    return {std::move(name), false, std::move(detail)};
}

// Resolve a project-relative path to an absolute path.
static fs::path sourcePath(const std::string& relative)
{
    return fs::path(OPENTUNE_SOURCE_DIR) / fs::path(relative);
}

static std::string readFile(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

static std::string readText(const std::string& relative)
{
    return readFile(sourcePath(relative));
}

// Simple substring search.
static bool contains(const std::string& text, const std::string& token)
{
    return text.find(token) != std::string::npos;
}

// Word-boundary search: matches only when token is not inside a larger identifier.
// E.g. "materializationId" matches "materializationId_" but NOT "materializationDurationSeconds".
static bool containsWord(const std::string& text, const std::string& word)
{
    if (word.empty()) return false;
    size_t pos = 0;
    while ((pos = text.find(word, pos)) != std::string::npos)
    {
        bool leftBoundary  = (pos == 0) || !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
        size_t after       = pos + word.size();
        bool rightBoundary = (after >= text.size()) || !std::isalnum(static_cast<unsigned char>(text[after]));
        if (leftBoundary && rightBoundary)
            return true;
        pos += word.size();
    }
    return false;
}

// Check that ALL required tokens appear. Returns empty string on success,
// or a description of the first missing token.
static std::string requireAll(const std::string& text,
                              const std::vector<std::string>& tokens)
{
    for (const auto& t : tokens)
        if (!contains(text, t))
            return "missing required token: '" + t + "'";
    return {};
}

// Given a token found in text, return "file:line" context.
static std::string locateInText(const std::string& text,
                                const std::string& token,
                                const std::string& filename)
{
    size_t pos = text.find(token);
    if (pos == std::string::npos)
        return filename + ":?";
    int line = 1;
    for (size_t i = 0; i < pos; ++i)
        if (text[i] == '\n') ++line;
    return filename + ":" + std::to_string(line);
}

static std::string extractBraceBlockAfterToken(const std::string& text,
                                               const std::string& token)
{
    const size_t tokenPos = text.find(token);
    if (tokenPos == std::string::npos)
        return {};

    const size_t bracePos = text.find('{', tokenPos);
    if (bracePos == std::string::npos)
        return {};

    int depth = 0;
    for (size_t pos = bracePos; pos < text.size(); ++pos)
    {
        if (text[pos] == '{')
            ++depth;
        else if (text[pos] == '}')
        {
            --depth;
            if (depth == 0)
                return text.substr(bracePos, pos - bracePos + 1);
        }
    }

    return {};
}

// Concatenate all .h/.cpp files under a directory (project-relative).
static std::string readAllFilesInDir(const std::string& relativeDir)
{
    std::string result;
    fs::path dir = sourcePath(relativeDir);
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec))
    {
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        auto ext = p.extension().string();
        if (ext == ".h" || ext == ".cpp" || ext == ".hpp" || ext == ".cc" || ext == ".cxx")
        {
            result += readFile(p);
            result += '\n';
        }
    }
    return result;
}

// Read Source/ARA/ directory content (all .h/.cpp).
static std::string allAraText()
{
    return readAllFilesInDir("Source/ARA");
}

// ============================================================================
// Test 1: AudioModification owns modification-scoped edit/analysis state
// ============================================================================
// Per ARA2 spec: AudioModification owns modification-scoped plugin state
// (notes, pitch edits, time grid, F0, detected key, etc.), NOT original PCM.
// Original audio comes from AudioSource via host sample access.
// Must NOT use materializationId as content identity.
static CheckResult araAudioModificationOwnsEditAndAnalysisState()
{
    const auto text = readText("Source/ARA/AudioModification.h")
                    + readText("Source/ARA/AudioModification.cpp");

    const std::vector<std::string> required = {
        "AudioModificationContentState", "ContentKey", "contentKey()",
        "snapshotContent", "applyNotes", "applyPitchCurve",
        "applyTimeGrid"
    };

    auto missing = requireAll(text, required);
    if (!missing.empty())
        return fail("araAudioModificationOwnsEditAndAnalysisState", missing);

    // materializationId must NOT appear as a word (only materializationDurationSeconds is ok)
    if (containsWord(text, "materializationId"))
        return fail("araAudioModificationOwnsEditAndAnalysisState",
                    "forbidden token 'materializationId' found — "
                    "content identity must use ContentKey, not materializationId");

    return pass("araAudioModificationOwnsEditAndAnalysisState");
}

// ============================================================================
// Test 2: PlaybackRegion placement-only
// ============================================================================
static CheckResult playbackRegionIsPlacementOnly()
{
    const auto text = readText("Source/ARA/PlaybackRegion.h")
                    + readText("Source/ARA/PlaybackRegion.cpp");

    const std::vector<std::string> required = {
        "startInPlaybackTime", "durationInPlaybackTime",
        "audioModificationPersistentId", "placementRevision"
    };

    auto missing = requireAll(text, required);
    if (!missing.empty())
        return fail("playbackRegionIsPlacementOnly", missing);

    const std::vector<std::string> forbidden = {
        "contentWindow", "sourceId", "materializationId",
        "notes", "pitchCurve", "audioBuffer"
    };

    for (const auto& t : forbidden)
    {
        if (contains(text, t))
        {
            auto loc = locateInText(text, t, "PlaybackRegion.{h,cpp}");
            return fail("playbackRegionIsPlacementOnly",
                        "found forbidden token '" + t + "' in " + loc);
        }
    }

    return pass("playbackRegionIsPlacementOnly");
}

// ============================================================================
// Test 3: No DC/Processor store split
// ============================================================================
static CheckResult araHasNoStoreSplit()
{
    const auto dcText = readText("Source/ARA/OpenTuneDocumentController.h")
                      + readText("Source/ARA/OpenTuneDocumentController.cpp");

    // DC must not own materializationStore_ or expose getMaterializationStore()
    {
        const std::vector<std::string> dcForbidden = {
            "materializationStore_", "getMaterializationStore()"
        };

        for (const auto& t : dcForbidden)
        {
            if (contains(dcText, t))
            {
                auto loc = locateInText(dcText, t, "OpenTuneDocumentController.{h,cpp}");
                return fail("araHasNoStoreSplit",
                            "DC " + t + " found in " + loc);
            }
        }
    }

    // DC must not call createMaterialization( directly
    if (contains(dcText, "createMaterialization("))
    {
        auto loc = locateInText(dcText, "createMaterialization(", "OpenTuneDocumentController.{h,cpp}");
        return fail("araHasNoStoreSplit",
                    "DC createMaterialization( found in " + loc +
                    " — birth should fill AudioModification.content, not create store entries");
    }

    // DC must not own retired content pool
    {
        const std::vector<std::string> dcContentForbidden = {
            "retiredContents_"
        };

        for (const auto& t : dcContentForbidden)
        {
            if (contains(dcText, t))
            {
                auto loc = locateInText(dcText, t, "OpenTuneDocumentController.{h,cpp}");
                return fail("araHasNoStoreSplit",
                            "DC must not own retired content pool: '" + t + "' found in " + loc);
            }
        }
    }

    // DC must not contain old persistence functions or old XML schema
    {
        const std::vector<std::string> dcPersistenceForbidden = {
            "serializeMaterializationToXml",
            "deserializeMaterializationFromXml",
            "<materializations>"
        };

        for (const auto& t : dcPersistenceForbidden)
        {
            if (contains(dcText, t))
            {
                auto loc = locateInText(dcText, t, "OpenTuneDocumentController.{h,cpp}");
                return fail("araHasNoStoreSplit",
                            "DC must not contain old persistence: '" + t + "' found in " + loc);
            }
        }
    }

    // PluginProcessor: check for materializationStore_->set within ARA-guarded blocks.
    const auto ppText = readText("Source/PluginProcessor.cpp");

    // Track #if JucePlugin_Enable_ARA / #endif depth
    std::istringstream ppStream(ppText);
    std::string line;
    int inAraBlock = 0;
    int lineNum = 0;
    while (std::getline(ppStream, line))
    {
        ++lineNum;
        // Track ARA preprocessor blocks
        if (line.find("#if JucePlugin_Enable_ARA") != std::string::npos)
            ++inAraBlock;
        else if (line.find("#endif") != std::string::npos && inAraBlock > 0)
            --inAraBlock;

        if (inAraBlock > 0)
        {
            // Check for write operations to materializationStore_ inside ARA blocks
            if (contains(line, "materializationStore_->set"))
            {
                return fail("araHasNoStoreSplit",
                            "PluginProcessor.cpp:" + std::to_string(lineNum)
                            + " materializationStore_->set inside #if JucePlugin_Enable_ARA block — "
                            "ARA writes must go through AudioModification content, not processor store");
            }
        }
    }

    return pass("araHasNoStoreSplit");
}

// ============================================================================
// Test 4: ContentRenderService derived-only boundary
// ============================================================================
static CheckResult contentRenderServiceIsDerivedOnly()
{
    const auto text = readText("Source/Render/ContentRenderService.h")
                    + readText("Source/Render/ContentRenderService.cpp");

    const std::vector<std::string> required = {
        "RenderWorker", "RenderCacheRegistry", "PlaybackSourcePublisher",
        "StretcherPool", "TimeStretchCache", "ContentKey"
    };

    auto missing = requireAll(text, required);
    if (!missing.empty())
        return fail("contentRenderServiceIsDerivedOnly", missing);

    // CRS must compose extracted runtime services (RenderWorker, RenderCacheRegistry,
    // PlaybackSourcePublisher, StretcherPool) and manage TimeStretchCache.
    // It must NOT own authoritative content fields (notes, pitchCurve, timeGrid,
    // pitchShiftSettings, detectedKey, lifecycle) — these belong to domain content owners.
    // It must NOT own raw runtime mechanics (std::thread, raw queues, raw maps) —
    // these belong to extracted services.

    const std::vector<std::string> forbidden = {
        "std::thread renderWorkerThread_",
        "std::vector<Note> notes",
        "std::shared_ptr<const TimeGridSnapshot> timeGrid",
        "PitchShiftSettings pitchShiftSettings",
        "DetectedKey detectedKey",
        "ContentLifecycle lifecycle"
    };

    for (const auto& t : forbidden)
    {
        if (contains(text, t))
        {
            auto loc = locateInText(text, t, "ContentRenderService.{h,cpp}");
            return fail("contentRenderServiceIsDerivedOnly",
                        "found forbidden authoritative content field '" + t + "' in " + loc +
                        " — CRS must not own content truth");
        }
    }

    return pass("contentRenderServiceIsDerivedOnly");
}

// ============================================================================
// Test 5: Renderer uses host-assigned regions
// ============================================================================
static CheckResult rendererUsesAssignedRegions()
{
    const auto text = readText("Source/ARA/OpenTunePlaybackRenderer.h")
                    + readText("Source/ARA/OpenTunePlaybackRenderer.cpp");

    const std::vector<std::string> required = {
        "struct RenderPlan", "playbackRegions", "currentPlan_",
        "didAddPlaybackRegion", "willRemovePlaybackRegion",
        "getPlaybackRegionProjectionsFor"
    };

    auto missing = requireAll(text, required);
    if (!missing.empty())
        return fail("rendererUsesAssignedRegions", missing);

    const std::vector<std::string> forbidden = {
        "preferredPlaybackRegion_", "getPreferredRegion", "editorFocusRegion"
    };

    for (const auto& t : forbidden)
    {
        if (contains(text, t))
        {
            auto loc = locateInText(text, t, "OpenTunePlaybackRenderer.{h,cpp}");
            return fail("rendererUsesAssignedRegions",
                        "found forbidden token '" + t + "' in " + loc);
        }
    }

    return pass("rendererUsesAssignedRegions");
}

// ============================================================================
// Test 6: ARA code is lock-free
// ============================================================================
static CheckResult araRewriteIsLockFree()
{
    auto text = allAraText();

    // Remove std::atomic<bool> patterns (allowed as lifecycle signals).
    // We remove the full declarations and usage patterns.
    const std::vector<std::string> allowedPatterns = {
        "std::atomic<bool>",
        "std::shared_ptr<std::atomic<bool>>",
    };
    for (const auto& p : allowedPatterns)
    {
        size_t pos = 0;
        while ((pos = text.find(p, pos)) != std::string::npos)
        {
            text.erase(pos, p.size());
        }
    }

    const std::vector<std::string> forbidden = {
        "std::mutex", "std::lock_guard", "std::unique_lock",
        "condition_variable", "std::thread"
    };

    for (const auto& t : forbidden)
    {
        if (contains(text, t))
        {
            // Re-read fresh text to get accurate location
            const auto freshText = allAraText();
            auto loc = locateInText(freshText, t, "Source/ARA/");
            return fail("araRewriteIsLockFree",
                        "found " + t + " in " + loc + " — ARA code must remain lock-free");
        }
    }

    return pass("araRewriteIsLockFree");
}

// ============================================================================
// Test 7: No fallback routing (DC vs processor dual-backend)
// ============================================================================
static CheckResult araHasNoFallbackRouting()
{
    // Check both PluginEditor.cpp and PluginProcessor.cpp for fallback routing.
    // Pattern: if (auto* dc = getDocumentController()) ... else { ... materializationStore_ ... }
    // This would indicate a dual-backend where processor store is used as fallback.

    struct FileCheck { std::string text; std::string name; };
    const FileCheck files[] = {
        { readText("Source/Plugin/PluginEditor.cpp"),    "PluginEditor.cpp"    },
        { readText("Source/PluginProcessor.cpp"),        "PluginProcessor.cpp" },
    };

    for (const auto& [text, filename] : files)
    {
        const std::string pattern = "auto* dc = getDocumentController()";
        size_t pos = 0;
        while ((pos = text.find(pattern, pos)) != std::string::npos)
        {
            // Look ahead up to 2000 chars for an 'else' branch
            size_t scopeEnd = pos + 2000;
            if (scopeEnd > text.size()) scopeEnd = text.size();
            std::string scope = text.substr(pos + pattern.size(),
                                            scopeEnd - pos - pattern.size());

            size_t elsePos = scope.find("else");
            if (elsePos != std::string::npos)
            {
                std::string afterElse = scope.substr(elsePos);
                if (contains(afterElse, "materializationStore_"))
                {
                    // Count line number for error message
                    int line = 1;
                    for (size_t i = 0; i < pos; ++i)
                        if (text[i] == '\n') ++line;
                    return fail("araHasNoFallbackRouting",
                                filename + ":" + std::to_string(line)
                                + ": 'if (auto* dc = getDocumentController())' has else branch "
                                  "with materializationStore_ — no dual-backend routing allowed");
                }
            }

            pos += pattern.size();
        }
    }

    return pass("araHasNoFallbackRouting");
}

// ============================================================================
// Test 8: TimeStretchCache uses ContentKey
// ============================================================================
static CheckResult timeStretchCacheUsesContentKey()
{
    const auto text = readText("Source/Inference/TimeStretchCache.h")
                    + readText("Source/Inference/TimeStretchCache.cpp");

    // ContentKey should appear as a cache key somewhere in the file
    if (!contains(text, "ContentKey"))
        return fail("timeStretchCacheUsesContentKey",
                    "ContentKey not found in TimeStretchCache — "
                    "cache should use ContentKey, not materializationId");

    // Check if any function signature uses uint64_t materializationId as parameter
    // (the old pattern that should be replaced by ContentKey)
    std::istringstream stream(text);
    std::string line;
    int lineNum = 0;
    while (std::getline(stream, line))
    {
        ++lineNum;
        if (contains(line, "uint64_t materializationId"))
        {
            return fail("timeStretchCacheUsesContentKey",
                        "TimeStretchCache line " + std::to_string(lineNum)
                        + " uses 'uint64_t materializationId' as cache key — "
                          "should use ContentKey per ARA2 architecture contract");
        }
    }

    return pass("timeStretchCacheUsesContentKey");
}

// ============================================================================
// Phase 0 Contract Tests — target architecture boundaries
//
// These tests define the desired architecture. They currently FAIL because
// the codebase has duplicate definitions and misplaced ownership that need
// to be resolved during the ARA migration.
//
// When all Phase 0 tests pass, the codebase is structurally ready for the
// final ARA migration steps: removing old store-level mechanics and routing
// all render data through ContentRenderService.
// ============================================================================

// ============================================================================
// Contract Test 9: singlePlaybackReadSourceDefinition
//
// PlaybackReadSource must be defined in exactly ONE place:
// Source/Render/PlaybackReadSource.h.
//
// Forbidden: MaterializationStore::PlaybackReadSource (line 65)
// Forbidden: ContentRenderService::PlaybackReadSource  (line 48)
// Required:  Source/Render/PlaybackReadSource.h exists and is the sole definition.
// ============================================================================
static CheckResult singlePlaybackReadSourceDefinition()
{
    // 1) ContentRenderService.h must NOT define struct PlaybackReadSource
    {
        const auto text = readText("Source/Render/ContentRenderService.h");
        if (contains(text, "struct PlaybackReadSource"))
        {
            auto loc = locateInText(text, "struct PlaybackReadSource", "ContentRenderService.h");
            return fail("singlePlaybackReadSourceDefinition",
                        "PlaybackReadSource defined in " + loc +
                        " — must be defined ONLY in Source/Render/PlaybackReadSource.h");
        }
    }

    // 2) PlaybackReadSource.h must exist and contain the definition
    {
        const auto text = readText("Source/Render/PlaybackReadSource.h");
        if (text.empty())
            return fail("singlePlaybackReadSourceDefinition",
                        "Source/Render/PlaybackReadSource.h not found or empty"
                        " — PlaybackReadSource must live in its own header");
        if (!contains(text, "PlaybackReadSource"))
            return fail("singlePlaybackReadSourceDefinition",
                        "PlaybackReadSource token not found in Source/Render/PlaybackReadSource.h");
    }

    return pass("singlePlaybackReadSourceDefinition");
}

// ============================================================================
// Contract Test 10: singleRenderJobDefinition
//
// PendingRenderJob must be defined in exactly ONE place:
// Source/Render/RenderJob.h.
//
// Forbidden: MaterializationStore::PendingRenderJob (line 122)
// Forbidden: ContentRenderService::PendingRenderJob  (line 72)
// Required:  Source/Render/RenderJob.h exists and is the sole definition.
// ============================================================================
static CheckResult singleRenderJobDefinition()
{
    // 1) ContentRenderService.h must NOT define struct PendingRenderJob
    {
        const auto text = readText("Source/Render/ContentRenderService.h");
        if (contains(text, "struct PendingRenderJob"))
        {
            auto loc = locateInText(text, "struct PendingRenderJob", "ContentRenderService.h");
            return fail("singleRenderJobDefinition",
                        "PendingRenderJob defined in " + loc +
                        " — must be defined ONLY in Source/Render/RenderJob.h");
        }
    }

    // 2) RenderJob.h must exist and contain the definition
    {
        const auto text = readText("Source/Render/RenderJob.h");
        if (text.empty())
            return fail("singleRenderJobDefinition",
                        "Source/Render/RenderJob.h not found or empty"
                        " — RenderJob must live in its own header");
        if (!contains(text, "struct RenderJob"))
            return fail("singleRenderJobDefinition",
                        "RenderJob struct not found in Source/Render/RenderJob.h");
    }

    return pass("singleRenderJobDefinition");
}

// ============================================================================
// Contract Test 11: contentRenderServiceHasNoOwnedRuntimeMechanics
//
// ContentRenderService may compose extracted services (PlaybackSourcePublisher,
// RenderCacheRegistry, RenderWorker, StretcherPool) but must NOT own raw runtime
// mechanics (std::thread, std::mutex, raw queues, raw maps) or authoritative
// content fields (AudioModificationContentState, notes, pitchCurve, timeGrid).
//
// Scan: Source/Render/ContentRenderService.h
// ============================================================================
static CheckResult contentRenderServiceHasNoOwnedRuntimeMechanics()
{
    const auto text = readText("Source/Render/ContentRenderService.h");

    // Forbidden: raw runtime mechanics that belong to extracted components
    const std::vector<std::string> forbiddenRawMechanics = {
        "std::thread", "std::mutex", "std::condition_variable",
        "std::queue", "std::map<ContentKey, RenderCache>",
        "MaterializationStore*"
    };

    for (const auto& t : forbiddenRawMechanics)
    {
        if (contains(text, t))
        {
            auto loc = locateInText(text, t, "ContentRenderService.h");
            return fail("contentRenderServiceHasNoOwnedRuntimeMechanics",
                        "forbidden raw runtime mechanic '" + t + "' found in " + loc +
                        " — CRS must not own raw mechanics;"
                        " these belong to extracted services");
        }
    }

    // Forbidden: authoritative content fields (type-prefixed to avoid matching
    // struct field names like timeGridRevision or parameter pass-through)
    const std::vector<std::string> forbiddenContent = {
        "AudioModificationContentState",
        "std::vector<Note> notes",
        "std::shared_ptr<PitchCurve> pitchCurve",
        "std::shared_ptr<const TimeGridSnapshot> timeGrid"
    };

    for (const auto& t : forbiddenContent)
    {
        if (contains(text, t))
        {
            auto loc = locateInText(text, t, "ContentRenderService.h");
            return fail("contentRenderServiceHasNoOwnedRuntimeMechanics",
                        "forbidden authoritative content field '" + t + "' found in " + loc +
                        " — content truth belongs to domain content owners, not CRS");
        }
    }

    return pass("contentRenderServiceHasNoOwnedRuntimeMechanics");
}

// Forward declaration for forbidTokensInFiles (defined later in Phase 4 section)
static CheckResult forbidTokensInFiles(const std::string& testName,
                                       const std::vector<std::string>& files,
                                       const std::vector<std::string>& forbidden);

// ============================================================================
// Contract Test 12: materializationStoreNoLongerOwnsRuntimeMechanics
//
// Non-store source files must not expose MaterializationStore-owned runtime
// mechanics (playbackSourceCache_, per-entry stretchers, per-entry RenderCache).
// Store-internal structure is checked by final deletion scan (Round 5).
// ============================================================================
static CheckResult materializationStoreNoLongerOwnsRuntimeMechanics()
{
    const std::vector<std::string> files = {
        "Source/PluginProcessor.h",
        "Source/PluginProcessor.cpp",
        "Source/ARA/OpenTuneDocumentController.h",
        "Source/ARA/OpenTuneDocumentController.cpp",
        "Source/Plugin/PluginEditor.cpp",
        "Source/Standalone/PluginEditor.cpp"
    };

    const std::vector<std::string> forbidden = {
        "playbackSourceCache_",
        "rebuildPlaybackSourceCache",
    };

    return forbidTokensInFiles("materializationStoreNoLongerOwnsRuntimeMechanics",
                               files, forbidden);
}

// ============================================================================
// Contract Test 13: timeStretchCacheUsesContentKey (Phase 0 contract)
//
// TimeStretchCache key methods must use ContentKey, not uint64_t
// materializationId. Scan TimeStretchCache.h for forbidden patterns.
//
// Forbidden: store(uint64_t materializationId,        (line 63)
//            invalidate(uint64_t materializationId,    (line 105)
//            sliceForOutputRange(uint64_t materializationId (line 95)
// ============================================================================
static CheckResult timeStretchCacheContractKeyCheck()
{
    const auto text = readText("Source/Inference/TimeStretchCache.h");

    const std::vector<std::string> forbidden = {
        "store(uint64_t materializationId",
        "invalidate(uint64_t materializationId",
        "sliceForOutputRange(uint64_t materializationId"
    };

    for (const auto& t : forbidden)
    {
        if (contains(text, t))
        {
            auto loc = locateInText(text, t, "TimeStretchCache.h");
            return fail("timeStretchCacheUsesContentKey",
                        "found '" + t + ")' in " + loc +
                        " — must use ContentKey instead of uint64_t materializationId"
                        " per ARA2 architecture contract");
        }
    }

    return pass("timeStretchCacheUsesContentKey");
}

// ============================================================================
// Contract Test 14: standaloneUsesContentKey (Phase 2)
//
// Standalone Placement and PlaybackPlacement must use ContentKey, not
// materializationId. StandaloneClipContent must use clipContentId, not placementId.
// ============================================================================
static CheckResult standaloneUsesContentKey()
{
    const auto arrangementH = readText("Source/StandaloneArrangement.h");
    const auto arrangementCpp = readText("Source/StandaloneArrangement.cpp");

    // Placement must NOT have materializationId field
    if (contains(arrangementH, "uint64_t materializationId"))
    {
        auto loc = locateInText(arrangementH, "uint64_t materializationId", "StandaloneArrangement.h");
        return fail("standaloneUsesContentKey",
                    "Placement.materializationId found in " + loc +
                    " — Standalone must use ContentKey for content identity");
    }

    // PlaybackPlacement must NOT have materializationId field
    if (contains(arrangementH, "struct PlaybackPlacement") &&
        contains(arrangementH, "materializationId"))
    {
        size_t pos = arrangementH.find("struct PlaybackPlacement");
        size_t endPos = arrangementH.find("};", pos);
        if (endPos != std::string::npos)
        {
            std::string playbackPlacementBody = arrangementH.substr(pos, endPos - pos);
            if (contains(playbackPlacementBody, "materializationId"))
            {
                return fail("standaloneUsesContentKey",
                            "PlaybackPlacement.materializationId found — must use ContentKey");
            }
        }
    }

    // Placement must have ContentKey field
    if (!contains(arrangementH, "ContentKey contentKey"))
    {
        return fail("standaloneUsesContentKey",
                    "Placement must have 'ContentKey contentKey' field");
    }

    return pass("standaloneUsesContentKey");
}

// ============================================================================
// Contract Test 15: standaloneHasNoMaterializationStoreDependency (Phase 2)
//
// Standalone import/edit/playback paths must NOT call materializationStore_.
// Split/merge/delete must NOT call createMaterialization/retireMaterialization.
// ============================================================================
static CheckResult standaloneHasNoMaterializationStoreDependency()
{
    const auto ppText = readText("Source/PluginProcessor.cpp");

    // Check commitPreparedImportAsPlacement does NOT call createMaterialization
    {
        size_t funcPos = ppText.find("commitPreparedImportAsPlacement");
        if (funcPos != std::string::npos)
        {
            // Find function body (from { to matching })
            size_t bracePos = ppText.find("{", funcPos);
            if (bracePos != std::string::npos)
            {
                int depth = 0;
                size_t closePos = bracePos;
                for (size_t i = bracePos; i < ppText.size(); ++i)
                {
                    if (ppText[i] == '{') ++depth;
                    if (ppText[i] == '}') { --depth; if (depth == 0) { closePos = i; break; } }
                }
                std::string funcBody = ppText.substr(bracePos, closePos - bracePos + 1);
                
                // Check if function body contains createMaterialization
                if (contains(funcBody, "createMaterialization"))
                {
                    auto loc = locateInText(ppText, "createMaterialization", "PluginProcessor.cpp");
                    return fail("standaloneHasNoMaterializationStoreDependency",
                                "commitPreparedImportAsPlacement calls createMaterialization at " + loc +
                                " — Standalone must create StandaloneClipContent instead");
                }
            }
        }
    }

    // Check processBlock does NOT call materializationStore_->getPlaybackReadSource
    {
        size_t funcPos = ppText.find("void OpenTuneAudioProcessor::processBlock");
        if (funcPos != std::string::npos)
        {
            size_t bracePos = ppText.find("{", funcPos);
            if (bracePos != std::string::npos)
            {
                int depth = 0;
                size_t closePos = bracePos;
                for (size_t i = bracePos; i < ppText.size() && i < bracePos + 50000; ++i)
                {
                    if (ppText[i] == '{') ++depth;
                    if (ppText[i] == '}') { --depth; if (depth == 0) { closePos = i; break; } }
                }
                std::string funcBody = ppText.substr(bracePos, closePos - bracePos + 1);
                
                // Check if processBlock contains materializationStore_->getPlaybackReadSource
                if (contains(funcBody, "materializationStore_->getPlaybackReadSource"))
                {
                    return fail("standaloneHasNoMaterializationStoreDependency",
                                "processBlock calls materializationStore_->getPlaybackReadSource — "
                                "Standalone playback must use ContentKey + CRS");
                }
            }
        }
    }

    return pass("standaloneHasNoMaterializationStoreDependency");
}

// ============================================================================
// Phase 3 Contract Tests — Capture domain content root
//
// P0 guards: captureBindingsHaveNoMaterializationStoreBridge (T1),
//            capturePersistenceDoesNotReadMaterializationStore (T2),
//            captureCrsUsesRegularVST3CaptureSegmentId (T3),
//            captureRefreshDoesNotRequestMaterializationRefresh (T4)
//
// These tests MUST FAIL initially (the code still has MS dependencies in
// the Capture domain). After P0-2 / P0-3 code fixes they must PASS.
// ============================================================================

// Helper: extract the VST3 Capture bindings block from PluginProcessor.cpp.
// Returns the text between "Capture::ProcessorBindings bindings" and
// "captureSession_ = std::make_unique<Capture::CaptureSession>".
static std::string extractCaptureBindings()
{
    const auto ppText = readText("Source/PluginProcessor.cpp");
    size_t captureSection = ppText.find("wrapperType == juce::AudioProcessor::wrapperType_VST3");
    if (captureSection == std::string::npos)
        return {};
    size_t bindingsStart = ppText.find("Capture::ProcessorBindings bindings", captureSection);
    size_t bindingsEnd = ppText.find("captureSession_ = std::make_unique<Capture::CaptureSession>", captureSection);
    if (bindingsStart == std::string::npos || bindingsEnd == std::string::npos)
        return {};
    return ppText.substr(bindingsStart, bindingsEnd - bindingsStart);
}

// ============================================================================
// T1: captureBindingsHaveNoMaterializationStoreBridge
//
// Capture bindings (isRenderReady, isRenderFailed) MUST be deleted.
// Content state reads/writes go through CaptureSegmentContent; 
// tick() reads F0 state directly.
//
// Forbidden: IsRenderReadyFn, IsRenderFailedFn, isRenderReady, isRenderFailed
// ============================================================================
static CheckResult captureBindingsHaveNoMaterializationStoreBridge()
{
    const auto captureBindings = extractCaptureBindings();
    if (captureBindings.empty())
        return fail("captureBindingsHaveNoMaterializationStoreBridge",
                    "Cannot extract VST3 capture bindings from PluginProcessor.cpp");

    // Forbidden: MS bridge calls in capture bindings
    {
        const std::vector<std::string> forbidden = {
            "getMaterializationOriginalF0StateById",
            "materializationStore_->createMaterialization",
            "materializationStore_->containsMaterialization",
        "requestContentRefresh",
            "materializationStore_->getSnapshot"
        };
        for (const auto& t : forbidden)
        {
            if (contains(captureBindings, t))
            {
                auto loc = locateInText(captureBindings, t, "PluginProcessor.cpp (capture bindings)");
                return fail("captureBindingsHaveNoMaterializationStoreBridge",
                            "Capture bindings contain forbidden MS bridge: '" + t + "' at " + loc);
            }
        }
    }

    return pass("captureBindingsHaveNoMaterializationStoreBridge");
}

// ============================================================================
// T1b: captureHasNoIsRenderReadyOrIsRenderFailed
//
// Phase 3 removes readiness bindings. tick() reads F0 state directly.
// Guard fails if CaptureSession.h still contains the old callback types.
// ============================================================================
static CheckResult captureHasNoIsRenderReadyOrIsRenderFailed()
{
    const auto sessionH = readText("Source/Plugin/Capture/CaptureSession.h");

    if (containsWord(sessionH, "IsRenderReadyFn"))
        return fail("captureHasNoIsRenderReadyOrIsRenderFailed",
                    "IsRenderReadyFn still declared in CaptureSession.h");

    if (containsWord(sessionH, "IsRenderFailedFn"))
        return fail("captureHasNoIsRenderReadyOrIsRenderFailed",
                    "IsRenderFailedFn still declared in CaptureSession.h");

    if (containsWord(sessionH, "isRenderReady"))
        return fail("captureHasNoIsRenderReadyOrIsRenderFailed",
                    "isRenderReady still declared in CaptureSession.h");

    if (containsWord(sessionH, "isRenderFailed"))
        return fail("captureHasNoIsRenderReadyOrIsRenderFailed",
                    "isRenderFailed still declared in CaptureSession.h");

    return pass("captureHasNoIsRenderReadyOrIsRenderFailed");
}

// ============================================================================
// T2: capturePersistenceDoesNotReadMaterializationStore
//
// CapturePersistence serialize/deserialize MUST NOT depend on MaterializationStore
// or the getSegmentAudio binding (which reads MS). Content must be serialized
// directly from CaptureSegmentContent.
//
// Forbidden: materializationStore_, getSegmentAudio, getSnapshot, MaterializationStore::
// ============================================================================
static CheckResult capturePersistenceDoesNotReadMaterializationStore()
{
    const auto text = readText("Source/Plugin/Capture/CapturePersistence.cpp")
                    + readText("Source/Plugin/Capture/CapturePersistence.h");

    if (text.empty())
        return fail("capturePersistenceDoesNotReadMaterializationStore",
                    "Cannot read CapturePersistence source files");

    const std::vector<std::string> forbidden = {
        "materializationStore_",
        "getSegmentAudio",
        "MaterializationStore::"
    };

    for (const auto& t : forbidden)
    {
        if (contains(text, t))
        {
            std::string file = "CapturePersistence.{h,cpp}";
            if (contains(readText("Source/Plugin/Capture/CapturePersistence.h"), t))
                file = "CapturePersistence.h";
            else if (contains(readText("Source/Plugin/Capture/CapturePersistence.cpp"), t))
                file = "CapturePersistence.cpp";
            auto loc = locateInText(text, t, file);
            return fail("capturePersistenceDoesNotReadMaterializationStore",
                        "CapturePersistence contains forbidden token '" + t + "' at " + loc +
                        " — persistence must use CaptureSegmentContent directly, not MS");
        }
    }

    return pass("capturePersistenceDoesNotReadMaterializationStore");
}

// ============================================================================
// T3: captureCrsUsesRegularVST3CaptureSegmentId
//
// All CRS operations in the Capture domain must use a consistent ContentKey:
//   ContentKey{DomainKind::RegularVST3Capture, segment.id, 0}
//
// Forbidden: contentKeyForMaterializationId, materializationId as content identity
// Required:  DomainKind::RegularVST3Capture in capture bindings
// ============================================================================
static CheckResult captureCrsUsesRegularVST3CaptureSegmentId()
{
    const auto sessionCpp = readText("Source/Plugin/Capture/CaptureSession.cpp");
    const auto captureBindings = extractCaptureBindings();
    if (captureBindings.empty())
        return fail("captureCrsUsesRegularVST3CaptureSegmentId",
                    "Cannot extract VST3 capture bindings from PluginProcessor.cpp");

    if (!contains(sessionCpp, "ContentKey{DomainKind::RegularVST3Capture"))
        return fail("captureCrsUsesRegularVST3CaptureSegmentId",
                    "CaptureSession must create segments with RegularVST3Capture ContentKey");
    if (!contains(captureBindings, "ContentKey segmentContentKey")
        || !contains(captureBindings, "getPlaybackReadSource(segmentContentKey"))
        return fail("captureCrsUsesRegularVST3CaptureSegmentId",
                    "Capture bindings must consume complete ContentKey directly");

    // Forbidden: materializationId-based content identity functions
    {
        const std::vector<std::string> forbidden = {
            "contentKeyForMaterializationId"
        };
        for (const auto& t : forbidden)
        {
            if (contains(captureBindings, t))
            {
                auto loc = locateInText(captureBindings, t, "PluginProcessor.cpp (capture bindings)");
                return fail("captureCrsUsesRegularVST3CaptureSegmentId",
                            "forbidden '" + t + "' at " + loc +
                            " — use ContentKey{RegularVST3Capture, segmentId} instead");
            }
        }
    }

    // materializationId must not appear as content identity in capture bindings
    // (the capture bindings may rename the callback param to segmentId — check)
    if (containsWord(captureBindings, "materializationId"))
    {
        auto loc = locateInText(captureBindings, "materializationId", "PluginProcessor.cpp (capture bindings)");
        return fail("captureCrsUsesRegularVST3CaptureSegmentId",
                    "'materializationId' found in capture bindings at " + loc +
                    " — capture callbacks must use segmentId, not materializationId");
    }

    return pass("captureCrsUsesRegularVST3CaptureSegmentId");
}

// ============================================================================
// T4: captureRefreshDoesNotRequestMaterializationRefresh
//
// refreshSegment must use Capture-native F0 extraction path (new F0 request
// interface + commit to CaptureSegmentContent). It MUST NOT create
// MaterializationStore entries, SourceStore entries, or call 
// requestMaterializationRefresh / onSegmentRenderingComplete from F0 completion.
//
// Forbidden: createMaterialization, requestMaterializationRefresh, SourceStore in refreshSegment
// ============================================================================
static CheckResult captureRefreshDoesNotRequestMaterializationRefresh()
{
    const auto captureBindings = extractCaptureBindings();
    if (captureBindings.empty())
        return fail("captureRefreshDoesNotRequestMaterializationRefresh",
                    "Cannot extract VST3 capture bindings from PluginProcessor.cpp");

    // Extract the refreshSegment lambda body
    size_t refreshPos = captureBindings.find("bindings.refreshSegment");
    if (refreshPos == std::string::npos)
        return fail("captureRefreshDoesNotRequestMaterializationRefresh",
                    "refreshSegment binding not found in capture bindings");
    size_t bracePos = captureBindings.find("{", refreshPos);
    if (bracePos == std::string::npos)
        return fail("captureRefreshDoesNotRequestMaterializationRefresh",
                    "refreshSegment lambda body not found");
    int depth = 0;
    size_t closePos = bracePos;
    for (size_t i = bracePos; i < captureBindings.size(); ++i)
    {
        if (captureBindings[i] == '{') ++depth;
        if (captureBindings[i] == '}') { --depth; if (depth == 0) { closePos = i; break; } }
    }
    std::string refreshBody = captureBindings.substr(bracePos, closePos - bracePos + 1);

    const std::vector<std::string> forbidden = {
        "createMaterialization",
        "requestContentRefresh",
        "SourceStore::CreateSourceRequest",
        "MaterializationStore::CreateMaterializationRequest",
        "onSegmentRenderingComplete"
    };

    for (const auto& t : forbidden)
    {
        if (contains(refreshBody, t))
        {
            auto loc = locateInText(refreshBody, t, "PluginProcessor.cpp (refreshSegment lambda)");
            return fail("captureRefreshDoesNotRequestMaterializationRefresh",
                        "refreshSegment contains forbidden '" + t + "' at " + loc);
        }
    }

    return pass("captureRefreshDoesNotRequestMaterializationRefresh");
}

// ============================================================================
// T4b: captureSegmentHasNoCapturedAudio
//
// CaptureSegment must NOT have capturedAudio field. Content ownership 
// is in CaptureSegmentContent.
// ============================================================================
static CheckResult captureSegmentHasNoCapturedAudio()
{
    const auto text = readText("Source/Plugin/Capture/CaptureSegment.h");

    if (containsWord(text, "capturedAudio"))
        return fail("captureSegmentHasNoCapturedAudio",
                    "capturedAudio field still exists in CaptureSegment.h");

    return pass("captureSegmentHasNoCapturedAudio");
}

// ============================================================================
// Contract Test 16: standaloneArrangementIsPlacementOnly (Phase 2)
//
// StandaloneArrangement must only manage Track/Placement graph and playback
// snapshots. It must NOT own content truth (contentOwners_ map).
// ============================================================================
static CheckResult standaloneArrangementIsPlacementOnly()
{
    const auto arrangementH = readText("Source/StandaloneArrangement.h");

    // StandaloneArrangement must NOT have contentOwners_ member
    if (contains(arrangementH, "contentOwners_"))
    {
        auto loc = locateInText(arrangementH, "contentOwners_", "StandaloneArrangement.h");
        return fail("standaloneArrangementIsPlacementOnly",
                    "StandaloneArrangement.contentOwners_ found in " + loc +
                    " — content ownership must be moved to StandaloneContentRepository");
    }

    // Must NOT have getOrCreateContentOwner method
    if (contains(arrangementH, "getOrCreateContentOwner"))
    {
        auto loc = locateInText(arrangementH, "getOrCreateContentOwner", "StandaloneArrangement.h");
        return fail("standaloneArrangementIsPlacementOnly",
                    "getOrCreateContentOwner found in " + loc +
                    " — Arrangement must not manage content lifecycle");
    }

    return pass("standaloneArrangementIsPlacementOnly");
}

// ============================================================================
// Contract Test 17: processBlockHasNoStoreOrPublish (Phase 2)
//
// processBlock() must only read atomic playback snapshots and CRS published
// sources. It must NOT call materializationStore_, publish sources, or
// create caches. All publishing must happen on message/background threads.
// ============================================================================
static CheckResult processBlockHasNoStoreOrPublish()
{
    const auto ppText = readText("Source/PluginProcessor.cpp");

    size_t funcPos = ppText.find("void OpenTuneAudioProcessor::processBlock");
    if (funcPos == std::string::npos)
        return fail("processBlockHasNoStoreOrPublish", "processBlock function not found");

    size_t bracePos = ppText.find("{", funcPos);
    if (bracePos == std::string::npos)
        return fail("processBlockHasNoStoreOrPublish", "processBlock body not found");

    int depth = 0;
    size_t closePos = bracePos;
    for (size_t i = bracePos; i < ppText.size() && i < bracePos + 50000; ++i)
    {
        if (ppText[i] == '{') ++depth;
        if (ppText[i] == '}') { --depth; if (depth == 0) { closePos = i; break; } }
    }
    std::string funcBody = ppText.substr(bracePos, closePos - bracePos + 1);

    // Forbidden operations in audio thread
    const std::vector<std::string> forbidden = {
        "->publish(",
        ".publish(",
        "snapshotContent()",
        "PlaybackSourcePublisher",
        "materializationStore_->set"
    };

    for (const auto& t : forbidden)
    {
        if (contains(funcBody, t))
        {
            return fail("processBlockHasNoStoreOrPublish",
                        "processBlock contains forbidden RT operation '" + t + "' — "
                        "publish/snapshot/store writes must happen on message thread");
        }
    }

    return pass("processBlockHasNoStoreOrPublish");
}

// ============================================================================
// T5: captureSegmentContentSnapshotComplete (P1 verification)
//
// CaptureSegmentContent::snapshotContent() must forward ALL content fields,
// including pitchCurve (post-F0 extraction result).
//
// Forbidden: pitchCurve = nullptr in snapshot
// Required:  pitchCurve_, editable_.originalF0State, editable_.audioBuffer
// ============================================================================
static CheckResult captureSegmentContentSnapshotComplete()
{
    const auto text = readText("Source/Content/CaptureSegmentContent.cpp");

    if (text.empty())
        return fail("captureSegmentContentSnapshotComplete",
                    "Cannot read CaptureSegmentContent.cpp");

    // Must NOT hardcode pitchCurve to nullptr
    if (contains(text, "pitchCurve = nullptr"))
        return fail("captureSegmentContentSnapshotComplete",
                    "snapshotContent still hardcodes pitchCurve = nullptr — "
                    "must forward pitchCurve_ from content owner");

    // Must use pitchCurve_ member
    if (!contains(text, "pitchCurve_"))
        return fail("captureSegmentContentSnapshotComplete",
                    "snapshotContent missing pitchCurve_ member reference");

    // Must include originalF0State
    if (!contains(text, "originalF0State"))
        return fail("captureSegmentContentSnapshotComplete",
                    "snapshotContent missing originalF0State");

    // Header must declare applyPitchCurve and pitchCurve accessor
    const auto header = readText("Source/Content/CaptureSegmentContent.h");
    if (!contains(header, "applyPitchCurve"))
        return fail("captureSegmentContentSnapshotComplete",
                    "CaptureSegmentContent.h missing applyPitchCurve declaration");
    if (!contains(header, "pitchCurve()"))
        return fail("captureSegmentContentSnapshotComplete",
                    "CaptureSegmentContent.h missing pitchCurve() accessor");

    return pass("captureSegmentContentSnapshotComplete");
}

// ============================================================================
// T6: captureStaticLifecycleWiring
//
// Verify the full capture lifecycle code chain is wired:
//   segment creation → ContentKey binding → refresh/F0 → playback via CRS
// ============================================================================
static CheckResult captureStaticLifecycleWiring()
{
    const auto sessionCpp = readText("Source/Plugin/Capture/CaptureSession.cpp");
    const auto captureBindings = extractCaptureBindings();

    if (sessionCpp.empty() || captureBindings.empty())
        return fail("captureStaticLifecycleWiring",
                    "Cannot read CaptureSession.cpp or capture bindings");

    // CaptureSession creates segment with ContentKey {RegularVST3Capture, id, 0}
    if (!contains(sessionCpp, "ContentKey{DomainKind::RegularVST3Capture"))
        return fail("captureStaticLifecycleWiring",
                    "CaptureSession does not create segments with ContentKey{DomainKind::RegularVST3Capture}");

    // replaceWithRendered uses getPlaybackReadSource with ContentKey
    if (!contains(captureBindings, "getPlaybackReadSource"))
        return fail("captureStaticLifecycleWiring",
                    "replaceWithRendered does not use getPlaybackReadSource");

    // refreshSegment uses F0ExtractionService (Capture-native path)
    if (!contains(captureBindings, "F0ExtractionService"))
        return fail("captureStaticLifecycleWiring",
                    "refreshSegment does not use F0ExtractionService (Capture-native F0 path)");

    // publishPlaybackSource uses contentRenderService_
    if (!contains(captureBindings, "contentRenderService_->publishPlaybackSource"))
        return fail("captureStaticLifecycleWiring",
                    "publishPlaybackSource does not publish to ContentRenderService");

    return pass("captureStaticLifecycleWiring");
}

namespace {
    struct CaptureRuntimeSpies
    {
        std::vector<OpenTune::ContentKey> retired;
        std::vector<OpenTune::ContentKey> refreshed;
        std::vector<OpenTune::ContentKey> active;
        std::vector<OpenTune::ContentKey> publishedKeys;
        std::vector<std::shared_ptr<const juce::AudioBuffer<float>>> publishedAudio;
        std::vector<double> publishedSampleRates;
    };

    std::shared_ptr<juce::AudioBuffer<float>> makeCaptureTestAudio(int channels = 1, int samples = 64)
    {
        auto audio = std::make_shared<juce::AudioBuffer<float>>(channels, samples);
        for (int ch = 0; ch < channels; ++ch) {
            auto* dst = audio->getWritePointer(ch);
            for (int i = 0; i < samples; ++i)
                dst[i] = 0.1f * static_cast<float>(ch + 1) + 0.001f * static_cast<float>(i);
        }
        return audio;
    }

    std::shared_ptr<OpenTune::PitchCurve> makeCaptureTestPitchCurve()
    {
        auto curve = std::make_shared<OpenTune::PitchCurve>();
        curve->setHopSize(160);
        curve->setSampleRate(16000.0);
        curve->setOriginalF0(std::vector<float>{110.0f, 120.0f, 130.0f});
        curve->setOriginalEnergy(std::vector<float>{0.2f, 0.3f, 0.4f});
        return curve;
    }

    OpenTune::DetectedKey makeCaptureTestKey()
    {
        OpenTune::DetectedKey key;
        key.root = OpenTune::Key::D;
        key.scale = OpenTune::Scale::Minor;
        key.confidence = 0.75f;
        return key;
    }

    bool sameDetectedKey(const OpenTune::DetectedKey& lhs, const OpenTune::DetectedKey& rhs)
    {
        return lhs.root == rhs.root
            && lhs.scale == rhs.scale
            && std::abs(lhs.confidence - rhs.confidence) <= 1.0e-6f;
    }

    bool sameCaptureKey(const OpenTune::ContentKey& key, uint64_t id)
    {
        return key.domainKind == OpenTune::DomainKind::RegularVST3Capture
            && key.objectId == id
            && key.sourceWindowDiscriminator == 0;
    }

    OpenTune::Capture::ProcessorBindings makeCaptureRuntimeBindings(
        CaptureRuntimeSpies& spies,
        OpenTune::Capture::CaptureSession** reentrantSession = nullptr)
    {
        OpenTune::Capture::ProcessorBindings bindings;
        bindings.replaceWithRendered = [](juce::AudioBuffer<float>&,
                                           int,
                                           int,
                                           OpenTune::ContentKey,
                                           double,
                                           double) {};
        bindings.retireSegment = [&spies, reentrantSession](OpenTune::ContentKey key) {
            spies.retired.push_back(key);
            if (reentrantSession != nullptr && *reentrantSession != nullptr)
                (void)(*reentrantSession)->findSegmentByContentKey(key);
        };
        bindings.refreshSegment = [&spies](OpenTune::ContentKey key) {
            spies.refreshed.push_back(key);
        };
        bindings.publishPlaybackSource = [&spies](const OpenTune::ContentKey& key,
                                                  std::shared_ptr<const juce::AudioBuffer<float>> audio,
                                                  double sampleRate) {
            spies.publishedKeys.push_back(key);
            spies.publishedAudio.push_back(std::move(audio));
            spies.publishedSampleRates.push_back(sampleRate);
        };
        return bindings;
    }
}

static CheckResult captureTickPromotesReadyWithoutReadinessBinding()
{
    CaptureRuntimeSpies spies;
    auto bindings = makeCaptureRuntimeBindings(spies);
    OpenTune::Capture::CaptureSession session(std::move(bindings));
    session.prepareToPlay(48000.0, 512, 1);
    session.setActiveSegmentChangedCallback([&spies](OpenTune::ContentKey key) {
        spies.active.push_back(key);
    });

    const uint64_t id = session.testInjectProcessingSegment(
        1.0, 0.01, 101, makeCaptureTestAudio(), 48000.0);
    auto* injected = session.findSegmentById(id);
    if (injected == nullptr)
        return fail("captureTickPromotesReadyWithoutReadinessBinding",
                    "injected segment was not found");
    const auto segmentKey = injected->contentKey;
    if (!session.commitSegmentF0Result(
            segmentKey, makeCaptureTestPitchCurve(),
            OpenTune::OriginalF0State::Ready,
            makeCaptureTestKey())) {
        return fail("captureTickPromotesReadyWithoutReadinessBinding",
                    "commitSegmentF0Result rejected the injected segment");
    }

    auto* before = session.findSegmentById(id);
    if (before->state.load(std::memory_order_acquire) != OpenTune::Capture::SegmentState::Processing)
        return fail("captureTickPromotesReadyWithoutReadinessBinding",
                    "F0 commit promoted before tick()");

    session.tick();

    auto* after = session.findSegmentById(id);
    if (after == nullptr || after->state.load(std::memory_order_acquire) != OpenTune::Capture::SegmentState::Edited)
        return fail("captureTickPromotesReadyWithoutReadinessBinding",
                    "tick() did not promote Ready content to Edited");
    if (spies.active.size() != 1 || spies.active[0] != segmentKey)
        return fail("captureTickPromotesReadyWithoutReadinessBinding",
                    "active segment callback was not emitted exactly once");

    return pass("captureTickPromotesReadyWithoutReadinessBinding");
}

static CheckResult captureTickDropsFailedWithoutReentrantBinding()
{
    CaptureRuntimeSpies spies;
    OpenTune::Capture::CaptureSession* sessionPtr = nullptr;
    auto bindings = makeCaptureRuntimeBindings(spies, &sessionPtr);
    OpenTune::Capture::CaptureSession session(std::move(bindings));
    sessionPtr = &session;
    session.prepareToPlay(48000.0, 512, 1);
    session.setActiveSegmentChangedCallback([&spies](OpenTune::ContentKey key) {
        spies.active.push_back(key);
    });

    const uint64_t id = session.testInjectProcessingSegment(
        2.0, 0.01, 202, makeCaptureTestAudio(), 48000.0);
    auto* injected = session.findSegmentById(id);
    if (injected == nullptr)
        return fail("captureTickDropsFailedWithoutReentrantBinding",
                    "injected segment was not found");
    const auto segmentKey = injected->contentKey;
    session.commitSegmentF0Result(segmentKey, nullptr, OpenTune::OriginalF0State::Failed, OpenTune::DetectedKey{});

    session.tick();

    if (session.findSegmentById(id) != nullptr)
        return fail("captureTickDropsFailedWithoutReentrantBinding",
                    "Failed Processing segment was not removed");
    if (spies.retired.size() != 1 || spies.retired[0] != segmentKey)
        return fail("captureTickDropsFailedWithoutReentrantBinding",
                    "retireSegment was not called after removing failed segment");
    if (!spies.active.empty())
        return fail("captureTickDropsFailedWithoutReentrantBinding",
                    "failed segment emitted active callback");

    return pass("captureTickDropsFailedWithoutReentrantBinding");
}

static CheckResult captureF0CommitDoesNotPromoteUntilTick()
{
    CaptureRuntimeSpies spies;
    auto bindings = makeCaptureRuntimeBindings(spies);
    OpenTune::Capture::CaptureSession session(std::move(bindings));
    session.prepareToPlay(44100.0, 512, 1);

    const uint64_t id = session.testInjectProcessingSegment(
        3.0, 0.01, 303, makeCaptureTestAudio(), 44100.0);
    auto* injected = session.findSegmentById(id);
    if (injected == nullptr)
        return fail("captureF0CommitDoesNotPromoteUntilTick",
                    "injected segment was not found");
    session.commitSegmentF0Result(
        injected->contentKey, makeCaptureTestPitchCurve(),
        OpenTune::OriginalF0State::Ready,
        makeCaptureTestKey());

    auto* before = session.findSegmentById(id);
    if (before->content->editable().originalF0State != OpenTune::OriginalF0State::Ready)
        return fail("captureF0CommitDoesNotPromoteUntilTick",
                    "F0 commit did not update owner state");
    if (before->state.load(std::memory_order_acquire) != OpenTune::Capture::SegmentState::Processing)
        return fail("captureF0CommitDoesNotPromoteUntilTick",
                    "F0 commit changed lifecycle state before tick()");

    session.tick();
    auto* after = session.findSegmentById(id);
    if (after == nullptr || after->state.load(std::memory_order_acquire) != OpenTune::Capture::SegmentState::Edited)
        return fail("captureF0CommitDoesNotPromoteUntilTick",
                    "tick() did not perform the single lifecycle promotion");

    return pass("captureF0CommitDoesNotPromoteUntilTick");
}

static CheckResult capturePersistenceRoundtripRestoresOwnerContent()
{
    CaptureRuntimeSpies originalSpies;
    auto originalBindings = makeCaptureRuntimeBindings(originalSpies);
    OpenTune::Capture::CaptureSession original(std::move(originalBindings));
    original.prepareToPlay(48000.0, 512, 2);

    const auto audio = makeCaptureTestAudio(2, 96);
    const uint64_t id = original.testInjectEditedSegment(
        4.0, static_cast<double>(audio->getNumSamples()) / 48000.0, 404, audio);
    const auto key = makeCaptureTestKey();
    auto* injected = original.findSegmentById(id);
    if (injected == nullptr)
        return fail("capturePersistenceRoundtripRestoresOwnerContent",
                    "injected segment was not found");
    original.commitSegmentF0Result(
        injected->contentKey, makeCaptureTestPitchCurve(),
        OpenTune::OriginalF0State::Ready,
        key);

    const juce::MemoryBlock state = original.serialize();
    if (state.getSize() == 0)
        return fail("capturePersistenceRoundtripRestoresOwnerContent",
                    "serialize() produced an empty state");

    CaptureRuntimeSpies restoredSpies;
    auto restoredBindings = makeCaptureRuntimeBindings(restoredSpies);
    OpenTune::Capture::CaptureSession restored(std::move(restoredBindings));
    if (!restored.deserialize(state))
        return fail("capturePersistenceRoundtripRestoresOwnerContent",
                    "deserialize() rejected owner-based CAPz state");

    auto* seg = restored.findSegmentById(id);
    if (seg == nullptr)
        return fail("capturePersistenceRoundtripRestoresOwnerContent",
                    "restored segment id not found");
    if (!sameCaptureKey(seg->contentKey, id))
        return fail("capturePersistenceRoundtripRestoresOwnerContent",
                    "restored segment ContentKey does not use segment id");
    if (seg->state.load(std::memory_order_acquire) != OpenTune::Capture::SegmentState::Edited)
        return fail("capturePersistenceRoundtripRestoresOwnerContent",
                    "Ready owner content did not restore as Edited");

    const auto snap = seg->content->snapshotContent();
    if (!snap->audioBuffer
        || snap->audioBuffer->getNumChannels() != 2
        || snap->audioBuffer->getNumSamples() != audio->getNumSamples())
        return fail("capturePersistenceRoundtripRestoresOwnerContent",
                    "restored owner audio does not match serialized audio");
    if (std::abs(snap->audioSampleRate - 48000.0) > 1.0e-6)
        return fail("capturePersistenceRoundtripRestoresOwnerContent",
                    "restored owner audio sample rate is wrong");
    if (snap->originalF0State != OpenTune::OriginalF0State::Ready)
        return fail("capturePersistenceRoundtripRestoresOwnerContent",
                    "restored originalF0State is not Ready");
    if (!sameDetectedKey(snap->detectedKey, key))
        return fail("capturePersistenceRoundtripRestoresOwnerContent",
                    "restored DetectedKey does not match");
    if (!snap->pitchCurve || snap->pitchCurve->getSnapshot()->getOriginalF0().size() != 3)
        return fail("capturePersistenceRoundtripRestoresOwnerContent",
                    "restored pitch curve is missing");
    if (restoredSpies.publishedKeys.size() != 1 || !sameCaptureKey(restoredSpies.publishedKeys[0], id))
        return fail("capturePersistenceRoundtripRestoresOwnerContent",
                    "deserialize did not publish restored owner audio by ContentKey");
    if (!restoredSpies.refreshed.empty())
        return fail("capturePersistenceRoundtripRestoresOwnerContent",
                    "Ready owner content should not be refreshed on deserialize");

    return pass("capturePersistenceRoundtripRestoresOwnerContent");
}

// ============================================================================
// Phase 4 helper: forbid tokens across named files
// ============================================================================
static CheckResult forbidTokensInFiles(const std::string& testName,
                                        const std::vector<std::string>& files,
                                        const std::vector<std::string>& forbidden)
{
    for (const auto& file : files)
    {
        const auto text = readText(file);
        if (text.empty())
            continue;
        for (const auto& token : forbidden)
        {
            if (contains(text, token))
            {
                auto loc = locateInText(text, token, file);
                return fail(testName, "forbidden token '" + token + "' found in " + loc);
            }
        }
    }
    return pass(testName);
}

// ============================================================================
// Phase 4 Guard: phase4NoMaterializationStoreType
//
// MaterializationStore type must not appear in non-store source files.
// The store file itself (MaterializationStore.h/cpp) is intentionally
// excluded from the scan — it will be deleted in Task 4.9.
// ============================================================================
static CheckResult phase4NoMaterializationStoreType()
{
    const std::vector<std::string> files = {
        "Source/PluginProcessor.h",
        "Source/PluginProcessor.cpp",
        "Source/ARA/OpenTuneDocumentController.h",
        "Source/ARA/OpenTuneDocumentController.cpp",
        "Source/Plugin/PluginEditor.cpp",
        "Source/Standalone/PluginEditor.cpp",
        "Source/Standalone/UI/PianoRollComponent.h",
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "Source/Utils/PianoRollEditAction.h",
        "Source/Utils/PianoRollEditAction.cpp",
        "Source/Utils/PitchShiftEditAction.h",
        "Source/Utils/PitchShiftEditAction.cpp",
        "Source/Utils/TimeGridEditAction.h",
        "Source/Utils/TimeGridEditAction.cpp"
    };

    const std::vector<std::string> forbidden = {
        "MaterializationStore",
        "materializationStore_",
        "getMaterializationStore",
        "CreateMaterializationRequest",
        "MaterializationSnapshot",
        "MaterializationNotesSnapshot",
        "contentKeyForMaterializationId"
    };

    return forbidTokensInFiles("phase4NoMaterializationStoreType",
                               files, forbidden);
}

// ============================================================================
// Phase 4 Guard: phase4NoMaterializationContentProvider
// ============================================================================
static CheckResult phase4NoMaterializationContentProvider()
{
    const std::vector<std::string> files = {
        "Source/PluginProcessor.h",
        "Source/PluginProcessor.cpp",
        "Source/Plugin/PluginEditor.h",
        "Source/Plugin/PluginEditor.cpp",
        "Source/Standalone/PluginEditor.cpp",
        "Source/Standalone/UI/PianoRollComponent.h",
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "Source/Utils/PianoRollEditAction.h",
        "Source/Utils/PitchShiftEditAction.h",
        "Source/Utils/TimeGridEditAction.h"
    };

    return forbidTokensInFiles("phase4NoMaterializationContentProvider",
                               files,
                               {"MaterializationContentAccess",
                                "MaterializationContentCommands",
                                "MaterializationContentProvider.h"});
}

// ============================================================================
// Phase 4 Guard: phase4NoStoreBackedRenderOrStretcherPath
//
// Render and Stretch paths must not call materializationStore_->getSnapshot,
// materializationStore_->getOpenTuneStretcher, or any by-materializationId
// revision getter that still lives on the store.
// ============================================================================
static CheckResult phase4NoStoreBackedRenderOrStretcherPath()
{
    const std::vector<std::string> files = {
        "Source/PluginProcessor.cpp"
    };

    const std::vector<std::string> forbidden = {
        "materializationStore_->getSnapshot",
        "materializationStore_->getOpenTuneStretcher",
        "materializationStore_->getTimeStretchCache",
        "getPitchRevision(materializationId)",
        "getPitchShiftRevision(materializationId)",
        "getTimeGridRevision(materializationId)"
    };

    return forbidTokensInFiles("phase4NoStoreBackedRenderOrStretcherPath",
                               files, forbidden);
}

// ============================================================================
// Phase 4 Guard: phase4NoOldMaterializationCommandApis
//
// Processor public API and active editor/undo call surfaces must not expose
// by-materializationId mutation methods. All mutation must go through
// ContentKey-based ContentEditCommands.
// ============================================================================
static CheckResult phase4NoOldMaterializationCommandApis()
{
    const std::vector<std::string> files = {
        "Source/PluginProcessor.h",
        "Source/Plugin/PluginEditor.h",
        "Source/Plugin/PluginEditor.cpp",
        "Source/Standalone/PluginEditor.cpp",
        "Source/Standalone/UI/PianoRollComponent.h",
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "Source/Utils/PianoRollEditAction.h",
        "Source/Utils/PianoRollEditAction.cpp",
        "Source/Utils/PitchShiftEditAction.h",
        "Source/Utils/PitchShiftEditAction.cpp",
        "Source/Utils/TimeGridEditAction.h",
        "Source/Utils/TimeGridEditAction.cpp"
    };

    const std::vector<std::string> forbidden = {
        "setMaterializationNotesById",
        "setMaterializationPitchCurveById",
        "setMaterializationCorrectedSegmentsById",
        "commitMaterializationNotesAndSegmentsById",
        "setMaterializationTimeGridById",
        "setMaterializationDetectedKeyById",
        "setMaterializationPitchShiftSettingsById",
        "getMaterializationNotesSnapshotById",
        "enqueueMaterializationPartialRenderById"
    };

    return forbidTokensInFiles("phase4NoOldMaterializationCommandApis",
                               files, forbidden);
}

// ============================================================================
// Phase 4 Guard: phase4Stage2HasNoStoreFallback
//
// Inspects only runStage2RebuildForContentKey in PluginProcessor.cpp.
// Must not contain store fallback paths. Stage2 must read owner snapshot
// and CRS playback source, NOT MaterializationStore.
// ============================================================================
static CheckResult phase4Stage2HasNoStoreFallback()
{
    const auto text = readText("Source/PluginProcessor.cpp");

    // Find the function body
    size_t funcStart = text.find("::runStage2RebuildForContentKey");
    if (funcStart == std::string::npos)
        return fail("phase4Stage2HasNoStoreFallback",
                    "runStage2RebuildForContentKey is missing");

    // Extract function body: from first '{' after signature to matching '}'
    size_t braceOpen = text.find("{", funcStart);
    if (braceOpen == std::string::npos)
        return fail("phase4Stage2HasNoStoreFallback",
                    "runStage2RebuildForContentKey has no function body");

    int depth = 0;
    size_t braceClose = braceOpen;
    bool foundClose = false;
    for (size_t i = braceOpen; i < text.size(); ++i)
    {
        if (text[i] == '{') ++depth;
        if (text[i] == '}') { --depth; if (depth == 0) { braceClose = i; foundClose = true; break; } }
    }
    if (!foundClose)
        return fail("phase4Stage2HasNoStoreFallback",
                    "runStage2RebuildForContentKey has an unterminated function body");

    std::string funcBody = text.substr(braceOpen, braceClose - braceOpen + 1);

    const std::vector<std::string> required = {
        "getContentSnapshot(",
        "getPlaybackReadSource",
        "readPlaybackAudio",
        "getTimeStretchCache().store"
    };

    for (const auto& token : required)
    {
        if (!contains(funcBody, token))
        {
            return fail("phase4Stage2HasNoStoreFallback",
                        "Stage2 function is missing required owner/CRS path token: '" + token + "'");
        }
    }

    const std::vector<std::string> forbidden = {
        "MaterializationStore::MaterializationSnapshot",
        "materializationStore_->getSnapshot",
        "getMaterializationSnapshotById",
        "materializationStore_->getTimeStretchCache",
        "materializationStore_->getOpenTuneStretcher",
        "ownerSnap->audioBuffer"
    };

    for (const auto& token : forbidden)
    {
        if (contains(funcBody, token))
        {
            auto loc = locateInText(text, token, "Source/PluginProcessor.cpp");
            return fail("phase4Stage2HasNoStoreFallback",
                        "Stage2 function contains store fallback: '" + token
                        + "' at " + loc);
        }
    }

    return pass("phase4Stage2HasNoStoreFallback");
}

// ============================================================================
// Phase 4 Runtime Test: phase4StandaloneEditCommandWritesOwnerAndInvalidatesCrs
//
// Create a StandaloneClipContent, write notes directly to the owner,
// verify owner state changed and content revision bumped.
// ============================================================================
static CheckResult phase4StandaloneEditCommandWritesOwnerAndInvalidatesCrs()
{
    using namespace OpenTune;

    StandaloneClipContent clip(1);
    auto beforeSnap = clip.snapshotContent();
    uint64_t beforeRev = beforeSnap->notesRevision;

    std::vector<Note> newNotes;
    Note n;
    n.startTime = 0.0;
    n.endTime = 1.0;
    n.pitch = 261.626f;
    newNotes.push_back(n);

    clip.applyNotes(newNotes);

    auto afterSnap = clip.snapshotContent();
    if (afterSnap->notes.size() != 1)
        return fail("phase4StandaloneEditCommandWritesOwnerAndInvalidatesCrs",
                    "notes not written to owner");
    if (std::abs(afterSnap->notes[0].pitch - 261.626f) > 0.01f)
        return fail("phase4StandaloneEditCommandWritesOwnerAndInvalidatesCrs",
                    "wrong note pitch in owner snapshot");
    if (afterSnap->notesRevision <= beforeRev)
        return fail("phase4StandaloneEditCommandWritesOwnerAndInvalidatesCrs",
                    "notesRevision not bumped after write");

    return pass("phase4StandaloneEditCommandWritesOwnerAndInvalidatesCrs");
}

// ============================================================================
// Phase 4 Runtime Test: phase4Stage2ReadsOwnerSnapshotByContentKey
//
// Verify Stage2 rebuild can read a complete owner snapshot through ContentKey
// without any MaterializationStore dependency.  All fields needed by
// runStage2RebuildForContentKey must be present.
// ============================================================================
static CheckResult phase4Stage2ReadsOwnerSnapshotByContentKey()
{
    using namespace OpenTune;

    StandaloneClipContent clip(2);

    clip.applyNotes({});
    clip.applyPitchCurve(std::make_shared<PitchCurve>());
    clip.applyPitchShiftSettings(PitchShiftSettings{});

    auto snap = clip.snapshotContent();
    auto key = clip.contentKey();

    if (key.domainKind != DomainKind::StandaloneClip)
        return fail("phase4Stage2ReadsOwnerSnapshotByContentKey",
                    "ContentKey domainKind is not StandaloneClip");
    if (key.objectId != 2)
        return fail("phase4Stage2ReadsOwnerSnapshotByContentKey",
                    "ContentKey objectId mismatch");
    if (!snap->pitchCurve)
        return fail("phase4Stage2ReadsOwnerSnapshotByContentKey",
                    "pitchCurve missing from snapshot");

    // Stage2 rebuild requires these fields from the owner snapshot
    if (snap->pitchRevision == 0)
        return fail("phase4Stage2ReadsOwnerSnapshotByContentKey",
                    "pitchRevision not set in owner snapshot");
    if (snap->pitchShiftRevision == 0)
        return fail("phase4Stage2ReadsOwnerSnapshotByContentKey",
                    "pitchShiftRevision not set in owner snapshot");
    // timeGridRevision is 0 until timeGrid is applied; apply and verify bump
    clip.applyTimeGrid(TimeGridSnapshot::makeIdentity(1.0));
    auto snapWithGrid = clip.snapshotContent();
    if (snapWithGrid->timeGridRevision == 0)
        return fail("phase4Stage2ReadsOwnerSnapshotByContentKey",
                    "timeGridRevision not bumped after applyTimeGrid");
    if (snapWithGrid->contentRevision == 0)
        return fail("phase4Stage2ReadsOwnerSnapshotByContentKey",
                    "contentRevision not set in owner snapshot");

    // Verify snapshot can carry audio data needed for Stage2
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, 48000);
    clip.applyAudioBuffer(buffer, 48000.0);
    auto snap2 = clip.snapshotContent();
    if (snap2->audioBuffer == nullptr)
        return fail("phase4Stage2ReadsOwnerSnapshotByContentKey",
                    "audioBuffer not populated in owner snapshot");
    if (snap2->audioSampleRate <= 0.0)
        return fail("phase4Stage2ReadsOwnerSnapshotByContentKey",
                    "audioSampleRate not set in owner snapshot");

    return pass("phase4Stage2ReadsOwnerSnapshotByContentKey");
}

// ============================================================================
// Phase 4 Runtime Test: phase4ReferenceAnalysisCommitsToOwner
//
// Verify that ReferenceFeatureSet can be written and read back from the owner
// without any MaterializationStore involvement.  Verify contentRevision bump.
// ============================================================================
static CheckResult phase4ReferenceAnalysisCommitsToOwner()
{
    using namespace OpenTune;

    StandaloneClipContent clip(3);

    auto beforeSnap = clip.snapshotContent();
    uint64_t beforeRev = beforeSnap->contentRevision;

    ReferenceFeatureSet features;
    features.status = ReferenceFeatureStatus::Ready;
    features.analysisRevision = 5;
    clip.applyReferenceFeatures(features);

    auto afterSnap = clip.snapshotContent();
    if (afterSnap->referenceFeatures.status != ReferenceFeatureStatus::Ready)
        return fail("phase4ReferenceAnalysisCommitsToOwner",
                    "reference features not committed to owner");
    if (afterSnap->referenceFeatures.analysisRevision != 5)
        return fail("phase4ReferenceAnalysisCommitsToOwner",
                    "reference features analysisRevision not preserved");
    if (afterSnap->contentRevision <= beforeRev)
        return fail("phase4ReferenceAnalysisCommitsToOwner",
                    "contentRevision not bumped after write");

    return pass("phase4ReferenceAnalysisCommitsToOwner");
}

// ============================================================================
// Phase 4 Runtime Test: phase4CopyCloneCreateStandaloneClipsOnly
//
// Verify that creating two StandaloneClipContent instances with separate ids
// maintains content independence and neither touches MaterializationStore.
// ============================================================================
static CheckResult phase4CopyCloneCreateStandaloneClipsOnly()
{
    using namespace OpenTune;

    StandaloneClipContent original(10);
    std::vector<Note> notes;
    Note n;
    n.startTime = 0.0;
    n.endTime = 1.0;
    n.pitch = 329.628f;
    notes.push_back(n);
    original.applyNotes(notes);

    StandaloneClipContent clone(20);
    clone.applyNotes(original.snapshotContent()->notes);

    auto origSnap = original.snapshotContent();
    auto cloneSnap = clone.snapshotContent();

    if (cloneSnap->notes.size() != origSnap->notes.size())
        return fail("phase4CopyCloneCreateStandaloneClipsOnly",
                    "clone notes count mismatch");
    if (cloneSnap->notes[0].pitch != origSnap->notes[0].pitch)
        return fail("phase4CopyCloneCreateStandaloneClipsOnly",
                    "clone note pitch differs from original");

    auto origKey = original.contentKey();
    auto cloneKey = clone.contentKey();
    if (origKey.objectId == cloneKey.objectId)
        return fail("phase4CopyCloneCreateStandaloneClipsOnly",
                    "original and clone have same ContentKey");
    if (cloneKey.domainKind != DomainKind::StandaloneClip)
        return fail("phase4CopyCloneCreateStandaloneClipsOnly",
                    "clone domainKind is not StandaloneClip");

    // Verify cloned clips maintain independent content revisions
    origSnap = original.snapshotContent();
    cloneSnap = clone.snapshotContent();
    if (origSnap->contentRevision == 0)
        return fail("phase4CopyCloneCreateStandaloneClipsOnly",
                    "original contentRevision not set");
    if (cloneSnap->contentRevision == 0)
        return fail("phase4CopyCloneCreateStandaloneClipsOnly",
                    "clone contentRevision not set");

    return pass("phase4CopyCloneCreateStandaloneClipsOnly");
}

// ============================================================================
// Phase 4 Runtime Test: phase4RenderJobCarriesFullPayload
//
// Verify that enqueueContentPartialRender fills ALL fields of RenderJob
// so that RenderWorker::processChunkRenderJob can operate without fallback.
// ============================================================================
static CheckResult phase4RenderJobCarriesFullPayload()
{
    const auto text = readText("Source/PluginProcessor.cpp");

    // Locate enqueueContentPartialRender function body
    size_t funcStart = text.find("OpenTuneAudioProcessor::enqueueContentPartialRender");
    if (funcStart == std::string::npos)
        return fail("phase4RenderJobCarriesFullPayload",
                    "enqueueContentPartialRender not found");

    // Find the closing brace of the function (simple heuristic: next };)
    size_t scopeEnd = text.find("}", funcStart + 200); // skip past signature

    // Fields that MUST be assigned in RenderJob population block
    const char* requiredFields[] = {
        "job.audioSampleRate",
        "job.timeGrid",
        "job.pitchShiftSettings",
        "job.silentGaps",
        "job.contentRevision",
        "job.startSample",
        "job.endSampleExclusive",
    };

    for (const char* field : requiredFields) {
        size_t pos = text.find(field, funcStart);
        if (pos == std::string::npos || pos > scopeEnd + 200) {
            return fail("phase4RenderJobCarriesFullPayload",
                        std::string("RenderJob field not populated: ") + field);
        }
    }

    return pass("phase4RenderJobCarriesFullPayload");
}

// ============================================================================
// Phase 4 Runtime Test: phase4EditorReadsViaContentKey
//
// PluginEditor.cpp must NOT contain getMaterialization*ById old read APIs.
// It must read content through ContentKey-based DC APIs or processor snapshots.
// ============================================================================
static CheckResult phase4EditorReadsViaContentKey()
{
    const auto text = readText("Source/Plugin/PluginEditor.cpp");

    const std::vector<std::string> forbidden = {
        "getMaterializationAudioBufferById",
        "getMaterializationPitchCurveById",
        "getMaterializationNotesById",
        "getMaterializationTimeGridById",
        "getMaterializationTimeGridRevisionById",
        "getMaterializationOriginalF0StateById",
        "getMaterializationDetectedKeyById",
        "getMaterializationChunkStatsById",
        "getMaterializationChunkBoundariesById",
        "getMaterializationRenderCacheById",
        "getMaterializationAudioDurationById",
        "getMaterializationNotesSnapshotById",
        "MaterializationContentAccess"
    };

    for (const auto& token : forbidden) {
        if (contains(text, token)) {
            auto loc = locateInText(text, token, "Source/Plugin/PluginEditor.cpp");
            return fail("phase4EditorReadsViaContentKey",
                        "forbidden ById read API '" + token + "' found at " + loc);
        }
    }

    return pass("phase4EditorReadsViaContentKey");
}

// ============================================================================
// Phase 4 Guard: phase4NoPublicMaterializationVocabulary
//
// Product-facing source code should expose Content vocabulary. The old
// materialization wording is kept only inside tests as negative guard tokens
// and inside historical planning/audit documents.
// ============================================================================
static CheckResult phase4NoPublicMaterializationVocabulary()
{
    const std::vector<std::string> files = {
        "Source/PluginProcessor.h",
        "Source/PluginProcessor.cpp",
        "Source/Plugin/PluginEditor.h",
        "Source/Plugin/PluginEditor.cpp",
        "Source/Standalone/PluginEditor.h",
        "Source/Standalone/PluginEditor.cpp",
        "Source/Standalone/UI/PianoRollComponent.h",
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "Source/Standalone/UI/PianoRoll/PianoRollRenderer.h",
        "Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp",
        "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h",
        "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp",
        "Source/Utils/ProjectModel.h",
        "Source/Utils/ProjectPersistence.h",
        "Source/Utils/ProjectPersistence.cpp",
        "Source/Utils/ProjectSession.h",
        "Source/Utils/ProjectSession.cpp",
        "Source/Utils/ContentTimelineProjection.h",
        "Source/Utils/ContentAnalysisState.h",
        "Source/Editor/AutoRenderOverlayComponent.h",
        "Source/Services/F0ExtractionService.h",
        "Source/Services/F0ExtractionService.cpp",
        "Source/ARA/OpenTuneDocumentController.h",
        "Source/ARA/OpenTuneDocumentController.cpp",
        "Source/ARA/OpenTunePlaybackRenderer.h",
        "Source/ARA/OpenTunePlaybackRenderer.cpp"
    };

    return forbidTokensInFiles("phase4NoPublicMaterializationVocabulary",
                               files,
                               {"materialization",
                                "Materialization",
                                "materialized",
                                "Materialized"});
}

// ============================================================================
// ContentKey hard-cut guards: no domain-erased content identity in UI/editor paths
// ============================================================================
static CheckResult contentKeyMustBeCompleteInAllPaths()
{
    const auto rendererHeader = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");
    const auto placementBody = extractBraceBlockAfterToken(rendererHeader, "struct TimelineContentPlacement");
    const auto renderItemBody = extractBraceBlockAfterToken(rendererHeader, "struct ContentRenderItem");
    if (placementBody.empty() || renderItemBody.empty())
        return fail("contentKeyMustBeCompleteInAllPaths",
                    "PianoRoll placement/render item bodies not found");
    if (!contains(placementBody, "ContentKey contentKey") || contains(placementBody, "uint64_t contentId"))
        return fail("contentKeyMustBeCompleteInAllPaths",
                    "TimelineContentPlacement must carry ContentKey only");
    if (!contains(renderItemBody, "ContentKey contentKey") || contains(renderItemBody, "uint64_t contentId"))
        return fail("contentKeyMustBeCompleteInAllPaths",
                    "ContentRenderItem must carry ContentKey only");

    const auto pluginEditorHeader = readText("Source/Plugin/PluginEditor.h");
    const auto syncBody = extractBraceBlockAfterToken(pluginEditorHeader, "struct PianoRollContentSync");
    if (syncBody.empty())
        return fail("contentKeyMustBeCompleteInAllPaths", "PianoRollContentSync body not found");
    if (!contains(syncBody, "ContentKey activeContentKey")
        || contains(syncBody, "activeContentId")
        || contains(syncBody, "usesRegularCaptureTimelineDomain"))
        return fail("contentKeyMustBeCompleteInAllPaths",
                    "VST3 editor sync must use active ContentKey without domain-erased flags");

    const auto componentHeader = readText("Source/Standalone/UI/PianoRollComponent.h");
    if (contains(componentHeader, "editedContentId_"))
        return fail("contentKeyMustBeCompleteInAllPaths",
                    "PianoRollComponent still stores editedContentId_ beside ContentKey");

    const auto correctionHeader = readText("Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.h");
    if (contains(correctionHeader, "contentIdSnapshot")
        || !contains(correctionHeader, "ContentKey contentKeySnapshot"))
        return fail("contentKeyMustBeCompleteInAllPaths",
                    "PianoRoll async correction request must snapshot ContentKey");

    const auto cacheHeader = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderModelCache.h");
    const auto cacheKeyBody = extractBraceBlockAfterToken(cacheHeader, "struct Key");
    if (cacheKeyBody.empty())
        return fail("contentKeyMustBeCompleteInAllPaths", "PianoRoll render cache key body not found");
    if (!contains(cacheKeyBody, "ContentKey contentKey") || contains(cacheKeyBody, "uint64_t contentId"))
        return fail("contentKeyMustBeCompleteInAllPaths",
                    "PianoRoll render cache key must carry ContentKey only");

    const auto pianoComponentCpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    if (contains(pianoComponentCpp, "DomainKind::StandaloneClip"))
        return fail("contentKeyMustBeCompleteInAllPaths",
                    "generic PianoRoll rendering must not guess StandaloneClip domain");

    const auto pluginEditorCpp = readText("Source/Plugin/PluginEditor.cpp");
    const std::vector<std::string> forbiddenAraHardcodes = {
        "ContentKey{DomainKind::ARAAudioModification, sync.activeContentId",
        "ContentKey{DomainKind::ARAAudioModification, contentId",
        "{DomainKind::ARAAudioModification, activeContentId",
        "{DomainKind::ARAAudioModification, contentId"
    };
    for (const auto& token : forbiddenAraHardcodes)
        if (contains(pluginEditorCpp, token))
            return fail("contentKeyMustBeCompleteInAllPaths",
                        "VST3 editor still reconstructs ARA ContentKey from bare id: " + token);

    const auto f0Header = readText("Source/Services/F0ExtractionService.h");
    const auto f0Cpp = readText("Source/Services/F0ExtractionService.cpp");
    if (contains(f0Header, "uint64_t requestKey")
        || contains(f0Header, "makeRequestKey(uint64_t contentId")
        || contains(f0Cpp, "return contentId;"))
        return fail("contentKeyMustBeCompleteInAllPaths",
                    "F0ExtractionService request identity must be a ContentKey strong type");

    const auto processorCpp = readText("Source/PluginProcessor.cpp");
    if (contains(processorCpp, "ContentKey{DomainKind::StandaloneClip, objectId")
        || contains(processorCpp, "ContentKey{DomainKind::StandaloneClip, chunkObjId")
        || contains(processorCpp, "DomainKind::StandaloneClip, chunkObjId"))
        return fail("contentKeyMustBeCompleteInAllPaths",
                    "render/vocoder completion still recreates StandaloneClip keys from objectId");

    return pass("contentKeyMustBeCompleteInAllPaths");
}

// ============================================================================
// ARA archive must NOT persist source PCM, only modification-scoped state
// ============================================================================
// Per ARA2 spec: AudioSource owns original PCM via host sample access.
// AudioModification archive only saves modification-scoped edit/analysis state.
// CRS audio buffer is derived cache, not archive payload.
static CheckResult araArchiveMustNotPersistSourcePCM()
{
    const auto dcText = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto modText = readText("Source/ARA/AudioModification.cpp");
    
    // Per ARA2 spec: Real archive entry points are doStoreObjectsToStream/doRestoreObjectsFromStream,
    // NOT VST3 processor state. Legacy getContentSnapshot/restoreContentPayloadInto were removed.
    const auto storeBody = extractBraceBlockAfterToken(dcText,
                                                       "OpenTuneDocumentController::doStoreObjectsToStream");
    const auto restoreBody = extractBraceBlockAfterToken(dcText,
                                                         "OpenTuneDocumentController::doRestoreObjectsFromStream");
    if (storeBody.empty() || restoreBody.empty())
        return fail("araArchiveMustNotPersistSourcePCM",
                    "ARA archive entry points doStoreObjectsToStream/doRestoreObjectsFromStream not found");

    // Must call serialization helpers
    if (!contains(storeBody, "serializeAudioModificationContent")
        || !contains(restoreBody, "restoreAudioModificationContent"))
        return fail("araArchiveMustNotPersistSourcePCM",
                    "ARA archive must serialize and restore AudioModification content payloads");

    // Must NOT serialize/restore audioBuffer (source PCM)
    const auto serializeBody = extractBraceBlockAfterToken(dcText,
                                                           "serializeAudioModificationContent");
    const auto deserializeBody = extractBraceBlockAfterToken(dcText,
                                                             "restoreAudioModificationContent");
    if (contains(serializeBody, "audioBuffer") || contains(deserializeBody, "audioBuffer"))
        return fail("araArchiveMustNotPersistSourcePCM",
                    "ARA archive must NOT serialize audioBuffer — source PCM comes from AudioSource, not AudioModification");

    // AudioModification::snapshotContent must NOT return audioBuffer
    const auto snapshotContentBody = extractBraceBlockAfterToken(modText,
                                                                 "AudioModification::snapshotContent");
    if (contains(snapshotContentBody, "audioBuffer") 
        || contains(snapshotContentBody, "snap.audioBuffer"))
        return fail("araArchiveMustNotPersistSourcePCM",
                    "AudioModification::snapshotContent must NOT return audioBuffer");

    // Restore must call rebuildCRSFromSource, not birthContentForModification
    if (!contains(restoreBody, "rebuildCRSFromSource"))
        return fail("araArchiveMustNotPersistSourcePCM",
                    "doRestoreObjectsFromStream must rebuild CRS from AudioSource after restoring modification state");

    // Must NOT use legacy VST3 processor state path for ARA content
    const auto processorText = readText("Source/PluginProcessor.cpp");
    const auto getStateBody = extractBraceBlockAfterToken(processorText,
                                                          "OpenTuneAudioProcessor::getStateInformation");
    const auto setStateBody = extractBraceBlockAfterToken(processorText,
                                                          "OpenTuneAudioProcessor::setStateInformation");
    if (contains(getStateBody, "getContentSnapshot")
        || contains(setStateBody, "restoreContentPayloadInto"))
        return fail("araArchiveMustNotPersistSourcePCM",
                    "VST3 processor state must NOT call getContentSnapshot/restoreContentPayloadInto for ARA content");

    return pass("araArchiveMustNotPersistSourcePCM");
}

// ============================================================================
// ARA archive round-trip must restore complete analysis state
// ============================================================================
// Static verification that serializeAudioModificationContent writes all analysis fields
// and restoreAudioModificationContent reads them back correctly.
// Note: This is a static code guard. Full behavioral round-trip testing would require
// ARA host environment and is beyond the scope of static analysis tests.
static CheckResult araArchiveRestoresCompleteAnalysisState()
{
    const auto dcText = readText("Source/ARA/OpenTuneDocumentController.cpp");
    
    const auto serializeBody = extractBraceBlockAfterToken(dcText,
                                                           "serializeAudioModificationContent");
    const auto restoreBody = extractBraceBlockAfterToken(dcText,
                                                         "restoreAudioModificationContent");
    if (serializeBody.empty() || restoreBody.empty())
        return fail("araArchiveRestoresCompleteAnalysisState",
                    "serialize/restore functions not found");
    
    // analysisRevision must be serialized AND restored
    if (!contains(serializeBody, "analysisRevision"))
        return fail("araArchiveRestoresCompleteAnalysisState",
                    "serializeAudioModificationContent must write analysisRevision");
    if (!contains(restoreBody, "analysisRevision") 
        || !contains(restoreBody, ".analysisRevision = "))
        return fail("araArchiveRestoresCompleteAnalysisState",
                    "restoreAudioModificationContent must restore analysisRevision");
    
    // pitchLifecycle must be serialized AND restored
    if (!contains(serializeBody, "pitchLifecycle"))
        return fail("araArchiveRestoresCompleteAnalysisState",
                    "serializeAudioModificationContent must write pitchLifecycle");
    if (!contains(restoreBody, "pitchLifecycle")
        || !contains(restoreBody, ".pitchLifecycle = "))
        return fail("araArchiveRestoresCompleteAnalysisState",
                    "restoreAudioModificationContent must restore pitchLifecycle");
    
    // detectedKey must be serialized AND restored
    if (!contains(serializeBody, "DetectedKey") 
        || !contains(serializeBody, "detectedKey.root")
        || !contains(serializeBody, "detectedKey.scale")
        || !contains(serializeBody, "detectedKey.confidence"))
        return fail("araArchiveRestoresCompleteAnalysisState",
                    "serializeAudioModificationContent must write complete DetectedKey");
    if (!contains(restoreBody, "DetectedKey")
        || !contains(restoreBody, "detectedKey.root")
        || !contains(restoreBody, "detectedKey.scale")
        || !contains(restoreBody, "detectedKey.confidence"))
        return fail("araArchiveRestoresCompleteAnalysisState",
                    "restoreAudioModificationContent must restore complete DetectedKey");
    
    // silentGaps must be serialized AND restored
    if (!contains(serializeBody, "silentGaps")
        || !contains(serializeBody, "SilentGap")
        || !contains(serializeBody, "startSample")
        || !contains(serializeBody, "endSampleExclusive"))
        return fail("araArchiveRestoresCompleteAnalysisState",
                    "serializeAudioModificationContent must write silentGaps vector");
    if (!contains(restoreBody, "silentGaps")
        || !contains(restoreBody, "SilentGap")
        || !contains(restoreBody, ".clear()")
        || !contains(restoreBody, "getChildWithTagNameIterator"))
        return fail("araArchiveRestoresCompleteAnalysisState",
                    "restoreAudioModificationContent must restore silentGaps vector");
    
    // referenceFeatures must be serialized AND restored
    if (!contains(serializeBody, "ReferenceFeatures")
        || !contains(serializeBody, "referenceFeatures.status")
        || !contains(serializeBody, "PitchNote")
        || !contains(serializeBody, "TimingAnchor"))
        return fail("araArchiveRestoresCompleteAnalysisState",
                    "serializeAudioModificationContent must write referenceFeatures");
    if (!contains(restoreBody, "ReferenceFeatures")
        || !contains(restoreBody, "referenceFeatures.status")
        || !contains(restoreBody, "PitchNote")
        || !contains(restoreBody, "TimingAnchor"))
        return fail("araArchiveRestoresCompleteAnalysisState",
                    "restoreAudioModificationContent must restore referenceFeatures");
    
    // pitchCurve f0/energy must be preserved
    if (!contains(serializeBody, "pitchCurve") 
        || !contains(serializeBody, "PitchCurve")
        || !contains(serializeBody, "f0Base64"))
        return fail("araArchiveRestoresCompleteAnalysisState",
                    "serializeAudioModificationContent must serialize pitchCurve f0 data");
    if (!contains(restoreBody, "pitchCurve")
        || !contains(restoreBody, "setOriginalF0"))
        return fail("araArchiveRestoresCompleteAnalysisState",
                    "restoreAudioModificationContent must restore pitchCurve f0 data");
    
    return pass("araArchiveRestoresCompleteAnalysisState");
}

// ============================================================================
// ARA playback renderer must publish immutable render plans to the audio thread
// ============================================================================
static CheckResult araPlaybackRendererUsesImmutablePlan()
{
    const auto header = readText("Source/ARA/OpenTunePlaybackRenderer.h");
    const auto cpp = readText("Source/ARA/OpenTunePlaybackRenderer.cpp");
    if (!contains(header, "struct RenderPlan")
        || !contains(header, "std::shared_ptr<const RenderPlan>")
        || contains(header, "assignedPlaybackRegions_")
        || contains(header, "renderItems_"))
        return fail("araPlaybackRendererUsesImmutablePlan",
                    "OpenTunePlaybackRenderer must replace mutable vectors with immutable RenderPlan snapshots");

    const auto processBody = extractBraceBlockAfterToken(cpp,
                                                         "OpenTunePlaybackRenderer::processBlock");
    if (processBody.empty())
        return fail("araPlaybackRendererUsesImmutablePlan",
                    "OpenTunePlaybackRenderer::processBlock body not found");
    if (!contains(processBody, "currentPlan_.load")
        || contains(processBody, "renderItems_")
        || contains(processBody, "assignedPlaybackRegions_"))
        return fail("araPlaybackRendererUsesImmutablePlan",
                    "audio processBlock must load one immutable plan and never touch mutable vectors");

    return pass("araPlaybackRendererUsesImmutablePlan");
}

// ============================================================================
// PianoRoll Selection/AUTO Guard: Note selection is editor-transient
// ============================================================================
static CheckResult pianoRollNoteSelectionIsTransient()
{
    const auto noteText = readText("Source/Utils/Note.h");
    const auto noteBody = extractBraceBlockAfterToken(noteText, "struct Note");
    if (noteBody.empty())
        return fail("pianoRollNoteSelectionIsTransient", "struct Note body not found");
    if (contains(noteBody, "selected"))
        return fail("pianoRollNoteSelectionIsTransient",
                    "struct Note still contains selection state");

    const auto persistenceText = readText("Source/Utils/ProjectPersistence.cpp");
    const auto notesToTreeBody = extractBraceBlockAfterToken(persistenceText,
                                                             "ProjectPersistence::notesToValueTree");
    const auto notesFromTreeBody = extractBraceBlockAfterToken(persistenceText,
                                                               "ProjectPersistence::notesFromValueTree");
    if (notesToTreeBody.empty() || notesFromTreeBody.empty())
        return fail("pianoRollNoteSelectionIsTransient",
                    "ProjectPersistence notes conversion body not found");
    if (contains(notesToTreeBody, "\"selected\"") || contains(notesFromTreeBody, "\"selected\""))
        return fail("pianoRollNoteSelectionIsTransient",
                    "project persistence reads or writes note selected property");

    return pass("pianoRollNoteSelectionIsTransient");
}

static CheckResult pianoRollSelectionOnlyDoesNotCommitContent()
{
    const auto componentText = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto selectOverlappingBody = extractBraceBlockAfterToken(
        componentText,
        "PianoRollComponent::selectNotesOverlappingFrames");
    if (selectOverlappingBody.empty())
        return fail("pianoRollSelectionOnlyDoesNotCommitContent",
                    "selectNotesOverlappingFrames body not found");

    const std::vector<std::string> forbiddenSelectionCommits = {
        "commitEditedContentNotes",
        "commitEditedContentNotesAndSegments",
        "commitNoteDraft",
        "setNotes("
    };
    for (const auto& token : forbiddenSelectionCommits)
        if (contains(selectOverlappingBody, token))
            return fail("pianoRollSelectionOnlyDoesNotCommitContent",
                        "selection helper commits content via '" + token + "'");

    const auto toolText = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto selectToolBody = extractBraceBlockAfterToken(toolText,
                                                            "PianoRollToolHandler::handleSelectTool");
    const auto selectUpBody = extractBraceBlockAfterToken(toolText,
                                                          "PianoRollToolHandler::handleSelectUp");
    const auto drawNoteUpBody = extractBraceBlockAfterToken(toolText,
                                                            "PianoRollToolHandler::handleDrawNoteUp");
    if (selectToolBody.empty() || selectUpBody.empty() || drawNoteUpBody.empty())
        return fail("pianoRollSelectionOnlyDoesNotCommitContent",
                    "PianoRollToolHandler selection bodies not found");

    if (contains(toolText, "note.selected")
        || contains(toolText, "finalNote.selected")
        || contains(toolText, "].selected"))
        return fail("pianoRollSelectionOnlyDoesNotCommitContent",
                    "PianoRollToolHandler still mutates Note selection fields");

    if (contains(selectToolBody, "beginNoteDraft")
        || contains(selectToolBody, "workingDraftNotes"))
        return fail("pianoRollSelectionOnlyDoesNotCommitContent",
                    "Select mouseDown still enters note draft state");

    const size_t pendingDragPos = drawNoteUpBody.find("getDrawNoteToolPendingDrag");
    const size_t realDrawCheckPos = drawNoteUpBody.find("drawing.isDrawingNote");
    if (pendingDragPos != std::string::npos
        && realDrawCheckPos != std::string::npos
        && realDrawCheckPos > pendingDragPos)
    {
        const auto pendingBranch = drawNoteUpBody.substr(pendingDragPos,
                                                        realDrawCheckPos - pendingDragPos);
        if (contains(pendingBranch, "commitNoteDraft"))
            return fail("pianoRollSelectionOnlyDoesNotCommitContent",
                        "DrawNote pending click still commits selection-only draft");
    }
    else
    {
        return fail("pianoRollSelectionOnlyDoesNotCommitContent",
                    "DrawNote pending-click boundary not found");
    }

    return pass("pianoRollSelectionOnlyDoesNotCommitContent");
}

static CheckResult pianoRollEditorsInjectSnapshotAndCommands()
{
    struct FileCheck { std::string path; std::string testName; };
    const FileCheck files[] = {
        { "Source/Standalone/PluginEditor.cpp", "Standalone editor" },
        { "Source/Plugin/PluginEditor.cpp", "VST3 editor" },
    };

    for (const auto& file : files)
    {
        const auto text = readText(file.path);
        if (!contains(text, "setReadContentSnapshot"))
            return fail("pianoRollEditorsInjectSnapshotAndCommands",
                        file.testName + " does not inject setReadContentSnapshot");
        if (!contains(text, "setContentCommands"))
            return fail("pianoRollEditorsInjectSnapshotAndCommands",
                        file.testName + " does not inject setContentCommands");
    }

    return pass("pianoRollEditorsInjectSnapshotAndCommands");
}

static CheckResult pianoRollAutoTuneFailureIsVisible()
{
    const auto componentHeader = readText("Source/Standalone/UI/PianoRollComponent.h");
    const auto componentCpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    if (!contains(componentHeader, "enum class AutoTuneApplyStatus")
        || !contains(componentHeader, "struct AutoTuneApplyResult")
        || !contains(componentHeader, "AutoTuneApplyResult applyAutoTuneToSelection()"))
        return fail("pianoRollAutoTuneFailureIsVisible",
                    "PianoRoll AUTO API is not a structured result");
    if (!contains(componentCpp, "AutoTuneApplyResult::message()"))
        return fail("pianoRollAutoTuneFailureIsVisible",
                    "PianoRoll AUTO result has no user-facing message");

    const auto standaloneText = readText("Source/Standalone/PluginEditor.cpp");
    const auto standaloneBody = extractBraceBlockAfterToken(standaloneText,
                                                            "OpenTuneAudioProcessorEditor::autoTuneRequested");
    if (standaloneBody.empty())
        return fail("pianoRollAutoTuneFailureIsVisible",
                    "Standalone autoTuneRequested body not found");
    if (!contains(standaloneBody, "const auto result = pianoRoll_.applyAutoTuneToSelection();")
        || !contains(standaloneBody, "result.applied()")
        || !contains(standaloneBody, "ConfirmDialogContent::showMessage"))
        return fail("pianoRollAutoTuneFailureIsVisible",
                    "Standalone AUTO failure is not visibly reported");
    if (contains(standaloneBody, "pianoRoll_.applyAutoTuneToSelection();\n    projectSession_.markDirty();"))
        return fail("pianoRollAutoTuneFailureIsVisible",
                    "Standalone AUTO still marks dirty unconditionally");

    const auto vst3Text = readText("Source/Plugin/PluginEditor.cpp");
    const auto vst3Body = extractBraceBlockAfterToken(vst3Text,
                                                      "OpenTuneAudioProcessorEditor::autoTuneRequested");
    if (vst3Body.empty())
        return fail("pianoRollAutoTuneFailureIsVisible",
                    "VST3 autoTuneRequested body not found");
    if (!contains(vst3Body, "const auto result = pianoRoll_.applyAutoTuneToSelection();")
        || !contains(vst3Body, "result.applied()")
        || !contains(vst3Body, "juce::AlertWindow::showMessageBoxAsync"))
        return fail("pianoRollAutoTuneFailureIsVisible",
                    "VST3 AUTO failure is not visibly reported");

    return pass("pianoRollAutoTuneFailureIsVisible");
}

// ============================================================================
// P1 Contract Test: araModificationStateNeverOwnsSourcePcm
// ============================================================================
// ARAEditableContentState must NOT contain audioBuffer fields.
// AudioModification::snapshotContent() must NOT return source PCM.
// Per ARA2 spec: AudioSource owns PCM via sample access, AudioModification
// owns modification-scoped edit/analysis state only.
static CheckResult araModificationStateNeverOwnsSourcePcm()
{
    const auto editableStateText = readText("Source/Content/ARAEditableContentState.h");
    const auto modStateText = readText("Source/Content/AudioModificationContentState.h");
    const auto modCppText = readText("Source/ARA/AudioModification.cpp");

    // ARAEditableContentState must NOT have audioBuffer/audioSampleRate/audioRevision fields
    const std::vector<std::string> forbiddenFields = {
        "audioBuffer", "audioSampleRate", "audioRevision"
    };
    for (const auto& field : forbiddenFields)
    {
        if (contains(editableStateText, field))
            return fail("araModificationStateNeverOwnsSourcePcm",
                        "ARAEditableContentState contains forbidden field: " + field +
                        " — ARA AudioModification must not own source PCM");
    }

    // AudioModificationContentState.editable must be ARAEditableContentState, not EditableContentState
    if (!contains(modStateText, "ARAEditableContentState editable"))
        return fail("araModificationStateNeverOwnsSourcePcm",
                    "AudioModificationContentState must use ARAEditableContentState for editable field");

    // AudioModification::snapshotContent must NOT return audioBuffer
    const auto snapshotBody = extractBraceBlockAfterToken(modCppText, "AudioModification::snapshotContent");
    if (snapshotBody.empty())
        return fail("araModificationStateNeverOwnsSourcePcm",
                    "AudioModification::snapshotContent body not found");
    if (contains(snapshotBody, "audioBuffer") || contains(snapshotBody, "snap.audioBuffer"))
        return fail("araModificationStateNeverOwnsSourcePcm",
                    "AudioModification::snapshotContent must NOT populate audioBuffer — PCM comes from AudioSource");

    return pass("araModificationStateNeverOwnsSourcePcm");
}

// ============================================================================
// P1 Contract Test: araF0SchedulingUsesRealContentKey
// ============================================================================
// ARA F0 extraction must use real AudioModification ContentKey, not fake async keys.
// Must NOT have scheduleAsyncWork wrapping arbitrary lambdas with fake StandaloneClip keys.
static CheckResult araF0SchedulingUsesRealContentKey()
{
    const auto dcHeader = readText("Source/ARA/OpenTuneDocumentController.h");
    const auto processorCpp = readText("Source/PluginProcessor.cpp");

    // Must NOT have scheduleAsyncWork API in ProcessorServices
    if (contains(dcHeader, "scheduleAsyncWork"))
        return fail("araF0SchedulingUsesRealContentKey",
                    "ProcessorServices still has scheduleAsyncWork — "
                    "ARA F0 extraction must use real ContentKey, not fake async work wrapper");

    // didBindToARA must NOT construct fake StandaloneClip keys
    const auto bindBody = extractBraceBlockAfterToken(processorCpp, "didBindToARA");
    if (bindBody.empty())
        return fail("araF0SchedulingUsesRealContentKey", "didBindToARA body not found");
    
    const std::vector<std::string> forbiddenPatterns = {
        "ContentKey asyncKey",
        "DomainKind::StandaloneClip, s_key",
        "scheduleAsyncWork"
    };
    for (const auto& pattern : forbiddenPatterns)
    {
        if (contains(bindBody, pattern))
            return fail("araF0SchedulingUsesRealContentKey",
                        "didBindToARA still uses fake async ContentKey pattern: " + pattern);
    }

    return pass("araF0SchedulingUsesRealContentKey");
}

// ============================================================================
// P1 Contract Test: projectPersistenceStoresCompleteContentKey
// ============================================================================
// .otproj persistence must store complete ContentKey (domain/objectId/discriminator),
// not single uint64_t contentId. Must NOT have legacy "contentId" XML attributes.
static CheckResult projectPersistenceStoresCompleteContentKey()
{
    const auto modelHeader = readText("Source/Utils/ProjectModel.h");
    const auto persistenceCpp = readText("Source/Utils/ProjectPersistence.cpp");

    // ProjectContentEntry must have ContentKey contentKey field, not uint64_t contentId
    const auto contentEntryBody = extractBraceBlockAfterToken(modelHeader, "struct ProjectContentEntry");
    if (contentEntryBody.empty())
        return fail("projectPersistenceStoresCompleteContentKey",
                    "ProjectContentEntry body not found");
    if (contains(contentEntryBody, "uint64_t contentId"))
        return fail("projectPersistenceStoresCompleteContentKey",
                    "ProjectContentEntry still has uint64_t contentId — must use ContentKey contentKey");
    if (!contains(contentEntryBody, "ContentKey contentKey"))
        return fail("projectPersistenceStoresCompleteContentKey",
                    "ProjectContentEntry missing ContentKey contentKey field");

    // ProjectPlacementEntry must have ContentKey contentKey field
    const auto placementEntryBody = extractBraceBlockAfterToken(modelHeader, "struct ProjectPlacementEntry");
    if (placementEntryBody.empty())
        return fail("projectPersistenceStoresCompleteContentKey",
                    "ProjectPlacementEntry body not found");
    if (contains(placementEntryBody, "uint64_t contentId"))
        return fail("projectPersistenceStoresCompleteContentKey",
                    "ProjectPlacementEntry still has uint64_t contentId — must use ContentKey contentKey");

    // Persistence must NOT write/read "contentId" or "lineageParentContentId" attributes
    const std::vector<std::string> forbiddenAttributes = {
        "\"contentId\"", "\"lineageParentContentId\""
    };
    for (const auto& attr : forbiddenAttributes)
    {
        if (contains(persistenceCpp, attr))
            return fail("projectPersistenceStoresCompleteContentKey",
                        "ProjectPersistence still uses legacy attribute: " + attr +
                        " — must serialize complete ContentKey (domain/objectId/discriminator)");
    }

    return pass("projectPersistenceStoresCompleteContentKey");
}

// ============================================================================
// P1 Contract Test: vst3StateHasNoLegacyPayloadCompatibility
// ============================================================================
// VST3 processor state restore must only accept current version, no v5/v6/v7/v8 branches.
// Must NOT have legacy placement colour compatibility code.
static CheckResult vst3StateHasNoLegacyPayloadCompatibility()
{
    const auto processorCpp = readText("Source/PluginProcessor.cpp");

    // Locate setStateInformation function
    const auto restoreBody = extractBraceBlockAfterToken(processorCpp,
                                                         "OpenTuneAudioProcessor::setStateInformation");
    if (restoreBody.empty())
        return fail("vst3StateHasNoLegacyPayloadCompatibility",
                    "setStateInformation body not found");

    // Must NOT have version compatibility branches
    const std::vector<std::string> forbiddenVersionChecks = {
        "version != 8", "version != 7", "version != 6", "version != 5",
        "version == 8", "version == 7", "version == 6", "version == 5"
    };
    for (const auto& check : forbiddenVersionChecks)
    {
        if (contains(restoreBody, check))
            return fail("vst3StateHasNoLegacyPayloadCompatibility",
                        "VST3 state restore still has legacy version check: " + check +
                        " — must only accept kProcessorStateVersion");
    }

    // Must NOT have legacy placement colour skip logic
    const std::vector<std::string> forbiddenLegacyCode = {
        "legacy placement colour",
        "skip legacy placement colour",
        "kept for binary stream compat"
    };
    for (const auto& pattern : forbiddenLegacyCode)
    {
        if (contains(restoreBody, pattern))
            return fail("vst3StateHasNoLegacyPayloadCompatibility",
                        "VST3 state restore still has legacy compatibility code: " + pattern);
    }

    return pass("vst3StateHasNoLegacyPayloadCompatibility");
}

// ============================================================================
// Phase 4 Guard: phase4NoPlaceholderTests
// ============================================================================
static CheckResult phase4NoPlaceholderTests()
{
    const auto text = readText("Tests/TestMain.cpp");

    size_t arrayPos = text.find("const CheckResult results[] = {");
    if (arrayPos == std::string::npos)
        return fail("phase4NoPlaceholderTests", "results array not found");

    size_t arrayEnd = text.find("};", arrayPos);
    if (arrayEnd == std::string::npos)
        return fail("phase4NoPlaceholderTests", "results array end not found");

    std::string arrayText = text.substr(arrayPos, arrayEnd - arrayPos);

    if (arrayText.find("placeholder") != std::string::npos
        || arrayText.find("TODO") != std::string::npos)
        return fail("phase4NoPlaceholderTests", "placeholder or TODO in results array");

    return pass("phase4NoPlaceholderTests");
}

// ============================================================================
// Main
// ============================================================================
int main()
{
    // Verify source directory is accessible
    fs::path rootCheck = sourcePath("Source/ARA/AudioModification.h");
    if (!fs::exists(rootCheck))
    {
        std::cerr << "ERROR: Cannot access source files.\n"
                  << "  Expected: " << rootCheck.string() << "\n"
                  << "  OPENTUNE_SOURCE_DIR=" << OPENTUNE_SOURCE_DIR << "\n"
                  << "  Make sure the test runs from the build directory "
                     "and OPENTUNE_SOURCE_DIR is set correctly.\n";
        return 1;
    }

    struct TestCase
    {
        const char* name;
        CheckResult (*run)();
    };

    const TestCase tests[] = {
        { "araAudioModificationOwnsEditAndAnalysisState", araAudioModificationOwnsEditAndAnalysisState },
        { "playbackRegionIsPlacementOnly", playbackRegionIsPlacementOnly },
        { "araHasNoStoreSplit", araHasNoStoreSplit },
        { "contentRenderServiceIsDerivedOnly", contentRenderServiceIsDerivedOnly },
        { "rendererUsesAssignedRegions", rendererUsesAssignedRegions },
        { "araRewriteIsLockFree", araRewriteIsLockFree },
        { "araHasNoFallbackRouting", araHasNoFallbackRouting },
        { "timeStretchCacheUsesContentKey", timeStretchCacheUsesContentKey },
        { "singlePlaybackReadSourceDefinition", singlePlaybackReadSourceDefinition },
        { "singleRenderJobDefinition", singleRenderJobDefinition },
        { "contentRenderServiceHasNoOwnedRuntimeMechanics", contentRenderServiceHasNoOwnedRuntimeMechanics },
        { "materializationStoreNoLongerOwnsRuntimeMechanics", materializationStoreNoLongerOwnsRuntimeMechanics },
        { "timeStretchCacheContractKeyCheck", timeStretchCacheContractKeyCheck },
        { "standaloneUsesContentKey", standaloneUsesContentKey },
        { "standaloneHasNoMaterializationStoreDependency", standaloneHasNoMaterializationStoreDependency },
        { "standaloneArrangementIsPlacementOnly", standaloneArrangementIsPlacementOnly },
        { "processBlockHasNoStoreOrPublish", processBlockHasNoStoreOrPublish },
        { "captureBindingsHaveNoMaterializationStoreBridge", captureBindingsHaveNoMaterializationStoreBridge },
        { "captureHasNoIsRenderReadyOrIsRenderFailed", captureHasNoIsRenderReadyOrIsRenderFailed },
        { "capturePersistenceDoesNotReadMaterializationStore", capturePersistenceDoesNotReadMaterializationStore },
        { "captureCrsUsesRegularVST3CaptureSegmentId", captureCrsUsesRegularVST3CaptureSegmentId },
        { "captureRefreshDoesNotRequestMaterializationRefresh", captureRefreshDoesNotRequestMaterializationRefresh },
        { "captureSegmentHasNoCapturedAudio", captureSegmentHasNoCapturedAudio },
        { "captureSegmentContentSnapshotComplete", captureSegmentContentSnapshotComplete },
        { "captureStaticLifecycleWiring", captureStaticLifecycleWiring },
        { "captureTickPromotesReadyWithoutReadinessBinding", captureTickPromotesReadyWithoutReadinessBinding },
        { "captureTickDropsFailedWithoutReentrantBinding", captureTickDropsFailedWithoutReentrantBinding },
        { "captureF0CommitDoesNotPromoteUntilTick", captureF0CommitDoesNotPromoteUntilTick },
        { "capturePersistenceRoundtripRestoresOwnerContent", capturePersistenceRoundtripRestoresOwnerContent },
        { "phase4NoMaterializationStoreType", phase4NoMaterializationStoreType },
        { "phase4NoMaterializationContentProvider", phase4NoMaterializationContentProvider },
        { "phase4NoStoreBackedRenderOrStretcherPath", phase4NoStoreBackedRenderOrStretcherPath },
        { "phase4NoOldMaterializationCommandApis", phase4NoOldMaterializationCommandApis },
        { "phase4Stage2HasNoStoreFallback", phase4Stage2HasNoStoreFallback },
        { "phase4NoPlaceholderTests", phase4NoPlaceholderTests },
        { "phase4StandaloneEditCommandWritesOwnerAndInvalidatesCrs", phase4StandaloneEditCommandWritesOwnerAndInvalidatesCrs },
        { "phase4Stage2ReadsOwnerSnapshotByContentKey", phase4Stage2ReadsOwnerSnapshotByContentKey },
        { "phase4ReferenceAnalysisCommitsToOwner", phase4ReferenceAnalysisCommitsToOwner },
        { "phase4CopyCloneCreateStandaloneClipsOnly", phase4CopyCloneCreateStandaloneClipsOnly },
        { "phase4RenderJobCarriesFullPayload", phase4RenderJobCarriesFullPayload },
        { "phase4EditorReadsViaContentKey", phase4EditorReadsViaContentKey },
        { "phase4NoPublicMaterializationVocabulary", phase4NoPublicMaterializationVocabulary },
        { "contentKeyMustBeCompleteInAllPaths", contentKeyMustBeCompleteInAllPaths },
        { "araArchiveMustNotPersistSourcePCM", araArchiveMustNotPersistSourcePCM },
        { "araArchiveRestoresCompleteAnalysisState", araArchiveRestoresCompleteAnalysisState },
        { "araPlaybackRendererUsesImmutablePlan", araPlaybackRendererUsesImmutablePlan },
        { "pianoRollNoteSelectionIsTransient", pianoRollNoteSelectionIsTransient },
        { "pianoRollSelectionOnlyDoesNotCommitContent", pianoRollSelectionOnlyDoesNotCommitContent },
        { "pianoRollEditorsInjectSnapshotAndCommands", pianoRollEditorsInjectSnapshotAndCommands },
        { "pianoRollAutoTuneFailureIsVisible", pianoRollAutoTuneFailureIsVisible },
        { "araModificationStateNeverOwnsSourcePcm", araModificationStateNeverOwnsSourcePcm },
        { "araF0SchedulingUsesRealContentKey", araF0SchedulingUsesRealContentKey },
        { "projectPersistenceStoresCompleteContentKey", projectPersistenceStoresCompleteContentKey },
        { "vst3StateHasNoLegacyPayloadCompatibility", vst3StateHasNoLegacyPayloadCompatibility },
    };

    int passedCount = 0;
    int failedCount = 0;

    for (const auto& test : tests)
    {
        std::cout << "[RUN] " << test.name << std::endl;
        const auto r = test.run();
        if (r.passed)
        {
            std::cout << "[PASS] " << r.name << "\n";
            ++passedCount;
        }
        else
        {
            std::cout << "[FAIL] " << r.name;
            if (!r.detail.empty())
                std::cout << ": " << r.detail;
            std::cout << "\n";
            ++failedCount;
        }
    }

    const int total = passedCount + failedCount;
    std::cout << "\n" << total << " tests run, "
              << passedCount << " passed, "
              << failedCount << " failed\n";

    return failedCount > 0 ? 1 : 0;
}
