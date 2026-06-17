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
    const auto rebuild = extractFunctionBlock(dc, "bool OpenTuneDocumentController::rebuildCRSFromSource");

    if (serialize.empty() || restore.empty() || rebuild.empty())
        return fail("araArchiveRestoresFromAudioSourceOnly", "could not locate archive/restore/CRS functions.");

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

    const auto restoreMissing = missingTokens(restore, {
        "targetMod->content = restoreAudioModificationContent",
        "rebuildCRSFromSource(mod)",
        "notifyContentChanged"
    });
    if (!restoreMissing.empty())
        return fail("araArchiveRestoresFromAudioSourceOnly", "restore no longer restores state then rebuilds CRS:" + restoreMissing);

    if (contains(restore, "targetMod->content.audio")
        || contains(restore, "targetMod->content.editable.audio")
        || contains(restore, "publishPlaybackSource"))
        return fail("araArchiveRestoresFromAudioSourceOnly",
                    "ARA restore must not hydrate AudioModification content or CRS directly from archive audio.");

    const auto rebuildMissing = missingTokens(rebuild, {
        "shareReaderLease",
        "readAudioSamples",
        "sourceWindow",
        "publishPlaybackReadSourceForModification",
        "Do NOT modify modification.content"
    });
    if (!rebuildMissing.empty())
        return fail("araArchiveRestoresFromAudioSourceOnly", "CRS rebuild no longer reads from AudioSource sample access:" + rebuildMissing);

    return pass("araArchiveRestoresFromAudioSourceOnly");
}

CheckResult araSampleAccessAndSourceUpdateMaintainCRS()
{
    const auto dc = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto didEnable = extractFunctionBlock(dc, "void OpenTuneDocumentController::didEnableAudioSourceSamplesAccess");
    const auto sourceUpdate = extractFunctionBlock(dc, "void OpenTuneDocumentController::doUpdateAudioSourceContent");
    const auto remove = extractFunctionBlock(dc, "void OpenTuneDocumentController::removeCRSArtifactsForModification");
    const auto rebuild = extractFunctionBlock(dc, "int OpenTuneDocumentController::rebuildCRSForSource");

    if (didEnable.empty() || sourceUpdate.empty() || remove.empty() || rebuild.empty())
        return fail("araSampleAccessAndSourceUpdateMaintainCRS", "CRS maintenance helpers or callbacks are missing.");

    if (!contains(didEnable, "source.createReaderLease()")
        || !contains(didEnable, "rebuildCRSForSource(source)"))
        return fail("araSampleAccessAndSourceUpdateMaintainCRS",
                    "sample access enable must rebuild CRS for restored modifications.");

    if (!contains(sourceUpdate, "removeCRSArtifactsForModification(modification)")
        || !contains(sourceUpdate, "modification.invalidateDerivedContent()"))
        return fail("araSampleAccessAndSourceUpdateMaintainCRS",
                    "AudioSource content updates must drop stale CRS artifacts before invalidating derived content (preserving user edits).");

    const auto removeMissing = missingTokens(remove, {
        "removePlaybackSource",
        "removeRenderCache",
        "removeStretcher",
        "getTimeStretchCache().invalidate"
    });
    if (!removeMissing.empty())
        return fail("araSampleAccessAndSourceUpdateMaintainCRS", "CRS artifact removal is incomplete:" + removeMissing);

    if (!contains(rebuild, "source.canReadSamples()")
        || !contains(rebuild, "rebuildCRSFromSource(modification)"))
        return fail("araSampleAccessAndSourceUpdateMaintainCRS",
                    "source-level CRS rebuild must be gated by sample access and rebuild matching modifications.");

    return pass("araSampleAccessAndSourceUpdateMaintainCRS");
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
    const auto setNotes = extractFunctionBlock(processor, "bool OpenTuneAudioProcessor::setContentNotes");
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
        || setNotes.empty() || commitNotesAndSegments.empty() || setPitchCurve.empty()
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

    if (!contains(setNotes, "session->applyNotes")
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

    if (!contains(undo, "affectedStartFrame_") || !contains(undo, "affectedEndFrame_"))
        return fail("pianoRollEditActionPassesRangeInUndoRedo",
                    "PianoRollEditAction::undo() must forward affectedStartFrame_/affectedEndFrame_ "
                    "to commitNotesAndSegments.");

    if (!contains(redo, "affectedStartFrame_") || !contains(redo, "affectedEndFrame_"))
        return fail("pianoRollEditActionPassesRangeInUndoRedo",
                    "PianoRollEditAction::redo() must forward affectedStartFrame_/affectedEndFrame_ "
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

CheckResult dcExecutorCallsProcessRenderRuntime()
{
    const auto dc = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto processJob = extractFunctionBlock(dc, "OpenTuneDocumentController::processDocumentRenderJob");

    if (processJob.empty())
        return fail("dcExecutorCallsProcessRenderRuntime",
                    "could not locate OpenTuneDocumentController::processDocumentRenderJob "
                    "in OpenTuneDocumentController.cpp.");

    if (!contains(processJob, "ProcessRenderRuntime::getInstance().processChunkRenderJob"))
        return fail("dcExecutorCallsProcessRenderRuntime",
                    "OpenTuneDocumentController::processDocumentRenderJob must delegate chunk "
                    "rendering to ProcessRenderRuntime::getInstance().processChunkRenderJob; "
                    "shared inference/render execution lives at the process level, not in DC.");

    return pass("dcExecutorCallsProcessRenderRuntime");
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

} // namespace

int main()
{
    const std::vector<CheckResult (*)()> tests = {
        araObjectBoundariesMatchOfficialRoles,
        araArchiveRestoresFromAudioSourceOnly,
        araSampleAccessAndSourceUpdateMaintainCRS,
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
        dcExecutorCallsProcessRenderRuntime,
        processRenderRuntimeInCMake,
        processorHasNoChunkRenderOrVocoderRuntime,
        processRenderRuntimeOwnsNoAraModels
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
