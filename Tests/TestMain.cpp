#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef OPENTUNE_SOURCE_DIR
#error "OPENTUNE_SOURCE_DIR must be defined by CMake"
#endif

namespace {

struct CheckResult {
    std::string name;
    bool passed{false};
    std::string detail;
};

CheckResult pass(std::string name)
{
    return {std::move(name), true, {}};
}

CheckResult fail(std::string name, std::string detail)
{
    return {std::move(name), false, std::move(detail)};
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

std::string missingTokens(std::string_view text, const std::vector<std::string_view>& tokens)
{
    std::ostringstream missing;
    for (const auto token : tokens) {
        if (!contains(text, token))
            missing << "\n  - " << token;
    }
    return missing.str();
}

bool inOrder(std::string_view text, const std::vector<std::string_view>& tokens)
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

std::string extractFunctionBlock(std::string_view text, std::string_view signature)
{
    const size_t sigPos = text.find(signature);
    if (sigPos == std::string_view::npos)
        return {};

    const size_t bracePos = text.find('{', sigPos);
    if (bracePos == std::string_view::npos)
        return {};

    int depth = 0;
    for (size_t i = bracePos; i < text.size(); ++i) {
        if (text[i] == '{') {
            ++depth;
        } else if (text[i] == '}') {
            --depth;
            if (depth == 0)
                return std::string(text.substr(sigPos, i - sigPos + 1));
        }
    }
    return {};
}

std::string extractBlock(std::string_view text, std::string_view marker, std::string_view endMarker)
{
    const size_t markerPos = text.find(marker);
    if (markerPos == std::string_view::npos)
        return {};

    const size_t endPos = text.find(endMarker, markerPos);
    if (endPos == std::string_view::npos)
        return {};

    return std::string(text.substr(markerPos, endPos - markerPos + endMarker.size()));
}

std::string readSourceFilesUnder(std::string_view relativeDir)
{
    std::ostringstream joined;
    const auto root = sourcePath(relativeDir);
    if (!std::filesystem::exists(root))
        return {};

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file())
            continue;

        const auto ext = entry.path().extension().string();
        if (ext != ".h" && ext != ".cpp" && ext != ".hpp")
            continue;

        std::ifstream in(entry.path(), std::ios::binary);
        joined << "\n// FILE: " << entry.path().string() << "\n" << in.rdbuf();
    }
    return joined.str();
}

CheckResult requireTokens(std::string name,
                          std::string_view text,
                          const std::vector<std::string_view>& tokens)
{
    const auto missing = missingTokens(text, tokens);
    if (!missing.empty())
        return fail(std::move(name), "missing required tokens:" + missing);
    return pass(std::move(name));
}

CheckResult araObjectBoundariesMatchOfficialRoles()
{
    const auto audioSource = readText("Source/ARA/AudioSource.cpp");
    const auto audioModification = readText("Source/ARA/AudioModification.cpp");
    const auto audioModificationState = readText("Source/Content/AudioModificationContentState.h");
    const auto playbackRegion = readText("Source/ARA/PlaybackRegion.cpp");

    const auto sourceCheck = missingTokens(audioSource, {
        "ARA::PlugIn::HostAudioReader",
        "readAudioSamples",
        "sourceSampleRate",
        "numSamples"
    });
    if (!sourceCheck.empty())
        return fail("araObjectBoundariesMatchOfficialRoles", "AudioSource no longer owns host sample access:" + sourceCheck);

    const auto modCheck = missingTokens(audioModification, {
        "snap->audioBuffer = nullptr",
        "snap->audioSampleRate = 0.0",
        "snap->sourceWindow = content.sourceWindow",
        "snap->notes = content.editable.notes",
        "snap->pitchCurve = content.analysis.pitchCurve"
    });
    if (!modCheck.empty())
        return fail("araObjectBoundariesMatchOfficialRoles", "AudioModification snapshot boundary regressed:" + modCheck);

    if (contains(audioModificationState, "audioBuffer;")
        || contains(audioModificationState, "AudioBuffer<"))
        return fail("araObjectBoundariesMatchOfficialRoles",
                    "AudioModificationContentState must not contain source PCM fields.");

    if (contains(playbackRegion, "AudioBuffer")
        || contains(playbackRegion, "PitchCurve")
        || contains(playbackRegion, "notes"))
        return fail("araObjectBoundariesMatchOfficialRoles",
                    "PlaybackRegion must stay placement-only.");

    return pass("araObjectBoundariesMatchOfficialRoles");
}

CheckResult araArchiveRestoresFromAudioSourceOnly()
{
    const auto dc = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto serialize = extractFunctionBlock(dc, "void serializeAudioModificationContent");
    const auto restore = extractFunctionBlock(dc, "bool OpenTuneDocumentController::doRestoreObjectsFromStream");

    if (serialize.empty() || restore.empty())
        return fail("araArchiveRestoresFromAudioSourceOnly", "could not locate archive/restore functions.");

    const auto serializeMissing = missingTokens(serialize, {
        "AudioModificationContent",
        "SourceWindow",
        "sourcePersistentId",
        "EditableContent",
        "AnalysisState"
    });
    if (!serializeMissing.empty())
        return fail("araArchiveRestoresFromAudioSourceOnly", "archive no longer writes modification-scoped state:" + serializeMissing);

    if (contains(serialize, "audioBuffer")
        || contains(serialize, "SourceAudio")
        || contains(serialize, "PCM")
        || contains(serialize, "copiedAudio"))
        return fail("araArchiveRestoresFromAudioSourceOnly",
                    "ARA archive serialization must not mention source PCM buffers.");

    // Per explicit-read architecture: restore only restores modification-scoped state.
    // It does NOT rebuild CRS, read AudioSource samples, or schedule F0.
    // CRS is lazily rebuilt by the next explicit user read (requestReadAudioForPlaybackRegions).
    if (contains(restore, "rebuildCRSFromSource"))
        return fail("araArchiveRestoresFromAudioSourceOnly",
                    "ARA restore must NOT rebuild CRS automatically. CRS is user-commanded only.");

    if (contains(restore, "readAudioSamples"))
        return fail("araArchiveRestoresFromAudioSourceOnly",
                    "ARA restore must NOT read AudioSource samples. Reading host audio requires explicit user intent.");

    if (contains(restore, "scheduleAsyncF0Extraction"))
        return fail("araArchiveRestoresFromAudioSourceOnly",
                    "ARA restore must NOT schedule F0 extraction. F0 inference is user-commanded only.");

    if (contains(restore, "targetMod->content.audio")
        || contains(restore, "targetMod->content.editable.audio")
        || contains(restore, "publishPlaybackSource"))
        return fail("araArchiveRestoresFromAudioSourceOnly",
                    "ARA restore must not hydrate AudioModification content or CRS directly from archive audio.");

    return pass("araArchiveRestoresFromAudioSourceOnly");
}

CheckResult araSampleAccessEnableDoesNotReadFreshContent()
{
    const auto dc = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto didEnable = extractFunctionBlock(dc, "void OpenTuneDocumentController::didEnableAudioSourceSamplesAccess");

    if (didEnable.empty())
        return fail("araSampleAccessEnableDoesNotReadFreshContent", "didEnableAudioSourceSamplesAccess callback is missing.");

// Per VST3 ARA explicit-read principle: didEnableAudioSourceSamplesAccess grants permission only.
// Per architecture decision (commit 5215c6f): sample access enable is NOT read intent.
// It must NOT create reader lease, read samples, rebuild CRS, materialize content, or schedule F0.
// The only user-read entry point is requestReadAudioForPlaybackRegions() via recordRequested().

    // Forbidden: createReaderLease - lease creation is user-commanded only
    if (contains(didEnable, "createReaderLease"))
        return fail("araSampleAccessEnableDoesNotReadFreshContent",
                    "didEnableAudioSourceSamplesAccess must NOT create reader lease. Lease creation is user-commanded only.");

    if (contains(didEnable, "rebuildCRSForSource"))
        return fail("araSampleAccessEnableDoesNotReadFreshContent",
                    "didEnableAudioSourceSamplesAccess must NOT rebuild CRS. CRS materialization is user-commanded only.");

    if (contains(didEnable, "readAudioSamples"))
        return fail("araSampleAccessEnableDoesNotReadFreshContent",
                    "didEnableAudioSourceSamplesAccess must NOT call readAudioSamples. Reading host audio requires explicit user intent.");

    if (contains(didEnable, "birthContentForModification"))
        return fail("araSampleAccessEnableDoesNotReadFreshContent",
                    "didEnableAudioSourceSamplesAccess must NOT call birthContentForModification. Content materialization is user-triggered.");

    if (contains(didEnable, "scheduleAsyncF0Extraction"))
        return fail("araSampleAccessEnableDoesNotReadFreshContent",
                    "didEnableAudioSourceSamplesAccess must NOT schedule F0 extraction. F0 inference is user-commanded only.");

    return pass("araSampleAccessEnableDoesNotReadFreshContent");
}

CheckResult araUserReadGateCreatesLease()
{
    const auto dc = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto birthFunc = extractFunctionBlock(dc, "bool OpenTuneDocumentController::birthContentForModification");
    
    if (birthFunc.empty())
        return fail("araUserReadGateCreatesLease", "birthContentForModification not found");
    
    if (birthFunc.find("createReaderLease()") == std::string::npos)
        return fail("araUserReadGateCreatesLease",
            "birthContentForModification must call createReaderLease() to ensure reader lease exists before canReadSamples()");
    
    const auto didEnableFunc = extractFunctionBlock(dc, "void OpenTuneDocumentController::didEnableAudioSourceSamplesAccess");
    if (!didEnableFunc.empty() && didEnableFunc.find("createReaderLease()") != std::string::npos)
        return fail("araUserReadGateCreatesLease",
            "didEnableAudioSourceSamplesAccess must NOT call createReaderLease()");
    
    return pass("araUserReadGateCreatesLease");
}

CheckResult refreshPlaybackReadSourceIsPrivate()
{
    const auto dcHeader = readText("Source/ARA/OpenTuneDocumentController.h");

    // refreshPlaybackReadSource was removed as dead code.
    // Verify it no longer exists in the header.
    if (contains(dcHeader, "refreshPlaybackReadSource"))
        return fail("refreshPlaybackReadSourceIsPrivate",
                    "refreshPlaybackReadSource was removed as dead code. Remove it from the header.");

    return pass("refreshPlaybackReadSourceIsPrivate");
}

CheckResult vst3OverlayInitiallyHidden()
{
    const auto vst3Editor = readText("Source/Plugin/PluginEditor.cpp");

    // 构造后必须显式隐藏 overlay 和 badge
    // 不用 extractFunctionBlock——构造函数初始化列表含花括号会干扰大括号匹配
    if (!contains(vst3Editor, "autoRenderOverlay_.setVisible(false)"))
        return fail("vst3OverlayInitiallyHidden",
            "VST3 PluginEditor must call autoRenderOverlay_.setVisible(false).");

    if (!contains(vst3Editor, "renderBadge_.setVisible(false)"))
        return fail("vst3OverlayInitiallyHidden",
            "VST3 PluginEditor must call renderBadge_.setVisible(false).");

    return pass("vst3OverlayInitiallyHidden");
}

CheckResult recordRequestedNoRegionNoAlert()
{
    const auto vst3Editor = readText("Source/Plugin/PluginEditor.cpp");
    const auto recordRequested = extractFunctionBlock(vst3Editor, "recordRequested");

    if (recordRequested.empty())
        return fail("recordRequestedNoRegionNoAlert", "Cannot locate recordRequested function.");

    // 无 region 时必须静默返回，不弹窗
    const auto emptyCheck = extractFunctionBlock(recordRequested, "allRegions.empty()");
    if (emptyCheck.empty())
        return fail("recordRequestedNoRegionNoAlert", "Cannot locate allRegions.empty() check.");

    if (contains(emptyCheck, "AlertWindow::showMessageBoxAsync"))
        return fail("recordRequestedNoRegionNoAlert",
            "recordRequested must NOT show AlertWindow when allRegions.empty(). Silent return only.");

    return pass("recordRequestedNoRegionNoAlert");
}

CheckResult playbackRendererUsesCrsSnapshotNotOwnerPointer()
{
    const auto renderer = readText("Source/ARA/OpenTunePlaybackRenderer.cpp");
    const auto processBlock = extractFunctionBlock(renderer, "OpenTunePlaybackRenderer::processBlock");

    if (processBlock.empty())
        return fail("playbackRendererUsesCrsSnapshotNotOwnerPointer",
                    "Cannot locate OpenTunePlaybackRenderer::processBlock in PlaybackRenderer.cpp.");

    // processBlock must NOT directly access documentController_->getContentRenderService()
    // which creates a TOCTOU race with DocumentController destruction
    if (contains(processBlock, "documentController_->getContentRenderService()"))
        return fail("playbackRendererUsesCrsSnapshotNotOwnerPointer",
                    "processBlock must NOT chase documentController_ pointer to get CRS. "
                    "Use contentRenderServiceSnapshot_ instead to avoid TOCTOU race with DC destruction.");

    // processBlock must use CRS snapshot
    if (!contains(processBlock, "contentRenderServiceSnapshot_"))
        return fail("playbackRendererUsesCrsSnapshotNotOwnerPointer",
                    "processBlock must use contentRenderServiceSnapshot_ for CRS access "
                    "to ensure atomic snapshot prevents use-after-free during DC teardown.");

    // Verify detachDocumentController has correct order
    const auto detach = extractFunctionBlock(renderer, "OpenTunePlaybackRenderer::detachDocumentController");
    if (detach.empty())
        return fail("playbackRendererUsesCrsSnapshotNotOwnerPointer",
                    "Cannot locate detachDocumentController in PlaybackRenderer.cpp.");

    // Check that empty plan is published BEFORE CRS snapshot is cleared
    // The order must be: store empty plan -> atomic_store CRS snapshot -> clear documentController_
    const auto storePos = detach.find("currentPlan_.store");
    const auto atomicStorePos = detach.find("atomic_store_explicit(&contentRenderServiceSnapshot_");
    const auto clearPos = detach.find("documentController_ = nullptr");

    if (storePos == std::string::npos || atomicStorePos == std::string::npos || clearPos == std::string::npos)
        return fail("playbackRendererUsesCrsSnapshotNotOwnerPointer",
                    "detachDocumentController must have all three steps: store empty plan, atomic_store CRS snapshot, clear pointer.");

    if (!(storePos < atomicStorePos && atomicStorePos < clearPos))
        return fail("playbackRendererUsesCrsSnapshotNotOwnerPointer",
                    "detachDocumentController order is wrong. Must be: (1) store empty plan FIRST, "
                    "(2) atomic_store CRS snapshot, (3) clear pointer LAST. "
                    "This prevents audio thread from entering render loop with stale CRS.");

    // Verify processBlock uses atomic_load for CRS snapshot (not plain shared_ptr copy)
    if (!contains(processBlock, "atomic_load_explicit(&contentRenderServiceSnapshot_"))
        return fail("playbackRendererUsesCrsSnapshotNotOwnerPointer",
                    "processBlock must use atomic_load_explicit for CRS snapshot access "
                    "to prevent data race with detach thread's atomic_store.");

    return pass("playbackRendererUsesCrsSnapshotNotOwnerPointer");
}

CheckResult araSourceContentUpdateInvalidatesOnly()
{
    const auto dc = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto sourceUpdate = extractFunctionBlock(dc, "void OpenTuneDocumentController::doUpdateAudioSourceContent");
    const auto remove = extractFunctionBlock(dc, "void OpenTuneDocumentController::removeCRSArtifactsForModification");

    if (sourceUpdate.empty() || remove.empty())
        return fail("araSourceContentUpdateInvalidatesOnly", "AudioSource content update or removal helpers are missing.");

    if (!contains(sourceUpdate, "removeCRSArtifactsForModification(modification)")
        || !contains(sourceUpdate, "modification.invalidateDerivedContent()"))
        return fail("araSourceContentUpdateInvalidatesOnly",
                    "AudioSource content updates must drop stale CRS artifacts before invalidating derived content.");

    const auto removeMissing = missingTokens(remove, {
        "removePlaybackSource",
        "removeRenderCache",
        "removeStretcher",
        "getTimeStretchCache().invalidate"
    });
    if (!removeMissing.empty())
        return fail("araSourceContentUpdateInvalidatesOnly", "CRS artifact removal is incomplete:" + removeMissing);

    if (contains(sourceUpdate, "readAudioSamples"))
        return fail("araSourceContentUpdateInvalidatesOnly",
                    "doUpdateAudioSourceContent must NOT read AudioSource samples. It only invalidates derived state.");

    if (contains(sourceUpdate, "scheduleAsyncF0Extraction"))
        return fail("araSourceContentUpdateInvalidatesOnly",
                    "doUpdateAudioSourceContent must NOT schedule F0 extraction. Source update is not user-read intent.");

    return pass("araSourceContentUpdateInvalidatesOnly");
}

