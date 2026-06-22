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

CheckResult refreshPlaybackReadSourceIsPrivate()
{
    const auto dcHeader = readText("Source/ARA/OpenTuneDocumentController.h");

    // Extract the public section (lines up to the first 'private:')
    const auto publicSectionEnd = dcHeader.find("private:");
    if (publicSectionEnd == std::string::npos)
        return fail("refreshPlaybackReadSourceIsPrivate", "Cannot locate private section marker in DocumentController.h.");

    const auto publicSection = dcHeader.substr(0, publicSectionEnd);

    // refreshPlaybackReadSource must NOT be in the public section
    if (contains(publicSection, "refreshPlaybackReadSource"))
        return fail("refreshPlaybackReadSourceIsPrivate",
                    "refreshPlaybackReadSource must be private to prevent bypassing user-intent read gate. "
                    "Only requestReadAudioForPlaybackRegions() should be the public read entry point.");

    // Verify it exists in the file (either in private section or removed entirely)
    if (!contains(dcHeader, "refreshPlaybackReadSource"))
        return fail("refreshPlaybackReadSourceIsPrivate",
                    "refreshPlaybackReadSource declaration is missing from DocumentController.h. "
                    "If intentionally removed, update this test accordingly.");

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

// ── Phase 6: VBlank paint contract tests ────────────────────────────────────

CheckResult paintDoesNotRequestCoverage()
{
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    // Extract the paint() method body
    const auto paint = extractFunctionBlock(cpp, "void PianoRollComponent::paint(juce::Graphics& g)");
    if (paint.empty())
        return fail("paintDoesNotRequestCoverage",
                     "Cannot find PianoRollComponent::paint()");

    if (contains(paint, "requestCoverage"))
        return fail("paintDoesNotRequestCoverage",
                     "paint() must not call requestCoverage(). "
                     "Tile generation must happen outside paint (e.g. flushPendingVisualInvalidation).");

    return pass("paintDoesNotRequestCoverage");
}

CheckResult paintDoesNotBuildSnapshot()
{
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    const auto paint = extractFunctionBlock(cpp, "void PianoRollComponent::paint(juce::Graphics& g)");
    if (paint.empty())
        return fail("paintDoesNotBuildSnapshot",
                     "Cannot find PianoRollComponent::paint()");

    if (contains(paint, "buildRenderSnapshot"))
        return fail("paintDoesNotBuildSnapshot",
                     "paint() must not call buildRenderSnapshot(). "
                     "Snapshot construction is work; paint must only consume pre-built data.");

    return pass("paintDoesNotBuildSnapshot");
}

CheckResult paintOnlyDrawsVisibleTilesForDetailLayer()
{
    const auto cpp = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    const auto paint = extractFunctionBlock(cpp, "void PianoRollComponent::paint(juce::Graphics& g)");
    if (paint.empty())
        return fail("paintOnlyDrawsVisibleTilesForDetailLayer",
                     "Cannot find PianoRollComponent::paint()");

    // paint must call drawVisibleTiles (consume published tiles)
    if (!contains(paint, "drawVisibleTiles"))
        return fail("paintOnlyDrawsVisibleTilesForDetailLayer",
                     "paint() must call drawVisibleTiles() to consume published tiles.");

    // paint must NOT call any tile generation method
    if (contains(paint, "renderTile"))
        return fail("paintOnlyDrawsVisibleTilesForDetailLayer",
                     "paint() must not call renderTile() — tile rendering is the worker's job.");

    return pass("paintOnlyDrawsVisibleTilesForDetailLayer");
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
        // Phase 6: VBlank paint contract
        paintDoesNotRequestCoverage,
        paintDoesNotBuildSnapshot,
        paintOnlyDrawsVisibleTilesForDetailLayer,
        // Kill List resurrection guards
        noInvalidateContentVisualResurrection,
        noRendererLambdaResurrection,
        noF0FrameToXYResurrection,
        noDrawPreparedF0CurveResurrection
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
