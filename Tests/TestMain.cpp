#include <cmath>
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

std::size_t countOccurrences(const std::string& text, const char* token)
{
    std::size_t count = 0;
    for (std::size_t pos = text.find(token); pos != std::string::npos;
         pos = text.find(token, pos + 1))
        ++count;
    return count;
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

void testArrangementForegroundScaleContract()
{
    const auto cacheHeader = readSource("Source/Standalone/UI/TimelineCompositeCache.h");
    const auto cacheSource = readSource("Source/Standalone/UI/TimelineCompositeCache.cpp");
    const auto arrangementSource = readSource("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto foregroundSignature = cacheHeader.substr(
        cacheHeader.find("struct ForegroundGenerationSignature"), 320);
    const auto equality = functionBlock(
        cacheSource, "bool ForegroundGenerationSignature::operator==");
    const auto makeSignature = functionBlock(
        arrangementSource,
        "ForegroundGenerationSignature ArrangementViewComponent::makeForegroundSignature");

    expect(contains(foregroundSignature, "double pixelsPerSecond"),
           "Arrangement foreground signature tracks horizontal scale");
    expect(contains(foregroundSignature, "int trackHeight"),
           "Arrangement foreground signature tracks vertical scale (track height)");
    expect(contains(foregroundSignature, "uint64_t selectionRevision = 0;"),
           "Arrangement foreground signature carries the selection revision");
    expect(contains(equality, "pixelsPerSecond == o.pixelsPerSecond"),
           "Arrangement foreground cache invalidates when horizontal scale changes");
    expect(contains(equality, "trackHeight == o.trackHeight"),
           "Arrangement foreground cache invalidates when track height changes");
    expect(contains(equality, "selectionRevision == o.selectionRevision"),
           "Arrangement foreground cache invalidates when the selection revision changes");
    expect(contains(makeSignature, "sig.pixelsPerSecond = camera_.pixelsPerSecond"),
           "Arrangement foreground raster uses the active camera scale in its signature");
    expect(contains(makeSignature, "sig.trackHeight = processor_.getTrackHeight()"),
           "Arrangement foreground signature uses live track height");
    expect(contains(makeSignature, "sig.selectionRevision = computeSelectionRevision();"),
           "Arrangement foreground signature is injected from the live selection revision");
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

    expect(contains(submit, "*state_->activeJob == jobKey"),
            "Analysis deduplicates only an identical active job");
    expect(contains(jobEquality, "contentKey == rhs.contentKey")
                && contains(jobEquality, "inputFingerprint == rhs.inputFingerprint")
                && contains(jobEquality, "producer == rhs.producer"),
            "AnalysisJobKey equality compares contentKey, inputFingerprint, and producer");
    expect(contains(submit, "state_->pendingJobs[key] = jobKey"),
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

    // 音高拖拽按 pitchGridMode_ 三分支投影：NoSnap 自由连续 cents、Chromatic 最近半音、
    // KeyScale 统一走 quantizeMidiToActiveScale 音阶吸附（Alt 拖拽临时解除吸附）。
    const auto dragPitch = functionBlock(
        toolHandler, "void PianoRollToolHandler::dragNotePitch");
    expect(contains(dragPitch, "switch (pitchGridMode_)"),
           "Pitch drag projects by the pitchGridMode_ branch");
    expect(contains(dragPitch, "case PitchGridMode::NoSnap:")
               && contains(dragPitch, "case PitchGridMode::Chromatic:")
               && contains(dragPitch, "case PitchGridMode::KeyScale"),
           "Pitch grid branches cover NoSnap, Chromatic, and KeyScale");
    expect(contains(dragPitch, "snappedMidi = std::round(targetMidi)"),
           "Chromatic branch snaps to the nearest semitone via round");
    expect(contains(dragPitch, "quantizeMidiToActiveScale"),
           "KeyScale pitch drag snaps through the single quantizeMidiToActiveScale entry");

    // topology command 不触发 render mutation completion
    expect(!contains(processor, "commitContentNoteTopologyPatch")
               || !contains(functionBlock(processor, "OpenTuneAudioProcessor::commitContentNoteTopologyPatch"),
                            "onContentLocalMutationCompleted"),
           "Topology command never triggers render mutation completion");

    // gain 命令只走 republishPlaybackSource，不 enqueue render
    const auto envelopePatch = functionBlock(processor, "OpenTuneAudioProcessor::commitVolumeEnvelope");
    expect(!contains(envelopePatch, "enqueueRender")
               && !contains(envelopePatch, "onContentLocalMutationCompleted")
               && contains(envelopePatch, "republishPlaybackSource"),
           "Volume envelope command republishes without render enqueue");
    expect(contains(commands, "republishPlaybackSource"),
           "ContentEditCommands exposes the no-render republish entry");

    // readPlaybackAudio 的 TimeStretch 与普通路径汇合到同一 gain apply；canonical 读取无包络
    const auto readPlayback = functionBlock(reader, "inline int readPlaybackAudio");
    const auto readCanonical = functionBlock(reader, "inline int readCanonicalAudio");
    expect(contains(readPlayback, "applyAutomationGain")
               && !contains(readCanonical, "applyAutomationGain"),
           "Playback gain applies once after both read paths; canonical read stays ungained");

    // OpenDyne F 键为用户可配置快捷键，定义在 KeyShortcutConfig 中
    expect(contains(keyShortcut, "ToolODPitch") && contains(keyShortcut, "ToolODScissors"),
           "OpenDyne F-keys are user-configurable shortcuts in KeyShortcutConfig");

    // 无第二波形缓存 / 独立 OpenDyne renderer 或 tool handler
    expect(!contains(pianoRollHeader, "OpenDyneRenderer")
               && !contains(pianoRollHeader, "OpenDyneToolHandler")
               && !contains(rendererHeader, "OpenDyneRenderer"),
           "No parallel OpenDyne renderer/tool-handler structures exist");

    // 主视图波形/F0/unvoiced 与 OpenDyne 共用单一路径：无 scheme 级硬隐藏
    const auto pianoRoll = readSource("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto drawContent = functionBlock(pianoRoll, "void PianoRollComponent::drawContent");
    const auto heartbeat = functionBlock(pianoRoll, "void PianoRollComponent::onHeartbeatTick");
    const auto mipmapSource = readSource("Source/Standalone/UI/WaveformMipmap.cpp");
    const auto automationLane = readSource("Source/Utils/AutomationLane.cpp");
    const auto rendererSource = readSource("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");

    expect(!contains(pianoRoll, "drawVolumeEnvelopePreview"),
           "Volume envelope line drawing is fully removed");
    expect(contains(rendererSource, "volumeEnvelope->evalAt(sourceTime)")
               && contains(rendererSource, "gainFactor"),
           "Blob height scales with volume gain envelope");
    expect(!contains(pianoRoll, "0xFFFF8C42"),
           "No orange parameter lines remain in PianoRollComponent");
    expect(contains(toolHandler, "evalAt(note.startTime) + deltaGainDb")
               && contains(toolHandler, "handleVolumeEnvelopeToolDoubleClick")
               && !contains(toolHandler, "volumePreviewGainDb"),
           "Volume envelope interaction previews relative multi-note edits and supports reset");
    expect(contains(reader, "timeGrid->tauInverse(outputSeconds)")
               && !contains(reader, "linearGains")
               && !contains(reader, "fallbackGains"),
           "Playback evaluates the source-time AutomationLane directly at every output sample");
    expect(contains(automationLane, "gainAfter = evalAt(rampEnd)")
               && contains(automationLane, "setRegionGain")
               && contains(automationLane, "fromLegacyNoteGains")
               && contains(automationLane, "fromLegacyStepPoints")
               && contains(automationLane, "AutomationLane::sum"),
           "AutomationLane preserves both ramp boundaries and owns legacy migration");
    expect(contains(projectPersistence, "fromLegacyNoteGains")
               && contains(capturePersistence, "hasUnifiedVolumeEnvelope"),
           "Project and capture loaders migrate the old note-plus-sibilant gain model once");

    expect(!contains(drawContent, "!notesPrimary && showWaveform_")
               && !contains(drawContent, "if (!notesPrimary)"),
           "drawContent shares one waveform/unvoiced/F0 path; no scheme-level hiding");
    expect(contains(heartbeat, "if (progressed)")
               && contains(heartbeat, "rasterizeDirtySurfaces()")
               && !contains(heartbeat, "isComplete()"),
           "Heartbeat re-rasterizes on mipmap progress without waiting for full cache completion");
    expect(contains(mipmapSource, "return -1;"),
           "selectBestLevelIndex returns -1 when no complete non-empty level exists");

    // OpenDyne blob 是主音符图形：只从持久 originalEnergy + F0Timeline 构建，
    // 不依赖可选 PCM/mipmap；background drawWaveform 仍引用 mipmap（只解除主音符依赖）。
    const auto blobBuilder = functionBlock(rendererSource, "static bool buildNoteBlobPath(");
    expect(contains(blobBuilder, "timeline.rangeForTimes(note.startTime, note.endTime)")
               && contains(blobBuilder, "timeline.timeAtFrame(frame)"),
           "Blob frame range and source time derive from the F0Timeline, no hop/sampleRate math");
    expect(!contains(blobBuilder, "WaveformMipmap") && !contains(blobBuilder, "wfLevel"),
           "Blob geometry never touches mipmap peaks");
    const auto drawNotesBlock = functionBlock(rendererSource, "void PianoRollRenderer::drawNotes");
    const auto drawSelectedBlock =
        functionBlock(rendererSource, "void PianoRollRenderer::drawSelectedNoteHighlights");
    expect(contains(drawNotesBlock, "item.pitchSnapshot->getOriginalEnergy()")
               && contains(drawSelectedBlock, "item.pitchSnapshot->getOriginalEnergy()"),
           "Both blob branches consume the persistent originalEnergy");
    expect(contains(drawNotesBlock, "buildNoteBlobPath(")
               && contains(drawSelectedBlock, "buildNoteBlobPath("),
           "Both blob branches share the single buildNoteBlobPath geometry");
    expect(!contains(functionBlock(rendererHeader, "struct ContentRenderItem"), "wfLevel"),
           "ContentRenderItem carries no mipmap level fields");
    const auto buildItem = functionBlock(pianoRoll, "PianoRollComponent::buildContentRenderItem");
    expect(!contains(buildItem, "wfLevel") && !contains(buildItem, "waveformMipmapCache_"),
           "buildContentRenderItem injects no OpenDyne mipmap level");
    expect(contains(rendererHeader, "const WaveformMipmap::Level& wfLevel"),
           "Background drawWaveform keeps consuming the mipmap level");

    // Note 拓扑和 AUTO 不再维护独立 A 层增益路径。
    const auto autoCommit = functionBlock(processor, "OpenTuneAudioProcessor::commitAutoTuneGeneratedNotesByContentKey");
    expect(!contains(processor, "applyNotesWithOutputGain")
               && contains(autoCommit, "clip->applyNotes("),
           "Notes and volume envelope have no parallel gain mutation path");

    // ContentEditCommands 只暴露 autoTuneContentRange，旧 commitAutoTuneGeneratedNotes 接口零残留
    expect(contains(commands, "autoTuneContentRange")
               && !contains(commands, "commitAutoTuneGeneratedNotes("),
           "ContentEditCommands exposes autoTuneContentRange without the legacy AUTO commit");

    // 普通 AUTO 生成入口唯一：LegacyNoteGenerator::generate 只存在于 helper，
    // AUTO core 通过 generateNotesFromOriginalF0 委托后 scaleSnap apply -> commit
    const auto autoTuneCore = functionBlock(processor, "OpenTuneAudioProcessor::autoTuneContentRangeByContentKey");
    expect(contains(autoTuneCore, "generateNotesFromOriginalF0")
               && !contains(autoTuneCore, "LegacyNoteGenerator::generate")
               && contains(autoTuneCore, "scaleSnap->applyToNotes")
               && contains(autoTuneCore, "commitAutoTuneGeneratedNotesByContentKey"),
           "AUTO core delegates generation to the helper, then scaleSnap apply -> commit once");

    // 手动 AUTO 走 ContentEditCommands，不直接调 processor、不内联生成
    const auto applyAutoTune = functionBlock(pianoRoll, "PianoRollComponent::applyAutoTuneToSelection");
    expect(contains(applyAutoTune, "contentCommands_->autoTuneContentRange")
               && !contains(applyAutoTune, "LegacyNoteGenerator::generate"),
           "Manual AUTO routes through ContentEditCommands without inline generation");

    // OpenDyne import 一次性整段音符生成：requestContentRefresh 完成链走 notes-only
    const auto contentRefresh = functionBlock(processor, "OpenTuneAudioProcessor::requestContentRefresh");
    expect(contains(contentRefresh, "generateNotesWholeContentOnReady")
               && contains(contentRefresh, "generateNotesOnlyByContentKey")
               && contains(contentRefresh, "onNotesGenerated")
               && !contains(contentRefresh, "autoTuneContentRangeByContentKey"),
           "Import one-shot note generation runs the notes-only path in the F0 completion chain");

    // Standalone import 只在 OpenDyne 模式设置整段音符生成
    const auto startImport = functionBlock(readSource("Source/Standalone/PluginEditor.cpp"),
                                           "void OpenTuneAudioProcessorEditor::startPendingImport");
    expect(contains(startImport, "isOpenDyne()")
               && contains(startImport, "generateNotesWholeContentOnReady"),
           "OpenDyne import sets the whole-content note-generation flag");

    // UI 与 DetectedKey 音阶吸附收敛到 ScaleUiMapping 唯一映射入口
    const auto scaleUiMapping = readSource("Source/Utils/ScaleUiMapping.h");
    expect(contains(scaleUiMapping, "makeScaleSnapConfigFromUi")
               && contains(scaleUiMapping, "makeScaleSnapConfig(const DetectedKey&"),
           "Scale snapping maps through the single ScaleUiMapping entry");

    // export 使用与 playback 同一 AutomationLane 包络（evalAt 逐样本求值）
    const auto exportRender = functionBlock(processor, "void renderPlacementForExport");
    expect(contains(exportRender, "volumeEnvelope->evalAt"),
           "Export bakes the same volume envelope as playback via evalAt");

    // OpenTune（CorrectedF0Primary）工具固定 Chromatic：DrawNote 不读活动调式；
    // LineAnchor/音符拖拽的音阶吸附被 NotesPrimary 门控，OpenTune 分支保持全半音。
    const auto drawNoteTool = functionBlock(toolHandler, "void PianoRollToolHandler::handleDrawNoteTool");
    expect(contains(drawNoteTool, "std::lround(PitchUtils::freqToMidi(targetF0))"),
           "DrawNote quantizes the raw clicked frequency to the nearest semitone");
    expect(!contains(drawNoteTool, "getActiveScaleSnap"),
           "DrawNote (OpenTune-only) stays chromatic and never reads the active scale");
    const auto lineAnchorDown = functionBlock(toolHandler, "void PianoRollToolHandler::handleLineAnchorMouseDown");
    expect(contains(lineAnchorDown, "usesNotesPrimaryScheme")
               && contains(lineAnchorDown, "quantizeMidiToActiveScale")
               && contains(lineAnchorDown, "std::lround(midiNote)"),
           "LineAnchor scale snap is gated to OpenDyne (NotesPrimary); OpenTune stays chromatic");
    expect(contains(lineAnchorDown, "openDyne && e.mods.isAltDown()"),
           "LineAnchor Alt bypass is gated to OpenDyne (NotesPrimary)");
    const auto openDyneLineAnchor = functionBlock(lineAnchorDown, "} else if (openDyne) {");
    expect(contains(openDyneLineAnchor, "quantizeMidiToActiveScale"),
           "LineAnchor scale snap lives only in the OpenDyne branch");
    const auto openTuneDragBlock = functionBlock(dragPitch, "if (!openDyne)");
    expect(contains(openTuneDragBlock, "std::round(targetMidi)")
               && !contains(openTuneDragBlock, "pitchGridMode_")
               && !contains(openTuneDragBlock, "getActiveScaleSnap"),
           "OpenTune note drag snaps to plain semitones without pitch grid or scale");

    // OpenDyne 滚轮导航固定契约：
    // Ctrl(Command)=横纵向同步缩放、Alt=纵向缩放、Shift=横向滚动、默认=纵向滚动
    const auto wheelMove = functionBlock(pianoRoll, "void PianoRollComponent::mouseWheelMove");
    const auto zoomAtMouse = functionBlock(
        pianoRoll, "void PianoRollComponent::handleOpenDyneZoomAtMouse");
    {
        // 完整 OpenDyne 分支块（含 return 收尾），不再按 return; 截断
        const auto dyneBlock = functionBlock(wheelMove, "if (isOpenDyne())");
        // Command/Alt/Shift 分支体按各自签名提取，默认块直接锁定 else 分支源码片段
        const auto commandBlock = functionBlock(dyneBlock, "if (e.mods.isCommandDown())");
        const auto altBlock = functionBlock(dyneBlock, "else if (e.mods.isAltDown())");
        const auto shiftBlock = functionBlock(dyneBlock, "else if (e.mods.isShiftDown())");
        const auto commandPos = dyneBlock.find("if (e.mods.isCommandDown())");
        const auto altPos = dyneBlock.find("else if (e.mods.isAltDown())");
        const auto shiftPos = dyneBlock.find("else if (e.mods.isShiftDown())");
        const auto defaultPos = shiftPos != std::string::npos
            ? shiftPos + shiftBlock.size() : std::string::npos;
        const auto defaultBlock = shiftPos != std::string::npos
            ? dyneBlock.substr(shiftPos + shiftBlock.size()) : std::string();

        expect(!dyneBlock.empty() && !commandBlock.empty() && !altBlock.empty()
                   && !shiftBlock.empty() && !defaultBlock.empty()
                   && commandPos < altPos && altPos < shiftPos && shiftPos < defaultPos,
               "OpenDyne wheel branch has four ordered blocks: Command, Alt, Shift, default");
        expect(contains(commandBlock, "handleOpenDyneZoomAtMouse")
                   && !contains(commandBlock, "handleVerticalZoomWheel")
                   && !contains(commandBlock, "handleOpenDyneHorizontalScrollWheel")
                   && !contains(commandBlock, "handleVerticalScrollWheel"),
               "Command branch calls only handleOpenDyneZoomAtMouse (sync zoom)");
        expect(contains(altBlock, "handleVerticalZoomWheel")
                   && !contains(altBlock, "handleOpenDyneZoomAtMouse")
                   && !contains(altBlock, "handleOpenDyneHorizontalScrollWheel")
                   && !contains(altBlock, "handleVerticalScrollWheel"),
               "Alt branch calls only handleVerticalZoomWheel");
        expect(contains(shiftBlock, "handleOpenDyneHorizontalScrollWheel")
                   && !contains(shiftBlock, "handleOpenDyneZoomAtMouse")
                   && !contains(shiftBlock, "handleVerticalZoomWheel")
                   && !contains(shiftBlock, "handleVerticalScrollWheel"),
               "Shift branch calls only handleOpenDyneHorizontalScrollWheel");
        expect(contains(defaultBlock, "handleVerticalScrollWheel")
                   && !contains(defaultBlock, "handleOpenDyneZoomAtMouse")
                   && !contains(defaultBlock, "handleVerticalZoomWheel")
                   && !contains(defaultBlock, "handleOpenDyneHorizontalScrollWheel"),
               "Default branch calls only handleVerticalScrollWheel");
        // Command 分支含 isCommandDown() 且不含 isAltDown()，证明旧复合条件已不存在
        expect(contains(commandBlock, "isCommandDown()") && !contains(commandBlock, "isAltDown()"),
               "No legacy combined Command+Alt wheel condition remains inside the OpenDyne branch");
        expect(!contains(dyneBlock, "handleOpenDyneVerticalScrollWheel"),
               "No legacy OpenDyne vertical-scroll-only wheel path remains");
        expect(contains(zoomAtMouse, "handleHorizontalZoomWheel")
                   && contains(zoomAtMouse, "handleVerticalZoomWheel"),
               "handleOpenDyneZoomAtMouse zooms the time and pitch axes together");
    }
}

void testOpenDyneRenderPreviewContract()
{
    const auto renderer = readSource("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");
    const auto drawWaveform = functionBlock(renderer, "void PianoRollRenderer::drawWaveform");
    const auto pianoRoll = readSource("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto transientOverlay = functionBlock(
        pianoRoll, "void PianoRollComponent::drawTransientOverlay");
    const auto dragPreview = functionBlock(
        pianoRoll, "void PianoRollComponent::drawNoteDragCurvePreview");

    expect(!contains(drawWaveform, "audioBuffer"),
           "drawWaveform draws the injected mipmap level without item.audioBuffer");
    // note-drag F0 preview 已收敛为 noteDrag.previewSnapshot（renderer 消费），
    // 不再在 transientOverlay 中直接绘制
    expect(!contains(transientOverlay, "drawVolumeEnvelopePreview")
               && contains(transientOverlay, "drawScissorsPreview(g)"),
           "OpenDyne transient previews stay scheme-specific; envelope line is gone");
}

void testOpenDyneToolSwitchingContract()
{
    const auto pianoRoll = readSource("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto setCurrentTool = functionBlock(
        pianoRoll, "void PianoRollComponent::setCurrentTool");
    const auto applyScheme = functionBlock(
        pianoRoll, "void PianoRollComponent::applyAudioEditingScheme");

    // 右键菜单已替换为纵向图标工具栏，旧 PopupMenu 字面量断言不再适用。
    // 改为检查工具栏构建入口存在：
    expect(contains(pianoRoll, "showToolSelectionBar"),
           "Right-click tool selection uses vertical icon toolbar");
    expect(contains(setCurrentTool, "!isOpenDyne()")
               && contains(setCurrentTool, "!experimentalFeaturesEnabled_"),
           "TimeTool gate applies only outside OpenDyne and stays behind the experimental switch");
    expect(contains(applyScheme, "currentTool_ == ToolId::LineAnchor")
               && contains(applyScheme, "setCurrentTool(ToolId::Select)")
               && !contains(applyScheme, "ToolId::HandDraw"),
           "Scheme entry falls LineAnchor back to Select; HandDraw stays unchanged");

    const auto parameterPanel = readSource("Source/Standalone/UI/ParameterPanel.cpp");
    const auto setOpenDyneMode = functionBlock(
        parameterPanel, "void ParameterPanel::setOpenDyneMode");
    const auto resized = functionBlock(parameterPanel, "void ParameterPanel::resized");
    const auto refreshText = functionBlock(
        parameterPanel, "void ParameterPanel::refreshLocalizedText");
    const auto layoutPos = resized.find("if (openDyneMode_)");

    // Select 与 AUTO 使用 addAndMakeVisible 恒可见，setOpenDyneMode 不再隐藏它们
    expect(!contains(setOpenDyneMode, "selectToolButton_->setVisible(!enabled)")
               && !contains(setOpenDyneMode, "autoTuneToolButton_->setVisible(!enabled)"),
           "Select and AUTO are never hidden by setOpenDyneMode");
    // AUTO 按钮：OpenTune 显示 AUTO，OpenDyne 显示 SNAP；构造初始 AUTO；模式切换经 refreshLocalizedText 同步
    expect(contains(parameterPanel, "setTextIcon(\"AUTO\")"),
           "ParameterPanel constructor initializes the AUTO button label as AUTO");
    const auto setPresentation = functionBlock(parameterPanel, "void ParameterPanel::setAutoButtonPresentation");
    expect(contains(setPresentation, "setTextIcon(openDyneMode_ ? \"SNAP\" : \"AUTO\")"),
           "AUTO button label resolves by scheme: SNAP in OpenDyne, AUTO in OpenTune");
    expect(contains(setOpenDyneMode, "refreshLocalizedText()"),
           "setOpenDyneMode refreshes the AUTO button label via refreshLocalizedText");
    // resized() 的 OpenDyne 分支从 if (openDyneMode_) 到函数尾之间布局 8 个工具按钮（含 AUTO）
    expect(layoutPos != std::string::npos
               && contains(resized.substr(layoutPos), "autoTuneToolButton_"),
           "OpenDyne layout includes the AUTO button");
    expect(contains(refreshText, "\\nF1"),
           "Select tooltip shows F1 in OpenDyne");

    const auto toolHandler = readSource(
        "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto keyPressed = functionBlock(
        toolHandler, "bool PianoRollToolHandler::keyPressed");
    const auto gate = keyPressed.find("if (!isOpenDyne)");
    const auto timeTool = keyPressed.find("ShortcutId::ToolTimeTool");
    expect(gate != std::string::npos
               && timeTool != std::string::npos
               && keyPressed.find("ShortcutId::ToolDrawNote") > gate
               && keyPressed.find("ShortcutId::ToolSelect") > gate
               && keyPressed.find("ShortcutId::ToolLineAnchor") > gate
               && keyPressed.find("ShortcutId::ToolHandDraw") > gate
               && keyPressed.find("ShortcutId::ToolDrawNote") < timeTool
               && keyPressed.find("ShortcutId::ToolSelect") < timeTool
               && keyPressed.find("ShortcutId::ToolLineAnchor") < timeTool
               && keyPressed.find("ShortcutId::ToolHandDraw") < timeTool,
           "Configurable tool shortcuts stay inside the !isOpenDyne gate; TimeTool remains shared");
}

void testOpenDyneNoteEdgeRetreatContract()
{
    // OpenDyne 音符边缘退让：邻居按时间序列相邻项（resizeIdx+1/resizeIdx-1）选择，
    // 不做音高匹配；右扩展推后继 startTime、左扩展提前前驱 endTime；
    // 提交 affectedRange 覆盖被推让邻居的旧/新边界。
    const auto toolHandler = readSource(
        "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto handleSelectDrag = functionBlock(
        toolHandler, "void PianoRollToolHandler::handleSelectDrag");
    const auto resizeStart = handleSelectDrag.find(
        "const int resizeIdx = static_cast<int>(ctx_.getState().noteResize.noteIndex);");
    const auto resizeEnd = handleSelectDrag.find("notes[static_cast<size_t>(resizeIdx)].dirty = true;");
    const auto resizeRegion = (resizeStart != std::string::npos && resizeEnd != std::string::npos)
        ? handleSelectDrag.substr(resizeStart, resizeEnd - resizeStart)
        : std::string();

    expect(resizeStart != std::string::npos && resizeEnd != std::string::npos,
           "Resize core block exists in handleSelectDrag");
    expect(!contains(toolHandler, "samePitchNotes") && !contains(toolHandler, "NeighborInfo"),
           "samePitchNotes neighbor collection is fully removed from the tool handler");
    expect(!contains(resizeRegion, "getAdjustedPitch"),
           "Resize retreat never matches neighbors by adjusted pitch");
    expect(contains(resizeRegion, "resizeIdx + 1"),
           "Right retreat addresses the immediate time-series successor");
    expect(contains(resizeRegion, "resizeIdx - 1"),
           "Left retreat addresses the immediate time-series predecessor");
    expect(contains(resizeRegion, "notes[static_cast<size_t>(resizeIdx + 1)].startTime = newEnd;"),
           "Right extension pushes the successor startTime");
    expect(contains(resizeRegion, "notes[static_cast<size_t>(resizeIdx - 1)].endTime = newStart;"),
           "Left extension pulls the predecessor endTime");
    expect(contains(resizeRegion, "std::max(next.startTime, next.endTime - minDuration)"),
           "Right retreat clamps the target end so even a short successor keeps minDuration");
    expect(contains(resizeRegion, "std::min(prev.endTime, prev.startTime + minDuration)"),
           "Left retreat clamps the target start so even a short predecessor keeps minDuration");

    // 退让只移动 edge 对应的唯一时间邻居（左 resizeIdx-1、右 resizeIdx+1）：
    // 提交 affectedRange 以 baselineTarget 的 startTime/endTime 与 resized 边界初始化，
    // 只折叠目标自身与该唯一邻居的 baseline/working 旧/新边界，
    // 不做全量 notes 扫描、不链式扩展。
    const auto handleSelectUp = functionBlock(
        toolHandler, "void PianoRollToolHandler::handleSelectUp");
    expect(contains(handleSelectUp, "baselineTarget.startTime != resizedNote.startTime")
               && contains(handleSelectUp, "baselineTarget.endTime != resizedNote.endTime"),
           "Resize move detection compares the baseline target against the current boundaries");
    expect(contains(handleSelectUp, "std::min(baseline[static_cast<size_t>(resizeIdx)].startTime, resizedStartTime)")
               && contains(handleSelectUp, "std::max(baseline[static_cast<size_t>(resizeIdx)].endTime, resizedEndTime)"),
           "Resize commit seeds the affected range with the target's baseline and new boundaries");
    expect(contains(handleSelectUp, "resizeIdx - 1") && contains(handleSelectUp, "resizeIdx + 1")
               && contains(handleSelectUp, "std::min(oldNeighbor.startTime, newNeighbor.startTime)")
               && contains(handleSelectUp, "std::max(oldNeighbor.endTime, newNeighbor.endTime)"),
           "Affected range folds only the edge's unique neighbor via its baseline/working old and new boundaries");
    expect(!contains(handleSelectUp, "if (!notes[i].dirty) continue;")
               && !contains(handleSelectUp, "for (size_t i = 0; i < notes.size(); ++i)"),
           "No whole-notes dirty scan contributes to the affected range");
    expect(contains(handleSelectUp, "f0tl.rangeForTimes(dirtyStartTime, dirtyEndTime)")
               && contains(handleSelectUp, "commitNoteBasedCorrection"),
           "The merged target/neighbor range feeds the F0 affected range of the resize commit");
}

void testOpenDyneScissorsMergeContract()
{
    const auto toolHandler = readSource(
        "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto merge = functionBlock(
        toolHandler, "bool PianoRollToolHandler::handleScissorsToolMerge");
    const auto cut = functionBlock(
        toolHandler, "void PianoRollToolHandler::handleScissorsToolUp");

    expect(!contains(merge, "collectSelectedNoteIndices")
               && !contains(merge, "hasSelection")
               && !contains(merge, "isSelected"),
           "Scissors separator merge is independent of the current note selection");
    expect(contains(merge, "std::abs(left.endTime - splitTime) >= tolerance")
               && contains(merge, "std::abs(beforeNotes[j].startTime - splitTime) < tolerance")
               && contains(merge, "merged.endTime = right.endTime"),
           "Scissors double-click merges the two notes adjacent to the hit separator");
    expect(contains(merge, "computeAvgF0InRange(f0tl, effectiveF0, left.startTime, right.endTime)")
               && contains(merge, "merged.pitchOffset = 0.0f"),
           "Scissors merge rebuilds one visible note over the complete joined F0 range");
    expect(contains(cut, "state.noteSelection.setSingle(lastRightIdx"),
           "Scissors cut may select only the right segment without blocking separator merge");
}

void testShortcutContract()
{
    const auto keyShortcut = readSource("Source/Utils/KeyShortcutConfig.h");
    const auto toolHandler = readSource(
        "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto toolHandlerHeader = readSource(
        "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h");
    const auto pianoRollHeader = readSource("Source/Standalone/UI/PianoRollComponent.h");
    const auto pianoRoll = readSource("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto pluginEditor = readSource("Source/Plugin/PluginEditor.cpp");
    const auto standaloneEditor = readSource("Source/Standalone/PluginEditor.cpp");
    const auto sharedPages = readSource("Source/Editor/Preferences/SharedPreferencePages.cpp");

    // 1. 默认绑定：PlayPause=Space、Stop=Enter、Delete=Delete/Backspace/'1'
    expect(contains(keyShortcut,
                    "ShortcutId::PlayPause, Loc::Keys::kPlayPause, { KeyBinding(juce::KeyPress::spaceKey, {}) }"),
           "PlayPause defaults to the Space key");
    expect(contains(keyShortcut,
                    "ShortcutId::Stop, Loc::Keys::kStop, { KeyBinding(juce::KeyPress::returnKey, {}) }"),
           "Stop defaults to the Enter key");
    const auto deleteDefaultStart = keyShortcut.find("ShortcutId::Delete");
    const auto deleteDefaultEnd = keyShortcut.find("} }", deleteDefaultStart);
    const auto deleteDefault = (deleteDefaultStart != std::string::npos
                                && deleteDefaultEnd != std::string::npos)
        ? keyShortcut.substr(deleteDefaultStart, deleteDefaultEnd - deleteDefaultStart)
        : std::string();
    expect(contains(deleteDefault, "KeyBinding(juce::KeyPress::deleteKey, {})")
               && contains(deleteDefault, "KeyBinding(juce::KeyPress::backspaceKey, {})")
               && contains(deleteDefault, "KeyBinding('1', {})"),
           "Delete defaults to Delete, Backspace, and '1'");

    // 2. ToolHandler Context 携带传输通知回调
    expect(contains(toolHandlerHeader, "std::function<void()> notifyPlayPauseToggle")
               && contains(toolHandlerHeader, "std::function<void()> notifyStopPlayback"),
           "ToolHandler Context carries the play/pause and stop notifications");

    // 3. ToolHandler keyPressed 分派 PlayPause/Stop/Delete/SelectAll/CancelSelection
    const auto keyPressed = functionBlock(
        toolHandler, "bool PianoRollToolHandler::keyPressed");
    expect(contains(keyPressed, "ShortcutId::PlayPause")
               && contains(keyPressed, "ctx_.notifyPlayPauseToggle();"),
           "ToolHandler PlayPause notifies the play/pause toggle");
    expect(contains(keyPressed, "ShortcutId::Stop")
               && contains(keyPressed, "ctx_.notifyStopPlayback();"),
           "ToolHandler Stop notifies the stop callback");
    expect(contains(keyPressed, "ShortcutId::Delete")
               && contains(keyPressed, "handleDeleteKey();")
               && contains(keyPressed, "handleTimeToolDeleteSelected();"),
           "ToolHandler Delete runs the shared delete command; Time tool consumes it separately");
    expect(contains(keyPressed, "ShortcutId::SelectAll")
               && contains(keyPressed, "selectAllNotes(notes)"),
           "ToolHandler SelectAll selects all notes");
    expect(contains(keyPressed, "ShortcutId::CancelSelection")
               && contains(keyPressed, "ctx_.notifyEscapeKey();"),
           "ToolHandler CancelSelection notifies the escape key path");

    // 4. PianoRoll Listener 两个传输回调 + buildToolHandlerContext 注入
    expect(contains(pianoRollHeader, "virtual void playPauseToggleRequested() = 0;")
               && contains(pianoRollHeader, "virtual void stopPlaybackRequested() = 0;"),
           "PianoRoll Listener declares the two transport callbacks");
    const auto buildContext = functionBlock(
        pianoRoll, "PianoRollToolHandler::Context PianoRollComponent::buildToolHandlerContext");
    expect(contains(buildContext, "toolCtx.notifyPlayPauseToggle")
               && contains(buildContext, "toolCtx.notifyStopPlayback"),
           "buildToolHandlerContext wires both transport notifications");

    // 5. VST3 handleEditorShortcut 调用统一命令函数，无内联 isPlaying
    const auto handleShortcut = functionBlock(
        pluginEditor, "bool OpenTuneAudioProcessorEditor::handleEditorShortcut");
    expect(contains(handleShortcut, "playPauseToggleRequested();")
               && contains(handleShortcut, "stopPlaybackRequested();"),
           "VST3 handleEditorShortcut dispatches the shared transport commands");
    expect(!contains(handleShortcut, "isPlaying")
               && !contains(handleShortcut, "processorRef_.play()")
               && !contains(handleShortcut, "processorRef_.stop()"),
           "VST3 handleEditorShortcut keeps no inline transport logic");
    expect(contains(functionBlock(pluginEditor,
                                  "void OpenTuneAudioProcessorEditor::playPauseToggleRequested"),
                    "isPlaying"),
           "The isPlaying state check lives only in the shared playPauseToggleRequested command");

    // 6. VST3 setShortcutSettings 注入共享快捷键设置
    expect(contains(pluginEditor, "pianoRoll_.setShortcutSettings(sharedPreferences.shortcuts)"),
           "VST3 sync pushes the shared shortcut settings into the piano roll");

    // 7. Standalone Stop 调用统一函数
    expect(contains(standaloneEditor, "stopPlaybackRequested();")
               && contains(functionBlock(standaloneEditor,
                                         "void OpenTuneAudioProcessorEditor::stopPlaybackRequested"),
                           "stopRequested();"),
           "Standalone Stop dispatches the shared stopPlaybackRequested command");

    // 8. SharedPreferencePages General 分组包含 Delete
    const auto generalStart = sharedPages.find("generalIds_ = {");
    const auto generalEnd = sharedPages.find("};", generalStart);
    const auto generalIds = (generalStart != std::string::npos
                             && generalEnd != std::string::npos)
        ? sharedPages.substr(generalStart, generalEnd - generalStart)
        : std::string();
    expect(contains(generalIds, "KeyShortcutConfig::ShortcutId::Delete,"),
           "Shortcut preference General group includes Delete");
    expect(contains(sharedPages, "makeSectionHeader(\"General\")"),
           "Shortcut preference page keeps the General section header");
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

void testPitchModulationDriftContract()
{
    const auto toolHandler = readSource(
        "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto keyPressed = functionBlock(
        toolHandler, "bool PianoRollToolHandler::keyPressed");

    // F2 cycling: F2×1=Pitch, F2×2=PitchModulation, F2×3=PitchDrift
    expect(contains(keyPressed, "f2PressCount_")
               && contains(keyPressed, "kF2DoubleClickMs"),
           "F2 cycling uses press count and time window");
    expect(contains(keyPressed, "PitchModulation")
               && contains(keyPressed, "PitchDrift"),
           "F2 cycling dispatches PitchModulation and PitchDrift");

    // PitchModulation/PitchDrift share mouse dispatch with Pitch
    const auto mouseDown = functionBlock(
        toolHandler, "void PianoRollToolHandler::mouseDown");
    expect(contains(mouseDown, "case ToolId::PitchModulation:")
               || (contains(mouseDown, "ToolId::PitchModulation")
                   && contains(mouseDown, "ToolId::PitchDrift")),
           "mouseDown switch includes PitchModulation and PitchDrift");

    // PitchDriftScale field on Note with default 1.0
    const auto noteHeader = readSource("Source/Utils/Note.h");
    expect(contains(noteHeader, "float pitchDriftScale = 1.0f"),
           "Note has pitchDriftScale field with default 1.0");

    // ToolId enum has PitchModulation=9 and PitchDrift=10
    const auto toolIds = readSource("Source/Standalone/UI/ToolIds.h");
    expect(contains(toolIds, "PitchModulation = 9")
               && contains(toolIds, "PitchDrift = 10"),
           "ToolId enum has PitchModulation=9 and PitchDrift=10");

    // PitchCurve::applyCorrectionToRange accepts pitchDriftScale
    const auto pitchCurve = readSource("Source/Utils/PitchCurve.h");
    expect(contains(pitchCurve, "float pitchDriftScale = 1.0f"),
           "PitchCurve applyCorrectionToRange accepts pitchDriftScale parameter");

    // OpenDyne hides Original F0
    const auto renderer = readSource(
        "Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");
    expect(contains(renderer, "notesPrimaryScheme")
               && contains(renderer, "showOriginalF0"),
           "Renderer skips Original F0 in OpenDyne mode");

    // PitchDriftScale persisted in ProjectPersistence
    const auto persistence = readSource("Source/Utils/ProjectPersistence.cpp");
    expect(contains(persistence, "\"pitchDriftScale\""),
           "ProjectPersistence serializes pitchDriftScale");

    // Migration mapping includes PitchModulation/PitchDrift
    const auto pianoRoll = readSource("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto applyScheme = functionBlock(
        pianoRoll, "void PianoRollComponent::applyAudioEditingScheme");
    expect(contains(applyScheme, "PitchModulation")
               && contains(applyScheme, "PitchDrift"),
           "Scheme migration includes PitchModulation and PitchDrift");

    // Mod/Drift 拖拽统一走 pitch drag 路径：mouseDown 的 Modulation/Drift
    // 分支以 beginNotePitchDrag(clickedNoteIndex, notes) 启动选中音符的拖拽。
    const auto handlePitchDown = functionBlock(
        toolHandler, "void PianoRollToolHandler::handlePitchToolMouseDown");
    const auto modDriftDownStart = handlePitchDown.find(
        "ToolId::PitchModulation || currentTool_ == ToolId::PitchDrift");
    const auto modDriftDownEnd = handlePitchDown.find("return;", modDriftDownStart);
    const auto modDriftDownRegion = (modDriftDownStart != std::string::npos
                                     && modDriftDownEnd != std::string::npos)
        ? handlePitchDown.substr(modDriftDownStart, modDriftDownEnd - modDriftDownStart)
        : std::string();
    expect(modDriftDownStart != std::string::npos && modDriftDownEnd != std::string::npos,
           "handlePitchToolMouseDown keeps a Modulation/Drift branch");
    expect(contains(modDriftDownRegion, "beginNotePitchDrag(clickedNoteIndex, notes)"),
           "Modulation/Drift mouseDown starts the shared pitch drag via beginNotePitchDrag");

    // Mod/Drift 提交路径：endNotePitchDrag 的 Modulation/Drift 分支先对最终
    // 拖拽采样（dragNotePitch），再走曲线提交（commitNoteBasedCorrection），
    // 不再用 note-only 的 commitNoteDraft；提交后清理 isModDriftDragging 与
    // noteDrag，采样必须先于曲线提交。
    const auto endDrag = functionBlock(
        toolHandler, "bool PianoRollToolHandler::endNotePitchDrag");
    const auto modDriftUpStart = endDrag.find(
        "ToolId::PitchModulation || currentTool_ == ToolId::PitchDrift");
    const auto modDriftUpFirstReturn = endDrag.find("return true;", modDriftUpStart);
    const auto modDriftUpEnd = endDrag.find("return true;", modDriftUpFirstReturn + 1);
    const auto modDriftUpRegion = (modDriftUpStart != std::string::npos
                                   && modDriftUpEnd != std::string::npos)
        ? endDrag.substr(modDriftUpStart, modDriftUpEnd - modDriftUpStart)
        : std::string();
    expect(modDriftUpStart != std::string::npos && modDriftUpEnd != std::string::npos,
           "endNotePitchDrag keeps a Modulation/Drift branch ending in return true");
    expect(countOccurrences(modDriftUpRegion, "dragNotePitch(e);") == 1
               && countOccurrences(modDriftUpRegion,
                                   "commitNoteBasedCorrection(ctx_, notes, pitchCurve, editRange);") == 1,
           "Modulation/Drift mouseUp samples the final drag once then commits the pitch curve exactly once");
    expect(!contains(modDriftUpRegion, "commitNoteDraft"),
           "Modulation/Drift mouseUp no longer commits via the note-only commitNoteDraft");
    const auto finalDragPos = modDriftUpRegion.find("dragNotePitch(e);");
    const auto curveCommitPos =
        modDriftUpRegion.find("commitNoteBasedCorrection(ctx_, notes, pitchCurve, editRange);");
    expect(finalDragPos != std::string::npos && curveCommitPos != std::string::npos
               && finalDragPos < curveCommitPos,
           "Modulation/Drift commit order: final drag sampling precedes the curve commit");
    expect(contains(modDriftUpRegion, "isModDriftDragging = false")
               && contains(modDriftUpRegion, "noteDrag.clear()"),
           "Modulation/Drift mouseUp clears isModDriftDragging and noteDrag");
}

void testAutoSnapRefactorContract()
{
    const auto processor = readSource("Source/PluginProcessor.cpp");
    const auto processorHeader = readSource("Source/PluginProcessor.h");
    const auto commands = readSource("Source/Content/ContentEditCommands.h");
    const auto pianoRoll = readSource("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto pianoRollHeader = readSource("Source/Standalone/UI/PianoRollComponent.h");
    const auto standaloneEditor = readSource("Source/Standalone/PluginEditor.cpp");
    const auto pluginEditorHeader = readSource("Source/Plugin/PluginEditor.h");
    const auto pluginEditor = readSource("Source/Plugin/PluginEditor.cpp");
    const auto f0Extraction = readSource("Source/Services/ImportedClipF0Extraction.h");

    // 1. ContentEditCommands 暴露仅生成音符唯一入口
    expect(contains(commands, "generateNotesOnly(ContentKey key, const NoteGeneratorParams& params)"),
           "ContentEditCommands exposes the notes-only generation interface");

    // 2. 生成收敛进 helper：一次 generate + validate；AUTO core 只调 helper
    const auto helper = functionBlock(processor, "OpenTuneAudioProcessor::generateNotesFromOriginalF0");
    const auto autoTuneCore = functionBlock(processor, "OpenTuneAudioProcessor::autoTuneContentRangeByContentKey");
    expect(countOccurrences(helper, "LegacyNoteGenerator::generate") == 1
               && contains(helper, "LegacyNoteGenerator::validate"),
           "Generate helper performs exactly one generator call plus validation");
    expect(contains(autoTuneCore, "generateNotesFromOriginalF0")
               && !contains(autoTuneCore, "LegacyNoteGenerator::generate"),
           "AUTO core delegates generation to the shared helper without direct generator calls");

    // 3. 导入完成链走 notes-only，禁止 AUTO core
    const auto contentRefresh = functionBlock(processor, "OpenTuneAudioProcessor::requestContentRefresh");
    expect(contains(contentRefresh, "generateNotesWholeContentOnReady")
               && contains(contentRefresh, "generateNotesOnlyByContentKey")
               && contains(contentRefresh, "onNotesGenerated")
               && !contains(contentRefresh, "autoTuneContentRangeByContentKey"),
           "Import one-shot note generation runs the notes-only path in the F0 completion chain");

    // 4. notes-only 路径契约：拓扑提交、无 correction/mutation、无冗余 drift 赋值
    const auto notesOnly = functionBlock(processor, "OpenTuneAudioProcessor::generateNotesOnlyByContentKey");
    expect(contains(notesOnly, "commitContentNoteTopologyPatch")
               && !contains(notesOnly, "applyCorrectionToRange")
               && !contains(notesOnly, "onContentLocalMutationCompleted"),
           "Notes-only path commits topology without corrections or render mutation");
    const auto autoCommit = functionBlock(processor, "OpenTuneAudioProcessor::commitAutoTuneGeneratedNotesByContentKey");
    expect(!contains(autoCommit, "pitchDriftScale = 1.0f"),
           "AUTO commit core leaves pitchDriftScale to the generator");
    const auto applyAutoTune = functionBlock(pianoRoll, "PianoRollComponent::applyAutoTuneToSelection");
    // Verify dispatch ordering by scanning the source area after the function signature.
    // "getSelectedNotesFrameRange" also appears in a comment; search for the actual call
    // pattern "getSelectedNotesFrameRange(" to skip the comment.
    const auto fnDefPos = pianoRoll.find("PianoRollComponent::applyAutoTuneToSelection()");
    if (fnDefPos != std::string::npos) {
        const auto snapDispatch = pianoRoll.find("applyAutoSnapToAllNotes", fnDefPos);
        const auto rangeParse = pianoRoll.find("getSelectedNotesFrameRange(", fnDefPos);
        expect(snapDispatch != std::string::npos
                   && rangeParse != std::string::npos
                   && snapDispatch < rangeParse,
               "Auto Snap dispatch precedes selection-range parsing in applyAutoTuneToSelection");
    } else {
        expect(false, "Auto Snap dispatch precedes selection-range parsing in applyAutoTuneToSelection");
    }
    const auto autoSnap = functionBlock(pianoRoll, "PianoRollComponent::applyAutoSnapToAllNotes");
    expect(contains(autoSnap, "quantizeMidiToActiveScale")
               && contains(autoSnap, "commitEditedContentNotesAndSegments"),
           "Auto Snap snaps via scale quantize and commits notes+segments once");
    expect(!contains(autoSnap, "pitchCurveEdited"),
           "Auto Snap never fires pitchCurveEdited listeners");
    expect(!contains(f0Extraction, "voicedFrames == 0"),
           "Silent clips are legal F0 extraction results");

    // 5. 前置检查单一快照 + commit 签名 + 5 个调用方（3 复用 + 2 新读）
    expect(!contains(applyAutoTune, "currentCurve_->getSnapshot()")
               && !contains(applyAutoTune, "currentF0Timeline()"),
           "AUTO preflight derives everything from the single read snapshot");
    expect(contains(pianoRollHeader,
                    "commitEditedContentNotesAndSegments(const EditableContentSnapshot& snapshot"),
           "Notes+segments commit takes the editable snapshot by const reference");
    expect(contains(functionBlock(pianoRoll, "PianoRollComponent::applyCorrectionToEntireClip"),
                    "commitEditedContentNotesAndSegments(*contentSnapshot"),
           "applyCorrectionToEntireClip reuses the read contentSnapshot at its commit call");
    expect(contains(functionBlock(pianoRoll, "PianoRollComponent::applyNoteParameterToSelectedNotes"),
                    "commitEditedContentNotesAndSegments(*contentSnapshot"),
           "applyNoteParameterToSelectedNotes reuses the read contentSnapshot at its commit call");
    expect(contains(functionBlock(pianoRoll, "PianoRollComponent::applyParameterToFrameRange"),
                    "commitEditedContentNotesAndSegments(*contentSnapshot"),
           "applyParameterToFrameRange reuses the read contentSnapshot at its commit call");
    expect(contains(functionBlock(pianoRoll, "PianoRollComponent::buildToolHandlerContext"),
                    "commitEditedContentNotesAndSegments(*snap"),
           "buildToolHandlerContext commit lambda reads its own snapshot");
    expect(contains(functionBlock(pianoRoll, "PianoRollComponent::commitEditedContentPitchCorrectionSegments"),
                    "commitEditedContentNotesAndSegments(*snap"),
           "commitEditedContentPitchCorrectionSegments reads its own snapshot");

    // 6. notes-only 单一快照读取 + affected range 覆盖生成音符实际边界
    expect(countOccurrences(notesOnly, "getContentSnapshot") == 1,
           "Notes-only generation reads the content snapshot exactly once");
    expect(contains(notesOnly, "rangeStartSec")
               && contains(notesOnly, "n.startTime")
               && contains(notesOnly, "n.endTime"),
           "Notes-only affected range expands over generated note boundaries");

    // 7. 全仓旧命名零残留（grep 断言旧名不存在，而非仅检查新名存在）
    expect(!contains(processorHeader, "autoTuneWholeContentOnReady")
               && !contains(processor, "autoTuneWholeContentOnReady")
               && !contains(standaloneEditor, "autoTuneWholeContentOnReady")
               && !contains(pluginEditorHeader, "autoTuneWholeContentOnReady")
               && !contains(pluginEditor, "autoTuneWholeContentOnReady"),
           "autoTuneWholeContentOnReady has zero residual across processor and editors");
    // getCurrentAutoTuneParams 保留，故用字段形态 .autoTuneParams 精确判定
    expect(!contains(processorHeader, "autoTuneParams")
               && !contains(processor, "autoTuneParams")
               && !contains(standaloneEditor, ".autoTuneParams")
               && !contains(pluginEditorHeader, "autoTuneParams")
               && !contains(pluginEditor, ".autoTuneParams"),
           "autoTuneParams field has zero residual (getCurrentAutoTuneParams stays)");
    expect(!contains(processorHeader, "onAutoTuneCommitted")
               && !contains(processor, "onAutoTuneCommitted")
               && !contains(standaloneEditor, "onAutoTuneCommitted")
               && !contains(pluginEditorHeader, "onAutoTuneCommitted")
               && !contains(pluginEditor, "onAutoTuneCommitted"),
           "onAutoTuneCommitted has zero residual across processor and editors");
    expect(!contains(processorHeader, "pendingAutoTuneOnReady_")
               && !contains(processor, "pendingAutoTuneOnReady_")
               && !contains(standaloneEditor, "pendingAutoTuneOnReady_")
               && !contains(pluginEditorHeader, "pendingAutoTuneOnReady_")
               && !contains(pluginEditor, "pendingAutoTuneOnReady_"),
           "pendingAutoTuneOnReady_ has zero residual across processor and editors");
}

// ── Auto Snap 数学模型 ──
// 与 ScaleSnapConfig::snapMidi / quantizeMidiToActiveScale
// （Source/Utils/LegacyNoteGenerator.cpp:51-84）及 applyAutoSnapToAllNotes 的
// snappedOffset 公式（Source/Standalone/UI/PianoRollComponent.cpp:4724-4729）
// 逐行同构。tones == nullptr 表示 Chromatic（生产实现直接 round，不投影音阶）。
float snapMidiModel(float midiNote, int root, const int* tones, int count)
{
    float pc = std::fmod(midiNote - static_cast<float>(root), 12.0f);
    if (pc < 0.0f) pc += 12.0f;

    float bestDiff = 999.0f;
    int   bestTone = tones[0];
    for (int i = 0; i < count; ++i) {
        float diff = pc - static_cast<float>(tones[i]);
        if (diff >  6.0f) diff -= 12.0f;
        if (diff < -6.0f) diff += 12.0f;
        if (std::abs(diff) < bestDiff) {
            bestDiff = std::abs(diff);
            bestTone = tones[i];
        }
    }

    float adj = static_cast<float>(bestTone) - pc;
    if (adj >  6.0f) adj -= 12.0f;
    if (adj < -6.0f) adj += 12.0f;

    return midiNote + adj;
}

float quantizeMidiToActiveScaleModel(float midiNote, int root, const int* tones, int count)
{
    if (tones == nullptr) return std::round(midiNote);
    return std::round(snapMidiModel(midiNote, root, tones, count));
}

void testAutoSnapTargetMathContract()
{
    // C Major 音级表（与 LegacyNoteGenerator.cpp kMajorSemitones 一致），root 0 = C
    static const int majorTones[] = {0, 2, 4, 5, 7, 9, 11};
    const int majorCount = 7;

    // 连续基准音高 60.32 → C Major SNAP：目标精确 60.0
    const float baseMidi = 60.32f;
    const float snapped = quantizeMidiToActiveScaleModel(baseMidi, 0, majorTones, majorCount);
    expect(snapped == 60.0f,
           "Auto Snap math: 60.32 in C Major quantizes exactly to 60.0");
    // 最终 adjusted MIDI = baseMidi + snappedOffset（生产：note.pitchOffset = snappedOffset）
    const float snappedOffset = snapped - baseMidi;
    expect(std::abs(snappedOffset + 0.32f) < 0.0001f,
           "Auto Snap math: snappedOffset preserves the -0.32 semitone correction");
    expect(std::abs((baseMidi + snappedOffset) - 60.0f) < 0.0001f,
           "Auto Snap math: final adjusted MIDI lands exactly on the 60.0 target");

    // 最近音级选择：61.51 偏向 62（距 62 为 0.49 < 距 60 的 1.51）
    expect(quantizeMidiToActiveScaleModel(61.51f, 0, majorTones, majorCount) == 62.0f,
           "Auto Snap math: 61.51 in C Major lands exactly on 62.0");
    // 跨 octave 音级折叠：59.4 的 pitch class 11.4 吸附到音级 11
    expect(quantizeMidiToActiveScaleModel(59.4f, 0, majorTones, majorCount) == 59.0f,
           "Auto Snap math: 59.4 in C Major lands exactly on 59.0 via octave folding");
    // Chromatic：round 到最近半音
    expect(quantizeMidiToActiveScaleModel(60.32f, 0, nullptr, 0) == 60.0f,
           "Auto Snap math: Chromatic rounds 60.32 exactly to 60.0");

    // 切调式二次 SNAP 反例：Q_new(Q_old(x)) 被禁止，必须按基准音高重新投影。
    // base=61.3：旧 C Major 吸附到 62；新 C Pentatonic Minor 对旧目标 62 再吸附 → 63；
    // 直接按基准 61.3 投影 → 60。63 != 60，证明链式投影与正确结果发散。
    static const int pentatonicMinorTones[] = {0, 3, 5, 7, 10};
    const int pentMinorCount = 5;
    const float reSnapBase = 61.3f;
    const float oldSnap = quantizeMidiToActiveScaleModel(reSnapBase, 0, majorTones, majorCount);
    const float reSnapOldTarget =
        quantizeMidiToActiveScaleModel(oldSnap, 0, pentatonicMinorTones, pentMinorCount);
    const float directBaseSnap =
        quantizeMidiToActiveScaleModel(reSnapBase, 0, pentatonicMinorTones, pentMinorCount);
    expect(oldSnap == 62.0f,
           "Auto Snap math: 61.3 in C Major lands exactly on 62.0");
    expect(reSnapOldTarget == 63.0f,
           "Auto Snap math: re-snapping the old 62.0 target in C Pentatonic Minor lands on 63.0");
    expect(directBaseSnap == 60.0f,
           "Auto Snap math: 61.3 projected directly in C Pentatonic Minor lands on 60.0");
    // 63 != 60（前三条已锁死）：链式投影与直接基准投影发散，Q_new(Q_old(x)) 被禁止。

    // ── 源码对照：生产实现与模型同构（捕获公式/路径漂移）──
    const auto generatorSource = readSource("Source/Utils/LegacyNoteGenerator.cpp");
    const auto quantizeFn = functionBlock(
        generatorSource, "float ScaleSnapConfig::quantizeMidiToActiveScale");
    const auto snapFn = functionBlock(generatorSource, "float ScaleSnapConfig::snapMidi");
    expect(contains(quantizeFn, "std::round(snapMidi(midiNote))"),
           "quantizeMidiToActiveScale projects through round(snapMidi)");
    expect(contains(snapFn, "std::fmod(midiNote - static_cast<float>(root), 12.0f)")
               && contains(snapFn, "midiNote + adj")
               && contains(snapFn, "diff >  6.0f")
               && contains(snapFn, "diff < -6.0f"),
           "snapMidi folds pitch-class distances and returns midiNote + adj");
    expect(contains(generatorSource, "{0, 2, 4, 5, 7, 9, 11}"),
           "C Major tone table matches the model");

    // 生产 snappedOffset 相对 baseMidi 计算，最终 adjusted MIDI = quantize 结果。
    // 基准 MIDI 直接进 quantizeMidiToActiveScale，pitchOffset 不参与量化输入，
    // 不存在 targetMidi 中间量（切调式反例证明 Q_new(Q_old(x)) 被禁止）。
    const auto autoSnap = functionBlock(
        readSource("Source/Standalone/UI/PianoRollComponent.cpp"),
        "PianoRollComponent::applyAutoSnapToAllNotes");
    expect(contains(autoSnap, "const float baseMidi = PitchUtils::freqToMidi(note.pitch);")
           && contains(autoSnap, "quantizeMidiToActiveScale(baseMidi)")
           && !contains(autoSnap, "baseMidi + note.pitchOffset")
           && !contains(autoSnap, "targetMidi")
           && contains(autoSnap, "- baseMidi")
           && !contains(autoSnap, "getBaseMidiNote()")
           && contains(autoSnap, "note.pitchOffset = snappedOffset;"),
           "Auto Snap quantizes the continuous base MIDI directly, never the offset-shifted target");

    // Pitch 工具双击（F2×1）与按钮入口同构：基准 MIDI 直接进 quantizeMidiToActiveScale，
    // original.pitchOffset 不参与量化输入，也不存在旧 targetMidi 中间量。
    const auto toolHandler = readSource(
        "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto pitchDoubleClick = functionBlock(
        toolHandler, "void PianoRollToolHandler::handlePitchToolDoubleClick");
    expect(contains(pitchDoubleClick, "const float baseMidi = PitchUtils::freqToMidi(original.pitch);")
           && contains(pitchDoubleClick, "quantizeMidiToActiveScale(baseMidi)")
           && !contains(pitchDoubleClick, "baseMidi + original.pitchOffset")
           && !contains(pitchDoubleClick, "targetMidi")
           && contains(pitchDoubleClick, "pitchOffset = snappedOffset"),
           "Pitch tool double-click quantizes the base MIDI directly, never the offset-shifted target");
}

void testNoteTopologyContract()
{
    // 1. 快照/三个 state/工程条目统一携带音符拓扑初始化标记
    expect(contains(readSource("Source/Content/EditableContentSnapshot.h"), "noteTopologyInitialized"),
           "EditableContentSnapshot carries noteTopologyInitialized");
    expect(contains(readSource("Source/Content/ContentPayloadState.h"), "noteTopologyInitialized")
               && contains(readSource("Source/Content/ARAEditableContentState.h"), "noteTopologyInitialized")
               && contains(readSource("Source/Content/EditableContentState.h"), "noteTopologyInitialized"),
           "All three content states carry noteTopologyInitialized");
    expect(contains(readSource("Source/Utils/ProjectModel.h"), "noteTopologyInitialized"),
           "ProjectContentEntry carries noteTopologyInitialized");

    // 2. 三个 applyNotes 提交块都置位拓扑初始化标记
    expect(contains(functionBlock(readSource("Source/ARA/AudioModification.cpp"),
                                  "void AudioModification::applyNotes"),
                    "noteTopologyInitialized = true")
               && contains(functionBlock(readSource("Source/Content/StandaloneClipContent.cpp"),
                                         "void StandaloneClipContent::applyNotes"),
                           "noteTopologyInitialized = true")
               && contains(functionBlock(readSource("Source/Content/CaptureSegmentContent.cpp"),
                                         "void CaptureSegmentContent::applyNotes"),
                           "noteTopologyInitialized = true"),
           "All three applyNotes blocks set noteTopologyInitialized = true");

    // 3. ensureOpenDyneNotesIfNeeded 以拓扑标记为判据，不依赖本地缓存，生成成功后走统一 contentEdited
    const auto ensureBlock = functionBlock(
        readSource("Source/Standalone/UI/PianoRollComponent.cpp"),
        "void PianoRollComponent::ensureOpenDyneNotesIfNeeded");
    expect(contains(ensureBlock, "snap->noteTopologyInitialized"),
           "OpenDyne note seeding gates on snap->noteTopologyInitialized");
    expect(!contains(ensureBlock, "cachedNotes_.empty()"),
           "OpenDyne note seeding never gates on cachedNotes_.empty()");
    expect(contains(ensureBlock, "generateNotesOnlyByContentKey")
               && contains(ensureBlock, "listener.contentEdited()"),
           "Successful note seeding notifies through the shared contentEdited listener");

    // 4. ProjectPersistence：写 property、hasProperty 读取、旧工程默认 !m.notes.empty()
    const auto persistence = readSource("Source/Utils/ProjectPersistence.cpp");
    expect(contains(persistence, "setProperty(\"noteTopologyInitialized\", mat.noteTopologyInitialized ? 1 : 0"),
           "ProjectPersistence writes noteTopologyInitialized as a content property");
    expect(contains(persistence, "tree.hasProperty(\"noteTopologyInitialized\")")
               && contains(persistence, ": !m.notes.empty();"),
           "ProjectPersistence reads via hasProperty and defaults legacy projects to !m.notes.empty()");

    // 5. ProjectSession：capture 复制 notes 与拓扑标记，applyNotes 后恢复持久值
    const auto projectSession = readSource("Source/Utils/ProjectSession.cpp");
    expect(contains(projectSession, "entry.notes = payload.notes;")
               && contains(projectSession, "entry.noteTopologyInitialized = payload.noteTopologyInitialized;"),
           "ProjectSession capture copies notes and the topology flag as one unit");
    // applySnapshot 块内锁定恢复顺序：applyNotes 必须先于拓扑标记恢复执行
    const auto applySnapshot = functionBlock(
        projectSession, "Result<void> ProjectSession::applySnapshot(const ProjectSnapshot& snapshot)");
    const auto applyNotesPos = applySnapshot.find("clip->applyNotes(contentEntry.notes);");
    const auto restoreFlagPos = applySnapshot.find(
        "clip->payload().noteTopologyInitialized = contentEntry.noteTopologyInitialized;");
    expect(applyNotesPos != std::string::npos && restoreFlagPos != std::string::npos
               && applyNotesPos < restoreFlagPos,
           "applySnapshot restores the persisted topology flag only after applyNotes ran");

    // 6. ARA 归档：写入断言 + 恢复端按拓扑标记回填旧归档缺省值
    const auto documentController = readSource("Source/ARA/OpenTuneDocumentController.cpp");
    const auto storeObjects = functionBlock(
        documentController, "bool OpenTuneDocumentController::doStoreObjectsToStream");
    const auto serializeContent = functionBlock(
        documentController,
        "void serializeAudioModificationContent(const AudioModification& mod, juce::XmlElement& el)");
    const auto restoreContent = functionBlock(
        documentController, "std::optional<AudioModificationContentState> restoreAudioModificationContent");
    expect(contains(storeObjects, "output.writeInt(kContentPayloadArchiveMagic)")
               && contains(storeObjects, "output.writeString(modification->persistentId)")
               && contains(storeObjects, "serializeAudioModificationContent(*modification, el)")
               && contains(storeObjects, "output.writeString(el.toString())"),
           "ARA archive write stores magic/version, persistentId and the serialized content payload");
    expect(contains(serializeContent, "setAttribute(\"noteTopologyInitialized\""),
           "ARA serialize writes the noteTopologyInitialized attribute directly, not only via the store caller");
    expect(contains(restoreContent, "getIntAttribute(\"noteTopologyInitialized\"")
               && contains(restoreContent, "content.editable.notes.empty() ? 0 : 1")
               && contains(restoreContent, "content.editable.noteTopologyInitialized ="),
           "ARA restore reads the topology flag and defaults legacy archives to notes.empty() ? 0 : 1");

    // 7. snapshotAudioModification：将 editable/analysis 状态传播进渲染快照
    const auto snapshotModification = functionBlock(
        documentController, "OpenTuneDocumentController::snapshotAudioModification(ContentKey key) const");
    expect(contains(snapshotModification, "snap->notes = content.editable.notes;")
               && contains(snapshotModification, "snap->pitchCurve = content.analysis.pitchCurve;")
               && contains(snapshotModification, "snap->timeGrid = content.editable.timeGrid;")
               && contains(snapshotModification, "snap->noteTopologyInitialized = content.editable.noteTopologyInitialized;"),
           "snapshotAudioModification propagates editable notes and analysis state into the render snapshot");
    expect(contains(snapshotModification, "snap->notesRevision = content.editable.notesRevision;")
               && contains(snapshotModification, "snap->contentRevision = content.contentRevision;")
               && contains(documentController, "snapshotAudioModification(job.contentKey)"),
           "snapshotAudioModification propagates the revisions and feeds the chunk render job");

    // 8. CapturePersistence：magic/读写基础断言 + 拓扑标记字段成对（property 写、hasProperty 读、最终回填）
    const auto capturePersistence = readSource("Source/Plugin/Capture/CapturePersistence.cpp");
    const auto captureSerialize = functionBlock(
        capturePersistence, "juce::MemoryBlock CapturePersistence::serialize");
    const auto captureDeserialize = functionBlock(
        capturePersistence, "bool CapturePersistence::deserialize");
    expect(contains(captureSerialize, "stream.writeInt(static_cast<int>(kCaptureMagic))")
               && contains(captureSerialize, "stream.writeInt(kCaptureArchiveVersion)")
               && contains(captureSerialize, "writePitchCurve(stream, snap->pitchCurve)")
               && contains(captureSerialize, "stream.writeDouble(note.startTime)")
               && contains(captureSerialize, "setProperty(\"noteTopologyInitialized\""),
           "CapturePersistence write emits magic/version, pitch curve and the topology flag property");
    expect(contains(captureDeserialize, "magic != kCaptureMagic")
               && contains(captureDeserialize, "note.startTime = stream.readDouble()")
               && contains(captureDeserialize, "readPitchCurve(stream, hasPitchDriftScale)")
               && contains(captureDeserialize, "hasProperty(\"noteTopologyInitialized\")")
               && contains(captureDeserialize, ": !p.notes.empty();")
               && contains(captureDeserialize, "editable().noteTopologyInitialized = p.noteTopologyInitialized"),
           "CapturePersistence read validates magic/version, restores curve and back-fills the topology flag");

    // 9. PluginProcessor 放置操作：merge 以双端 OR 合并拓扑标记，copy 继承源快照状态
    const auto processorSource = readSource("Source/PluginProcessor.cpp");
    const auto placementsMerge = functionBlock(
        processorSource, "std::optional<MergeOutcome> OpenTuneAudioProcessor::mergePlacements");
    const auto rangeCopy = functionBlock(
        processorSource, "ContentKey OpenTuneAudioProcessor::copyContentRange");
    expect(contains(placementsMerge, "mergedPayload.noteTopologyInitialized")
               && contains(placementsMerge, "leadingSnapshot->noteTopologyInitialized")
               && contains(placementsMerge, "trailingSnapshot->noteTopologyInitialized")
               && contains(placementsMerge, "||"),
           "Placement merge ORs the leading/trailing topology flags into the merged payload");
    expect(contains(rangeCopy, "payload.noteTopologyInitialized = sourceSnap->noteTopologyInitialized"),
           "Range copy inherits the topology flag from the source snapshot");

    // 10. 删除键契约：逐选中音符换算清除范围，排序合并后单克隆逐 range 清空，单次原子提交
    const auto deleteKey = functionBlock(
        readSource("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp"),
        "void PianoRollToolHandler::handleDeleteKey");
    expect(contains(deleteKey, "for (int noteIndex : selectedIndices) {")
               && contains(deleteKey, "f0tl.rangeForTimes(note.startTime, note.endTime)"),
           "Delete key converts each selected note to a clear range inside the selection loop");
    expect(contains(deleteKey, "std::sort(correctionClearRanges.begin(), correctionClearRanges.end(),")
               && contains(deleteKey, "mergedRanges"),
           "Delete key sorts and merges the per-note clear ranges");
    expect(contains(deleteKey, "auto clonedCurve = curve->clone();")
               && contains(deleteKey, "for (const auto& range : correctionClearRanges) {")
               && contains(deleteKey, "clonedCurve->clearCorrectionRange(range.startFrame, range.endFrameExclusive)"),
           "Delete key clears each merged range on one cloned curve");
    expect(contains(deleteKey, "auto snap = clonedCurve->getSnapshot();")
               && contains(deleteKey, "auto allSegments = snap->getCorrectionSegments();")
               && contains(deleteKey, "std::vector<PitchCorrectionSegment> segmentsInRange;")
               && contains(deleteKey, "for (const auto& seg : allSegments)")
               && contains(deleteKey, "segmentsInRange.push_back(seg)"),
           "Delete key rebuilds segments from the cleared clone and re-commits only the in-range ones, "
           "so unselected middle-note corrections cleared from the clone are re-submitted and preserved");
    expect(countOccurrences(deleteKey, "commitNotesAndSegments") == 1,
           "Delete key commits notes and segments exactly once");

    // 11. F0 契约：唯一外层 ownerSnapshot truthy 守卫，模式分流决策位于该块内
    const auto drawF0Curve = functionBlock(
        readSource("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp"),
        "void PianoRollRenderer::drawF0Curve");
    expect(countOccurrences(drawF0Curve, "ctx.showCorrectedF0 && item.ownerSnapshot") == 1,
           "drawF0Curve has exactly one outer corrected-F0 guard on the owner snapshot");
    expect(contains(drawF0Curve, "const bool hasCorrections = item.pitchSnapshot->hasCorrectionLayer()")
               && contains(drawF0Curve, "const bool shouldDrawCorrected = item.notesPrimaryScheme"),
           "hasCorrections and shouldDrawCorrected live inside the corrected-F0 block");
    expect(contains(drawF0Curve, "item.notesPrimaryScheme")
               && contains(drawF0Curve, "hasCorrections"),
           "drawF0Curve keeps the OpenDyne/OpenTune corrected-F0 mode semantics");
}

void testARARestoreAutoReadContract()
{
    // ARA 归档恢复自动读取契约：readRestoredAudio 只在 archive 已有有效 F0
    // 时自动 birth；新插入无有效 F0 的内容保持等待用户 Read，不提前 Stage1。
    const auto araArchive = readSource("Source/ARA/OpenTuneDocumentController.cpp");
    const auto araHeader = readSource("Source/ARA/OpenTuneDocumentController.h");

    // 1. readRestoredAudio 只处理 WaitingForSource + Ready F0 + 有效 pitchCurve
    //    + 有有效 PlaybackRegion 的 modification，并调用 birthContentForModification
    const auto readRestored = functionBlock(
        araArchive, "void OpenTuneDocumentController::readRestoredAudio");
    expect(contains(readRestored, "mod.birthState != AudioModificationBirthState::WaitingForSource"),
           "readRestoredAudio processes only WaitingForSource births");
    expect(contains(readRestored, "content.analysis.originalF0State != OriginalF0State::Ready")
               && contains(readRestored, "content.analysis.pitchCurve == nullptr")
               && contains(readRestored, "!content.analysis.pitchCurve->hasOriginalF0Data()"),
           "readRestoredAudio auto-reads only Ready OriginalF0 with non-empty pitch curve data");
    expect(contains(readRestored, "region.hasValidPlacement()"),
           "readRestoredAudio requires a valid playback region placement");
    expect(contains(readRestored, "birthContentForModification(mod);"),
           "readRestoredAudio births the restored modification automatically");

    // 2. 编辑会话结束后统一自动读取
    expect(contains(functionBlock(araArchive, "void OpenTuneDocumentController::didEndEditing"),
                    "readRestoredAudio(nullptr);"),
           "didEndEditing auto-reads restored content after the edit session");

    // 3. 编辑会话外的 samples access 也触发自动读取，且限定到该 source
    const auto samplesAccess = functionBlock(
        araArchive, "void OpenTuneDocumentController::didEnableAudioSourceSamplesAccess");
    expect(contains(samplesAccess, "!getDocumentController()->isHostEditingDocument()"),
           "samples-access auto-read is gated outside the host edit session");
    expect(contains(samplesAccess, "readRestoredAudio(&source);"),
           "samples-access auto-read targets the just-enabled source");

    // 4. 用户 Read 只允许 WaitingForSource/Failed 两种 birth 状态
    const auto readRequest = functionBlock(
        araArchive, "int OpenTuneDocumentController::requestReadAudioForPlaybackRegions");
    expect(contains(readRequest, "modification->birthState != AudioModificationBirthState::WaitingForSource")
               && contains(readRequest, "modification->birthState != AudioModificationBirthState::Failed")
               && contains(readRequest, "continue;"),
           "user Read admits only WaitingForSource/Failed births and skips the rest");

    // 5. birthContentForModification：已有有效 F0（archive 恢复）跳过提取并自动
    //    Stage1；无有效 F0（新内容 Read）不提前 Stage1，只调度异步 F0 提取
    const auto birth = functionBlock(
        araArchive, "bool OpenTuneDocumentController::birthContentForModification");
    expect(contains(birth, "const bool alreadyHasF0 = content.analysis.originalF0State == OriginalF0State::Ready")
               && contains(birth, "content.analysis.pitchCurve != nullptr")
               && contains(birth, "content.analysis.pitchCurve->hasOriginalF0Data();"),
           "alreadyHasF0 requires Ready OriginalF0 with non-empty pitch curve data");
    expect(contains(birth, "notifyContentChanged(juce::ARAContentUpdateScopes(), !alreadyHasF0)"),
           "restored content suppresses the host content-changed notification");
    expect(contains(functionBlock(birth, "if (alreadyHasF0)"),
                    "requestFullModificationRender(modification.contentKey())"),
           "restored content with valid F0 auto-triggers the full Stage1 render");
    expect(contains(functionBlock(birth, "if (!alreadyHasF0 && contentRenderService_ != nullptr)"),
                    "scheduleAsyncF0Extraction"),
           "new content without valid F0 schedules async F0 extraction instead of Stage1");

    // 6. applyTimeGridToModification：isIdentity 在 std::move(grid) 前捕获，
    //    move 后不再解引用 grid；非 identity 才 enqueue Stage2 重建
    const auto timeGrid = functionBlock(
        araArchive, "bool OpenTuneDocumentController::applyTimeGridToModification");
    const auto identityPos = timeGrid.find("const bool isIdentity = grid->isIdentity();");
    const auto movePos = timeGrid.find("std::move(grid)");
    expect(identityPos != std::string::npos && movePos != std::string::npos
               && identityPos < movePos,
           "applyTimeGridToModification captures isIdentity before moving the grid");
    expect(countOccurrences(timeGrid, "grid->") == 1,
           "applyTimeGridToModification never dereferences the grid after std::move(grid)");
    expect(contains(timeGrid, "getTimeStretchCache().invalidate(key)"),
           "TimeGrid edits invalidate the Stage2 stretch cache");
    expect(contains(functionBlock(timeGrid, "if (!isIdentity)"),
                    "enqueueStage2RebuildWhenCanonicalSettled(request)"),
           "non-identity TimeGrid enqueues the Stage2 rebuild on canonical settle");

    // 7. Stage1 → Stage2：chunk settled 经 CompletionContext gate 回调，
    //    handleStage1ChunkSettled 统一 enqueue Stage2；析构关闭 gate
    const auto processJob = functionBlock(
        araArchive, "void OpenTuneDocumentController::processDocumentRenderJob");
    expect(contains(processJob, "ProcessRenderRuntime::CompletionContext completion;")
               && contains(processJob, "completion.gate = completionGate_;")
               && contains(processJob, "completion.chunkSettled = [this](ContentKey key) {")
               && contains(processJob, "handleStage1ChunkSettled(key);"),
           "Stage1 chunk completion flows through the shared gate into handleStage1ChunkSettled");
    expect(contains(functionBlock(araArchive, "void OpenTuneDocumentController::handleStage1ChunkSettled"),
                    "enqueueStage2RebuildWhenCanonicalSettled(request);"),
           "settled Stage1 chunks enqueue the Stage2 rebuild");
    expect(contains(functionBlock(araArchive, "OpenTuneDocumentController::~OpenTuneDocumentController()"),
                    "completionGate_->closed = true;"),
           "destructor closes the Stage1-to-Stage2 completion gate");

    // 8. 旧 restore 意图命名在 DC cpp/header 零残留
    expect(!contains(araArchive, "restoredFromArchive")
               && !contains(araArchive, "BirthIntent")
               && !contains(araArchive, "archiveRestoreGraphReady")
               && !contains(araArchive, "restoreMaterializationPending"),
           "no legacy restore-intent naming remains in the document controller source");
    expect(!contains(araHeader, "restoredFromArchive")
               && !contains(araHeader, "BirthIntent")
               && !contains(araHeader, "archiveRestoreGraphReady")
               && !contains(araHeader, "restoreMaterializationPending"),
           "no legacy restore-intent naming remains in the document controller header");

    // 9. reader lease 只在 ARA sample-access callback 创建；
    //    birthContentForModification 只消费 shareReaderLease，不在非实时路径创建
    expect(contains(samplesAccess, "source.createReaderLease();"),
           "reader lease is created only in the ARA sample-access callback");
    expect(!contains(birth, "createReaderLease"),
           "birthContentForModification consumes the reader lease without creating one");

    // 10. silent-gap 检测只在用户 Read 分支（!alreadyHasF0）执行一次，
    //     archive 恢复（alreadyHasF0）保留 restored silentGaps
    const auto f0SkipBranchPos = birth.find("if (!alreadyHasF0)");
    const auto silentGapBranchPos = birth.find("if (!alreadyHasF0)", f0SkipBranchPos + 1);
    const auto newReadBranch = silentGapBranchPos != std::string::npos
        ? functionBlock(birth.substr(silentGapBranchPos), "if (!alreadyHasF0)")
        : std::string();
    expect(contains(newReadBranch, "SilentGapDetector::detectAllGapsAdaptive")
               && contains(newReadBranch, "submitSilentGaps")
               && contains(newReadBranch, "applyOriginalF0State"),
           "silent-gap detection and F0-state reset live only in the !alreadyHasF0 branch");
    expect(countOccurrences(birth, "submitSilentGaps") == 1,
           "birthContentForModification submits silent gaps exactly once");

    // 11. 析构中 completion gate 锁作用域在 detachExecutionLease 之前结束
    const auto destructor = functionBlock(
        araArchive, "OpenTuneDocumentController::~OpenTuneDocumentController()");
    const auto gateClosedPos = destructor.find("completionGate_->closed = true;");
    const auto gateBracePos = destructor.find("\n    }", gateClosedPos);
    const auto detachPos = destructor.find("detachExecutionLease(this)");
    expect(gateClosedPos != std::string::npos && gateBracePos != std::string::npos
               && detachPos != std::string::npos && gateBracePos < detachPos,
           "completion gate lock scope ends before detachExecutionLease");
}

void testCaptureF0KeyContract()
{
    const auto captureSession = readSource("Source/Plugin/Capture/CaptureSession.cpp");
    const auto commitF0 = functionBlock(
        captureSession, "bool CaptureSession::commitSegmentF0Result(");
    const auto processor = readSource("Source/PluginProcessor.cpp");
    const auto successCommit = functionBlock(
        processor, "if (session->commitSegmentF0Result(");

    expect(!contains(commitF0, "DetectedKey") && !contains(commitF0, "applyDetectedKey"),
           "commitSegmentF0Result applies F0 state without any detected-key side effect");
    expect(contains(successCommit, "updateContentKeyFromOriginalF0(segContentKey)"),
           "Segment F0 success path updates the content key from the committed F0");
}

void testF0KeyDetectionContract()
{
    // 1. ChromaKeyDetector 零残留：Source/DSP 文件不存在，CMake 无引用
    expect(!std::filesystem::exists(sourceRoot / "Source/DSP/ChromaKeyDetector.cpp")
               && !std::filesystem::exists(sourceRoot / "Source/DSP/ChromaKeyDetector.h"),
           "ChromaKeyDetector source files are fully removed from Source/DSP");
    expect(!contains(readSource("CMakeLists.txt"), "ChromaKeyDetector"),
           "CMakeLists.txt no longer references ChromaKeyDetector");

    // 2. F0KeyDetector 为 F0 轨迹驱动，无 PCM chroma/FFT/Pearson/Temperley 残留
    const auto f0Detector = readSource("Source/DSP/F0KeyDetector.cpp");
    expect(!contains(f0Detector, "FFT")
               && !contains(f0Detector, "pearson")
               && !contains(f0Detector, "Temperley")
               && !contains(f0Detector, "juce::dsp"),
           "F0KeyDetector is F0-driven with no PCM chroma/FFT/Pearson/Temperley machinery");

    // 3. 类型迁移完成：8 个头文件 include Utils/DetectedKey.h，无 ChromaKeyDetector.h
    const auto migratedHeaders = {
        "Source/Content/AnalysisState.h",
        "Source/Content/ContentEditCommands.h",
        "Source/Content/ContentPayloadState.h",
        "Source/Content/EditableContentSnapshot.h",
        "Source/Content/EditableContentState.h",
        "Source/PluginProcessor.h",
        "Source/Utils/ProjectModel.h",
        "Source/Utils/ScaleUiMapping.h",
    };
    for (const char* path : migratedHeaders) {
        const auto header = readSource(path);
        expect(contains(header, "DetectedKey.h")
                   && !contains(header, "ChromaKeyDetector.h"),
               "DetectedKey types migrate to Utils/DetectedKey.h in the header list");
    }

    // 4. 调用入口收敛：PluginProcessor 唯一 F0 驱动入口，UI 无重复入口，ARA 链走 F0KeyDetector
    const auto processor = readSource("Source/PluginProcessor.cpp");
    const auto pluginEditor = readSource("Source/Plugin/PluginEditor.cpp");
    const auto araArchive = readSource("Source/ARA/OpenTuneDocumentController.cpp");
    expect(contains(processor, "updateContentKeyFromOriginalF0")
               && !contains(processor, "detectContentKeyIfUnset"),
           "F0-driven key update is the only detection entry in PluginProcessor");
    expect(!contains(pluginEditor, "updateContentKeyFromOriginalF0")
               && !contains(pluginEditor, "detectContentKeyIfUnset"),
           "PluginEditor has no duplicate key-detection entry point");
    expect(contains(araArchive, "F0KeyDetector"),
           "ARA chain drives detection through F0KeyDetector");

    // 5. Manual guard：updateContentKeyFromOriginalF0 永不覆盖手动调式
    const auto updateF0 = functionBlock(
        processor, "void OpenTuneAudioProcessor::updateContentKeyFromOriginalF0");
    expect(contains(updateF0, "Origin::Manual"),
           "updateContentKeyFromOriginalF0 never overwrites a manual key");

    // 6. UI 手动构造器写入 Origin::Manual
    const auto scaleUiMapping = readSource("Source/Utils/ScaleUiMapping.h");
    expect(contains(functionBlock(scaleUiMapping, "makeDetectedKeyFromUi"), "Origin::Manual"),
           "makeDetectedKeyFromUi marks UI-set keys as manual");

    // 7. 持久化三路径写入 origin（写法与既有 root/scale/confidence 持久化同构）
    const auto projectPersistence = readSource("Source/Utils/ProjectPersistence.cpp");
    const auto capturePersistence = readSource("Source/Plugin/Capture/CapturePersistence.cpp");
    expect(contains(projectPersistence, "setProperty(\"origin\""),
           "ProjectPersistence writes the detected-key origin property");
    expect(contains(functionBlock(capturePersistence,
                                  "juce::MemoryBlock CapturePersistence::serialize"),
                    "detectedKey.origin"),
           "CapturePersistence serialize writes the detected-key origin");
    expect(contains(araArchive, "setAttribute(\"origin\""),
           "ARA archive writes the detected-key origin attribute");

    // 8. 相等比较与去重提交包含 origin
    expect(contains(functionBlock(processor, "bool detectedKeysMatch"), "origin"),
           "detectedKeysMatch compares the detected-key origin");
    expect(contains(functionBlock(readSource("Source/Content/CaptureSegmentContent.cpp"),
                                  "void CaptureSegmentContent::applyDetectedKey"),
                    "origin"),
           "applyDetectedKey compares the detected-key origin");
}

void testPianoRollViewportSessionContract()
{
    const auto header = readSource("Source/Standalone/UI/PianoRollComponent.h");
    const auto source = readSource("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto pluginEditor = readSource("Source/Plugin/PluginEditor.cpp");

    // 1. 私有 ViewState 收敛为公开 ViewportState：字段不变，零残留，无别名/兼容层
    expect(contains(header, "struct ViewportState")
               && contains(header, "TimelineViewportCamera camera{")
               && contains(header, "float pixelsPerSemitone = 25.0f")
               && contains(header, "float verticalScrollOffset = 0.0f"),
           "PianoRoll exposes the public ViewportState with camera/pixelsPerSemitone/verticalScrollOffset");
    expect(!contains(header, "struct ViewState") && !contains(source, "ViewState"),
           "Private ViewState has zero residual in PianoRollComponent");

    // 2. 完整状态读取 + 原子恢复公共 API
    expect(contains(header, "ViewportState viewportState() const noexcept"),
           "PianoRoll exposes the complete-state read viewportState()");
    expect(contains(header, "void restoreViewportState(const ViewportState& state)"),
           "PianoRoll exposes the atomic restoreViewportState()");
    const auto restore = functionBlock(
        source, "void PianoRollComponent::restoreViewportState");
    expect(contains(restore, "camera_ = state.camera")
               && contains(restore, "pixelsPerSemitone_ = state.pixelsPerSemitone")
               && contains(restore, "verticalScrollOffset_ ="),
           "restoreViewportState writes camera, vertical zoom and vertical offset");
    expect(contains(restore, "staticDirty_ = true") && contains(restore, "contentDirty_ = true")
               && contains(restore, "rasterizeDirtySurfaces()")
               && contains(restore, "updateScrollBars()")
               && contains(restore, "repaint()")
               && contains(restore, "overlay_->repaint()"),
           "restoreViewportState synchronizes scrollbars, raster and repaint in one pass");
    expect(contains(restore, "userHasManuallyZoomed_ = true"),
           "restoreViewportState marks the restored camera as user intent so fitToScreen never overrides it");

    // 3. fitToScreen 水平逻辑：精确覆盖 [timelineStartSeconds, timelineEndSeconds]，
    //    不再"起点前移 10% 但缩放仍按原 duration"裁掉尾部；保留手动缩放守卫
    const auto fit = functionBlock(source, "void PianoRollComponent::fitToScreen()");
    expect(contains(fit, "userHasManuallyZoomed_"),
           "fitToScreen keeps the manual-zoom guard");
    expect(contains(fit, "fitStartSeconds = activeProjection.timelineStartSeconds")
               && contains(fit, "activeProjection.timelineEndSeconds() - fitStartSeconds"),
           "fitToScreen fits the exact timelineStartSeconds..timelineEndSeconds span");
    expect(!contains(fit, "duration * 0.1"),
           "fitToScreen no longer offsets the camera start by 10% of the duration");

    // 4. horizontalScrollBar_ 全路径删除（成员/构造监听/布局/scrollBarMoved/updateScrollBars/高度依赖）
    expect(!contains(header, "horizontalScrollBar_") && !contains(source, "horizontalScrollBar_"),
           "PianoRoll horizontal scrollbar is fully removed");
    const auto scrollBarMoved = functionBlock(source, "void PianoRollComponent::scrollBarMoved");
    expect(contains(scrollBarMoved, "verticalScrollBar_") && !contains(scrollBarMoved, "horizontal"),
           "scrollBarMoved keeps only the vertical branch");
    const auto updateBars = functionBlock(source, "void PianoRollComponent::updateScrollBars");
    expect(contains(updateBars, "verticalScrollBar_") && !contains(updateBars, "computeViewportRange"),
           "updateScrollBars keeps only the vertical range");
    const auto viewportBounds = functionBlock(
        source, "juce::Rectangle<int> PianoRollComponent::getTimelineViewportBounds");
    expect(!contains(viewportBounds, "horizontalScrollBar_") && contains(viewportBounds, "getHeight()"),
           "getTimelineViewportBounds uses the full height with no scrollbar layout dependency");

    // 4b. 水平滚动条删除后 TimelineViewportRange/computeViewportRange 零残留，
    //     保留 resolve/clampStartSeconds/visibleEndSeconds/computePlayheadPresentation
    const auto policyHeader = readSource("Source/Standalone/UI/TimelineViewportPolicy.h");
    const auto policySource = readSource("Source/Standalone/UI/TimelineViewportPolicy.cpp");
    expect(!contains(policyHeader, "TimelineViewportRange") && !contains(policySource, "TimelineViewportRange"),
           "TimelineViewportRange has zero residual after the horizontal scrollbar removal");
    expect(!contains(policyHeader, "computeViewportRange") && !contains(policySource, "computeViewportRange"),
           "computeViewportRange has zero residual after the horizontal scrollbar removal");
    expect(contains(policyHeader, "static TimelineViewportCamera resolve(")
               && contains(policyHeader, "clampStartSeconds")
               && contains(policyHeader, "visibleEndSeconds")
               && contains(policyHeader, "computePlayheadPresentation"),
           "TimelineViewportPolicy keeps resolve/clampStartSeconds/visibleEndSeconds/computePlayheadPresentation");

    // 5. Processor 会话状态：身份 = ContentKey + 四时间字段；中立 primitive；无 UI 类型、无锁、无序列化
    const auto processorHeader = readSource("Source/PluginProcessor.h");
    const auto processor = readSource("Source/PluginProcessor.cpp");
    const auto sessionStart = processorHeader.find("struct PianoRollPlacementIdentity");
    const auto sessionEnd = processorHeader.find("class OpenTuneAudioProcessor", sessionStart);
    const auto sessionRegion = (sessionStart != std::string::npos && sessionEnd != std::string::npos)
        ? processorHeader.substr(sessionStart, sessionEnd - sessionStart) : std::string();
    expect(contains(processorHeader, "#include \"Utils/ContentTimelineProjection.h\""),
           "PluginProcessor includes ContentTimelineProjection for the session identity");
    expect(contains(sessionRegion, "ContentKey contentKey")
               && contains(sessionRegion, "ContentTimelineProjection projection"),
           "Placement identity is ContentKey + ContentTimelineProjection");
    expect(contains(sessionRegion, "timelineStartSeconds")
               && contains(sessionRegion, "timelineDurationSeconds")
               && contains(sessionRegion, "contentStartSeconds")
               && contains(sessionRegion, "contentDurationSeconds"),
           "Placement identity carries the four projection time fields");
    expect(contains(sessionRegion, "cameraStartSeconds")
               && contains(sessionRegion, "cameraPixelsPerSecond")
               && contains(sessionRegion, "pixelsPerSemitone")
               && contains(sessionRegion, "verticalScrollOffset"),
           "Session viewport is the four primitive values of ViewportState");
    expect(!contains(sessionRegion, "PianoRollComponent")
               && !contains(sessionRegion, "ViewMapper")
               && !contains(sessionRegion, "TimelineViewportCamera"),
           "Processor session state includes no PianoRoll UI types");
    expect(!contains(sessionRegion, "mutex") && !contains(sessionRegion, "atomic"),
           "Session state is plain message-thread data without locks");
    expect(contains(processorHeader, "rememberPianoRollViewport")
               && contains(processorHeader, "readPianoRollViewport")
               && contains(processorHeader, "lastActivePianoRollPlacement"),
           "Processor exposes the remember/read/last-active session API");
    expect(contains(processorHeader, "PluginPianoRollSessionState pianoRollSession_"),
           "Processor owns one piano roll session state member");
    expect(!contains(functionBlock(processor, "void OpenTuneAudioProcessor::getStateInformation"),
                     "pianoRollSession_"),
           "Session memory is never serialized");
    const auto remember = functionBlock(
        processor, "void OpenTuneAudioProcessor::rememberPianoRollViewport");
    expect(contains(remember, "lastActivePlacement = placement")
               && contains(remember, "emplace_back(placement, viewport)"),
           "remember updates last-active and stores one entry per placement");
    const auto readViewport = functionBlock(
        processor, "OpenTuneAudioProcessor::readPianoRollViewport");
    expect(contains(readViewport, "find_if") && contains(readViewport, "std::nullopt"),
           "read looks the placement up and returns nullopt when absent");
    const auto lastActive = functionBlock(
        processor, "OpenTuneAudioProcessor::lastActivePianoRollPlacement");
    expect(contains(lastActive, "pianoRollSession_.lastActivePlacement"),
           "last-active returns the stored placement identity");

    expect(!contains(pluginEditor, "requestInitialF0View("),
           "VST3 viewport is controlled only by session restore or whole-item fit");
}

void testPrivateOnnxRuntimeContract()
{
    const auto cmake = readSource("CMakeLists.txt");
    const auto delayLoadHook = readSource("Source/Utils/OnnxRuntimeDelayLoadHook.cpp");
    const auto modelPathResolver = readSource("Source/Utils/ModelPathResolver.h");
    const auto importDefinition = readSource("cmake/OpenTuneOnnxRuntime_1_24_4.def");

    expect(contains(cmake, "OpenTuneOnnxRuntime_1_24_4.dll")
               && contains(cmake, "/DELAYLOAD:${OPENTUNE_ORT_DLL_NAME}")
               && contains(cmake, "OpenTuneOnnxRuntime_1_24_4.lib"),
           "Windows targets link and delay-load the private versioned ONNX Runtime DLL");
    expect(!contains(cmake, "${ONNXRUNTIME_LIB_DIR}/onnxruntime.lib")
               && !contains(cmake, "Source/Utils/WindowsDllSearchPath.cpp")
               && !contains(cmake, "ONNXRUNTIME_PROVIDERS_SHARED_DLL"),
           "The old import library, host-global DLL search mutation, and shared-provider deployment are removed");
    expect(contains(delayLoadHook, "OpenTuneOnnxRuntime_1_24_4.dll")
               && !contains(delayLoadHook, "GetModuleHandleW(L\"onnxruntime.dll\")")
               && contains(modelPathResolver, "OpenTuneOnnxRuntime_1_24_4.dll")
               && !contains(modelPathResolver, "GetModuleHandleW(L\"onnxruntime.dll\")"),
           "Runtime loading accepts only the private versioned ONNX Runtime DLL");
    expect(contains(importDefinition, "LIBRARY OpenTuneOnnxRuntime_1_24_4.dll")
               && contains(importDefinition, "OrtGetApiBase")
               && contains(importDefinition, "OrtSessionOptionsAppendExecutionProviderEx_DML"),
           "The generated import library names the private DLL and exact DML exports");
    expect(!std::filesystem::exists(sourceRoot / "Source/Utils/WindowsDllSearchPath.cpp"),
           "No source file remains that mutates the DAW process DLL search policy");
}

int main()
{
    testVisibleEntryContract();
    testArrangementForegroundScaleContract();
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
    testOpenDyneRenderPreviewContract();
    testOpenDyneToolSwitchingContract();
    testOpenDyneNoteEdgeRetreatContract();
    testOpenDyneScissorsMergeContract();
    testShortcutContract();
    testPitchModulationDriftContract();
    testAutoSnapRefactorContract();
    testAutoSnapTargetMathContract();
    testNoteTopologyContract();
    testARARestoreAutoReadContract();
    testCaptureF0KeyContract();
    testF0KeyDetectionContract();
    testPianoRollViewportSessionContract();
    testPrivateOnnxRuntimeContract();

    if (failures != 0) {
        std::cerr << failures << " reference contract test(s) failed\n";
        return 1;
    }

    std::cout << "Reference contracts passed\n";
    return 0;
}