CheckResult araRendererUsesAssignedRegionsAndMixesOverlap()
{
    const auto renderer = readText("Source/ARA/OpenTunePlaybackRenderer.cpp");

    const auto missing = missingTokens(renderer, {
        "didAddPlaybackRegion",
        "publishRenderPlanFor(std::move(playbackRegions))",
        "getPlaybackRegionProjectionsFor(nextPlan->playbackRegions)",
        "for (const auto& region : plan->items)",
        "dest[sample] += src[sample]"
    });
    if (!missing.empty())
        return fail("araRendererUsesAssignedRegionsAndMixesOverlap", "renderer no longer follows host-assigned region set:" + missing);

    if (contains(renderer, "getFocusedEditorPlaybackRegionProjection")
        || contains(renderer, "preferredRegion"))
        return fail("araRendererUsesAssignedRegionsAndMixesOverlap",
                    "ARA renderer must not use editor focus or global preferred region.");

    return pass("araRendererUsesAssignedRegionsAndMixesOverlap");
}

CheckResult standaloneMainSignalChainIsConnected()
{
    const auto processor = readText("Source/PluginProcessor.cpp");
    const auto standaloneEditor = readText("Source/Standalone/PluginEditor.cpp");
    const auto arrangement = readText("Source/StandaloneArrangement.h");

    const auto import = extractFunctionBlock(processor, "OpenTuneAudioProcessor::ensureSourceAndCreateStandaloneClip");
    const auto placement = extractFunctionBlock(processor, "OpenTuneAudioProcessor::commitPreparedImportAsPlacement");
    if (import.empty() || placement.empty())
        return fail("standaloneMainSignalChainIsConnected", "standalone import/placement functions are missing.");

    const auto importMissing = missingTokens(import, {
        "sourceStore_->createSource",
        "standaloneContentRepository_->createClip",
        "clip->applyAudioBuffer",
        "publishStandalonePlaybackSource"
    });
    if (!importMissing.empty())
        return fail("standaloneMainSignalChainIsConnected", "standalone import no longer publishes content to CRS:" + importMissing);

    if (!contains(placement, "standaloneArrangement_->insertPlacement")
        || !contains(placement, "contentKey"))
        return fail("standaloneMainSignalChainIsConnected",
                    "standalone placement must reference content by ContentKey.");

    const auto editorMissing = missingTokens(standaloneEditor, {
        "setReadContentSnapshot",
        "setContentCommands",
        "getContentCommands"
    });
    if (!editorMissing.empty())
        return fail("standaloneMainSignalChainIsConnected", "standalone PianoRoll command injection is incomplete:" + editorMissing);

    if (contains(arrangement, "AudioBuffer")
        || contains(arrangement, "PitchCurve")
        || contains(arrangement, "notes"))
        return fail("standaloneMainSignalChainIsConnected",
                    "StandaloneArrangement must remain placement-only.");

    return pass("standaloneMainSignalChainIsConnected");
}

CheckResult standaloneRestoreKeepsWindowedClipAudio()
{
    const auto session = readText("Source/Utils/ProjectSession.cpp");
    const auto apply = extractFunctionBlock(session, "Result<void> ProjectSession::applySnapshot");

    if (apply.empty())
        return fail("standaloneRestoreKeepsWindowedClipAudio", "ProjectSession::applySnapshot is missing.");

    const auto missing = missingTokens(apply, {
        "rebuildStandaloneClipAudioFromSourceWindow",
        "contentEntry.sourceWindow",
        "clip->payload().sourceWindow = contentEntry.sourceWindow",
        "clip->applyAudioBuffer(windowedBuffer",
        "readSource.audioBuffer = snap->audioBuffer"
    });
    if (!missing.empty())
        return fail("standaloneRestoreKeepsWindowedClipAudio",
                    "standalone restore must rebuild clip-local audio from persisted sourceWindow before republishing CRS:" + missing);

    const size_t fullBufferLookup = apply.find("sourceStore->getAudioBuffer(contentEntry.sourceId, sourceBuf)");
    const size_t windowedBuild = apply.find("rebuildStandaloneClipAudioFromSourceWindow");
    const size_t applyWindowed = apply.find("clip->applyAudioBuffer(windowedBuffer");
    if (fullBufferLookup == std::string::npos || windowedBuild == std::string::npos || applyWindowed == std::string::npos)
        return fail("standaloneRestoreKeepsWindowedClipAudio",
                    "restore must look up the source buffer, build a windowed buffer, and apply that windowed buffer.");

    if (!(fullBufferLookup < windowedBuild && windowedBuild < applyWindowed))
        return fail("standaloneRestoreKeepsWindowedClipAudio",
                    "restore applies audio before rebuilding the persisted sourceWindow slice.");

    if (contains(apply, "clip->applyAudioBuffer(sourceBuf"))
        return fail("standaloneRestoreKeepsWindowedClipAudio",
                    "restore must not apply the full source buffer to a windowed Standalone clip.");

    return pass("standaloneRestoreKeepsWindowedClipAudio");
}

CheckResult nonAraVst3CaptureSignalChainIsConnected()
{
    const auto cmake = readText("CMakeLists.txt");
    const auto processor = readText("Source/PluginProcessor.cpp");
    const auto captureSession = readText("Source/Plugin/Capture/CaptureSession.cpp");
    const auto capturePersistence = readText("Source/Plugin/Capture/CapturePersistence.cpp");
    const auto vst3Editor = readText("Source/Plugin/PluginEditor.cpp");

    if (!contains(cmake, "option(OPENTUNE_ENABLE_ARA")
        || !contains(cmake, "if(OPENTUNE_ENABLE_ARA)")
        || !contains(cmake, "Source/ARA/OpenTunePlaybackRenderer.cpp"))
        return fail("nonAraVst3CaptureSignalChainIsConnected",
                    "CMake must keep ARA implementation sources behind OPENTUNE_ENABLE_ARA.");

    const auto constructor = extractFunctionBlock(processor, "OpenTuneAudioProcessor::OpenTuneAudioProcessor()");
    const auto getCapture = extractFunctionBlock(processor, "OpenTuneAudioProcessor::getCaptureSession()");
    const auto processBlock = extractFunctionBlock(
        processor,
        "void OpenTuneAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer");
    const auto replaceNotes = extractFunctionBlock(processor, "bool OpenTuneAudioProcessor::replaceContentNotesForFullMutation");
    const auto commitNotesAndSegments = extractFunctionBlock(processor, "bool OpenTuneAudioProcessor::commitContentNotesAndSegments");
    const auto setPitchCurve = extractFunctionBlock(processor, "bool OpenTuneAudioProcessor::setContentPitchCurve");
    const auto writePitchCurve = extractFunctionBlock(processor, "bool OpenTuneAudioProcessor::writePitchCurveToOwner");
    const auto setTimeGrid = extractFunctionBlock(processor, "bool OpenTuneAudioProcessor::setContentTimeGrid");
    const auto setPitchShift = extractFunctionBlock(processor, "bool OpenTuneAudioProcessor::setContentPitchShiftSettings");
    const auto durationHelper = extractFunctionBlock(processor, "double contentDurationSeconds");
    const auto commitAutoTune = extractFunctionBlock(processor, "bool OpenTuneAudioProcessor::commitAutoTuneGeneratedNotesByContentKey");
    const auto editorTimer = extractFunctionBlock(vst3Editor, "void OpenTuneAudioProcessorEditor::timerCallback");
    const auto updateCaptureCallback = extractFunctionBlock(vst3Editor, "void OpenTuneAudioProcessorEditor::updateRegularCaptureSessionCallback");
    const auto autoTune = extractFunctionBlock(vst3Editor, "void OpenTuneAudioProcessorEditor::autoTuneRequested");
    if (constructor.empty() || getCapture.empty() || processBlock.empty()
        || replaceNotes.empty() || commitNotesAndSegments.empty() || setPitchCurve.empty()
        || writePitchCurve.empty() || setTimeGrid.empty() || setPitchShift.empty() || durationHelper.empty()
        || commitAutoTune.empty() || editorTimer.empty()
        || updateCaptureCallback.empty() || autoTune.empty())
        return fail("nonAraVst3CaptureSignalChainIsConnected", "processor VST3 capture functions are missing.");

    const auto constructorMissing = missingTokens(constructor, {
        "wrapperType == juce::AudioProcessor::wrapperType_VST3",
        "captureSession_ = std::make_unique<Capture::CaptureSession>",
        "bindings.publishPlaybackSource",
        "readSource.audioSampleRate = sampleRate",
        "bindings.requestFullRender"
    });
    if (!constructorMissing.empty())
        return fail("nonAraVst3CaptureSignalChainIsConnected", "regular VST3 capture bindings are incomplete:" + constructorMissing);

    if (!contains(getCapture, "if (isBoundToARA())")
        || !contains(getCapture, "return nullptr")
        || !contains(getCapture, "return captureSession_.get()"))
        return fail("nonAraVst3CaptureSignalChainIsConnected",
                    "ARA-bound VST3 instances must not use regular capture session.");

    if (!inOrder(processBlock, {
            "if (isBoundToARA())",
            "processBlockForARA",
            "if (auto* captureSession = getCaptureSession())",
            "captureSession->processBlock"
        }))
        return fail("nonAraVst3CaptureSignalChainIsConnected",
                    "processBlock must route ARA first, then regular VST3 capture.");

    const auto captureMissing = missingTokens(captureSession, {
        "ContentKey{DomainKind::RegularVST3Capture",
        "bindings_.publishPlaybackSource",
        "applyAudioBuffer",
        "OriginalF0State::Ready",
        "onSegmentRenderingComplete"
    });
    if (!captureMissing.empty())
        return fail("nonAraVst3CaptureSignalChainIsConnected", "capture session no longer publishes owner audio/analysis to CRS:" + captureMissing);

    if (!contains(constructor, "bindings.refreshSegment")
        || !contains(constructor, "submit(")
        || !contains(constructor, "commitSegmentF0Result"))
        return fail("nonAraVst3CaptureSignalChainIsConnected",
                    "regular VST3 capture must submit F0 refresh and commit results to segment owner truth.");

    if (!contains(capturePersistence, "publishPlaybackSource")
        || !contains(capturePersistence, "requestFullRender"))
        return fail("nonAraVst3CaptureSignalChainIsConnected",
                    "capture restore must republish CRS and enqueue render from restored owner truth.");

    if (!contains(vst3Editor, "setReadContentSnapshot")
        || !contains(vst3Editor, "setContentCommands"))
        return fail("nonAraVst3CaptureSignalChainIsConnected",
                    "VST3 editor must inject ContentKey read/write commands into PianoRoll.");

    if (!contains(autoTune, "processorRef_.getContentSnapshot(activeKey)")
        || !contains(autoTune, "snap->originalF0State"))
        return fail("nonAraVst3CaptureSignalChainIsConnected",
                    "regular VST3 AUTO must read OriginalF0 readiness from capture content snapshots, not only ARA document state.");

    if (!contains(commitAutoTune, "case DomainKind::RegularVST3Capture")
        || !contains(commitAutoTune, "applyAutoTuneGeneratedNotes")
        || !contains(commitAutoTune, "onContentLocalMutationCompleted"))
        return fail("nonAraVst3CaptureSignalChainIsConnected",
                    "regular VST3 AUTO must commit generated notes/pitch curve into capture owner truth and trigger render invalidation.");

    if (!contains(replaceNotes, "session->applyNotes")
        || !contains(commitNotesAndSegments, "session->applyNotesAndPitchCurve")
        || !contains(writePitchCurve, "session->applyPitchCurve")
        || !contains(setPitchCurve, "writePitchCurveToOwner")
        || !contains(setTimeGrid, "session->applyTimeGrid")
        || !contains(setPitchShift, "session->applyPitchShiftSettings")
        || !contains(durationHelper, "snap.audioBuffer->getNumSamples()")
        || !contains(commitAutoTune, "onContentLocalMutationCompleted"))
        return fail("nonAraVst3CaptureSignalChainIsConnected",
                    "regular VST3 capture edit commands must write to capture owner truth and trigger range-local render.");

    if (countOf(vst3Editor, "updateRegularCaptureSessionCallback();") < 2
        || !contains(editorTimer, "updateRegularCaptureSessionCallback();")
        || !contains(updateCaptureCallback, "setActiveSegmentChangedCallback")
        || !contains(updateCaptureCallback, "syncContentProjectionToPianoRoll();"))
        return fail("nonAraVst3CaptureSignalChainIsConnected",
                    "regular VST3 editor must bind capture completion callbacks and refresh UI projections when edited segments arrive.");

    if (!contains(capturePersistence, "endMagic != kCaptureEndMagic")
        || contains(capturePersistence, "juce::ignoreUnused(endMagic)"))
        return fail("nonAraVst3CaptureSignalChainIsConnected",
                    "capture persistence must reject corrupted CAPz end magic instead of ignoring it.");

    return pass("nonAraVst3CaptureSignalChainIsConnected");
}

CheckResult mutationSinkAndRenderPipelineStayUnified()
{
    const auto processor = readText("Source/PluginProcessor.cpp");
    const auto localMutation = extractFunctionBlock(processor, "void OpenTuneAudioProcessor::onContentLocalMutationCompleted");
    const auto fullMutation = extractFunctionBlock(processor, "void OpenTuneAudioProcessor::onContentFullMutationCompleted");
    const auto stage1 = extractFunctionBlock(processor, "void OpenTuneAudioProcessor::handleStage1ChunkPublished");
    const auto renderRange = extractFunctionBlock(processor, "void OpenTuneAudioProcessor::requestRenderForLocalMutationRange");
    const auto read = extractFunctionBlock(processor, "int OpenTuneAudioProcessor::readPlaybackAudio");
    const auto inlineReader = readText("Source/Utils/PlaybackAudioReader.h");

    if (localMutation.empty() || fullMutation.empty() || stage1.empty() || renderRange.empty() || read.empty())
        return fail("mutationSinkAndRenderPipelineStayUnified", "mutation/render/read pipeline functions are missing.");

    const auto localMissing = missingTokens(localMutation, {
        "refreshCRSMetadata(key)",
        "requestRenderForLocalMutationRange"
    });
    if (!localMissing.empty())
        return fail("mutationSinkAndRenderPipelineStayUnified", "local mutation sink must refresh CRS metadata and request range render:" + localMissing);

    const auto fullMissing = missingTokens(fullMutation, {
        "refreshCRSMetadata(key)",
        "requestFullContentRender"
    });
    if (!fullMissing.empty())
        return fail("mutationSinkAndRenderPipelineStayUnified", "full mutation sink must refresh CRS metadata and request full render:" + fullMissing);

    if (contains(localMutation, "invalidateRenderFor(key)")
        || contains(localMutation, "removeRenderCache(key)")
        || contains(localMutation, "clear()"))
        return fail("mutationSinkAndRenderPipelineStayUnified",
                    "local mutation sink must not do whole-key render invalidation.");

    if (contains(stage1, "invalidateRenderFor("))
        return fail("mutationSinkAndRenderPipelineStayUnified",
                    "Stage1 publication must not invalidate the render it just published.");

    const auto renderMissing = missingTokens(renderRange, {
        "getPlaybackReadSource",
        "readSource.audioSampleRate",
        "RenderJob job",
        // non-ARA render path: processor enqueues via resolved local CRS.
        // ARA render path delegates to OpenTuneDocumentController via the
        // DC's own CRS execution lease (see processDocumentRenderJob), so it
        // is intentionally absent from the processor's mutation-sink path.
        "enqueueRender"
    });
    if (!renderMissing.empty())
        return fail("mutationSinkAndRenderPipelineStayUnified", "render range request lost CRS/read-source inputs:" + renderMissing);

    if (!contains(read, "const double srcSampleRate = request.source.audioSampleRate")
        || !contains(inlineReader, "const double srcSampleRate = request.source.audioSampleRate")
        || contains(read, "constexpr double srcSampleRate = TimeCoordinate::kRenderSampleRate")
        || contains(inlineReader, "constexpr double srcSampleRate = TimeCoordinate::kRenderSampleRate"))
        return fail("mutationSinkAndRenderPipelineStayUnified",
                    "readPlaybackAudio must use PlaybackReadSource.audioSampleRate, not a hard-coded render rate.");

    return pass("mutationSinkAndRenderPipelineStayUnified");
}

