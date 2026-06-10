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
#include "Plugin/Capture/CaptureSession.h"
#include "Utils/PitchCurve.h"

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
// Test 1: AudioModification content ownership
// ============================================================================
// AudioModification owns content state. Must have content identity and
// content mutation APIs. Must NOT use materializationId as ARA content identity.
static CheckResult audioModificationOwnsContent()
{
    const auto text = readText("Source/ARA/AudioModification.h")
                    + readText("Source/ARA/AudioModification.cpp");

    const std::vector<std::string> required = {
        "AudioModificationContentState", "ContentKey", "contentKey()",
        "snapshotContent", "applyNotes", "applyPitchCurve",
        "applyTimeGrid", "retireContent", "reviveContent"
    };

    auto missing = requireAll(text, required);
    if (!missing.empty())
        return fail("audioModificationOwnsContent", missing);

    // materializationId must NOT appear as a word (only materializationDurationSeconds is ok)
    if (containsWord(text, "materializationId"))
        return fail("audioModificationOwnsContent",
                    "forbidden token 'materializationId' found — "
                    "content identity must use ContentKey, not materializationId");

    return pass("audioModificationOwnsContent");
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
        "assignedPlaybackRegions_", "didAddPlaybackRegion",
        "willRemovePlaybackRegion", "getPlaybackRegionProjectionsFor"
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
    // 1) MaterializationStore.h must NOT define struct PlaybackReadSource
    {
        const auto text = readText("Source/MaterializationStore.h");
        if (contains(text, "struct PlaybackReadSource"))
        {
            auto loc = locateInText(text, "struct PlaybackReadSource", "MaterializationStore.h");
            return fail("singlePlaybackReadSourceDefinition",
                        "PlaybackReadSource defined in " + loc +
                        " — must be defined ONLY in Source/Render/PlaybackReadSource.h");
        }
    }

    // 2) ContentRenderService.h must NOT define struct PlaybackReadSource
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

    // 3) PlaybackReadSource.h must exist and contain the definition
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
    // 1) MaterializationStore.h must NOT define struct PendingRenderJob
    {
        const auto text = readText("Source/MaterializationStore.h");
        if (contains(text, "struct PendingRenderJob"))
        {
            auto loc = locateInText(text, "struct PendingRenderJob", "MaterializationStore.h");
            return fail("singleRenderJobDefinition",
                        "PendingRenderJob defined in " + loc +
                        " — must be defined ONLY in Source/Render/RenderJob.h");
        }
    }

    // 2) ContentRenderService.h must NOT define struct PendingRenderJob
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

    // 3) RenderJob.h must exist and contain the definition
    {
        const auto text = readText("Source/Render/RenderJob.h");
        if (text.empty())
            return fail("singleRenderJobDefinition",
                        "Source/Render/RenderJob.h not found or empty"
                        " — PendingRenderJob must live in its own header");
        if (!contains(text, "PendingRenderJob"))
            return fail("singleRenderJobDefinition",
                        "PendingRenderJob token not found in Source/Render/RenderJob.h");
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

// ============================================================================
// Contract Test 12: materializationStoreNoLongerOwnsRuntimeMechanics
//
// MaterializationStore must NOT own playback source caches, per-entry render
// caches, rebuildPlaybackSourceCache, or per-materialization
// SoundTouch stretchers. These belong to ContentRenderService.
//
// Scan: Source/MaterializationStore.h, Source/MaterializationStore.cpp
// ============================================================================
static CheckResult materializationStoreNoLongerOwnsRuntimeMechanics()
{
    const auto hText   = readText("Source/MaterializationStore.h");
    const auto cppText = readText("Source/MaterializationStore.cpp");
    const auto combined = hText + cppText;

    // 1) playbackSourceCache_ and rebuildPlaybackSourceCache at file scope
    {
        const std::vector<std::string> forbidden = {
            "playbackSourceCache_", "rebuildPlaybackSourceCache"
        };
        for (const auto& t : forbidden)
        {
            if (contains(combined, t))
            {
                std::string file;
                if (contains(hText, t))
                    file = "MaterializationStore.h";
                else if (contains(cppText, t))
                    file = "MaterializationStore.cpp";
                else
                    file = "MaterializationStore.{h,cpp}";

                auto src = (file == "MaterializationStore.h") ? hText : cppText;
                auto loc = locateInText(src, t, file);
                return fail("materializationStoreNoLongerOwnsRuntimeMechanics",
                            "forbidden token '" + t + "' in " + loc +
                            " — Store must not own playback source cache mechanics");
            }
        }
    }

    // 2) std::unique_ptr<SoundTouchStretcher> stretcher in MaterializationEntry
    if (contains(hText, "std::unique_ptr<SoundTouchStretcher> stretcher"))
    {
        auto loc = locateInText(hText,
                                "std::unique_ptr<SoundTouchStretcher> stretcher",
                                "MaterializationStore.h");
        return fail("materializationStoreNoLongerOwnsRuntimeMechanics",
                    "per-entry std::unique_ptr<SoundTouchStretcher> stretcher in " + loc +
                    " — stretcher ownership belongs to ContentRenderService, not Store");
    }

    // 3) Entry-owned renderCache inside MaterializationEntry struct
    {
        size_t entryPos = hText.find("struct MaterializationEntry");
        if (entryPos != std::string::npos)
        {
            size_t bracePos = hText.find("{", entryPos);
            if (bracePos != std::string::npos)
            {
                int depth = 0;
                size_t closePos = bracePos;
                for (size_t i = bracePos; i < hText.size(); ++i)
                {
                    if (hText[i] == '{') ++depth;
                    if (hText[i] == '}') { --depth; if (depth == 0) { closePos = i; break; } }
                }
                std::string entryBody = hText.substr(
                    bracePos, closePos - bracePos + 1);
                if (contains(entryBody, "std::shared_ptr<RenderCache> renderCache"))
                {
                    auto loc = locateInText(hText,
                                            "std::shared_ptr<RenderCache> renderCache",
                                            "MaterializationStore.h");
                    return fail("materializationStoreNoLongerOwnsRuntimeMechanics",
                                "MaterializationEntry owns renderCache at " + loc +
                                " — per-entry renderCache should not live in Store;"
                                " ContentRenderService manages RenderCache lifecycle");
                }
            }
        }
    }

    return pass("materializationStoreNoLongerOwnsRuntimeMechanics");
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
            "requestMaterializationRefresh",
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
    const auto captureBindings = extractCaptureBindings();
    if (captureBindings.empty())
        return fail("captureCrsUsesRegularVST3CaptureSegmentId",
                    "Cannot extract VST3 capture bindings from PluginProcessor.cpp");

    // Required: ContentKey with RegularVST3Capture domain
    if (!contains(captureBindings, "DomainKind::RegularVST3Capture"))
        return fail("captureCrsUsesRegularVST3CaptureSegmentId",
                    "Capture bindings must use ContentKey with DomainKind::RegularVST3Capture");

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
        "requestMaterializationRefresh",
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
        std::vector<uint64_t> retired;
        std::vector<uint64_t> refreshed;
        std::vector<uint64_t> active;
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
                                           uint64_t,
                                           double,
                                           double) {};
        bindings.retireSegment = [&spies, reentrantSession](uint64_t id) {
            spies.retired.push_back(id);
            if (reentrantSession != nullptr && *reentrantSession != nullptr)
                (void)(*reentrantSession)->findSegmentById(id);
        };
        bindings.refreshSegment = [&spies](uint64_t id) {
            spies.refreshed.push_back(id);
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
    session.setActiveSegmentChangedCallback([&spies](uint64_t id) {
        spies.active.push_back(id);
    });

    const uint64_t id = session.testInjectProcessingSegment(
        1.0, 0.01, 101, makeCaptureTestAudio(), 48000.0);
    if (!session.commitSegmentF0Result(
            id, makeCaptureTestPitchCurve(),
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
    if (spies.active.size() != 1 || spies.active[0] != id)
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
    session.setActiveSegmentChangedCallback([&spies](uint64_t id) {
        spies.active.push_back(id);
    });

    const uint64_t id = session.testInjectProcessingSegment(
        2.0, 0.01, 202, makeCaptureTestAudio(), 48000.0);
    session.commitSegmentF0Result(id, nullptr, OpenTune::OriginalF0State::Failed, OpenTune::DetectedKey{});

    session.tick();

    if (session.findSegmentById(id) != nullptr)
        return fail("captureTickDropsFailedWithoutReentrantBinding",
                    "Failed Processing segment was not removed");
    if (spies.retired.size() != 1 || spies.retired[0] != id)
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
    session.commitSegmentF0Result(
        id, makeCaptureTestPitchCurve(),
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
    original.commitSegmentF0Result(
        id, makeCaptureTestPitchCurve(),
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

    const CheckResult results[] = {
        audioModificationOwnsContent(),
        playbackRegionIsPlacementOnly(),
        araHasNoStoreSplit(),
        contentRenderServiceIsDerivedOnly(),
        rendererUsesAssignedRegions(),
        araRewriteIsLockFree(),
        araHasNoFallbackRouting(),
        timeStretchCacheUsesContentKey(),
        // Phase 0 contract tests
        singlePlaybackReadSourceDefinition(),
        singleRenderJobDefinition(),
        contentRenderServiceHasNoOwnedRuntimeMechanics(),
        materializationStoreNoLongerOwnsRuntimeMechanics(),
        timeStretchCacheContractKeyCheck(),
        // Phase 2 contract tests
        standaloneUsesContentKey(),
        standaloneHasNoMaterializationStoreDependency(),
        standaloneArrangementIsPlacementOnly(),
        processBlockHasNoStoreOrPublish(),
        // Phase 3 capture contract tests (P0 guards)
        captureBindingsHaveNoMaterializationStoreBridge(),
        captureHasNoIsRenderReadyOrIsRenderFailed(),
        capturePersistenceDoesNotReadMaterializationStore(),
        captureCrsUsesRegularVST3CaptureSegmentId(),
        captureRefreshDoesNotRequestMaterializationRefresh(),
        captureSegmentHasNoCapturedAudio(),
        // Phase 3 P1/P2 verification tests
        captureSegmentContentSnapshotComplete(),
        captureStaticLifecycleWiring(),
        captureTickPromotesReadyWithoutReadinessBinding(),
        captureTickDropsFailedWithoutReentrantBinding(),
        captureF0CommitDoesNotPromoteUntilTick(),
        capturePersistenceRoundtripRestoresOwnerContent(),
    };

    int passedCount = 0;
    int failedCount = 0;

    for (const auto& r : results)
    {
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
