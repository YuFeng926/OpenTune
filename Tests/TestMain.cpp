#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef OPENTUNE_SOURCE_DIR
#define OPENTUNE_SOURCE_DIR "."
#endif

namespace {

struct CheckResult
{
    std::string name;
    bool passed{false};
    std::string detail;
};

std::filesystem::path sourceRoot()
{
    return std::filesystem::path(OPENTUNE_SOURCE_DIR);
}

std::string readText(const std::filesystem::path& relativePath)
{
    const auto path = sourceRoot() / relativePath;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};

    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

bool fileExists(const std::filesystem::path& relativePath)
{
    return std::filesystem::exists(sourceRoot() / relativePath);
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

bool containsAll(const std::string& text, const std::vector<std::string>& required)
{
    for (const auto& token : required)
        if (!contains(text, token))
            return false;

    return true;
}

bool lacksAll(const std::string& text, const std::vector<std::string>& forbidden)
{
    for (const auto& token : forbidden)
        if (contains(text, token))
            return false;

    return true;
}

CheckResult pass(std::string name)
{
    return CheckResult{std::move(name), true, {}};
}

CheckResult fail(std::string name, std::string detail)
{
    return CheckResult{std::move(name), false, std::move(detail)};
}

std::string removedPublicationName()
{
    return std::string("Render") + "Snapshot";
}

std::vector<std::string> noLockTokens()
{
    return {
        std::string("std::") + "mutex",
        std::string("std::") + "lock_guard",
        std::string("condition_") + "variable",
        std::string("std::") + "thread",
        std::string("std::") + "atomic",
        std::string("atomic") + "<",
        std::string("Spin") + "Lock",
        std::string("Critical") + "Section",
        std::string("Scoped") + "Lock",
        std::string("App") + "Logger",
    };
}

std::string allAraText()
{
    const std::vector<std::filesystem::path> files{
        "Source/ARA/AudioSource.h",
        "Source/ARA/AudioSource.cpp",
        "Source/ARA/AudioModification.h",
        "Source/ARA/AudioModification.cpp",
        "Source/ARA/PlaybackRegion.h",
        "Source/ARA/PlaybackRegion.cpp",
        "Source/ARA/OpenTuneDocumentController.h",
        "Source/ARA/OpenTuneDocumentController.cpp",
        "Source/ARA/OpenTuneEditorView.h",
        "Source/ARA/OpenTuneEditorView.cpp",
        "Source/ARA/OpenTunePlaybackRenderer.h",
        "Source/ARA/OpenTunePlaybackRenderer.cpp",
    };

    std::string combined;
    for (const auto& file : files)
    {
        combined += "\n// ";
        combined += file.generic_string();
        combined += "\n";
        combined += readText(file);
    }

    return combined;
}

CheckResult araModelFilesUseOfficialNames()
{
    const std::vector<std::filesystem::path> requiredFiles{
        "Source/ARA/AudioSource.h",
        "Source/ARA/AudioSource.cpp",
        "Source/ARA/AudioModification.h",
        "Source/ARA/AudioModification.cpp",
        "Source/ARA/PlaybackRegion.h",
        "Source/ARA/PlaybackRegion.cpp",
        "Source/ARA/OpenTuneDocumentController.h",
        "Source/ARA/OpenTuneDocumentController.cpp",
        "Source/ARA/OpenTuneEditorView.h",
        "Source/ARA/OpenTuneEditorView.cpp",
        "Source/ARA/OpenTunePlaybackRenderer.h",
        "Source/ARA/OpenTunePlaybackRenderer.cpp",
    };

    for (const auto& file : requiredFiles)
        if (!fileExists(file))
            return fail("ARA files use official model names", "missing " + file.generic_string());

    const auto removedHeader = std::filesystem::path("Source/ARA") / (removedPublicationName() + ".h");
    const auto removedSource = std::filesystem::path("Source/ARA") / (removedPublicationName() + ".cpp");
    if (fileExists(removedHeader) || fileExists(removedSource))
        return fail("ARA files use official model names", "old publication object still exists");

    return pass("ARA files use official model names");
}

CheckResult cmakeMountsOnlyNewAraModel()
{
    const auto cmake = readText("CMakeLists.txt");
    const std::vector<std::string> required{
        "Source/ARA/AudioSource.cpp",
        "Source/ARA/AudioModification.cpp",
        "Source/ARA/PlaybackRegion.cpp",
        "Source/ARA/OpenTuneDocumentController.cpp",
        "Source/ARA/OpenTuneEditorView.cpp",
        "Source/ARA/OpenTunePlaybackRenderer.cpp",
        "Tests/TestMain.cpp",
        "OPENTUNE_SOURCE_DIR",
    };

    if (!containsAll(cmake, required))
        return fail("CMake mounts only the new ARA model", "missing required source/test entry");

    const std::vector<std::string> forbidden{
        std::string("Source/ARA/") + removedPublicationName() + ".cpp",
        std::string("Source/ARA/") + removedPublicationName() + ".h",
        std::string("VST3") + "AraSession",
        "AraMinimalTest",
        "AraFinalTest",
        "AraRuntimeSmoke",
    };

    if (!lacksAll(cmake, forbidden))
        return fail("CMake mounts only the new ARA model", "old ARA source/test token remains");

    return pass("CMake mounts only the new ARA model");
}

CheckResult araRewriteIsLockFree()
{
    const auto araText = allAraText();
    if (!lacksAll(araText, noLockTokens()))
        return fail("Source/ARA rewrite is lock-free", "lock/thread primitive found in Source/ARA");

    return pass("Source/ARA rewrite is lock-free");
}

CheckResult sourceAraIsolatedFromOldAndNonAraRuntime()
{
    const auto araText = allAraText();
    const std::vector<std::string> forbidden{
        removedPublicationName(),
        std::string("load") + "Snapshot",
        std::string("requestBirthForPreferred") + "Region",
        std::string("VST3") + "AraSession",
        std::string("AraDocument") + "Model",
        std::string("Source") + "Store",
        std::string("Materialization") + "Store",
        std::string("Standalone") + "Arrangement",
        std::string("Capture") + "Session",
    };

    if (!lacksAll(araText, forbidden))
        return fail("Source/ARA stays isolated from old and non-ARA runtime", "forbidden token found");

    return pass("Source/ARA stays isolated from old and non-ARA runtime");
}

CheckResult playbackRegionIsPlacementOnly()
{
    const auto header = readText("Source/ARA/PlaybackRegion.h");
    const std::vector<std::string> required{
        "startInPlaybackTime",
        "startInModificationTime",
        "durationInPlaybackTime",
        "durationInModificationTime",
        "audioModificationPersistentId",
        "placementRevision",
    };

    if (!containsAll(header, required))
        return fail("PlaybackRegion is placement-only", "missing placement field");

    const std::vector<std::string> forbidden{
        "contentWindow",
        "sourceId",
        "materializationId",
        "materializationRevision",
        "birthState",
        "HostAudioReader",
        "readAudioSamples",
    };

    if (!lacksAll(header, forbidden))
        return fail("PlaybackRegion is placement-only", "content/materialization field leaked into placement");

    return pass("PlaybackRegion is placement-only");
}

CheckResult audioModificationOwnsContent()
{
    const auto text = readText("Source/ARA/AudioModification.h")
        + readText("Source/ARA/AudioModification.cpp");

    const std::vector<std::string> required{
        "contentWindow",
        "sourceId",
        "materializationId",
        "materializationRevision",
        "birthState",
        "attachSource",
        "resetContent",
        "isRenderable",
    };

    if (!containsAll(text, required))
        return fail("AudioModification owns content and materialization", "missing content/materialization token");

    return pass("AudioModification owns content and materialization");
}

CheckResult documentControllerOwnsTopLevelAraModel()
{
    const auto text = readText("Source/ARA/OpenTuneDocumentController.h")
        + readText("Source/ARA/OpenTuneDocumentController.cpp");

    const std::vector<std::string> required{
        "std::vector<AudioSource>",
        "std::vector<AudioModification>",
        "std::vector<PlaybackRegion>",
        "PlaybackRegionProjection",
        "getPlaybackRegionProjectionsFor",
        "getEditorSelectionPlaybackRegionProjections",
        "getFocusedEditorPlaybackRegionProjection",
        "setEditorViewSelectionPlaybackRegions",
        "requestBirthForFocusedEditorPlaybackRegion",
        "referencesMaterialization",
        "birthMaterializationForRegion",
    };

    if (!containsAll(text, required))
        return fail("DocumentController owns top-level ARA model", "missing top-level model API");

    const std::vector<std::string> forbidden{
        std::string("load") + "Snapshot",
        std::string("requestBirthForPreferred") + "Region",
        std::string("requestBirthFor") + "PlaybackRegion",
        "preferredPlaybackRegion_",
        "getPreferredPlaybackRegionProjection",
        "selectPlaybackRegion",
        "prepareModificationContentFromSelectedRegionLocked",
        std::string("getSharedSource") + "Store",
        std::string("getSharedMaterialization") + "Store",
    };

    if (!lacksAll(text, forbidden))
        return fail("DocumentController owns top-level ARA model", "old controller API remains");

    return pass("DocumentController owns top-level ARA model");
}

CheckResult documentControllerOwnsMaterializationBindingPersistence()
{
    const auto text = readText("Source/ARA/OpenTuneDocumentController.h")
        + readText("Source/ARA/OpenTuneDocumentController.cpp");

    const std::vector<std::string> required{
        "RestoredMaterializationBinding",
        "pendingRestoredBindings_",
        "applyRestoredBinding",
        "applyPendingRestoredBinding",
        "rememberPendingRestoredBinding",
        "doRestoreObjectsFromStream",
        "doStoreObjectsToStream",
        "getAudioModificationToRestoreStateWithID",
        "kMaterializationBindingArchiveMagic",
        "kMaterializationBindingArchiveVersion",
        "kMaxMaterializationBindingRecords",
    };

    if (!containsAll(text, required))
        return fail("DocumentController owns materialization binding persistence",
                    "missing store/restore helper or pending binding token");

    return pass("DocumentController owns materialization binding persistence");
}

CheckResult editorViewFollowsViewSelectionRole()
{
    const auto araText = readText("Source/ARA/OpenTuneEditorView.h")
        + readText("Source/ARA/OpenTuneEditorView.cpp")
        + readText("Source/ARA/OpenTuneDocumentController.h")
        + readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto pluginEditorText = readText("Source/Plugin/PluginEditor.h")
        + readText("Source/Plugin/PluginEditor.cpp");
    const auto text = araText + pluginEditorText;

    const std::vector<std::string> required{
        "OpenTuneEditorView",
        "ARAEditorView",
        "doCreateEditorView",
        "doNotifySelection",
        "getEffectivePlaybackRegions",
        "setEditorViewSelectionPlaybackRegions",
        "getEditorSelectionPlaybackRegionProjections",
        "getFocusedEditorPlaybackRegionProjection",
        "requestBirthForFocusedEditorPlaybackRegion",
        "AudioProcessorEditorARAExtension",
    };

    if (!containsAll(text, required))
        return fail("EditorView follows ARA ViewSelection role", "missing EditorView/ViewSelection token");

    const std::vector<std::string> forbidden{
        "preferredPlaybackRegion_",
        "getPreferredPlaybackRegionProjection",
        "selectPlaybackRegion",
        std::string("requestBirthForPreferred") + "Region",
        std::string("requestBirthFor") + "PlaybackRegion",
    };

    if (!lacksAll(text, forbidden))
        return fail("EditorView follows ARA ViewSelection role", "old preferred-region UI path remains");

    return pass("EditorView follows ARA ViewSelection role");
}

CheckResult playbackRendererFollowsAssignedRegionRole()
{
    const auto headerText = readText("Source/ARA/OpenTunePlaybackRenderer.h");
    const auto rendererText = readText("Source/ARA/OpenTunePlaybackRenderer.cpp");
    const auto text = headerText + rendererText;

    const std::vector<std::string> required{
        "PlaybackRegionRenderItem",
        "assignedPlaybackRegions_",
        "renderItems_",
        "refreshRenderPlanFromDocument",
        "didAddPlaybackRegion",
        "willRemovePlaybackRegion",
        "getPlaybackRegionProjectionsFor(assignedPlaybackRegions_)",
        "getPlaybackReadSource",
        "readPlaybackAudio",
    };

    if (!containsAll(text, required))
        return fail("PlaybackRenderer follows assigned playback-region role", "missing assigned-region render contract");

    const std::vector<std::string> forbidden{
        std::string("load") + "Snapshot",
        std::string("VST3") + "AraSession",
        std::string("Source") + "Store",
        std::string("Capture") + "Session",
    };

    if (!lacksAll(text, forbidden))
        return fail("PlaybackRenderer follows assigned playback-region role", "old runtime dependency remains");

    const std::vector<std::string> silentHandlingRequired{
        "buffer.clear();",
        "return true;",
    };

    if (!containsAll(rendererText, silentHandlingRequired))
        return fail("PlaybackRenderer follows assigned playback-region role",
                    "renderer must clear buffers and report handled ARA silence");

    const std::vector<std::string> rendererForbidden{
        "return false;",
        "return processedAny;",
    };

    if (!lacksAll(rendererText, rendererForbidden))
        return fail("PlaybackRenderer follows assigned playback-region role",
                    "renderer must not trigger non-ARA fallback from ARA processBlock");

    return pass("PlaybackRenderer follows assigned playback-region role");
}

CheckResult araHostTransportMirrorIsLockFree()
{
    const auto headerText = readText("Source/PluginProcessor.h");
    const auto processorText = readText("Source/PluginProcessor.cpp");
    const auto pluginEditorText = readText("Source/Plugin/PluginEditor.cpp");
    const auto text = headerText + processorText + pluginEditorText;

    const std::vector<std::string> required{
        "updateHostTransportSnapshot",
        "getHostTransportSnapshot",
        "PositionInfo",
        "getIsLooping",
        "loopEnabled",
        "setLoopEnabled",
        "isLoopEnabled",
        "const bool loopEnabled = processorRef_.isLoopEnabled();",
        "transportBar_.setLoopEnabled(loopEnabled);",
    };

    if (!containsAll(text, required))
        return fail("ARA host transport UI mirror is lock-free",
                    "missing host transport mirror API or PositionInfo source");

    const std::vector<std::string> forbidden{
        "hostTransportSnapshotLock_",
        "juce::SpinLock hostTransportSnapshotLock_",
        "HostTransportSnapshot:",
        "AppLogger::log(\"HostTransportSnapshot",
        "AppLogger::info(\"HostTransportSnapshot",
        "AppLogger::warn(\"HostTransportSnapshot",
        "AppLogger::debug(\"HostTransportSnapshot",
    };

    if (!lacksAll(text, forbidden))
        return fail("ARA host transport UI mirror is lock-free",
                    "explicit lock or realtime HostTransportSnapshot logging remains");

    return pass("ARA host transport UI mirror is lock-free");
}

CheckResult standaloneArrangementSnapshotIsLockFree()
{
    const std::string text = readText("Source/StandaloneArrangement.h")
                           + readText("Source/StandaloneArrangement.cpp");

    // --- Required: lock-free atomic pattern ---
    const std::vector<std::string> required{
        "std::atomic_load",
        "std::atomic_exchange",
        "retiredSnapshots_",
    };
    if (!containsAll(text, required))
        return fail("StandaloneArrangement snapshot is lock-free",
                    "missing atomic_load/exchange or retiredSnapshots_");

    // --- Forbidden: SpinLock or single-slot retirement workaround ---
    const std::vector<std::string> forbidden{
        "snapshotLock_",
        "juce::SpinLock",
        "previousSnapshot_",
    };
    if (!lacksAll(text, forbidden))
        return fail("StandaloneArrangement snapshot is lock-free",
                    "snapshotLock_/SpinLock/previousSnapshot_ found — must use atomic exchange + retire list");

    return pass("StandaloneArrangement snapshot is lock-free");
}

} // namespace

int main()
{
    const std::vector<CheckResult> results{
        araModelFilesUseOfficialNames(),
        cmakeMountsOnlyNewAraModel(),
        araRewriteIsLockFree(),
        sourceAraIsolatedFromOldAndNonAraRuntime(),
        playbackRegionIsPlacementOnly(),
        audioModificationOwnsContent(),
        documentControllerOwnsTopLevelAraModel(),
        documentControllerOwnsMaterializationBindingPersistence(),
        editorViewFollowsViewSelectionRole(),
        playbackRendererFollowsAssignedRegionRole(),
        araHostTransportMirrorIsLockFree(),
        standaloneArrangementSnapshotIsLockFree(),
    };

    bool allPassed = true;
    for (const auto& result : results)
    {
        std::cout << (result.passed ? "[PASS] " : "[FAIL] ") << result.name;
        if (!result.detail.empty())
            std::cout << " - " << result.detail;
        std::cout << '\n';
        allPassed = allPassed && result.passed;
    }

    return allPassed ? 0 : 1;
}