CheckResult contentRenderServiceUsesGlobalSilentGapChunkSelection()
{
    const auto service = readText("Source/Render/ContentRenderService.cpp");
    const auto plannerHeader = readText("Source/Render/RenderChunkPlanner.h");
    const auto enqueue = extractFunctionBlock(service, "void ContentRenderService::enqueueRender");

    if (enqueue.empty())
        return fail("contentRenderServiceUsesGlobalSilentGapChunkSelection",
                    "ContentRenderService::enqueueRender is missing.");

    if (contains(service, "makeRangeLocalSilentGaps")
        || contains(enqueue, "localGaps")
        || contains(enqueue, "rangeSampleCount")
        || contains(enqueue, "job.startSample + boundaries"))
        return fail("contentRenderServiceUsesGlobalSilentGapChunkSelection",
                    "render chunking must not rebase silent gaps or boundaries to the edit range.");

    const auto missingPlannerContract = missingTokens(plannerHeader, {
        "selectChunksIntersectingRange",
        "contentSampleCount",
        "requestStartSample",
        "requestEndSampleExclusive"
    });
    if (!missingPlannerContract.empty())
        return fail("contentRenderServiceUsesGlobalSilentGapChunkSelection",
                    "RenderChunkPlanner lacks a global-boundary overlap-selection contract:" + missingPlannerContract);

    const auto missingEnqueueContract = missingTokens(enqueue, {
        "job.audioBuffer->getNumSamples()",
        "RenderChunkPlanner::selectChunksIntersectingRange",
        "chunk.startSample",
        "chunk.endSampleExclusive",
        "requestRenderPending"
    });
    if (!missingEnqueueContract.empty())
        return fail("contentRenderServiceUsesGlobalSilentGapChunkSelection",
                    "ContentRenderService no longer selects complete global chunks for local edits:" + missingEnqueueContract);

    return pass("contentRenderServiceUsesGlobalSilentGapChunkSelection");
}

CheckResult noRemovedAraArchitectureVocabularyInAraSource()
{
    const auto araSource = readSourceFilesUnder("Source/ARA");

    const std::vector<std::string_view> banned = {
        "MaterializationStore",
        "materializationStore_",
        "MaterializationSnapshot",
        "materializationId",
        "Stage1Completed",
        "autoRenderOverlayRenderQueued",
        "autoRenderOverlayTargetContentKey",
        "autoRenderOverlayLatched"
    };

    for (const auto token : banned) {
        if (contains(araSource, token))
            return fail("noRemovedAraArchitectureVocabularyInAraSource",
                        "removed ARA architecture token is present in ARA source: " + std::string(token));
    }

    return pass("noRemovedAraArchitectureVocabularyInAraSource");
}

// =============================================================================
// Render mutation hard-cut architecture contract tests
// =============================================================================
//
// The render mutation refactor enforces the following contract:
//   - ContentEditCommands no longer exposes enqueuePartialRender (deleted).
//   - commitNotesAndSegments now carries a ContentEditRangeFrames affected range.
//   - PianoRollEditAction::undo/redo forward the stored affected range.
//   - Editors do not directly enqueue partial renders; the processor's local
//     mutation sink consumes the affected range instead.
//   - PluginProcessor has shed enqueueContentPartialRender / invalidateRenderFor
//     in favour of onContentLocalMutationCompleted / onContentFullMutationCompleted
//     and requestFullContentRender.

CheckResult contentEditCommandsHasNoEnqueuePartialRender()
{
    const auto cec = readText("Source/Content/ContentEditCommands.h");

    if (contains(cec, "enqueuePartialRender"))
        return fail("contentEditCommandsHasNoEnqueuePartialRender",
                    "ContentEditCommands.h still declares enqueuePartialRender; "
                    "render enqueue must move to the processor's mutation sink.");

    return pass("contentEditCommandsHasNoEnqueuePartialRender");
}

CheckResult commitNotesAndSegmentsCarriesAffectedRange()
{
    const auto cec = readText("Source/Content/ContentEditCommands.h");

    const auto sigPos = cec.find("commitNotesAndSegments");
    if (sigPos == std::string_view::npos)
        return fail("commitNotesAndSegmentsCarriesAffectedRange",
                    "could not locate commitNotesAndSegments declaration in ContentEditCommands.h.");

    const auto sigEnd = cec.find(')', sigPos);
    if (sigEnd == std::string_view::npos)
        return fail("commitNotesAndSegmentsCarriesAffectedRange",
                    "commitNotesAndSegments signature is malformed (no closing parenthesis).");

    const auto signature = cec.substr(sigPos, sigEnd - sigPos);
    if (!contains(signature, "ContentEditRangeFrames") && !contains(signature, "affectedRange"))
        return fail("commitNotesAndSegmentsCarriesAffectedRange",
                    "commitNotesAndSegments must accept a ContentEditRangeFrames affectedRange parameter "
                    "so undo/redo can forward the edit scope to the mutation sink.");

    return pass("commitNotesAndSegmentsCarriesAffectedRange");
}

CheckResult pianoRollEditActionPassesRangeInUndoRedo()
{
    const auto action = readText("Source/Utils/PianoRollEditAction.cpp");
    const auto undo = extractFunctionBlock(action, "PianoRollEditAction::undo()");
    const auto redo = extractFunctionBlock(action, "PianoRollEditAction::redo()");

    if (undo.empty() || redo.empty())
        return fail("pianoRollEditActionPassesRangeInUndoRedo",
                    "could not locate PianoRollEditAction::undo() or redo().");

    if (!contains(undo, "affectedRange_"))
        return fail("pianoRollEditActionPassesRangeInUndoRedo",
                    "PianoRollEditAction::undo() must forward affectedRange_ "
                    "to commitNotesAndSegments.");

    if (!contains(redo, "affectedRange_"))
        return fail("pianoRollEditActionPassesRangeInUndoRedo",
                    "PianoRollEditAction::redo() must forward affectedRange_ "
                    "to commitNotesAndSegments.");

    return pass("pianoRollEditActionPassesRangeInUndoRedo");
}

CheckResult editorsDoNotEnqueueRenderDirectly()
{
    const auto standaloneEditor = readText("Source/Standalone/PluginEditor.cpp");
    const auto vst3Editor = readText("Source/Plugin/PluginEditor.cpp");

    for (const auto token : { "enqueueContentPartialRender", "enqueuePartialRender" }) {
        if (contains(standaloneEditor, token))
            return fail("editorsDoNotEnqueueRenderDirectly",
                        std::string("Source/Standalone/PluginEditor.cpp still invokes ") + token +
                        "; editors must delegate render triggering to the processor mutation sink.");
        if (contains(vst3Editor, token))
            return fail("editorsDoNotEnqueueRenderDirectly",
                        std::string("Source/Plugin/PluginEditor.cpp still invokes ") + token +
                        "; editors must delegate render triggering to the processor mutation sink.");
    }

    return pass("editorsDoNotEnqueueRenderDirectly");
}

CheckResult processorLocalMutationSinkUsesRange()
{
    const auto processor = readText("Source/PluginProcessor.cpp");
    const auto local = extractFunctionBlock(processor,
                                           "void OpenTuneAudioProcessor::onContentLocalMutationCompleted");

    if (local.empty())
        return fail("processorLocalMutationSinkUsesRange",
                    "could not locate OpenTuneAudioProcessor::onContentLocalMutationCompleted "
                    "implementation; the local mutation sink must exist.");

    if (!contains(local, "requestRenderForLocalMutationRange"))
        return fail("processorLocalMutationSinkUsesRange",
                    "onContentLocalMutationCompleted must trigger "
                    "requestRenderForLocalMutationRange using the supplied affectedRange.");

    if (contains(local, "ignoreUnused(affectedRange)"))
        return fail("processorLocalMutationSinkUsesRange",
                    "onContentLocalMutationCompleted must not ignore the affectedRange argument.");

    return pass("processorLocalMutationSinkUsesRange");
}

CheckResult processorHasNoEnqueueContentPartialRender()
{
    const auto header = readText("Source/PluginProcessor.h");

    if (contains(header, "enqueueContentPartialRender"))
        return fail("processorHasNoEnqueueContentPartialRender",
                    "PluginProcessor.h still declares enqueueContentPartialRender; "
                    "this entry point has been replaced by onContentLocalMutationCompleted.");

    return pass("processorHasNoEnqueueContentPartialRender");
}

CheckResult processorHasNoInvalidateRenderFor()
{
    const auto header = readText("Source/PluginProcessor.h");

    if (contains(header, "invalidateRenderFor"))
        return fail("processorHasNoInvalidateRenderFor",
                    "PluginProcessor.h still declares invalidateRenderFor; "
                    "whole-key invalidation has been replaced by range-targeted mutation sinks.");

    return pass("processorHasNoInvalidateRenderFor");
}

CheckResult mutationSinkDoesNotIgnoreRange()
{
    const auto processor = readText("Source/PluginProcessor.cpp");

    if (contains(processor, "ignoreUnused(affectedStartSeconds)")
        || contains(processor, "ignoreUnused(affectedEndSeconds)"))
        return fail("mutationSinkDoesNotIgnoreRange",
                    "PluginProcessor.cpp still uses juce::ignoreUnused on the affected-range "
                    "seconds; the mutation sink must consume the range to drive partial renders.");

    return pass("mutationSinkDoesNotIgnoreRange");
}

CheckResult requestRenderForLocalMutationRangeHasSingleEntryPath()
{
    const auto processor = readText("Source/PluginProcessor.cpp");

    // Count occurrences of requestRenderForLocalMutationRange
    int count = 0;
    size_t pos = 0;
    while ((pos = processor.find("requestRenderForLocalMutationRange", pos)) != std::string::npos) {
        ++count;
        pos += 34; // length of "requestRenderForLocalMutationRange"
    }

    // Expected locations:
    // 1. Definition (void OpenTuneAudioProcessor::requestRenderForLocalMutationRange)
    // 2. Call in onContentLocalMutationCompleted
    // 3. Call in requestFullContentRender
    // Total: 3 occurrences in .cpp file
    if (count != 3)
        return fail("requestRenderForLocalMutationRangeHasSingleEntryPath",
                    "requestRenderForLocalMutationRange must appear exactly 3 times in PluginProcessor.cpp "
                    "(definition + local sink call + full bridge call); found " + std::to_string(count) +
                    " occurrences. This primitive must only be called from the two mutation sinks.");

    // Verify it's called from the two sinks
    const auto localSink = extractFunctionBlock(processor,
                                                "void OpenTuneAudioProcessor::onContentLocalMutationCompleted");
    const auto fullBridge = extractFunctionBlock(processor,
                                                 "void OpenTuneAudioProcessor::requestFullContentRender");

    if (!contains(localSink, "requestRenderForLocalMutationRange"))
        return fail("requestRenderForLocalMutationRangeHasSingleEntryPath",
                    "onContentLocalMutationCompleted must call requestRenderForLocalMutationRange.");

    if (!contains(fullBridge, "requestRenderForLocalMutationRange"))
        return fail("requestRenderForLocalMutationRangeHasSingleEntryPath",
                    "requestFullContentRender must call requestRenderForLocalMutationRange.");

    return pass("requestRenderForLocalMutationRangeHasSingleEntryPath");
}

CheckResult noEmptyRangeAsFullSentinel()
{
    const auto processor = readText("Source/PluginProcessor.cpp");

    // ContentEditRangeFrames{} should not appear as a sentinel for full-content render
    if (contains(processor, "ContentEditRangeFrames{}"))
        return fail("noEmptyRangeAsFullSentinel",
                    "PluginProcessor.cpp uses ContentEditRangeFrames{} as a sentinel; "
                    "full-content renders must use writePitchCurveToOwner + onContentFullMutationCompleted, "
                    "not empty range to trigger full render.");

    return pass("noEmptyRangeAsFullSentinel");
}

CheckResult timeGridEditActionHasNoAffectedRange()
{
    const auto header = readText("Source/Utils/TimeGridEditAction.h");
    const auto impl = readText("Source/Utils/TimeGridEditAction.cpp");

    // Check header doesn't have affected range members
    if (contains(header, "affectedSrcStartFrame")
        || contains(header, "affectedSrcEndFrame")
        || contains(header, "getAffectedSrcStartFrame")
        || contains(header, "getAffectedSrcEndFrame"))
        return fail("timeGridEditActionHasNoAffectedRange",
                    "TimeGridEditAction.h still declares affected range members/getters; "
                    "setTimeGrid no longer takes range parameters, so the action must not store them.");

    // Check implementation doesn't have affected range members
    if (contains(impl, "affectedSrcStartFrame_")
        || contains(impl, "affectedSrcEndFrame_"))
        return fail("timeGridEditActionHasNoAffectedRange",
                    "TimeGridEditAction.cpp still stores affected range members; "
                    "these are ceremonial residue since setTimeGrid no longer takes range.");

    return pass("timeGridEditActionHasNoAffectedRange");
}

CheckResult pitchCurveEditingHasNoSourcePriorityProtection()
{
    const auto pitchCurve = readText("Source/Utils/PitchCurve.cpp");
    const auto pitchCurveHeader = readText("Source/Utils/PitchCurve.h");
    const auto legacyNoteGeneratorHeader = readText("Source/Utils/LegacyNoteGenerator.h");
    const auto legacyNoteGenerator = readText("Source/Utils/LegacyNoteGenerator.cpp");
    const auto noteGeneratorInterface = readText("Source/Inference/INoteGenerator.h");
    const auto pianoRoll = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    const std::vector<std::string_view> bannedPitchCurveTokens = {
        "clearSegmentsMatchingSourceInRangePreserveOutside",
        "insertNoteBasedSegmentPreservingNonNoteBasedSegments",
        "PreservingNonNoteBased",
        "seg.source != source",
        "audioSampleRate",
        "audioSamplePos"
    };

    for (const auto token : bannedPitchCurveTokens) {
        if (contains(pitchCurve, token))
            return fail("pitchCurveEditingHasNoSourcePriorityProtection",
                        "PitchCurve must not give one correction source priority over another: "
                            + std::string(token));
    }

    if (contains(pitchCurveHeader, "audioSampleRate")
        || contains(legacyNoteGeneratorHeader, "hostSampleRate")
        || contains(legacyNoteGenerator, "hostSampleRate")
        || contains(noteGeneratorInterface, "hostSampleRate"))
        return fail("pitchCurveEditingHasNoSourcePriorityProtection",
                    "F0 correction and frame-domain note generation must use hopSize/f0SampleRate, not source audio sample-rate parameters.");

    if (contains(pianoRoll, "hasManualCorrectionInRange")
        || contains(pianoRoll, "isManualCorrectionSource"))
        return fail("pitchCurveEditingHasNoSourcePriorityProtection",
                    "PianoRoll parameter edits must not preserve manual correction segments as a protected source.");

    return pass("pitchCurveEditingHasNoSourcePriorityProtection");
}

CheckResult dcOwnsCrsAndRejectsProcessorInjection()
{
    const auto dcCpp = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto dcH = readText("Source/ARA/OpenTuneDocumentController.h");
    const auto procCpp = readText("Source/PluginProcessor.cpp");

    if (contains(dcCpp, "setDocumentServices"))
        return fail("dcOwnsCrsAndRejectsProcessorInjection",
                    "OpenTuneDocumentController.cpp must not contain setDocumentServices; DC creates its own CRS.");

    if (contains(dcH, "setDocumentServices"))
        return fail("dcOwnsCrsAndRejectsProcessorInjection",
                    "OpenTuneDocumentController.h must not declare setDocumentServices.");

    if (!contains(dcCpp, "make_shared<ContentRenderService>()"))
        return fail("dcOwnsCrsAndRejectsProcessorInjection",
                    "OpenTuneDocumentController.cpp constructor must create its own CRS via make_shared<ContentRenderService>().");

    if (contains(procCpp, "setDocumentServices"))
        return fail("dcOwnsCrsAndRejectsProcessorInjection",
                    "PluginProcessor.cpp must not call setDocumentServices; processor does not inject services into DC.");

    return pass("dcOwnsCrsAndRejectsProcessorInjection");
}

