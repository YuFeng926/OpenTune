// OpenTune ARA Architecture Contract Tests
// Source-scan contract tests verifying architecture boundaries at compile time.
// Oracle review: commit e0326f0 / docs/plans/2026-06-05-ara-audiomodification-content-root.md
//
// All tests use text scanning — no runtime object creation.
// Each test checks that required tokens exist and forbidden tokens do not exist
// in the specified source files.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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

// Check that ALL forbidden tokens are absent. Returns empty string on success,
// or a description of the first found forbidden token.
static std::string forbidAll(const std::string& text,
                             const std::vector<std::string>& tokens)
{
    for (const auto& t : tokens)
        if (contains(text, t))
            return "found forbidden token: '" + t + "'";
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
        // Phase 0 contract tests — all expected to FAIL with current code
        singlePlaybackReadSourceDefinition(),
        singleRenderJobDefinition(),
        contentRenderServiceHasNoOwnedRuntimeMechanics(),
        materializationStoreNoLongerOwnsRuntimeMechanics(),
        timeStretchCacheContractKeyCheck(),
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
