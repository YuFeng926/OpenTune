#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;
const std::filesystem::path sourceRoot{OPENTUNE_SOURCE_DIR};

std::string readSource(const char* relativePath)
{
    std::ifstream input(sourceRoot / relativePath, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

std::string functionBlock(const std::string& source, const std::string& signature)
{
    const auto signaturePos = source.find(signature);
    if (signaturePos == std::string::npos)
        return {};

    const auto bracePos = source.find('{', signaturePos);
    if (bracePos == std::string::npos)
        return {};

    int depth = 0;
    for (std::size_t index = bracePos; index < source.size(); ++index) {
        if (source[index] == '{')
            ++depth;
        else if (source[index] == '}' && --depth == 0)
            return source.substr(signaturePos, index - signaturePos + 1);
    }
    return {};
}

void expect(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "[FAIL] " << message << '\n';
}

bool contains(const std::string& text, const char* token)
{
    return text.find(token) != std::string::npos;
}

void testVisibleEntryContract()
{
    const auto source = readSource("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto foreground = functionBlock(source, "void ArrangementViewComponent::buildCompositeForeground");
    const auto mouseDown = functionBlock(source, "void ArrangementViewComponent::mouseDown");

    expect(contains(source, "paintHistoricalClipReferenceBadge"),
           "Arrangement clip paints a visible reference badge");
    expect(contains(foreground, "experimentalReferenceControlsEnabled_"),
           "Retained reference badge is gated by the experimental master switch");
    expect(contains(mouseDown, "experimentalReferenceControlsEnabled_"),
           "Reference hit target is gated by the experimental master switch");
    expect(contains(mouseDown, "referenceButtonBoundsForClip"),
           "Reference painting and hit-testing share one bounds function");
}

void testModeAndOverlayContract()
{
    const auto preferences = readSource("Source/Utils/AppPreferences.h");
    const auto modeEnum = preferences.substr(
        preferences.find("enum class ExperimentalReferenceAlignMode"), 180);
    const auto editor = readSource("Source/Standalone/PluginEditor.cpp");
    const auto refresh = functionBlock(editor, "void OpenTuneAudioProcessorEditor::refreshReferenceContext");
    const auto overlay = functionBlock(
        readSource("Source/Standalone/UI/PianoRollComponent.cpp"),
        "void PianoRollComponent::setReferenceOverlay");

    expect(contains(modeEnum, "StandardAuto = 0") && contains(modeEnum, "Game = 1"),
           "AUTO Ref exposes StandardAuto and Game modes");
    expect(!contains(modeEnum, "Off"), "AUTO Ref mode has no Off value");
    expect(contains(refresh, "ReferenceFeatureProducer::StandardAuto")
               && contains(refresh, "ReferenceFeatureProducer::Game"),
           "Both AUTO Ref producers drive ghost notes");
    expect(contains(refresh, "overlay.timeGrid = refSnapshot->timeGrid"),
           "Reference overlay receives the reference TimeGrid directly");
    expect(!contains(overlay, "timelineContentPlacements_"),
           "Reference overlay is not discarded by target-placement matching");
}

void testAnalysisQueueContract()
{
    const auto serviceHeader = readSource("Source/Services/ReferenceAnalysisService.h");
    const auto service = readSource("Source/Services/ReferenceAnalysisService.cpp");
    const auto submit = functionBlock(service, "void ReferenceAnalysisService::submitAnalysis");
    const auto jobEquality = functionBlock(
        serviceHeader, "bool operator==(const AnalysisJobKey& rhs) const noexcept");
    const auto processor = readSource("Source/PluginProcessor.cpp");
    const auto preheat = functionBlock(
        processor, "OpenTuneAudioProcessor::preheatReferenceAlignmentFeatures");
    const auto finished = functionBlock(processor, "void OpenTuneAudioProcessor::analysisFinished");

    expect(contains(submit, "*activeJob_ == jobKey"),
            "Analysis deduplicates only an identical active job");
    expect(contains(jobEquality, "contentKey == rhs.contentKey")
                && contains(jobEquality, "inputFingerprint == rhs.inputFingerprint")
                && contains(jobEquality, "producer == rhs.producer"),
            "AnalysisJobKey equality compares contentKey, inputFingerprint, and producer");
    expect(contains(submit, "pendingJobs_[key] = jobKey"),
           "Analysis queue keeps the latest pending job per content");
    expect(contains(preheat, "case OriginalF0State::NotRequested")
               && contains(preheat, "requestContentRefresh(refreshRequest)"),
           "StandardAuto preheat starts Original F0 extraction");
    expect(contains(finished, "result.producer != resolveReferenceFeatureProducer()"),
           "Stale producer results are rejected");
}

void testPitchOnlyExecutionContract()
{
    const auto alignHeader = readSource("Source/DSP/ReferenceAutoAlign.h");
    const auto alignSource = readSource("Source/DSP/ReferenceAutoAlign.cpp");
    const auto align = functionBlock(alignSource, "AlignmentPatch ReferenceAutoAlign::align");
    const auto processor = readSource("Source/PluginProcessor.cpp");
    const auto execute = functionBlock(
        processor, "OpenTuneAudioProcessor::executeReferenceAlignmentForPlacement");

    expect(!contains(alignHeader, "TimeGridIntent")
               && !contains(alignHeader, "timingChanged")
               && !contains(alignHeader, "targetTimeGridBefore"),
           "ReferenceAutoAlign has no automatic timing mutation output");
    expect(contains(align, "targetNote.pitch = referencePitch")
               && contains(align, "targetNote.pitchOffset = 0.0f"),
           "ReferenceAutoAlign changes pitch only");
    expect(!contains(align, "targetNote.startTime =")
               && !contains(align, "targetNote.endTime ="),
           "ReferenceAutoAlign preserves target note positions");
    expect(contains(execute, "derivedCurve->applyCorrectionToRange")
               && contains(execute, "std::make_unique<PianoRollEditAction>"),
           "AUTO Ref uses canonical F0 generation and one pitch undo action");
    expect(!contains(execute, "setContentTimeGrid")
               && !contains(execute, "TimeGridPatchBuilder")
               && !contains(execute, "TimeGridEditAction")
               && !contains(execute, "CompositeUndoAction"),
           "AUTO Ref execution never writes a TimeGrid");
    expect(contains(alignHeader, "sourceStartSeconds")
               && contains(alignSource, "timeMap.tau(clip.sourceStartSeconds)"),
           "Trim offsets participate in reference pitch projection");
}

void testBindingAndProjectionContract()
{
    const auto arrangementHeader = readSource("Source/StandaloneArrangement.h");
    const auto arrangement = readSource("Source/StandaloneArrangement.cpp");
    const auto setBinding = functionBlock(
        arrangement, "bool StandaloneArrangement::setPlacementReferencePlacement");
    const auto move = functionBlock(arrangement, "bool StandaloneArrangement::movePlacementToTrack");
    const auto trim = functionBlock(arrangement, "bool StandaloneArrangement::setPlacementTrim");
    const auto projection = readSource("Source/Utils/ContentTimelineProjection.h");

    expect(!contains(arrangementHeader, "referenceBindingRevision")
               && !contains(arrangementHeader, "bindingRevision"),
           "Reference binding has no parallel revision state");
    expect(!contains(setBinding, "publishPlaybackSnapshotLocked"),
           "Reference binding changes do not publish playback snapshots");
    expect(!contains(move, "clearReferenceBinding")
               && !contains(trim, "clearReferenceBinding"),
           "Move and trim preserve reference binding");
    expect(contains(projection, "contentStartSeconds")
               && contains(projection, "contentSeconds - contentStartSeconds"),
           "Timeline projection carries the trim start offset");
    expect(!std::filesystem::exists(sourceRoot / "Source/DSP/TimeGridPatchBuilder.cpp")
               && !std::filesystem::exists(sourceRoot / "Source/Utils/CompositeUndoAction.h"),
           "Removed automatic timing paths do not remain in the tree");
}

void testEffectiveF0Contract()
{
    const auto snapshot = readSource("Source/Content/EditableContentSnapshot.h");
    const auto pitchCurveHeader = readSource("Source/Utils/PitchCurve.h");
    const auto pitchCurveSource = readSource("Source/Utils/PitchCurve.cpp");
    const auto runtime = readSource("Source/Runtime/ProcessRenderRuntime.cpp");
    const auto processor = readSource("Source/PluginProcessor.cpp");
    const auto commitPitchShift = functionBlock(
        processor, "OpenTuneAudioProcessor::commitPitchShiftEdit");
    const auto renderer = readSource(
        "Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");
    const auto toolHandler = readSource(
        "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto payloadState = readSource("Source/Content/ContentPayloadState.h");
    const auto editableState = readSource("Source/Content/EditableContentState.h");
    const auto araState = readSource("Source/Content/ARAEditableContentState.h");
    const auto araArchive = readSource("Source/ARA/OpenTuneDocumentController.cpp");
    const auto captureArchive = readSource("Source/Plugin/Capture/CapturePersistence.cpp");

    expect(contains(snapshot, "forEachEffectiveF0Span")
               && contains(snapshot, "originalF0.data() + spanStartFrame")
               && contains(snapshot, "isCorrection ? 1.0f : gapGain"),
           "Effective F0 exposes correction spans at gain 1 and shifted OriginalF0 gaps");
    expect(!contains(pitchCurveHeader, "renderFinalF0Range")
               && !contains(pitchCurveSource, "renderFinalF0Range")
               && !contains(runtime, "renderFinalF0Range")
               && !contains(toolHandler, "renderFinalF0Range"),
           "The obsolete final-F0 materialization API is removed");
    expect(contains(pitchCurveHeader, "float sourcePitchRatio")
               && contains(pitchCurveSource, "rawF0 * sourcePitchRatio"),
           "New correction segments are generated in the current pitch-shift coordinate");
    expect(contains(runtime, "forEachEffectiveF0Span")
               && !contains(runtime, "getPitchRatio()")
               && contains(runtime,
                           "lightPitchEnabled && contentSnap->pitchShiftSettings.isIdentity()"),
           "Rendering consumes Effective F0 once and bypasses AutoTune for global shift");
    expect(contains(renderer, "ownerSnapshot->forEachEffectiveF0Span")
               && contains(toolHandler, "contentSnapshot->forEachEffectiveF0Span"),
           "PianoRoll drawing and hit-testing use the same Effective F0 as rendering");
    expect(contains(commitPitchShift, "note.originalPitch *= delta")
               && contains(commitPitchShift, "f0 *= delta")
               && !contains(commitPitchShift, "setOriginalF0"),
           "Global pitch shift rebases notes and segments without mutating OriginalF0");
    expect(!contains(payloadState, "correctionSegments")
               && !contains(editableState, "correctionSegments")
               && !contains(araState, "correctionSegments")
               && !contains(snapshot, "std::vector<PitchCorrectionSegment> correctionSegments"),
           "PitchCurve is the only runtime correction state");
    expect(contains(araArchive, "f0Base64")
               && contains(araArchive, "replaceCorrectionSegments(restoredCorrectionSegments)")
               && contains(captureArchive, "snap->notes")
               && contains(captureArchive, "snap->pitchShiftSettings"),
           "ARA and Capture persist complete pitch-shift state");
}

void testOpenDyneContract()
{
    const auto noteHeader = readSource("Source/Utils/Note.h");
    const auto araArchive = readSource("Source/ARA/OpenTuneDocumentController.cpp");
    const auto projectPersistence = readSource("Source/Utils/ProjectPersistence.cpp");
    const auto capturePersistence = readSource("Source/Plugin/Capture/CapturePersistence.cpp");
    const auto toolHandler = readSource("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto toolHandlerHeader = readSource("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h");
    const auto processor = readSource("Source/PluginProcessor.cpp");
    const auto commands = readSource("Source/Content/ContentEditCommands.h");
    const auto reader = readSource("Source/Utils/PlaybackAudioReader.h");
    const auto keyShortcut = readSource("Source/Utils/KeyShortcutConfig.h");
    const auto pianoRollHeader = readSource("Source/Standalone/UI/PianoRollComponent.h");
    const auto rendererHeader = readSource("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");

    // 旧 velocity 字段与持久化键一次性删除，无兼容层
    expect(!contains(noteHeader, "velocity")
               && !contains(araArchive, "\"velocity\"")
               && !contains(projectPersistence, "\"velocity\"")
               && !contains(capturePersistence, "velocity"),
           "Note::velocity and legacy velocity persistence keys are removed");

    // 音高拖拽无裸 round(targetMidi) 吸附；统一走 quantizeMidiToActiveScale
    expect(!contains(toolHandler, "std::round(targetMidi)"),
           "Pitch drag uses the single quantizeMidiToActiveScale entry");
    expect(contains(toolHandler, "quantizeMidiToActiveScale"),
           "Pitch drag calls quantizeMidiToActiveScale");

    // topology command 不触发 render mutation completion
    expect(!contains(processor, "commitContentNoteTopologyPatch")
               || !contains(functionBlock(processor, "OpenTuneAudioProcessor::commitContentNoteTopologyPatch"),
                            "onContentLocalMutationCompleted"),
           "Topology command never triggers render mutation completion");

    // gain command 只走 republishPlaybackSource，不 enqueue render
    const auto gainPatch = functionBlock(processor, "OpenTuneAudioProcessor::commitNoteOutputGainPatch");
    const auto envelopePatch = functionBlock(processor, "OpenTuneAudioProcessor::commitSibilantGainEnvelope");
    expect(!contains(gainPatch, "enqueueRender")
               && !contains(gainPatch, "onContentLocalMutationCompleted")
               && contains(gainPatch, "republishPlaybackSource"),
           "A-layer command republishes without render enqueue");
    expect(!contains(envelopePatch, "enqueueRender")
               && !contains(envelopePatch, "onContentLocalMutationCompleted")
               && contains(envelopePatch, "republishPlaybackSource"),
           "B-layer command republishes without render enqueue");
    expect(contains(commands, "republishPlaybackSource"),
           "ContentEditCommands exposes the no-render republish entry");

    // readPlaybackAudio 的 TimeStretch 与普通路径汇合到同一 gain apply；canonical 读取无包络
    const auto readPlayback = functionBlock(reader, "inline int readPlaybackAudio");
    const auto readCanonical = functionBlock(reader, "inline int readCanonicalAudio");
    expect(contains(readPlayback, "applyPreparedOutputGain")
               && !contains(readCanonical, "applyPreparedOutputGain"),
           "Playback gain applies once after both read paths; canonical read stays ungained");

    // OpenDyne F 键为 scheme 固定映射，不写入 KeyShortcutConfig / 无新 ShortcutId
    expect(contains(toolHandler, "F2Key") && contains(toolHandler, "F6Key")
               && !contains(keyShortcut, "Pitch")
               && !contains(keyShortcut, "Scissors")
               && !contains(keyShortcut, "VolumeEnvelope"),
           "OpenDyne F-keys are scheme-fixed, not user-configurable shortcuts");

    // 无第二波形缓存 / 独立 OpenDyne renderer 或 tool handler
    expect(!contains(pianoRollHeader, "OpenDyneRenderer")
               && !contains(pianoRollHeader, "OpenDyneToolHandler")
               && !contains(rendererHeader, "OpenDyneRenderer"),
           "No parallel OpenDyne renderer/tool-handler structures exist");

    // AUTO 提交推进 outputGainRevision（A 投影逐点变化，计划五.3）
    const auto autoCommit = functionBlock(processor, "OpenTuneAudioProcessor::commitAutoTuneGeneratedNotesByContentKey");
    expect(contains(processor, "applyNotesWithOutputGain")
               && contains(autoCommit, "applyNotesWithOutputGain")
               && !contains(autoCommit, "clip->applyNotes("),
           "AUTO note commit advances outputGainRevision, no bare applyNotes");

    // export 使用与 playback 同一 outputGain 包络
    const auto exportRender = functionBlock(processor, "void renderPlacementForExport");
    expect(contains(exportRender, "outputGainEnvelope->linearGains"),
           "Export bakes the same outputGain envelope as playback");

    // DrawNote/LineAnchor 吸附根除裸 round(midiNote)，统一 quantizeMidiToActiveScale 入口
    expect(!contains(toolHandler, "std::round(midiNote)"),
           "DrawNote/LineAnchor snap through the single quantizeMidiToActiveScale entry");
}

void testKillListContract()
{
    const auto owner = readSource("Source/Content/DomainContentOwner.h");
    const auto standaloneOwner = readSource("Source/Content/StandaloneClipContent.cpp");
    const auto captureOwner = readSource("Source/Content/CaptureSegmentContent.cpp");
    const auto commands = readSource("Source/Content/ContentEditCommands.h");
    const auto processorHeader = readSource("Source/PluginProcessor.h");
    const auto processor = readSource("Source/PluginProcessor.cpp");
    const auto serviceHeader = readSource("Source/Services/ReferenceAnalysisService.h");
    const auto service = readSource("Source/Services/ReferenceAnalysisService.cpp");
    const auto araArchive = readSource("Source/ARA/OpenTuneDocumentController.cpp");
    const auto araHeader = readSource("Source/ARA/OpenTuneDocumentController.h");
    const auto audioModification = readSource("Source/ARA/AudioModification.h");
    const auto align = readSource("Source/DSP/ReferenceAutoAlign.cpp");
    const auto pitchShiftAction = readSource("Source/Utils/PitchShiftEditAction.cpp");
    const auto pitchShiftActionHeader = readSource("Source/Utils/PitchShiftEditAction.h");
    const auto pianoRollActionHeader = readSource("Source/Utils/PianoRollEditAction.h");
    const auto timeGridActionHeader = readSource("Source/Utils/TimeGridEditAction.h");
    const auto placementActions = readSource("Source/Utils/PlacementActions.cpp");
    const auto pianoRollHeader = readSource("Source/Standalone/UI/PianoRollComponent.h");

    expect(!contains(owner, "applyContentCommand")
               && !contains(standaloneOwner, "applyContentCommand")
               && !contains(captureOwner, "applyContentCommand"),
           "Undefined ContentCommand and its empty owner implementations are removed");
    expect(!contains(commands, "FullRenderReason")
               && !contains(processorHeader, "MutationScope")
               && !contains(processor, "MutationScope::")
               && !contains(processor, "FullRenderReason::"),
           "Unused mutation classification layers are removed");
    expect(!contains(serviceHeader, "cancelledActiveJobs_")
               && !contains(serviceHeader, "cancelAll")
               && !contains(service, "cancelledActiveJobs_")
               && !contains(service, "if (result.status == ReferenceFeatureStatus::Ready)")
               && contains(service, "memory_order_release"),
           "Analysis shutdown has one lifecycle path and no empty success branch");
    expect(!contains(processor, "normalizeStoredNotes(patch.notesAfter)")
               && contains(processor, "const auto& normalizedNotes = patch.notesAfter")
               && contains(processor, "pendingTimeToolSeedKeys_.count(segContentKey)"),
           "Reference commit avoids duplicate normalization and capture F0 retries pending seeds");
    expect(contains(araArchive, "semitone < -24")
               && contains(araArchive, "cents < -99"),
           "ARA pitch-shift validation matches the editor range");
    expect(contains(processor, "OpenTuneAudioProcessor& proc_")
               && !contains(processor, "if (!proc_)"),
           "Content command adapter has no impossible null-owner guards");
    expect(!contains(pianoRollHeader, "bool hitTest(int x, int y) override"),
           "PianoRoll overlay relies on its configured mouse interception policy");
    expect(!contains(audioModification, "AudioModificationReadIntent")
               && !contains(araHeader, "continuePendingUserReadForSource")
               && !contains(araArchive, "RenderChunkPlanner.h"),
           "ARA read-intent remnants and unused render-planner dependency are removed");
    expect(!contains(align, "normalizeStoredNotes(seedNotes)")
               && contains(align, "auto& notesAfter = patch.notesAfter")
               && !contains(align, "bool isReady("),
           "Reference alignment edits the canonical note copy without re-normalizing it");
    expect(!contains(pitchShiftAction, "commands_ != nullptr")
               && contains(pitchShiftAction, "commands_->applyPitchShiftState"),
           "Pitch-shift undo uses its mandatory command owner without fallback guards");
    expect(!contains(pitchShiftActionHeader, "getContentKey()")
               && !contains(pianoRollActionHeader, "getContentKey()")
               && !contains(pianoRollActionHeader, "getAffectedStartFrame()")
               && !contains(timeGridActionHeader, "getContentKey()"),
           "Unused undo-action inspection accessors are removed");
    expect(!contains(placementActions, "if (arrangement != nullptr)"),
            "Standalone placement actions do not guard their mandatory arrangement owner");
}

void testEffectiveF0SourceContract()
{
    const auto pitchCurveHeader = readSource("Source/Utils/PitchCurve.h");
    const auto runtime = readSource("Source/Runtime/ProcessRenderRuntime.cpp");
    const auto materializer = functionBlock(
        runtime, "std::vector<float> materializeEffectiveF0Range");
    const auto renderJob = functionBlock(
        runtime, "void ProcessRenderRuntime::processChunkRenderJob");
    const auto gapFiller = functionBlock(runtime, "void fillF0GapsForVocoder");

    expect(contains(pitchCurveHeader, "hasOriginalF0Data"),
            "PitchCurve exposes hasOriginalF0Data for validity checks");
    expect(!contains(pitchCurveHeader, "hasFinalF0Data")
                && !contains(runtime, "hasFinalF0Data"),
            "The obsolete hasFinalF0Data gate is removed from PitchCurve and Runtime");

    expect(contains(materializer, "forEachEffectiveF0Span"),
            "Runtime has one file-local Effective F0 materialization entry point");
    expect(contains(runtime, "effectiveF0"),
            "Runtime carries the materialized effective F0 buffer");
    expect(contains(runtime, "vocoderF0"),
            "Runtime carries the vocoder-domain F0 buffer");
    expect(!contains(runtime, "sourceF0"),
            "The obsolete sourceF0 naming is removed from Runtime");
    expect(!contains(runtime, "correctedF0"),
            "The obsolete correctedF0 naming is removed from Runtime");

    expect(contains(renderJob, "materializeEffectiveF0Range"),
            "Runtime main render path calls the materialization function");
    expect(contains(gapFiller, "materializeEffectiveF0Range"),
            "Runtime lookback and lookahead gap-filling call the materialization function");
    expect(renderJob.find("materializeEffectiveF0Range") != std::string::npos
                && renderJob.find("materializeEffectiveF0Range",
                    renderJob.find("materializeEffectiveF0Range") + 1) == std::string::npos,
            "Runtime main path invokes materialization exactly once");
    const auto firstGapCall = gapFiller.find("materializeEffectiveF0Range");
    const auto secondGapCall = gapFiller.find("materializeEffectiveF0Range", firstGapCall + 1);
    expect(firstGapCall != std::string::npos
                && secondGapCall != std::string::npos
                && gapFiller.find("materializeEffectiveF0Range", secondGapCall + 1) == std::string::npos
                && !contains(renderJob, "forEachEffectiveF0Span")
                && !contains(gapFiller, "forEachEffectiveF0Span"),
            "Main, lookback, and lookahead paths share the single materializer");
}

void testTimeGridStage2Contract()
{
    const auto processor = readSource("Source/PluginProcessor.cpp");
    const auto setTimeGrid = functionBlock(
        processor, "bool OpenTuneAudioProcessor::setContentTimeGrid");
    const auto replaceNotes = functionBlock(
        processor, "bool OpenTuneAudioProcessor::replaceContentNotesForFullMutation");
    const auto timeGridHeader = readSource("Source/Utils/TimeGrid.h");

    expect(!contains(setTimeGrid, "onContentFullMutationCompleted")
                && !contains(setTimeGrid, "requestFullContentRender")
                && !contains(setTimeGrid, "requestRenderForLocalMutationRange"),
            "TimeGrid edits do not re-run Stage1 rendering");
    expect(contains(setTimeGrid, "getTimeStretchCache().invalidate(key)")
                && contains(setTimeGrid, "enqueueStandaloneStage2WhenCanonicalSettled(key)"),
            "TimeGrid edits invalidate Stage2 output and use the canonical-settled gate");
    expect(!contains(replaceNotes, "getTimeStretchCache().invalidate")
                && !contains(replaceNotes, "enqueueStandaloneStage2WhenCanonicalSettled"),
            "Notes-only mutations do not trigger Stage2 rebuilding");
    expect(!contains(timeGridHeader, "bool locked"),
            "TimeHandle has no stored lock state");
}

void testEditorStateProjectionContract()
{
    const auto standaloneEditor = readSource("Source/Standalone/PluginEditor.cpp");
    const auto standaloneTimer = functionBlock(standaloneEditor, "void OpenTuneAudioProcessorEditor::timerCallback()");

    expect(contains(standaloneTimer, "lastPianoRollOriginalF0State_ != OriginalF0State::Ready"),
           "Standalone timerCallback gates initial F0 view on previous state not Ready");
    expect(contains(standaloneTimer, "currentOriginalF0State == OriginalF0State::Ready"),
           "Standalone timerCallback fires initial F0 view only when current state is Ready");
    expect(contains(standaloneTimer, "pianoRoll_.requestInitialF0View(activeKey)"),
           "Standalone timerCallback delegates initial F0 view to piano roll");
    expect(!contains(standaloneTimer, "lastPianoRollOriginalF0State_ == OriginalF0State::Extracting"),
           "Standalone timerCallback has no legacy Extracting gate");

    const auto pianoRoll = readSource("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto requestF0 = functionBlock(pianoRoll, "void PianoRollComponent::requestInitialF0View");
    const auto consumeF0 = functionBlock(pianoRoll, "bool PianoRollComponent::tryConsumeInitialF0View");

    expect(contains(requestF0, "pendingInitialF0ViewRequests_.insert"),
           "requestInitialF0View inserts into the pending set");
    expect(contains(consumeF0, "pendingInitialF0ViewRequests_.erase"),
           "tryConsumeInitialF0View erases on the success path");

    const auto pluginEditor = readSource("Source/Plugin/PluginEditor.cpp");
    const auto pluginTimer = functionBlock(pluginEditor, "void OpenTuneAudioProcessorEditor::timerCallback()");

    expect(contains(standaloneTimer, "pitchShiftSettings")
                && contains(standaloneTimer, "lastPitchShiftIndicatorSettings_")
                && contains(standaloneTimer, "setPitchShiftIndicator"),
           "Standalone timerCallback projects pitch shift from snapshot to indicator");
    expect(contains(pluginTimer, "pitchShiftSettings")
                && contains(pluginTimer, "lastPitchShiftIndicatorSettings_")
                && contains(pluginTimer, "setPitchShiftIndicator"),
           "Plugin timerCallback projects pitch shift from snapshot to indicator");

    const auto standalonePitchShift = functionBlock(standaloneEditor, "void OpenTuneAudioProcessorEditor::pitchShiftRequested()");
    const auto pluginPitchShift = functionBlock(pluginEditor, "void OpenTuneAudioProcessorEditor::pitchShiftRequested()");

    expect(!contains(standalonePitchShift, "setPitchShiftIndicator"),
           "Standalone pitchShiftRequested does not drive the indicator directly");
    expect(!contains(pluginPitchShift, "setPitchShiftIndicator"),
           "Plugin pitchShiftRequested does not drive the indicator directly");
}

void testChunkBlankCorrectionContract()
{
    const auto runtime = readSource("Source/Runtime/ProcessRenderRuntime.cpp");
    const auto processChunk = functionBlock(
        runtime, "void ProcessRenderRuntime::processChunkRenderJob");

    expect(contains(processChunk, "hasCorrectionInRange"),
           "Chunk render blanks when the chunk frame range has no correction segment");
    expect(contains(processChunk, "markChunkAsBlank"),
           "Chunk render still uses the Blank fallback path");
    expect(contains(processChunk, "hasOriginalF0Data"),
           "Chunk render keeps the original-F0-missing Blank guard");
    expect(processChunk.find("hasCorrectionInRange") < processChunk.find("materializeEffectiveF0Range"),
           "Correction-range blank guard must precede materializeEffectiveF0Range");
    expect(contains(processChunk, "isIdentity() && !snap->hasCorrectionInRange"),
           "Blank requires identity global pitch shift with no correction segment; "
           "a non-identity global shift without corrections must still reach the vocoder");
}

void testRenderWorkerPauseContract()
{
    const auto source = readSource("Source/Render/RenderWorker.cpp");
    const auto pause = functionBlock(source, "void RenderWorker::pause()");
    const auto drain = functionBlock(source, "void RenderWorker::drain()");

    expect(contains(pause, "paused_ = true") && contains(pause, "inFlight_ == 0"),
           "pause stops new submissions and waits only for in-flight sync callbacks");
    expect(!contains(pause, "asyncInFlight_"),
           "pause never waits for async vocoder inference");
    expect(contains(drain, "queue_.empty()")
                && contains(drain, "inFlight_ == 0")
                && contains(drain, "asyncInFlight_ == 0"),
           "drain keeps the full empty-queue and zero-in-flight contract");
}

} // namespace

int main()
{
    testVisibleEntryContract();
    testModeAndOverlayContract();
    testAnalysisQueueContract();
    testPitchOnlyExecutionContract();
    testBindingAndProjectionContract();
    testEffectiveF0Contract();
    testKillListContract();
    testEffectiveF0SourceContract();
    testTimeGridStage2Contract();
    testEditorStateProjectionContract();
    testChunkBlankCorrectionContract();
    testRenderWorkerPauseContract();
    testOpenDyneContract();

    if (failures != 0) {
        std::cerr << failures << " reference contract test(s) failed\n";
        return 1;
    }

    std::cout << "Reference contracts passed\n";
    return 0;
}