CheckResult dcGetsF0FromProcessRuntime()
{
    const auto dcCpp = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto dcH = readText("Source/ARA/OpenTuneDocumentController.h");

    if (!contains(dcCpp, "ProcessF0Runtime"))
        return fail("dcGetsF0FromProcessRuntime",
                    "OpenTuneDocumentController.cpp must use ProcessF0Runtime to obtain F0 service.");

    if (contains(dcH, "shared_ptr<F0InferenceService> f0Service_"))
        return fail("dcGetsF0FromProcessRuntime",
                    "OpenTuneDocumentController.h must not store F0InferenceService as a member; use ProcessF0Runtime on demand.");

    return pass("dcGetsF0FromProcessRuntime");
}

CheckResult processorF0ServiceHasSingleProcessOwner()
{
    const auto procH = readText("Source/PluginProcessor.h");
    const auto procCpp = readText("Source/PluginProcessor.cpp");

    if (contains(procH, "shared_ptr<F0InferenceService> f0Service_"))
        return fail("processorF0ServiceHasSingleProcessOwner",
                    "PluginProcessor.h must not keep a processor-owned F0InferenceService cache.");

    if (contains(procH, "getF0Service() const { return f0Service_.get(); }"))
        return fail("processorF0ServiceHasSingleProcessOwner",
                    "OpenTuneAudioProcessor::getF0Service() must expose ProcessF0Runtime, not a processor cache.");

    if (contains(procCpp, "f0Service_.reset()"))
        return fail("processorF0ServiceHasSingleProcessOwner",
                    "resetInferenceBackend() must not reset a processor-level F0 service cache.");

    return pass("processorF0ServiceHasSingleProcessOwner");
}

// =============================================================================
// ARA CRS Runtime hard-cut architecture contract tests
// =============================================================================
//
// The ARA render-ownership refactor enforces the following contract:
//   - didBindToARA must not attach a processor-owned execution lease to the
//     DC's ContentRenderService; DC owns its own CRS execution path.
//   - OpenTuneDocumentController::processDocumentRenderJob must delegate to
//     the process-level ProcessRenderRuntime singleton, not run inference
//     inline.
//   - ProcessRenderRuntime.cpp must be a first-class source in the CMake
//     build (alongside ProcessF0Runtime.cpp).
//   - PluginProcessor must shed chunk-render and vocoder-runtime ownership
//     (processChunkRenderJob / ensureVocoderReady / modelPathForWeight all
//     moved to ProcessRenderRuntime).
//   - ProcessRenderRuntime must stay infrastructure-only: it must not
//     reference any ARA model concept (DocumentController, AudioModification,
//     AudioSource, PlaybackRegion, ARAArchive, archive store/restore hooks).

CheckResult didBindToArADoesNotAttachProcessorLease()
{
    const auto processor = readText("Source/PluginProcessor.cpp");
    const auto didBind = extractFunctionBlock(processor, "OpenTuneAudioProcessor::didBindToARA()");

    if (didBind.empty())
        return fail("didBindToArADoesNotAttachProcessorLease",
                    "could not locate OpenTuneAudioProcessor::didBindToARA() in PluginProcessor.cpp.");

    const std::vector<std::string_view> banned = {
        "attachExecutionLease",
        "dcCrs",
        "attached execution lease to DC CRS"
    };

    for (const auto token : banned) {
        if (contains(didBind, token))
            return fail("didBindToArADoesNotAttachProcessorLease",
                        std::string("didBindToARA() must not reference '") + std::string(token) +
                        "'; DC owns its own CRS execution path and the processor must not attach a lease to it.");
    }

    return pass("didBindToArADoesNotAttachProcessorLease");
}

CheckResult renderExecutionLeaseLifecycleSafety()
{
    const auto worker = readText("Source/Render/RenderWorker.cpp");
    const auto dc = readText("Source/ARA/OpenTuneDocumentController.cpp");

    // Per architecture: ExecutionLease is a short-term lease with drain-before-detach lifecycle safety.
    // RenderWorker invokes owner callback via lease, but the lease must be detached before owner destruction.
    // The critical safety is: DC destructor must call detachExecutionLease(this) after draining.

    if (!contains(worker, "attachExecutionLease")
        || !contains(worker, "detachExecutionLease")
        || !contains(worker, "leaseCopy.renderJobCallback"))
        return fail("renderExecutionLeaseLifecycleSafety",
                    "RenderWorker must use ExecutionLease with attach/detach mechanism and callback through lease.");

    if (!contains(worker, "drain()")
        || !contains(worker, "lease_ = RenderExecutionLease{}"))
        return fail("renderExecutionLeaseLifecycleSafety",
                    "detachExecutionLease must drain queue then clear lease to prevent dangling callback.");

    if (!contains(dc, "installDocumentRenderExecution()"))
        return fail("renderExecutionLeaseLifecycleSafety",
                    "DC must install ExecutionLease to enable render pipeline.");

    // P0 critical fix: DC destructor must detach ExecutionLease
    if (!contains(dc, "detachExecutionLease(this)")
        || !contains(dc, "drainRenderWorker()"))
        return fail("renderExecutionLeaseLifecycleSafety",
                    "DC destructor must drain and detach ExecutionLease before clearing to prevent dangling lambda callback.");

    return pass("renderExecutionLeaseLifecycleSafety");
}

CheckResult processRenderRuntimeInCMake()
{
    const auto cmake = readText("CMakeLists.txt");

    if (!contains(cmake, "Source/Runtime/ProcessRenderRuntime.cpp"))
        return fail("processRenderRuntimeInCMake",
                    "CMakeLists.txt must include Source/Runtime/ProcessRenderRuntime.cpp as a first-class "
                    "source (alongside Source/Runtime/ProcessF0Runtime.cpp); the process-level render "
                    "runtime must be built.");

    return pass("processRenderRuntimeInCMake");
}

CheckResult processorHasNoChunkRenderOrVocoderRuntime()
{
    const auto processor = readText("Source/PluginProcessor.cpp");

    const std::vector<std::string_view> banned = {
        "OpenTuneAudioProcessor::processChunkRenderJob",
        "OpenTuneAudioProcessor::ensureVocoderReady",
        "OpenTuneAudioProcessor::modelPathForWeight"
    };

    for (const auto token : banned) {
        if (contains(processor, token))
            return fail("processorHasNoChunkRenderOrVocoderRuntime",
                        std::string("PluginProcessor.cpp must not define '") + std::string(token) +
                        "'; chunk rendering and vocoder runtime ownership have moved to ProcessRenderRuntime.");
    }

    return pass("processorHasNoChunkRenderOrVocoderRuntime");
}

CheckResult processRenderRuntimeOwnsNoAraModels()
{
    const auto runtimeH = readText("Source/Runtime/ProcessRenderRuntime.h");
    const auto runtimeCpp = readText("Source/Runtime/ProcessRenderRuntime.cpp");

    const std::vector<std::string_view> banned = {
        "OpenTuneDocumentController",
        "AudioModification",
        "AudioSource",
        "PlaybackRegion",
        "ARAArchive",
        "doStoreObjectsToStream",
        "doRestoreObjectsFromStream"
    };

    for (const auto token : banned) {
        if (contains(runtimeH, token))
            return fail("processRenderRuntimeOwnsNoAraModels",
                        std::string("ProcessRenderRuntime.h must not reference ARA model concept '") +
                        std::string(token) + "'; the process runtime is infrastructure-only.");
        if (contains(runtimeCpp, token))
            return fail("processRenderRuntimeOwnsNoAraModels",
                        std::string("ProcessRenderRuntime.cpp must not reference ARA model concept '") +
                        std::string(token) + "'; the process runtime is infrastructure-only.");
    }

    return pass("processRenderRuntimeOwnsNoAraModels");
}

// ---------------------------------------------------------------------------
// RenderCache 状态机回归测试（BUG3 根因：渲染风暴）
// 验证关键序列：Running rev N → 编辑 requeue rev N+1 → 旧 completion ignored → 新 completion published
// ---------------------------------------------------------------------------

CheckResult renderCacheRequestRenderPendingCancelsRunning()
{
    const auto rc = readText("Source/Inference/RenderCache.cpp");
    const auto reqFn = extractFunctionBlock(rc, "void RenderCache::requestRenderPending");

    if (reqFn.empty())
        return fail("renderCacheRequestRenderPendingCancelsRunning",
                    "requestRenderPending function not found.");

    // 关键：Running 状态下必须清零 runningRevision 并重新入 Pending
    if (!contains(reqFn, "runningRevision = 0"))
        return fail("renderCacheRequestRenderPendingCancelsRunning",
                    "requestRenderPending must clear runningRevision=0 on Running state. "
                    "Without this, stale Vocoder completions are not ignored.");

    if (!contains(reqFn, "Chunk::Status::Pending") || !contains(reqFn, "Chunk::Status::Running"))
        return fail("renderCacheRequestRenderPendingCancelsRunning",
                    "requestRenderPending must handle Running state by requeueing to Pending.");

    return pass("renderCacheRequestRenderPendingCancelsRunning");
}

CheckResult renderCacheMarkChunkAsBlankUsesRunningRevisionToken()
{
    const auto rc = readText("Source/Inference/RenderCache.cpp");
    const auto blankFn = extractFunctionBlock(rc, "void RenderCache::markChunkAsBlank");

    if (blankFn.empty())
        return fail("renderCacheMarkChunkAsBlankUsesRunningRevisionToken",
                    "markChunkAsBlank function not found.");

    // 关键：stale 检测必须用 runningRevision，不是 desiredRevision
    if (contains(blankFn, "desiredRevision") && contains(blankFn, "revision != chunk.desiredRevision"))
        return fail("renderCacheMarkChunkAsBlankUsesRunningRevisionToken",
                    "markChunkAsBlank must use runningRevision token for stale detection, "
                    "not desiredRevision. Blank is a Running completion variant.");

    if (!contains(blankFn, "runningRevision"))
        return fail("renderCacheMarkChunkAsBlankUsesRunningRevisionToken",
                    "markChunkAsBlank must reference runningRevision for stale detection.");

    return pass("renderCacheMarkChunkAsBlankUsesRunningRevisionToken");
}

CheckResult renderCacheCompleteChunkRenderWithAudioReturnsTriState()
{
    const auto rcH = readText("Source/Inference/RenderCache.h");

    // 关键：ChunkRenderResult 必须是三值枚举
    if (!contains(rcH, "ChunkRenderResult"))
        return fail("renderCacheCompleteChunkRenderWithAudioReturnsTriState",
                    "ChunkRenderResult enum not found in RenderCache.h.");

    if (!contains(rcH, "Published") || !contains(rcH, "Stale") || !contains(rcH, "InvalidInput"))
        return fail("renderCacheCompleteChunkRenderWithAudioReturnsTriState",
                    "ChunkRenderResult must have Published, Stale, and InvalidInput values. "
                    "Two-value (Published/Stale) conflates input errors with stale completions.");

    // 关键：completeChunkRenderWithAudio 必须返回 ChunkRenderResult，不是 bool
    if (contains(rcH, "bool completeChunkRenderWithAudio"))
        return fail("renderCacheCompleteChunkRenderWithAudioReturnsTriState",
                    "completeChunkRenderWithAudio must return ChunkRenderResult, not bool.");

    // 关键：旧 addChunk 和 completeChunkRender(CompletionResult) 必须已删除
    if (contains(rcH, "bool addChunk("))
        return fail("renderCacheCompleteChunkRenderWithAudioReturnsTriState",
                    "Old addChunk() must be removed. Publication is now atomic in completeChunkRenderWithAudio.");

    if (contains(rcH, "CompletionResult"))
        return fail("renderCacheCompleteChunkRenderWithAudioReturnsTriState",
                    "Old CompletionResult enum must be removed. Replaced by ChunkRenderResult + completeChunkRenderFailure.");

    return pass("renderCacheCompleteChunkRenderWithAudioReturnsTriState");
}

// ── Kill List resurrection guards ──

CheckResult noTileCacheInSourceTree()
{
    // 扫描 Source/ 和 CMakeLists.txt，确保不再出现 tile cache 引用
    const std::vector<std::string> forbidden = {
        "PianoRollTimelineSurfaceCache",
        "PianoRollTileRenderer",
        "PianoRollRenderSnapshot"
    };
    
    // 递归扫描 Source/ 目录下所有 .h, .cpp, .hpp 文件
    const auto allSourceFiles = readSourceFilesUnder("Source");
    for (const auto& keyword : forbidden) {
        if (contains(allSourceFiles, keyword))
            return fail("noTileCacheInSourceTree",
                         "Found '" + keyword + "' in Source/ tree");
    }
    
    // 额外检查 CMakeLists.txt
    const auto cmake = readText("CMakeLists.txt");
    for (const auto& keyword : forbidden) {
        if (contains(cmake, keyword))
            return fail("noTileCacheInSourceTree",
                         "Found '" + keyword + "' in CMakeLists.txt");
    }
    
    return pass("noTileCacheInSourceTree");
}

// ── Kill List resurrection guards ──

CheckResult noInvalidateContentVisualResurrection()
{
    const auto h = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h");
    const auto cpp = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto comp = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    if (contains(h, "invalidateContentVisual"))
        return fail("noInvalidateContentVisualResurrection",
                     "PianoRollToolHandler.h must not contain 'invalidateContentVisual' — deleted, use invalidateInteractionVisual.");
    if (contains(cpp, "invalidateContentVisual"))
        return fail("noInvalidateContentVisualResurrection",
                     "PianoRollToolHandler.cpp must not contain 'invalidateContentVisual' — deleted, use invalidateInteractionVisual.");
    if (contains(comp, "invalidateContentVisual"))
        return fail("noInvalidateContentVisualResurrection",
                     "PianoRollComponent.cpp must not contain 'invalidateContentVisual' — deleted, use invalidateInteractionVisual.");
    return pass("noInvalidateContentVisualResurrection");
}

CheckResult noRendererLambdaResurrection()
{
    const auto h = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");
    if (contains(h, "contentTimeToTimeline"))
        return fail("noRendererLambdaResurrection",
                     "PianoRollRenderer.h must not contain 'contentTimeToTimeline' — replaced by activeProjection value field.");
    if (contains(h, "projectSourceTime"))
        return fail("noRendererLambdaResurrection",
                     "PianoRollRenderer.h must not contain 'projectSourceTime' — replaced by sourceProjection value field.");
    return pass("noRendererLambdaResurrection");
}

CheckResult noF0FrameToXYResurrection()
{
    const auto h = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");
    if (contains(h, "F0FrameToX") || contains(h, "F0FrameToY"))
        return fail("noF0FrameToXYResurrection",
                     "PianoRollRenderer.h must not contain F0FrameToX/Y typedefs — deleted, buildF0VisualSegments takes value-object params.");
    return pass("noF0FrameToXYResurrection");
}

CheckResult noDrawPreparedF0CurveResurrection()
{
    const auto h = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");
    if (contains(h, "drawPreparedF0Curve"))
        return fail("noDrawPreparedF0CurveResurrection",
                     "PianoRollRenderer.h must not contain drawPreparedF0Curve — deleted as dead code. Rewrite F0 rendering from scratch when needed.");
    return pass("noDrawPreparedF0CurveResurrection");
}

// =============================================================================
// v12 Timeline/Camera/ViewMapper architecture contract tests
// =============================================================================

CheckResult v12ViewMapperExistsWithRequiredMethods()
{
    const auto header = readText("Source/Standalone/UI/ViewMapper.h");

    const auto missing = missingTokens(header, {
        "struct ViewMapper",
        "double visibleStartSeconds",
        "double pixelsPerSecond",
        "int contentStartX",
        "int contentWidth",
        "int contentHeight",
        "float pixelsPerSemitone",
        "float verticalScrollOffset",
        "float maxMidi",
        "int timeToX(",
        "int timeToXWithScroll(",
        "int timeToContentX(",
        "double xToTime(",
        "float midiToY(",
        "float yToMidi(",
        "float freqToMidi(",
        "float midiToFreq(",
        "float freqToY(",
        "float yToFreq(",
        "ViewMapper withBand("
    });
    if (!missing.empty())
        return fail("v12ViewMapperExistsWithRequiredMethods",
                    "ViewMapper.h missing required members/methods:" + missing);

    return pass("v12ViewMapperExistsWithRequiredMethods");
}

CheckResult v12TimelineViewportCameraHasDefaultConstant()
{
    const auto header = readText("Source/Standalone/UI/TimelineViewportCamera.h");

    if (!contains(header, "kDefaultPixelsPerSecond"))
        return fail("v12TimelineViewportCameraHasDefaultConstant",
                    "TimelineViewportCamera must define kDefaultPixelsPerSecond constant.");

    if (!contains(header, "static constexpr double kDefaultPixelsPerSecond = 100.0"))
        return fail("v12TimelineViewportCameraHasDefaultConstant",
                    "kDefaultPixelsPerSecond must be 'static constexpr double = 100.0'.");

    return pass("v12TimelineViewportCameraHasDefaultConstant");
}

CheckResult v12PlayheadOverlayOnlyHasSetPresentation()
{
    const auto header = readText("Source/Standalone/UI/PlayheadOverlayComponent.h");

    // 必须有 setPresentation 和 PlayheadPresentation
    if (!contains(header, "struct PlayheadPresentation"))
        return fail("v12PlayheadOverlayOnlyHasSetPresentation",
                    "PlayheadOverlayComponent.h must define PlayheadPresentation struct.");
    if (!contains(header, "void setPresentation(const PlayheadPresentation&"))
        return fail("v12PlayheadOverlayOnlyHasSetPresentation",
                    "PlayheadOverlayComponent must have setPresentation(PlayheadPresentation).");

    // setPlayheadColour 是允许的 — 它是颜色 setter，不是坐标/时间/滚动状态 setter
    // 不强制必须保留（以后颜色可能并入 PlayheadPresentation），但当前不禁止

    // 禁止旧坐标/时间/滚动状态 API
    const std::vector<std::string_view> banned = {
        "setPlayheadSeconds",
        "setTimelineStartSeconds",
        "setZoomLevel",
        "setScrollOffset",
        "setPinnedViewportX",
        "clearPinnedViewportX",
        "setPlaying",
        "playheadSeconds_",
        "timelineStartSeconds_",
        "pinnedViewportX_",
        "usePinnedViewportX_"
    };
    for (const auto token : banned) {
        if (contains(header, token))
            return fail("v12PlayheadOverlayOnlyHasSetPresentation",
                        "PlayheadOverlayComponent.h still contains banned old API: " + std::string(token));
    }

    return pass("v12PlayheadOverlayOnlyHasSetPresentation");
}

CheckResult v12NoDeletedTypesInSource()
{
    // 扫描全部 Source/ 下的 .h/.cpp，确认旧类型零残留
    const auto allSource = readSourceFilesUnder("Source");

    const std::vector<std::string_view> banned = {
        "TimeConverter",
        "PianoRollCoordinateMapper",
        "TimelineViewportState",
        "timeConverter_",
        "coordinateMapper_",
        "viewportState_"
    };
    for (const auto token : banned) {
        if (contains(allSource, token))
            return fail("v12NoDeletedTypesInSource",
                        "Deleted type/member still referenced in Source/: " + std::string(token));
    }

    return pass("v12NoDeletedTypesInSource");
}

CheckResult v12NoDeletedFunctionsInSource()
{
    const auto allSource = readSourceFilesUnder("Source");

    const std::vector<std::string_view> banned = {
        "toVisibleTimelineSeconds",
        "toAbsoluteTimelineSeconds",
        "getPlayheadAbsolutePixelX",
        "timeToXForRenderScroll",
        "xToTimeForRenderScroll",
        "getTimelinePixelsPerSecond",
        "makeTimelineViewportState",
        "applyDerivedZoomLevel",
        "applyDerivedScrollOffset",
        "projectPlayheadTime",
        "readProjectedPlayheadTime"
    };
    for (const auto token : banned) {
        if (contains(allSource, token))
            return fail("v12NoDeletedFunctionsInSource",
                        "Deleted function still referenced in Source/: " + std::string(token));
    }

    return pass("v12NoDeletedFunctionsInSource");
}

CheckResult v12PianoRollComponentHasCameraArchitecture()
{
    const auto header = readText("Source/Standalone/UI/PianoRollComponent.h");

    const auto missing = missingTokens(header, {
        "TimelineViewportCamera camera_",
        "ViewMapper makeViewMapper()",
        "int computeScrollOffsetPx()",
        "void setTimelineViewport(",
        "void publishPlayheadPresentation("
    });
    if (!missing.empty())
        return fail("v12PianoRollComponentHasCameraArchitecture",
                    "PianoRollComponent.h missing v12 camera architecture:" + missing);

    // 禁止旧成员
    if (contains(header, "zoomLevel_")
        || contains(header, "scrollOffset_")
        || contains(header, "viewMapperCache_"))
        return fail("v12PianoRollComponentHasCameraArchitecture",
                    "PianoRollComponent.h still contains banned old viewport members.");

    return pass("v12PianoRollComponentHasCameraArchitecture");
}

CheckResult v12ArrangementViewComponentHasCameraArchitecture()
{
    const auto header = readText("Source/Standalone/UI/ArrangementViewComponent.h");

    const auto missing = missingTokens(header, {
        "TimelineViewportCamera camera_",
        "ViewMapper makeViewMapper()",
        "int computeScrollOffsetPx()",
        "void setTimelineViewport("
    });
    if (!missing.empty())
        return fail("v12ArrangementViewComponentHasCameraArchitecture",
                    "ArrangementViewComponent.h missing v12 camera architecture:" + missing);

    if (contains(header, "zoomLevel_")
        || contains(header, "scrollOffset_")
        || contains(header, "viewportState_"))
        return fail("v12ArrangementViewComponentHasCameraArchitecture",
                    "ArrangementViewComponent.h still contains banned old viewport members.");

    return pass("v12ArrangementViewComponentHasCameraArchitecture");
}

CheckResult v12RendererUsesViewMapperInRenderContext()
{
    const auto header = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");

    if (!contains(header, "ViewMapper"))
        return fail("v12RendererUsesViewMapperInRenderContext",
                    "PianoRollRenderer.h must reference ViewMapper in RenderContext.");

    // 禁止旧坐标映射器
    if (contains(header, "PianoRollCoordinateMapper"))
        return fail("v12RendererUsesViewMapperInRenderContext",
                    "PianoRollRenderer.h still references deleted PianoRollCoordinateMapper.");

    return pass("v12RendererUsesViewMapperInRenderContext");
}

CheckResult v12ToolHandlerUsesViewMapper()
{
    const auto header = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h");

    if (!contains(header, "ViewMapper"))
        return fail("v12ToolHandlerUsesViewMapper",
                    "PianoRollToolHandler.h must reference ViewMapper.");

    // 禁止旧坐标回调
    const std::vector<std::string_view> banned = {
        "timeToX",
        "xToTime",
        "freqToY",
        "yToFreq"
    };
    for (const auto token : banned) {
        // 允许出现在注释中，但不能作为函数声明/定义
        if (contains(header, std::string("std::function<") + std::string(token))
            || contains(header, std::string(token) + std::string("(")))
            return fail("v12ToolHandlerUsesViewMapper",
                        "PianoRollToolHandler.h still declares old coordinate callback: " + std::string(token));
    }

    return pass("v12ToolHandlerUsesViewMapper");
}

CheckResult v12DeletedFilesAreGone()
{
    const std::vector<std::string_view> deletedFiles = {
        "Source/Standalone/UI/TimeConverter.h",
        "Source/Standalone/UI/TimeConverter.cpp",
        "Source/Standalone/UI/PianoRoll/PianoRollCoordinateMapper.h",
        "Source/Standalone/UI/TimelineViewportState.h"
    };

    for (const auto file : deletedFiles) {
        const auto path = sourcePath(file);
        if (std::filesystem::exists(path))
            return fail("v12DeletedFilesAreGone",
                        "Deleted file still exists: " + std::string(file));
    }

    return pass("v12DeletedFilesAreGone");
}

CheckResult v12CMakeListsHasViewMapper()
{
    const auto cmake = readText("CMakeLists.txt");

    if (!contains(cmake, "ViewMapper.h"))
        return fail("v12CMakeListsHasViewMapper",
                    "CMakeLists.txt must reference ViewMapper.h.");

    // 禁止已删除文件
    if (contains(cmake, "TimeConverter.h")
        || contains(cmake, "TimeConverter.cpp")
        || contains(cmake, "PianoRollCoordinateMapper.h")
        || contains(cmake, "TimelineViewportState.h"))
        return fail("v12CMakeListsHasViewMapper",
                    "CMakeLists.txt still references deleted files.");

    return pass("v12CMakeListsHasViewMapper");
}

// =============================================================================
// v12 Kill List 契约测试 — 防止已修复的 Kill List 问题复活
// =============================================================================

CheckResult v12KillList_noDeadGetCameraGetter()
{
    // K1/K2: getCamera() 是死 getter，已删除
    const auto pianoRollHeader = readText("Source/Standalone/UI/PianoRollComponent.h");
    const auto arrangementHeader = readText("Source/Standalone/UI/ArrangementViewComponent.h");

    if (contains(pianoRollHeader, "getCamera()"))
        return fail("v12KillList_noDeadGetCameraGetter",
                    "PianoRollComponent.h must not contain dead getCamera() getter (K1).");

    if (contains(arrangementHeader, "getCamera()"))
        return fail("v12KillList_noDeadGetCameraGetter",
                    "ArrangementViewComponent.h must not contain dead getCamera() getter (K2).");

    return pass("v12KillList_noDeadGetCameraGetter");
}

CheckResult v12KillList_noRenderScrollOffsetPxParam()
{
    // K3: buildContentRenderItem/buildRenderContext 旧参数链已收口
    // 检测方式：1) renderScrollOffsetPx 零残留  2) buildContentRenderItem 参数区 0 逗号（1 参数）  3) buildRenderContext(int...) 参数区 1 逗号（2 参数）
    const auto pianoRollHeader = readText("Source/Standalone/UI/PianoRollComponent.h");
    const auto pianoRollCpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    // 1. renderScrollOffsetPx 零残留
    if (contains(pianoRollHeader, "renderScrollOffsetPx"))
        return fail("v12KillList_noRenderScrollOffsetPxParam",
                    "PianoRollComponent.h must not contain renderScrollOffsetPx (K3).");
    if (contains(pianoRollCpp, "renderScrollOffsetPx"))
        return fail("v12KillList_noRenderScrollOffsetPxParam",
                    "PianoRollComponent.cpp must not contain renderScrollOffsetPx (K3).");

    // 2. buildContentRenderItem 仅 1 参数（参数区 0 逗号）
    {
        const size_t sigPos = pianoRollHeader.find("buildContentRenderItem(");
        if (sigPos == std::string::npos)
            return fail("v12KillList_noRenderScrollOffsetPxParam",
                        "Cannot locate buildContentRenderItem declaration.");
        const size_t parenEnd = pianoRollHeader.find(')', sigPos);
        if (parenEnd == std::string::npos)
            return fail("v12KillList_noRenderScrollOffsetPxParam",
                        "buildContentRenderItem signature malformed.");
        const std::string params = pianoRollHeader.substr(sigPos, parenEnd - sigPos);
        if (contains(params, ","))
            return fail("v12KillList_noRenderScrollOffsetPxParam",
                        "buildContentRenderItem must have only 1 parameter (placement) — comma detected in param list (K3).");
        if (!contains(params, "placement"))
            return fail("v12KillList_noRenderScrollOffsetPxParam",
                        "buildContentRenderItem must accept a placement parameter.");
    }

    // 3. buildRenderContext(int...) 仅 2 参数（参数区恰好 1 逗号）
    {
        const size_t sigPos = pianoRollHeader.find("buildRenderContext(int");
        if (sigPos == std::string::npos)
            return fail("v12KillList_noRenderScrollOffsetPxParam",
                        "Cannot locate 2-param buildRenderContext(int...) declaration.");
        const size_t parenEnd = pianoRollHeader.find(')', sigPos);
        if (parenEnd == std::string::npos)
            return fail("v12KillList_noRenderScrollOffsetPxParam",
                        "buildRenderContext signature malformed.");
        const std::string params = pianoRollHeader.substr(sigPos, parenEnd - sigPos);
        const size_t commaCount = countOf(params, ",");
        if (commaCount != 1)
            return fail("v12KillList_noRenderScrollOffsetPxParam",
                        "buildRenderContext must have exactly 2 parameters — expected 1 comma, found "
                        + std::to_string(commaCount) + " (K3).");
    }

    return pass("v12KillList_noRenderScrollOffsetPxParam");
}

CheckResult v12KillList_noRulerSurfaceStateZoomLevel()
{
    // K4: RulerSurfaceState::zoomLevel 实存 pixelsPerSecond，已重命名
    const auto header = readText("Source/Standalone/UI/ArrangementViewComponent.h");
    const auto cpp = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");

    // RulerSurfaceState 内不应有 zoomLevel 字段
    const auto rulerBlock = extractFunctionBlock(header, "struct RulerSurfaceState");
    if (!rulerBlock.empty() && contains(rulerBlock, "zoomLevel"))
        return fail("v12KillList_noRulerSurfaceStateZoomLevel",
                    "RulerSurfaceState must not have zoomLevel field (K4 renamed to pixelsPerSecond).");

    if (contains(cpp, "rulerSurfaceState_.zoomLevel"))
        return fail("v12KillList_noRulerSurfaceStateZoomLevel",
                    "ArrangementViewComponent.cpp must not reference rulerSurfaceState_.zoomLevel (K4).");

    return pass("v12KillList_noRulerSurfaceStateZoomLevel");
}

CheckResult v12KillList_fitToContentHasNoZoomTautology()
{
    // K5: fitToContent 不应有 zoom 中间变量和 /100*100 同义反复
    const auto cpp = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto fitBlock = extractFunctionBlock(cpp, "ArrangementViewComponent::fitToContent");

    if (fitBlock.empty())
        return fail("v12KillList_fitToContentHasNoZoomTautology",
                    "Cannot locate fitToContent function.");

    // 禁止 zoom 中间变量（任何形式的 double zoom 都不应出现）
    if (contains(fitBlock, "double zoom"))
        return fail("v12KillList_fitToContentHasNoZoomTautology",
                    "fitToContent must not declare 'double zoom' intermediate variable (K5).");

    // 禁止 / 100.0 + * 100.0 同义反复（任何写法）
    if (contains(fitBlock, "/ 100.0") && contains(fitBlock, "* 100.0"))
        return fail("v12KillList_fitToContentHasNoZoomTautology",
                    "fitToContent must not have /100*100 tautology (K5).");

    // 必须直接用 drawableWidth / maxEndTime 计算 pps
    if (!contains(fitBlock, "drawableWidth / maxEndTime") && !contains(fitBlock, "drawableWidth) / maxEndTime"))
        return fail("v12KillList_fitToContentHasNoZoomTautology",
                    "fitToContent must directly compute pps = drawableWidth / maxEndTime (K5).");

    return pass("v12KillList_fitToContentHasNoZoomTautology");
}

CheckResult v12KillList_maxEndNoViewportRight()
{
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto fn = extractFunctionBlock(cpp, "PianoRollComponent::computeMaxTimelineEndSeconds");
    
    if (fn.empty())
        return fail("v12KillList_maxEndNoViewportRight",
                    "Cannot locate computeMaxTimelineEndSeconds function.");
    
    if (contains(fn, "viewportRight") || contains(fn, "visibleStartSeconds") || contains(fn, "visibleDuration"))
        return fail("v12KillList_maxEndNoViewportRight",
                    "computeMaxTimelineEndSeconds must not depend on viewport state (viewportRight/visibleStartSeconds/visibleDuration).");
    
    return pass("v12KillList_maxEndNoViewportRight");
}

CheckResult v12KillList_maxEndNoHostPlayhead()
{
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto fn = extractFunctionBlock(cpp, "PianoRollComponent::computeMaxTimelineEndSeconds");
    
    if (fn.empty())
        return fail("v12KillList_maxEndNoHostPlayhead",
                    "Cannot locate computeMaxTimelineEndSeconds function.");
    
    if (contains(fn, "readPlayheadTime") || contains(fn, "isPlaying_") || contains(fn, "positionAtomic_"))
        return fail("v12KillList_maxEndNoHostPlayhead",
                    "computeMaxTimelineEndSeconds must not depend on host playhead time.");
    
    return pass("v12KillList_maxEndNoHostPlayhead");
}

CheckResult v12KillList_setTimelineViewportNoUserZoomFlag()
{
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto fn = extractFunctionBlock(cpp, "PianoRollComponent::setTimelineViewport");
    
    if (fn.empty())
        return fail("v12KillList_setTimelineViewportNoUserZoomFlag",
                    "Cannot locate setTimelineViewport function.");
    
    if (contains(fn, "userHasManuallyZoomed_"))
        return fail("v12KillList_setTimelineViewportNoUserZoomFlag",
                    "setTimelineViewport must not set userHasManuallyZoomed_ (only user zoom operations should).");
    
    return pass("v12KillList_setTimelineViewportNoUserZoomFlag");
}

CheckResult v12KillList_followAndPublishReceivesParam()
{
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto fn = extractFunctionBlock(cpp, "PianoRollComponent::followAndPublishPlayhead");
    
    if (fn.empty())
        return fail("v12KillList_followAndPublishReceivesParam",
                    "Cannot locate followAndPublishPlayhead function.");
    
    // 函数签名应接收参数
    if (!contains(cpp, "followAndPublishPlayhead(double timelinePlayheadTime)"))
        return fail("v12KillList_followAndPublishReceivesParam",
                    "followAndPublishPlayhead must receive double timelinePlayheadTime parameter.");
    
    // 函数体内不应自己读墙钟
    if (contains(fn, "getDisplayPlayheadTime") || contains(fn, "Time::getMillisecondCounterHiRes"))
        return fail("v12KillList_followAndPublishReceivesParam",
                    "followAndPublishPlayhead must not read wall clock internally (receive param instead).");
    
    // Verify that callers (updatePlayheadPresentationPolicy, onScrollVBlankCallback) DELEGATE
    // to followAndPublishPlayhead, not inline CONT follow
    const auto policyFn = extractFunctionBlock(cpp, "PianoRollComponent::updatePlayheadPresentationPolicy");
    if (!policyFn.empty()) {
        if (!contains(policyFn, "followAndPublishPlayhead"))
            return fail("v12KillList_followAndPublishReceivesParam",
                        "updatePlayheadPresentationPolicy must delegate to followAndPublishPlayhead.");
        if (contains(policyFn, "setTimelineViewport"))
            return fail("v12KillList_followAndPublishReceivesParam",
                        "updatePlayheadPresentationPolicy must not inline setTimelineViewport (delegate to followAndPublishPlayhead).");
    }
    
    const auto vblankFn = extractFunctionBlock(cpp, "PianoRollComponent::onScrollVBlankCallback");
    if (!vblankFn.empty()) {
        if (!contains(vblankFn, "followAndPublishPlayhead"))
            return fail("v12KillList_followAndPublishReceivesParam",
                        "onScrollVBlankCallback must call followAndPublishPlayhead.");
        // CONT follow logic must NOT be inlined
        if (contains(vblankFn, "pinnedContentX"))
            return fail("v12KillList_followAndPublishReceivesParam",
                        "onScrollVBlankCallback must not inline CONT follow logic (pinnedContentX belongs in followAndPublishPlayhead).");
    }

    return pass("v12KillList_followAndPublishReceivesParam");
}

CheckResult v12CMakeListsHasViewMapperTestTarget()
{
    const auto cmake = readText("CMakeLists.txt");

    if (!contains(cmake, "OpenTuneViewMapperTests"))
        return fail("v12CMakeListsHasViewMapperTestTarget",
                    "CMakeLists.txt must define OpenTuneViewMapperTests target.");

    if (!contains(cmake, "Tests/TestViewMapper.cpp"))
        return fail("v12CMakeListsHasViewMapperTestTarget",
                    "CMakeLists.txt must include Tests/TestViewMapper.cpp in test target.");

    if (!contains(cmake, "add_test(NAME OpenTuneViewMapperTests"))
        return fail("v12CMakeListsHasViewMapperTestTarget",
                    "CMakeLists.txt must register OpenTuneViewMapperTests with add_test().");

    return pass("v12CMakeListsHasViewMapperTestTarget");
}

// ============================================================================
// F0 可见性刷新链守卫测试
// ============================================================================

// 测试：OriginalF0 写入不触发 full render
CheckResult testOriginalF0WriteDoesNotTriggerFullRender()
{
    const auto source = readText("Source/PluginProcessor.cpp");

    // writeOriginalF0ToOwner 不应该调用 onContentFullMutationCompleted
    const auto writeOriginalF0 = extractFunctionBlock(source, "bool OpenTuneAudioProcessor::writeOriginalF0ToOwner");
    if (writeOriginalF0.empty())
        return fail("testOriginalF0WriteDoesNotTriggerFullRender",
            "writeOriginalF0ToOwner function not found");
    if (contains(writeOriginalF0, "onContentFullMutationCompleted"))
        return fail("testOriginalF0WriteDoesNotTriggerFullRender",
            "writeOriginalF0ToOwner MUST NOT call onContentFullMutationCompleted");

    // OriginalF0 完成回调附近不应有 onContentFullMutationCompleted
    // 搜索 "writeOriginalF0ToOwner(capturedRequest.contentKey, pitchCurve)" 前后 10 行
    const auto callSite = source.find("writeOriginalF0ToOwner(capturedRequest.contentKey, pitchCurve)");
    if (callSite == std::string::npos)
        return fail("testOriginalF0WriteDoesNotTriggerFullRender",
            "OriginalF0 completion call site not found");
    const auto callContext = source.substr(
        callSite > 500 ? callSite - 500 : 0,
        std::min(size_t(1000), source.size() - (callSite > 500 ? callSite - 500 : 0)));
    if (contains(callContext, "onContentFullMutationCompleted"))
        return fail("testOriginalF0WriteDoesNotTriggerFullRender",
            "OriginalF0 completion MUST NOT call onContentFullMutationCompleted");

    return pass("testOriginalF0WriteDoesNotTriggerFullRender");
}

// 测试：pitchRevision 回写闭环
CheckResult testPitchRevisionTrackingClosedLoop()
{
    const auto source = readText("Source/Standalone/PluginEditor.cpp");

    // lastPianoRollPitchRevision_ 必须在 timerCallback 中回写
    const auto timerCallback = extractFunctionBlock(source, "void OpenTuneAudioProcessorEditor::timerCallback()");
    if (!contains(timerCallback, "lastPianoRollPitchRevision_ = currentPitchRevision"))
        return fail("testPitchRevisionTrackingClosedLoop",
            "timerCallback MUST write back lastPianoRollPitchRevision_");

    return pass("testPitchRevisionTrackingClosedLoop");
}

// 测试：导入路径不传 nullptr curve
CheckResult testImportDoesNotPassNullCurve()
{
    const auto source = readText("Source/Standalone/PluginEditor.cpp");

    // 搜索导入路径中 setEditedContent 调用附近上下文
    const auto callSite = source.find("setEditedContent(committedPlacement.contentKey");
    if (callSite == std::string::npos)
        return fail("testImportDoesNotPassNullCurve",
            "Import setEditedContent call site not found");
    const auto callContext = source.substr(
        callSite > 300 ? callSite - 300 : 0,
        std::min(size_t(600), source.size() - (callSite > 300 ? callSite - 300 : 0)));

    // 导入路径中 setEditedContent 不应该传 nullptr 作为 curve 参数
    // 检查 "importCurve," 存在（说明传了实际 curve）
    if (!contains(callContext, "importCurve,"))
        return fail("testImportDoesNotPassNullCurve",
            "Import path MUST pass importCurve as curve to setEditedContent");

    // lastPianoRollCurve_ 不应该 reset 为 nullptr
    if (contains(callContext, "lastPianoRollCurve_.reset()"))
        return fail("testImportDoesNotPassNullCurve",
            "Import path MUST NOT reset lastPianoRollCurve_ to nullptr");

    return pass("testImportDoesNotPassNullCurve");
}

// 测试：selection sync 同步 revision baseline
CheckResult testSelectionSyncRevisions()
{
    const auto source = readText("Source/Standalone/PluginEditor.cpp");

    // 验证 selection sync 路径中存在 pitchRevision baseline 赋值
    // 特征模式：lastPianoRollPitchRevision_ = snap ? snap->pitchRevision
    if (!contains(source, "lastPianoRollPitchRevision_ = snap"))
        return fail("testSelectionSyncRevisions",
            "Selection sync MUST sync pitchRevision baseline");

    return pass("testSelectionSyncRevisions");
}

// 测试：applyOriginalF0 不递增 contentRevision
CheckResult testApplyOriginalF0DoesNotBumpContentRevision()
{
    const auto standaloneSource = readText("Source/Content/StandaloneClipContent.cpp");
    const auto applyOrigF0 = extractFunctionBlock(standaloneSource, "void StandaloneClipContent::applyOriginalF0(");
    if (applyOrigF0.empty())
        return fail("testApplyOriginalF0DoesNotBumpContentRevision",
            "StandaloneClipContent::applyOriginalF0 not found");
    if (contains(applyOrigF0, "bumpContentRevision"))
        return fail("testApplyOriginalF0DoesNotBumpContentRevision",
            "applyOriginalF0 MUST NOT call bumpContentRevision");

    const auto captureSource = readText("Source/Content/CaptureSegmentContent.cpp");
    // Use "applyOriginalF0(" with opening paren to avoid matching applyOriginalF0State
    const auto captureApplyOrigF0 = extractFunctionBlock(captureSource, "void CaptureSegmentContent::applyOriginalF0(");
    if (captureApplyOrigF0.empty())
        return fail("testApplyOriginalF0DoesNotBumpContentRevision",
            "CaptureSegmentContent::applyOriginalF0 not found");
    if (contains(captureApplyOrigF0, "contentRevision"))
        return fail("testApplyOriginalF0DoesNotBumpContentRevision",
            "CaptureSegmentContent::applyOriginalF0 MUST NOT increment contentRevision");

    return pass("testApplyOriginalF0DoesNotBumpContentRevision");
}

// 测试：AudioModification 无重复 API
CheckResult testNoDuplicateF0AnalysisAPI()
{
    const auto source = readText("Source/ARA/AudioModification.h");
    if (contains(source, "applyF0Analysis"))
        return fail("testNoDuplicateF0AnalysisAPI",
            "AudioModification MUST NOT have applyF0Analysis (use applyOriginalF0)");

    const auto cppSource = readText("Source/ARA/AudioModification.cpp");
    if (contains(cppSource, "void AudioModification::applyF0Analysis"))
        return fail("testNoDuplicateF0AnalysisAPI",
            "AudioModification.cpp MUST NOT define applyF0Analysis");

    return pass("testNoDuplicateF0AnalysisAPI");
}

CheckResult scaleUiMappingCoversAllEightTypes()
{
    const auto source = readText("Source/Utils/ScaleUiMapping.h");

    const std::vector<std::string_view> required = {
        "scaleToUiScaleType",
        "uiScaleTypeToScale",
        "makeDetectedKeyFromUi",
        "Scale::HarmonicMinor",
        "Scale::Dorian",
        "Scale::Mixolydian",
        "Scale::PentatonicMajor",
        "Scale::PentatonicMinor",
    };

    for (const auto token : required) {
        if (!contains(source, token))
            return fail("scaleUiMappingCoversAllEightTypes",
                "ScaleUiMapping.h missing: " + std::string(token));
    }

    return pass("scaleUiMappingCoversAllEightTypes");
}

CheckResult vst3PluginEditorDoesNotUseThreeStateScaleMapping()
{
    const auto source = readText("Source/Plugin/PluginEditor.cpp");

    if (contains(source, "clampedType == 2") && contains(source, "Scale::Minor"))
        return fail("vst3PluginEditorDoesNotUseThreeStateScaleMapping",
            "PluginEditor.cpp MUST NOT have three-state scale mapping (scaleType==2 ? Minor ...)");

    if (contains(source, "clampedType == 3") && contains(source, "Scale::Chromatic"))
        return fail("vst3PluginEditorDoesNotUseThreeStateScaleMapping",
            "PluginEditor.cpp MUST NOT have three-state scale mapping (scaleType==3 ? Chromatic ...)");

    if (contains(source, "detectedKey.scale == Scale::Minor") && contains(source, "? 2 :"))
        return fail("vst3PluginEditorDoesNotUseThreeStateScaleMapping",
            "PluginEditor.cpp MUST NOT have reverse three-state mapping (Minor ? 2 : ...)");

    return pass("vst3PluginEditorDoesNotUseThreeStateScaleMapping");
}

CheckResult standaloneAndPluginUseSharedScaleMapping()
{
    const auto standaloneHeader = readText("Source/Standalone/PluginEditor.h");
    const auto standaloneSource = readText("Source/Standalone/PluginEditor.cpp");
    const auto pluginSource = readText("Source/Plugin/PluginEditor.cpp");

    // Standalone must NOT declare private scaleToUiScaleType/uiScaleTypeToScale
    if (contains(standaloneHeader, "static int scaleToUiScaleType"))
        return fail("standaloneAndPluginUseSharedScaleMapping",
            "Standalone PluginEditor.h MUST NOT declare private scaleToUiScaleType");

    if (contains(standaloneHeader, "static Scale uiScaleTypeToScale"))
        return fail("standaloneAndPluginUseSharedScaleMapping",
            "Standalone PluginEditor.h MUST NOT declare private uiScaleTypeToScale");

    if (contains(standaloneHeader, "static DetectedKey makeDetectedKeyFromUi"))
        return fail("standaloneAndPluginUseSharedScaleMapping",
            "Standalone PluginEditor.h MUST NOT declare private makeDetectedKeyFromUi");

    // Standalone source must NOT define these functions as member implementations
    if (contains(standaloneSource, "OpenTuneAudioProcessorEditor::scaleToUiScaleType"))
        return fail("standaloneAndPluginUseSharedScaleMapping",
            "Standalone PluginEditor.cpp MUST NOT define scaleToUiScaleType as member");

    if (contains(standaloneSource, "OpenTuneAudioProcessorEditor::uiScaleTypeToScale"))
        return fail("standaloneAndPluginUseSharedScaleMapping",
            "Standalone PluginEditor.cpp MUST NOT define uiScaleTypeToScale as member");

    // Both must include the shared header
    if (!contains(standaloneSource, "ScaleUiMapping.h") && !contains(standaloneHeader, "ScaleUiMapping.h"))
        return fail("standaloneAndPluginUseSharedScaleMapping",
            "Standalone MUST include ScaleUiMapping.h");

    if (!contains(pluginSource, "ScaleUiMapping.h"))
        return fail("standaloneAndPluginUseSharedScaleMapping",
            "Plugin PluginEditor.cpp MUST include ScaleUiMapping.h");

    return pass("standaloneAndPluginUseSharedScaleMapping");
}

CheckResult f0RendererHasNoVerticalBucketDecimation()
{
    const auto source = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");

    // Extract the drawF0Curve function block
    const auto drawF0Curve = extractFunctionBlock(source, "void PianoRollRenderer::drawF0Curve(");
    if (drawF0Curve.empty())
        return fail("f0RendererHasNoVerticalBucketDecimation",
            "drawF0Curve function not found in PianoRollRenderer.cpp");

    if (contains(drawF0Curve, "drawVerticalLine"))
        return fail("f0RendererHasNoVerticalBucketDecimation",
            "drawF0Curve MUST NOT use drawVerticalLine for F0 rendering");

    if (contains(drawF0Curve, "std::map<int, std::pair<float, float>>"))
        return fail("f0RendererHasNoVerticalBucketDecimation",
            "drawF0Curve MUST NOT use std::map buckets for F0 decimation");

    if (contains(drawF0Curve, "std::map<int, std::pair"))
        return fail("f0RendererHasNoVerticalBucketDecimation",
            "drawF0Curve MUST NOT use any std::map bucket pattern");

    return pass("f0RendererHasNoVerticalBucketDecimation");
}

CheckResult f0RendererUsesSingleContinuousPathHelper()
{
    const auto source = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");

    // The shared helper must exist
    if (!contains(source, "buildF0ContinuousPath"))
        return fail("f0RendererUsesSingleContinuousPathHelper",
            "PianoRollRenderer.cpp MUST define buildF0ContinuousPath helper");

    // Extract drawF0Curve
    const auto drawF0Curve = extractFunctionBlock(source, "void PianoRollRenderer::drawF0Curve(");
    if (drawF0Curve.empty())
        return fail("f0RendererUsesSingleContinuousPathHelper",
            "drawF0Curve function not found");

    // Both OriginalF0 and CorrectedF0 must use the shared helper
    if (!contains(drawF0Curve, "buildF0ContinuousPath"))
        return fail("f0RendererUsesSingleContinuousPathHelper",
            "drawF0Curve MUST call buildF0ContinuousPath");

    // Count how many times buildF0ContinuousPath is called in drawF0Curve
    // It should be called exactly twice (once for original, once for corrected)
    const size_t callCount = countOf(drawF0Curve, "buildF0ContinuousPath(");
    if (callCount < 2)
        return fail("f0RendererUsesSingleContinuousPathHelper",
            "drawF0Curve MUST call buildF0ContinuousPath at least twice (original + corrected), found " + std::to_string(callCount));

    // Must NOT have separate decimation branches with their own path building
    if (contains(drawF0Curve, "useDecimation") && contains(drawF0Curve, "std::map"))
        return fail("f0RendererUsesSingleContinuousPathHelper",
            "drawF0Curve MUST NOT have separate decimation branches with std::map");

    return pass("f0RendererUsesSingleContinuousPathHelper");
}

// =============================================================================
// PianoRollSurfaceCache architecture guard tests
// =============================================================================

CheckResult surfaceCache_noOldBandCacheTokensInSource()
{
    // Verify old band cache tokens are gone from source tree
    const std::vector<std::string> forbidden = {
        "backgroundCacheImage_",
        "detailCacheImage_",
        "cacheBandStartX_",
        "cacheBandWidth_",
        "cacheBandHeight_",
        "backgroundCacheDirty_",
        "detailCacheDirty_",
        "rebuildDirtyCaches",
        "rebuildBackgroundCache",
        "rebuildDetailCache",
        "needsGeometryRebuild",
        "makeBandViewMapper"
    };
    
    const std::vector<std::string> sourceFiles = {
        "Source/Standalone/UI/PianoRollComponent.h",
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "Source/Standalone/UI/PianoRoll/PianoRollSurfaceCache.h",
        "Source/Standalone/UI/PianoRoll/PianoRollSurfaceCache.cpp"
    };
    
    for (const auto& file : sourceFiles) {
        std::ifstream ifs(sourcePath(file));
        if (!ifs.is_open()) continue;
        std::string line;
        int lineNum = 0;
        while (std::getline(ifs, line)) {
            ++lineNum;
            for (const auto& token : forbidden) {
                if (line.find(token) != std::string::npos) {
                    return fail("surfaceCache_noOldBandCacheTokensInSource",
                                "Forbidden token '" + token + "' found in " + file + ":" + std::to_string(lineNum));
                }
            }
        }
    }
    return pass("surfaceCache_noOldBandCacheTokensInSource");
}

CheckResult surfaceCache_hasFiveSemanticSlots()
{
    // Verify PianoRollSurfaceCache has exactly 5 slots
    const auto header = readText("Source/Standalone/UI/PianoRoll/PianoRollSurfaceCache.h");
    
    if (!contains(header, "kSlotCount"))
        return fail("surfaceCache_hasFiveSemanticSlots",
                    "PianoRollSurfaceCache must define kSlotCount constant.");
    
    // Verify all 5 slots exist: Background, Waveform, Notes, F0, TimeAnchors
    const std::vector<std::string_view> requiredSlots = {
        "Background", "Waveform", "Notes", "F0", "TimeAnchors"
    };
    for (const auto slot : requiredSlots) {
        if (!contains(header, slot))
            return fail("surfaceCache_hasFiveSemanticSlots",
                        std::string("PianoRollSurfaceCache::Slot must define ") + std::string(slot) + ".");
    }
    
    return pass("surfaceCache_hasFiveSemanticSlots");
}

CheckResult surfaceCache_setTimelineViewportNoUserZoomFlag()
{
    // Verify setTimelineViewport() does NOT set userHasManuallyZoomed_
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto fn = extractFunctionBlock(cpp, "PianoRollComponent::setTimelineViewport");
    
    if (fn.empty())
        return fail("surfaceCache_setTimelineViewportNoUserZoomFlag",
                    "Cannot locate setTimelineViewport function.");
    
    if (contains(fn, "userHasManuallyZoomed_"))
        return fail("surfaceCache_setTimelineViewportNoUserZoomFlag",
                    "setTimelineViewport must NOT set userHasManuallyZoomed_ (only direct user zoom operations should).");
    
    return pass("surfaceCache_setTimelineViewportNoUserZoomFlag");
}

CheckResult surfaceCache_fullClipDomainUsesPlacementMinMax()
{
    // Surface domain must be derived from clip placement min/max timeline times
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    // Must have computeSurfaceStartTimelineSeconds using timelineStartSeconds
    const auto startFn = extractFunctionBlock(cpp, "PianoRollComponent::computeSurfaceStartTimelineSeconds");
    if (startFn.empty() || !contains(startFn, "timelineStartSeconds"))
        return fail("surfaceCache_fullClipDomainUsesPlacementMinMax",
                    "computeSurfaceStartTimelineSeconds must scan placements for timelineStartSeconds.");

    // Must have computeSurfaceEndTimelineSeconds using timelineEndSeconds
    const auto endFn = extractFunctionBlock(cpp, "PianoRollComponent::computeSurfaceEndTimelineSeconds");
    if (endFn.empty() || !contains(endFn, "timelineEndSeconds"))
        return fail("surfaceCache_fullClipDomainUsesPlacementMinMax",
                    "computeSurfaceEndTimelineSeconds must scan placements for timelineEndSeconds.");

    // Neither function may reference viewportRight
    if (contains(startFn, "viewportRight") || contains(endFn, "viewportRight"))
        return fail("surfaceCache_fullClipDomainUsesPlacementMinMax",
                    "Surface domain must not reference viewportRight.");

    return pass("surfaceCache_fullClipDomainUsesPlacementMinMax");
}

CheckResult surfaceCache_noViewportSizedOrOverscanTokens()
{
    // Cache surface must be full-clip, not viewport-sized — no overscan/coverage/fallback tokens
    const auto cacheCpp = readText("Source/Standalone/UI/PianoRoll/PianoRollSurfaceCache.cpp");
    const auto cacheH = readText("Source/Standalone/UI/PianoRoll/PianoRollSurfaceCache.h");
    const std::vector<std::string> forbidden = {"overscan", "coverage", "fallback", "exposed"};
    for (const auto& token : forbidden) {
        if (contains(cacheCpp, token) || contains(cacheH, token))
            return fail("surfaceCache_noViewportSizedOrOverscanTokens",
                        "Forbidden token '" + token + "' found in surface cache files.");
    }

    return pass("surfaceCache_noViewportSizedOrOverscanTokens");
}

CheckResult surfaceCache_scrollOnlyDoesNotDirtySlots()
{
    // Scroll-only (same pps) must not dirty cache slots; invalidateAll must be guarded by zoomChanged
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto setFn = extractFunctionBlock(cpp, "PianoRollComponent::setTimelineViewport");

    if (setFn.empty())
        return fail("surfaceCache_scrollOnlyDoesNotDirtySlots",
                    "Cannot locate setTimelineViewport.");

    // setTimelineViewport must guard invalidateAll behind zoomChanged — scroll alone does not dirty
    if (contains(setFn, "invalidateAll") && !contains(setFn, "zoomChanged"))
        return fail("surfaceCache_scrollOnlyDoesNotDirtySlots",
                    "invalidateAll must be guarded by zoomChanged (scroll-only must not rebuild cache).");

    // Direct markDirty calls must not appear in scroll-mode code paths
    const auto followFn = extractFunctionBlock(cpp, "PianoRollComponent::followAndPublishPlayhead");
    if (!followFn.empty() && (contains(followFn, "surfaceCache_.markDirty") || contains(followFn, "surfaceCache_.invalidate")))
        return fail("surfaceCache_scrollOnlyDoesNotDirtySlots",
                    "followAndPublishPlayhead must not call surfaceCache_.markDirty or invalidate.");

    return pass("surfaceCache_scrollOnlyDoesNotDirtySlots");
}

CheckResult surfaceCache_zoomDirtyAllSlots()
{
    // Zoom change must dirty all 4 slots for resolution-dependent rebuild
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto setFn = extractFunctionBlock(cpp, "PianoRollComponent::setTimelineViewport");

    if (setFn.empty())
        return fail("surfaceCache_zoomDirtyAllSlots",
                    "Cannot locate setTimelineViewport.");

    // Must have zoom detection (pixelsPerSecond comparison)
    if (!contains(setFn, "zoomChanged") || !contains(setFn, "pixelsPerSecond"))
        return fail("surfaceCache_zoomDirtyAllSlots",
                    "setTimelineViewport must detect zoom change via pixelsPerSecond comparison.");

    // Must call invalidateAll on zoom change
    if (!contains(setFn, "invalidateAll"))
        return fail("surfaceCache_zoomDirtyAllSlots",
                    "Zoom change must call surfaceCache_.invalidateAll() to dirty all slots.");

    return pass("surfaceCache_zoomDirtyAllSlots");
}

CheckResult surfaceCache_paintUsesFullClipOffset()
{
    // computeScrollOffsetPx must subtract surfaceStartSec for full-clip domain
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto scrollFn = extractFunctionBlock(cpp, "PianoRollComponent::computeScrollOffsetPx");

    if (scrollFn.empty())
        return fail("surfaceCache_paintUsesFullClipOffset",
                    "Cannot locate computeScrollOffsetPx.");

    // Must reference surfaceStartSec (not assume origin = 0)
    if (!contains(scrollFn, "surfaceStartSec"))
        return fail("surfaceCache_paintUsesFullClipOffset",
                    "computeScrollOffsetPx must subtract surfaceStartSec for full-clip offset.");

    return pass("surfaceCache_paintUsesFullClipOffset");
}

CheckResult surfaceCache_buildUsesFullClipMapper()
{
    // Cache build in paint() must use ViewMapper with full-clip domain (visibleStart=surfaceStart, contentStartX=0)
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto paintFn = extractFunctionBlock(cpp, "PianoRollComponent::paint");

    if (paintFn.empty())
        return fail("surfaceCache_buildUsesFullClipMapper",
                    "Cannot locate PianoRollComponent::paint.");

    // Must call configureGeometry (sets full-clip surface geometry)
    if (!contains(paintFn, "configureGeometry"))
        return fail("surfaceCache_buildUsesFullClipMapper",
                    "paint() must call surfaceCache_.configureGeometry for full-clip domain.");

    // Must reference buildAllDirty (the cache rebuild entry point)
    if (!contains(paintFn, "buildAllDirty"))
        return fail("surfaceCache_buildUsesFullClipMapper",
                    "paint() must call surfaceCache_.buildAllDirty.");

    return pass("surfaceCache_buildUsesFullClipMapper");
}

CheckResult surfaceCache_paintNoRendererDrawNoFallback()
{
    // paint() must only blit cache surfaces — no renderer draw calls, no fallback
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto paintFn = extractFunctionBlock(cpp, "PianoRollComponent::paint");

    if (paintFn.empty())
        return fail("surfaceCache_paintNoRendererDrawNoFallback",
                    "Cannot locate paint().");

    // Must call surfaceCache_.paint (the blit)
    if (!contains(paintFn, "surfaceCache_.paint"))
        return fail("surfaceCache_paintNoRendererDrawNoFallback",
                    "paint() must call surfaceCache_.paint().");

    // Must NOT call renderer_->draw* within paint() body
    const std::vector<std::string> forbiddenRenders = {
        "renderer_->drawLanes", "renderer_->drawNotes", "renderer_->drawWaveform",
        "renderer_->drawF0Curve", "renderer_->drawGridLines", "renderer_->drawTimeRuler",
        "renderer_->drawChunkBoundaries", "renderer_->drawUnvoicedFrameBands"
    };
    for (const auto& token : forbiddenRenders) {
        if (contains(paintFn, token))
            return fail("surfaceCache_paintNoRendererDrawNoFallback",
                        "paint() must not call '" + token + "' — cache blit only.");
    }

    return pass("surfaceCache_paintNoRendererDrawNoFallback");
}

CheckResult surfaceCache_forbiddenTransientStateNotCached()
{
    // Interactive overlay state must never enter cache slots
    const auto cacheCpp = readText("Source/Standalone/UI/PianoRoll/PianoRollSurfaceCache.cpp");
    const auto buildSlotFn = extractFunctionBlock(cacheCpp, "PianoRollSurfaceCache::buildSlot");

    if (buildSlotFn.empty())
        return fail("surfaceCache_forbiddenTransientStateNotCached",
                    "Cannot locate buildSlot.");

    // Must not reference playhead within buildSlot
    const std::vector<std::string> forbidden = {
        "playhead", "pending",
        "transport", "seeking", "seek"
    };
    for (const auto& token : forbidden) {
        if (contains(buildSlotFn, token))
            return fail("surfaceCache_forbiddenTransientStateNotCached",
                        "buildSlot must not cache transient state token '" + token + "'.");
    }

    return pass("surfaceCache_forbiddenTransientStateNotCached");
}

CheckResult surfaceCache_timeGridHandlesStayOverlay()
{
    // Time-grid handles must be painted as overlay (paintOverChildren), never in cache slots
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto overlayFn = extractFunctionBlock(cpp, "PianoRollComponent::paintOverChildren");

    if (overlayFn.empty())
        return fail("surfaceCache_timeGridHandlesStayOverlay",
                    "Cannot locate paintOverChildren.");

    // paintOverChildren must draw time grid handles
    if (!contains(overlayFn, "drawTimeGridHandles"))
        return fail("surfaceCache_timeGridHandlesStayOverlay",
                    "paintOverChildren must call drawTimeGridHandles.");

    // Cache buildSlot must not draw time grid handles
    const auto cacheCpp = readText("Source/Standalone/UI/PianoRoll/PianoRollSurfaceCache.cpp");
    const auto buildSlotFn = extractFunctionBlock(cacheCpp, "PianoRollSurfaceCache::buildSlot");
    if (contains(buildSlotFn, "drawTimeGridHandles"))
        return fail("surfaceCache_timeGridHandlesStayOverlay",
                    "buildSlot must NOT call drawTimeGridHandles (overlay only).");

    return pass("surfaceCache_timeGridHandlesStayOverlay");
}

CheckResult surfaceCache_verticalGeometryDirtiesYSlots()
{
    // Vertical scroll/zoom must dirty Background, Notes, F0 (y-dependent slots)
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto refreshFn = extractFunctionBlock(cpp, "PianoRollComponent::refreshVerticalViewportGeometry");

    if (refreshFn.empty())
        return fail("surfaceCache_verticalGeometryDirtiesYSlots",
                    "Cannot locate refreshVerticalViewportGeometry.");

    // Must mark Background dirty
    if (!contains(refreshFn, "Slot::Background"))
        return fail("surfaceCache_verticalGeometryDirtiesYSlots",
                    "refreshVerticalViewportGeometry must markDirty Slot::Background.");

    // Must mark Notes dirty
    if (!contains(refreshFn, "Slot::Notes"))
        return fail("surfaceCache_verticalGeometryDirtiesYSlots",
                    "refreshVerticalViewportGeometry must markDirty Slot::Notes.");

    // Must mark F0 dirty
    if (!contains(refreshFn, "Slot::F0"))
        return fail("surfaceCache_verticalGeometryDirtiesYSlots",
                    "refreshVerticalViewportGeometry must markDirty Slot::F0.");

    // Must NOT mark Waveform dirty (not y-dependent)
    if (contains(refreshFn, "Slot::Waveform"))
        return fail("surfaceCache_verticalGeometryDirtiesYSlots",
                    "refreshVerticalViewportGeometry must NOT dirty Waveform (not y-dependent).");

    return pass("surfaceCache_verticalGeometryDirtiesYSlots");
}

CheckResult surfaceCache_configureGeometryDetectsDomainShift()
{
    // configureGeometry must detect domain shift (startSec/endSec change)
    const auto cacheCpp = readText("Source/Standalone/UI/PianoRoll/PianoRollSurfaceCache.cpp");
    const auto configFn = extractFunctionBlock(cacheCpp, "PianoRollSurfaceCache::configureGeometry");

    if (configFn.empty())
        return fail("surfaceCache_configureGeometryDetectsDomainShift",
                    "Cannot locate configureGeometry.");

    // Must check startSec against surfaceStartSec_
    if (!contains(configFn, "surfaceStartSec_"))
        return fail("surfaceCache_configureGeometryDetectsDomainShift",
                    "configureGeometry must detect startSec change via surfaceStartSec_ comparison.");

    // Must check endSec against surfaceEndSec_  
    if (!contains(configFn, "surfaceEndSec_"))
        return fail("surfaceCache_configureGeometryDetectsDomainShift",
                    "configureGeometry must detect endSec change via surfaceEndSec_ comparison.");

    return pass("surfaceCache_configureGeometryDetectsDomainShift");
}

CheckResult surfaceCache_noVBlankCacheTestNamesRemain()
{
    // Verify no old vBlankCache test names remain in TestMain.cpp
    const auto testMain = readText("Tests/TestMain.cpp");
    
    // Check for specific old test function patterns (not this test's own name)
    // Use string concatenation to avoid self-matching
    const std::string prefix = "vBlank" + std::string("Cache_");
    if (contains(testMain, prefix + "paint") ||
        contains(testMain, prefix + "cacheMembers") ||
        contains(testMain, prefix + "flush") ||
        contains(testMain, prefix + "rebuild"))
        return fail("surfaceCache_noVBlankCacheTestNamesRemain",
                    "TestMain.cpp must not contain old vBlankCache test functions.");
    
    return pass("surfaceCache_noVBlankCacheTestNamesRemain");
}

CheckResult surfaceCache_noGenericCachedSurfaceInPianoRoll()
{
    // Verify PianoRoll doesn't use ArrangementCachedSurface or generic cached surface
    const std::vector<std::string> forbidden = {
        "ArrangementCachedSurface",
        "GenericCachedSurface",
        "PianoRollCachedSurface"
    };
    
    const std::vector<std::string> sourceFiles = {
        "Source/Standalone/UI/PianoRollComponent.h",
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "Source/Standalone/UI/PianoRoll/PianoRollSurfaceCache.h",
        "Source/Standalone/UI/PianoRoll/PianoRollSurfaceCache.cpp"
    };
    
    for (const auto& file : sourceFiles) {
        const auto content = readText(file);
        for (const auto& token : forbidden) {
            if (contains(content, token))
                return fail("surfaceCache_noGenericCachedSurfaceInPianoRoll",
                            "Forbidden token '" + token + "' found in " + file);
        }
    }
    return pass("surfaceCache_noGenericCachedSurfaceInPianoRoll");
}

CheckResult surfaceCache_surfaceRenderContextExists()
{
    const auto hdr = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");
    if (!contains(hdr, "struct SurfaceRenderContext"))
        return fail("surfaceCache_surfaceRenderContextExists",
                    "PianoRollRenderer.h must define SurfaceRenderContext struct.");
    return pass("surfaceCache_surfaceRenderContextExists");
}

CheckResult surfaceCache_surfaceRenderContextExcludesTransient()
{
    const auto hdr = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");
    
    const auto sctxStart = hdr.find("struct SurfaceRenderContext");
    const auto sctxEnd = hdr.find("};", sctxStart);
    if (sctxStart == std::string::npos || sctxEnd == std::string::npos)
        return fail("surfaceCache_surfaceRenderContextExcludesTransient",
                    "Cannot locate SurfaceRenderContext struct.");
    
    const auto sctxBlock = hdr.substr(sctxStart, sctxEnd - sctxStart);
    
    const std::vector<std::string> forbidden = {
        "pressedPianoKey", "hasF0Selection", "f0SelectionStartFrame",
        "f0SelectionEndFrameExclusive", "referenceOverlay",
        "timeGridHoveredHandleId", "timeGridSelectedHandleId",
        "additionalSelectedHandleIds", "selectedLineAnchorSegmentIds"
    };
    for (const auto& token : forbidden) {
        if (contains(sctxBlock, token))
            return fail("surfaceCache_surfaceRenderContextExcludesTransient",
                        "SurfaceRenderContext must not contain '" + token + "'.");
    }
    return pass("surfaceCache_surfaceRenderContextExcludesTransient");
}

CheckResult surfaceCache_revisionRouteNotesOnlyDirtiesNotes()
{
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto fn = extractFunctionBlock(cpp, "PianoRollComponent::onNotesRevisionChanged");
    if (fn.empty())
        return fail("surfaceCache_revisionRouteNotesOnlyDirtiesNotes",
                    "Cannot locate onNotesRevisionChanged.");
    if (!contains(fn, "Slot::Notes"))
        return fail("surfaceCache_revisionRouteNotesOnlyDirtiesNotes",
                    "onNotesRevisionChanged must dirty Slot::Notes.");
    // Must NOT dirty ChunkBoundaries
    if (contains(fn, "Slot::ChunkBoundaries"))
        return fail("surfaceCache_revisionRouteNotesOnlyDirtiesNotes",
                    "onNotesRevisionChanged must NOT dirty ChunkBoundaries.");
    if (contains(fn, "Slot::F0") || contains(fn, "Slot::Waveform") || contains(fn, "Slot::TimeAnchors"))
        return fail("surfaceCache_revisionRouteNotesOnlyDirtiesNotes",
                    "onNotesRevisionChanged must only dirty Notes.");
    return pass("surfaceCache_revisionRouteNotesOnlyDirtiesNotes");
}

CheckResult surfaceCache_revisionRouteTimeGridDirtiesAllFourContentSlots()
{
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto fn = extractFunctionBlock(cpp, "PianoRollComponent::onTimeGridRevisionChanged");
    if (fn.empty())
        return fail("surfaceCache_revisionRouteTimeGridDirtiesAllFourContentSlots",
                    "Cannot locate onTimeGridRevisionChanged.");
    for (const auto& slot : {"Slot::Waveform", "Slot::Notes", "Slot::F0", "Slot::TimeAnchors"}) {
        if (!contains(fn, slot))
            return fail("surfaceCache_revisionRouteTimeGridDirtiesAllFourContentSlots",
                        std::string("onTimeGridRevisionChanged must dirty ") + slot + ".");
    }
    return pass("surfaceCache_revisionRouteTimeGridDirtiesAllFourContentSlots");
}


CheckResult surfaceCache_timeAnchorsSlotExists()
{
    const auto hdr = readText("Source/Standalone/UI/PianoRoll/PianoRollSurfaceCache.h");
    if (!contains(hdr, "TimeAnchors"))
        return fail("surfaceCache_timeAnchorsSlotExists", "Slot enum must include TimeAnchors.");
    const auto cpp = readText("Source/Standalone/UI/PianoRoll/PianoRollSurfaceCache.cpp");
    const auto buildFn = extractFunctionBlock(cpp, "PianoRollSurfaceCache::buildSlot");
    if (!contains(buildFn, "Slot::TimeAnchors"))
        return fail("surfaceCache_timeAnchorsSlotExists", "buildSlot must handle Slot::TimeAnchors.");
    if (!contains(buildFn, "drawTimeGridAnchors"))
        return fail("surfaceCache_timeAnchorsSlotExists", "TimeAnchors slot must call drawTimeGridAnchors.");
    return pass("surfaceCache_timeAnchorsSlotExists");
}

CheckResult surfaceCache_noChunkBoundariesInPianoRoll()
{
    // PianoRoll 源码中不得出现 ChunkBoundaries (slot enum除外)、chunkBoundaries、drawChunkBoundaries
    const auto rendererCpp = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");
    const auto rendererH = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");
    const auto compCpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto compH = readText("Source/Standalone/UI/PianoRollComponent.h");
    
    const std::vector<std::string> forbidden = {
        "chunkBoundaries", "drawChunkBoundaries", "setShowChunkBoundaries",
        "RenderChunkPlanner::buildChunkBoundariesFromSilentGaps"
    };
    for (const auto& token : forbidden) {
        if (contains(rendererCpp, token) || contains(rendererH, token) ||
            contains(compCpp, token) || contains(compH, token))
            return fail("surfaceCache_noChunkBoundariesInPianoRoll",
                        "Forbidden token '" + token + "' found in PianoRoll source.");
    }
    return pass("surfaceCache_noChunkBoundariesInPianoRoll");
}

CheckResult surfaceCache_fiveSlotEnum()
{
    const auto hdr = readText("Source/Standalone/UI/PianoRoll/PianoRollSurfaceCache.h");
    if (!contains(hdr, "Background") || !contains(hdr, "Waveform") || 
        !contains(hdr, "Notes") || !contains(hdr, "F0") || !contains(hdr, "TimeAnchors"))
        return fail("surfaceCache_fiveSlotEnum", "Slot enum must contain all 5 slots.");
    if (contains(hdr, "ChunkBoundaries"))
        return fail("surfaceCache_fiveSlotEnum", "Slot enum must NOT contain ChunkBoundaries.");
    return pass("surfaceCache_fiveSlotEnum");
}

CheckResult surfaceCache_buildSlotDoesNotConstructRenderContext()
{
    const auto cpp = readText("Source/Standalone/UI/PianoRoll/PianoRollSurfaceCache.cpp");
    const auto buildFn = extractFunctionBlock(cpp, "PianoRollSurfaceCache::buildSlot");
    if (buildFn.empty())
        return fail("surfaceCache_buildSlotDoesNotConstructRenderContext", "Cannot locate buildSlot.");
    // buildSlot must NOT construct a full RenderContext
    if (contains(buildFn, "RenderContext ctx") || contains(buildFn, "RenderContext{"))
        return fail("surfaceCache_buildSlotDoesNotConstructRenderContext",
                    "buildSlot must not construct a full RenderContext — use SurfaceRenderContext directly.");
    return pass("surfaceCache_buildSlotDoesNotConstructRenderContext");
}

CheckResult surfaceCache_drawTimeGridHandlesOnlyInteractive()
{
    const auto cpp = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");
    const auto handlesFn = extractFunctionBlock(cpp, "PianoRollRenderer::drawTimeGridHandles");
    if (handlesFn.empty())
        return fail("surfaceCache_drawTimeGridHandlesOnlyInteractive",
                    "Cannot locate drawTimeGridHandles.");
    // Must have the interactive-only guard
    if (!contains(handlesFn, "!selected && !hovered"))
        return fail("surfaceCache_drawTimeGridHandlesOnlyInteractive",
                    "drawTimeGridHandles must only draw interactive (selected/hovered) handles, skip non-interactive ones.");
    return pass("surfaceCache_drawTimeGridHandlesOnlyInteractive");
}

} // namespace

int main()
{
    const std::vector<CheckResult (*)()> tests = {
        // RenderCache 状态机回归测试（BUG3 根因）
        renderCacheRequestRenderPendingCancelsRunning,
        renderCacheMarkChunkAsBlankUsesRunningRevisionToken,
        renderCacheCompleteChunkRenderWithAudioReturnsTriState,
        // ARA 架构测试
        araObjectBoundariesMatchOfficialRoles,
        araArchiveRestoresFromAudioSourceOnly,
        araSampleAccessEnableDoesNotReadFreshContent,
        araUserReadGateCreatesLease,
        refreshPlaybackReadSourceIsPrivate,
        playbackRendererUsesCrsSnapshotNotOwnerPointer,
        araSourceContentUpdateInvalidatesOnly,
        araRendererUsesAssignedRegionsAndMixesOverlap,
        standaloneMainSignalChainIsConnected,
        standaloneRestoreKeepsWindowedClipAudio,
        nonAraVst3CaptureSignalChainIsConnected,
        mutationSinkAndRenderPipelineStayUnified,
        contentRenderServiceUsesGlobalSilentGapChunkSelection,
        noRemovedAraArchitectureVocabularyInAraSource,
        contentEditCommandsHasNoEnqueuePartialRender,
        commitNotesAndSegmentsCarriesAffectedRange,
        pianoRollEditActionPassesRangeInUndoRedo,
        editorsDoNotEnqueueRenderDirectly,
        processorLocalMutationSinkUsesRange,
        processorHasNoEnqueueContentPartialRender,
        processorHasNoInvalidateRenderFor,
        mutationSinkDoesNotIgnoreRange,
        requestRenderForLocalMutationRangeHasSingleEntryPath,
        noEmptyRangeAsFullSentinel,
        timeGridEditActionHasNoAffectedRange,
        pitchCurveEditingHasNoSourcePriorityProtection,
        dcOwnsCrsAndRejectsProcessorInjection,
        dcGetsF0FromProcessRuntime,
        processorF0ServiceHasSingleProcessOwner,
        didBindToArADoesNotAttachProcessorLease,
        renderExecutionLeaseLifecycleSafety,
        processRenderRuntimeInCMake,
        processorHasNoChunkRenderOrVocoderRuntime,
        processRenderRuntimeOwnsNoAraModels,
        vst3OverlayInitiallyHidden,
        recordRequestedNoRegionNoAlert,
        noTileCacheInSourceTree,
        // Kill List resurrection guards
        noInvalidateContentVisualResurrection,
        noRendererLambdaResurrection,
        noF0FrameToXYResurrection,
        noDrawPreparedF0CurveResurrection,
        // v12 Timeline/Camera/ViewMapper architecture contracts
        v12ViewMapperExistsWithRequiredMethods,
        v12TimelineViewportCameraHasDefaultConstant,
        v12PlayheadOverlayOnlyHasSetPresentation,
        v12NoDeletedTypesInSource,
        v12NoDeletedFunctionsInSource,
        v12PianoRollComponentHasCameraArchitecture,
        v12ArrangementViewComponentHasCameraArchitecture,
        v12RendererUsesViewMapperInRenderContext,
        v12ToolHandlerUsesViewMapper,
        v12DeletedFilesAreGone,
        v12CMakeListsHasViewMapper,
        // v12 Kill List 契约
        v12KillList_noDeadGetCameraGetter,
        v12KillList_noRenderScrollOffsetPxParam,
        v12KillList_noRulerSurfaceStateZoomLevel,
        v12KillList_fitToContentHasNoZoomTautology,
        v12KillList_maxEndNoViewportRight,
        v12KillList_maxEndNoHostPlayhead,
        v12KillList_setTimelineViewportNoUserZoomFlag,
        v12KillList_followAndPublishReceivesParam,
        v12CMakeListsHasViewMapperTestTarget,
        // F0 可见性刷新链守卫测试
        testOriginalF0WriteDoesNotTriggerFullRender,
        testPitchRevisionTrackingClosedLoop,
        testImportDoesNotPassNullCurve,
        testSelectionSyncRevisions,
        testApplyOriginalF0DoesNotBumpContentRevision,
        testNoDuplicateF0AnalysisAPI,
        // 调式映射/F0渲染唯一架构守卫
        scaleUiMappingCoversAllEightTypes,
        vst3PluginEditorDoesNotUseThreeStateScaleMapping,
        standaloneAndPluginUseSharedScaleMapping,
        f0RendererHasNoVerticalBucketDecimation,
        f0RendererUsesSingleContinuousPathHelper,
        // PianoRollSurfaceCache architecture guards
        surfaceCache_noOldBandCacheTokensInSource,
        surfaceCache_hasFiveSemanticSlots,
        surfaceCache_setTimelineViewportNoUserZoomFlag,
        surfaceCache_fullClipDomainUsesPlacementMinMax,
        surfaceCache_noViewportSizedOrOverscanTokens,
        surfaceCache_scrollOnlyDoesNotDirtySlots,
        surfaceCache_zoomDirtyAllSlots,
        surfaceCache_paintUsesFullClipOffset,
        surfaceCache_buildUsesFullClipMapper,
        surfaceCache_paintNoRendererDrawNoFallback,
        surfaceCache_forbiddenTransientStateNotCached,
        surfaceCache_timeGridHandlesStayOverlay,
        surfaceCache_verticalGeometryDirtiesYSlots,
        surfaceCache_configureGeometryDetectsDomainShift,
        surfaceCache_noVBlankCacheTestNamesRemain,
        surfaceCache_noGenericCachedSurfaceInPianoRoll,
        surfaceCache_surfaceRenderContextExists,
        surfaceCache_surfaceRenderContextExcludesTransient,
        surfaceCache_revisionRouteNotesOnlyDirtiesNotes,
        surfaceCache_revisionRouteTimeGridDirtiesAllFourContentSlots,
        surfaceCache_noChunkBoundariesInPianoRoll,
        surfaceCache_fiveSlotEnum,
        surfaceCache_buildSlotDoesNotConstructRenderContext,
        surfaceCache_drawTimeGridHandlesOnlyInteractive,
        surfaceCache_timeAnchorsSlotExists
    };

    int failed = 0;
    for (const auto test : tests) {
        CheckResult result;
        try {
            result = test();
        } catch (const std::exception& e) {
            result = fail("uncaught exception", e.what());
        }

        std::cout << (result.passed ? "[PASS] " : "[FAIL] ") << result.name << "\n";
        if (!result.passed) {
            ++failed;
            std::cout << result.detail << "\n";
        }
    }

    if (failed != 0) {
        std::cout << failed << " architecture/signal-chain checks failed.\n";
        return 1;
    }

    std::cout << "All architecture/signal-chain checks passed.\n";
    return 0;
}
