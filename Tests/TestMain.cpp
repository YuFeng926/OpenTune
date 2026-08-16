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

void testParameterPanelLayoutContract()
{
    const auto panelHeader = readSource("Source/Standalone/UI/ParameterPanel.h");
    const auto panel = readSource("Source/Standalone/UI/ParameterPanel.cpp");

    // 固定尺寸契约：OpenDyne 内容需求（Overdose/BlueBreeze 115px 旋钮）与面板最小高度关系
    expect(contains(panelHeader, "kMinimumContentHeight = 794"),
           "OpenDyne content need stays at 794 with 115px knobs and five tool rows");
    expect(contains(panelHeader, "kMinimumPanelHeight = kMinimumContentHeight + 40"),
           "the minimum panel height is exactly content + 40");

    // OpenDyne 工具区：9 按钮 / 5 行网格（ceil(9/2)）
    const auto resized = functionBlock(panel, "void ParameterPanel::resized");
    const auto dyneLayout = functionBlock(resized, "if (openDyneMode_)");
    expect(contains(dyneLayout, "selectToolButton_.get()")
               && contains(dyneLayout, "pitchToolButton_.get()")
               && contains(dyneLayout, "pitchModulationToolButton_.get()")
               && contains(dyneLayout, "pitchDriftToolButton_.get()")
               && contains(dyneLayout, "volumeEnvelopeToolButton_.get()")
               && contains(dyneLayout, "timeToolButton_.get()")
               && contains(dyneLayout, "scissorsToolButton_.get()")
               && contains(dyneLayout, "autoTuneToolButton_.get()")
               && contains(dyneLayout, "eqToolButton_.get()")
               && countOccurrences(dyneLayout, ".get()") == 9
               && contains(dyneLayout, "gridRows = (static_cast<int>(buttons.size()) + 1) / 2;"),
           "the OpenDyne grid lays out exactly nine tool buttons in ceil(9/2)=5 rows");

    // EQ 按钮：同时设置图标与 "EQ" 文本标签
    expect(contains(panel, "eqToolButton_->setIcon(ToolbarIcons::getEqIcon(), false)")
               && contains(panel, "eqToolButton_->setTextIcon(\"EQ\");"),
           "the EQ button sets both the icon and the EQ text label");

    // ToolIconButton 图标 + 文本组合模式：两者都设置时同时绘制
    const auto paintButton = functionBlock(
        panel, "void ParameterPanel::ToolIconButton::paintButton");
    expect(contains(paintButton, "!iconPath_.isEmpty() && textIcon_.isNotEmpty()")
               && contains(paintButton, "ToolbarIcons::drawIcon(g, iconPath_, iconArea, iconColor, 2.0f, fillIcon_)")
               && contains(paintButton, "g.drawText(textIcon_, textArea, juce::Justification::centred);"),
           "ToolIconButton paints the icon and the text label together when both are set");

    // 内容绘制唯一性：图标+文本组合条件恰出现1次（通用链唯一内容入口）
    expect(countOccurrences(paintButton, "!iconPath_.isEmpty() && textIcon_.isNotEmpty()") == 1,
           "the icon+text content guard appears exactly once (single content chain)");

    // Overdose 分支只画按钮外壳，不含 drawIcon/fillPath/strokePath/drawText 内容调用
    const auto overdoseBranch = functionBlock(paintButton, "if (themeId == ThemeId::Overdose)");
    expect(overdoseBranch.find("drawIcon") == std::string::npos
               && overdoseBranch.find("fillPath") == std::string::npos
               && overdoseBranch.find("strokePath") == std::string::npos
               && overdoseBranch.find("drawText") == std::string::npos,
           "Overdose branch draws only chrome (shadow/gradient/border), no content");

    // 通用内容链含 EQ 图文绘制
    expect(contains(paintButton, "isOverdose ? juce::Colour(Overdose::Colors::PrimaryPink)")
               && countOccurrences(paintButton, "isOverdose ? juce::Colour(Overdose::Colors::PrimaryPink)") >= 3,
           "the content chain selects Overdose content colors inline without helper extraction");
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

void testNoteEqDataContract()
{
    const auto eqSettings = readSource("Source/Utils/NoteEqSettings.h");
    const auto noteHeader = readSource("Source/Utils/Note.h");
    const auto processor = readSource("Source/DSP/NoteEqProcessor.h");

    // 契约 §2：EqSettings 保存 9 个字段
    expect(contains(eqSettings, "bool active"),
           "EqSettings carries the global active flag");
    expect(contains(eqSettings, "float lowCutFrequencyHz"),
           "EqSettings stores lowCutFrequencyHz");
    expect(contains(eqSettings, "float lowShelfFrequencyHz"),
           "EqSettings stores lowShelfFrequencyHz");
    expect(contains(eqSettings, "float lowShelfGainDb"),
           "EqSettings stores lowShelfGainDb");
    expect(contains(eqSettings, "float peakFrequencyHz"),
           "EqSettings stores peakFrequencyHz");
    expect(contains(eqSettings, "float peakGainDb"),
           "EqSettings stores peakGainDb");
    expect(contains(eqSettings, "float highShelfFrequencyHz"),
           "EqSettings stores highShelfFrequencyHz");
    expect(contains(eqSettings, "float highShelfGainDb"),
           "EqSettings stores highShelfGainDb");
    expect(contains(eqSettings, "float highCutFrequencyHz"),
           "EqSettings stores highCutFrequencyHz");

    // 契约 §2/§11：无 band 数组、无 type/Q/bypass 存储（错误草稿格式全部丢弃）
    expect(!contains(eqSettings, "EqBandSettings"),
           "no band array draft struct survives in the data contract");
    expect(!contains(eqSettings, "kNumBands"),
           "no band count constant survives in the data contract");
    expect(!contains(eqSettings, "bands["),
           "no band array member survives in the data contract");
    expect(!contains(eqSettings, "kShelfQ"),
           "fixed Q is a DSP-layer constant, not stored in the data contract");

    // 契约 §1/§2：EqSettings 默认值为用户确认值（active 开、三段增益 0 dB）
    expect(contains(eqSettings, "bool active = true"),
           "EqSettings active defaults to true");
    expect(contains(eqSettings, "float lowCutFrequencyHz = 80.0f")
               && contains(eqSettings, "float lowShelfFrequencyHz = 500.0f")
               && contains(eqSettings, "float peakFrequencyHz = 3000.0f")
               && contains(eqSettings, "float highShelfFrequencyHz = 8000.0f")
               && contains(eqSettings, "float highCutFrequencyHz = 12000.0f"),
           "cut and shelf frequencies default to 80/500/3000/8000/12000 Hz");
    expect(contains(eqSettings, "float lowShelfGainDb = 0.0f")
               && contains(eqSettings, "float peakGainDb = 0.0f")
               && contains(eqSettings, "float highShelfGainDb = 0.0f"),
           "shelf and peak gains default to 0 dB");

    // 契约 §2：Note 保持 std::optional<EqSettings> eq（nullopt = 无 EQ）
    expect(contains(noteHeader, "std::optional<EqSettings> eq"),
           "Note carries std::optional<EqSettings> eq");

    // 契约 §3：LowCut / HighCut 用 8 阶 Butterworth 级联（FilterDesign）
    expect(contains(processor, "designIIRHighpassHighOrderButterworthMethod")
               && contains(processor, "designIIRLowpassHighOrderButterworthMethod"),
           "cuts use the high-order Butterworth design");
    expect(contains(processor, "kButterworthOrder = 8"),
           "the Butterworth order is fixed at 8");

    // 契约 §3：LowShelf / Peak / HighShelf 用 RBJ 最小相位二阶段，Q 固定 2.0
    expect(contains(processor, "makeLowShelf") && contains(processor, "makePeakFilter")
               && contains(processor, "makeHighShelf"),
           "shelf and peak bands use the RBJ biquads");
    expect(contains(processor, "kShelfQ = 2.0"),
           "shelf/peak Q is fixed at 2.0");

    // 契约 §3：固定双声道契约——prepare 无声道数参数，接口收束为
    // prepare(sampleRate, settings) / reset() / process(AudioBuffer&) / isActive()
    expect(contains(processor, "void prepare(double sampleRate, const EqSettings& settings)")
               && !contains(processor, "int numChannels, const EqSettings&"),
           "prepare binds coefficients by sample rate and settings; the numChannels parameter is removed");
    expect(contains(processor, "void process(juce::AudioBuffer<float>&")
               && !contains(processor, "float* const*"),
           "process applies the AudioBuffer in place; the raw-pointer process is removed");
    expect(contains(processor, "bool isActive() const"),
           "isActive reports the bypass state");

    // 契约 §3：每声道独立滤波状态——固定 std::array 两套 ChannelState，
    // 状态结构直接表达领域顺序，无动态声道 vector
    expect(contains(processor, "struct ChannelState"),
           "per-channel state is a named fixed structure");
    expect(contains(processor, "std::array<juce::dsp::IIR::Filter<float>, kSectionsPerCut> lowCut")
               && contains(processor, "juce::dsp::IIR::Filter<float> lowShelf")
               && contains(processor, "juce::dsp::IIR::Filter<float> peak")
               && contains(processor, "juce::dsp::IIR::Filter<float> highShelf")
               && contains(processor, "std::array<juce::dsp::IIR::Filter<float>, kSectionsPerCut> highCut"),
           "ChannelState declares the fixed five-band order LowCut[4] → LowShelf → Peak → HighShelf → HighCut[4]");
    expect(contains(processor, "std::array<ChannelState, kMaxChannels> channels_")
               && !contains(processor, "std::vector<"),
           "the stereo pair is a fixed std::array; no dynamic channel vector");
    expect(contains(processor, "jassert(numChannels == 1 || numChannels == 2)"),
           "channel count is a jassert-locked mono/stereo invariant without runtime fallback");
    expect(contains(processor, "void reset()"),
           "the processor resets state at each note start");
    expect(contains(processor, "filter.reset()"),
           "reset clears every biquad state");

    // 契约 §3：处理顺序固定 LowCut → LowShelf → Peak → HighShelf → HighCut
    const auto processSample = functionBlock(processor, "float processSample(float sample)");
    const auto lowCutPos = processSample.find("lowCut");
    const auto lowShelfPos = processSample.find("lowShelf");
    const auto peakPos = processSample.find("peak");
    const auto highShelfPos = processSample.find("highShelf");
    const auto highCutPos = processSample.find("highCut");
    expect(lowCutPos != std::string::npos && lowCutPos < lowShelfPos
               && lowShelfPos < peakPos && peakPos < highShelfPos
               && highShelfPos < highCutPos,
           "processSample chains LowCut → LowShelf → Peak → HighShelf → HighCut in fixed order");

    // 契约 §3/§7.5：视觉数学不进 DSP（视觉曲线由 EqGraphRenderer 复刻 SRC）
    expect(!contains(processor, "filterResponseDb") && !contains(processor, "logGaussian")
               && !contains(processor, "lowShelfResponseDb")
               && !contains(processor, "highCutResponseDb"),
           "visual response math does not live inside the DSP");
}

void testPerNoteEqStage1Contract()
{
    const auto readSourceHeader = readSource("Source/Render/PlaybackReadSource.h");
    const auto reader = readSource("Source/Utils/PlaybackAudioReader.h");
    const auto runtime = readSource("Source/Runtime/ProcessRenderRuntime.cpp");
    const auto plannerHeader = readSource("Source/Render/RenderChunkPlanner.h");
    const auto planner = readSource("Source/Render/RenderChunkPlanner.cpp");
    const auto serviceHeader = readSource("Source/Render/ContentRenderService.h");
    const auto service = readSource("Source/Render/ContentRenderService.cpp");
    const auto processor = readSource("Source/PluginProcessor.cpp");
    const auto controller = readSource("Source/ARA/OpenTuneDocumentController.cpp");
    const auto renderCacheHeader = readSource("Source/Inference/RenderCache.h");
    const auto renderCacheSource = readSource("Source/Inference/RenderCache.cpp");
    const auto stage2 = readSource("Source/Render/Stage2TimeStretchRebuilder.cpp");

    // 契约 §4：reader 错误路径零残留
    expect(!contains(readSourceHeader, "const std::vector<Note>*")
               && !contains(readSourceHeader, "../Utils/Note.h"),
           "PlaybackReadSource no longer carries a notes member or its Note include");
    expect(!contains(reader, "applyPerNoteEq"),
           "PlaybackAudioReader no longer contains applyPerNoteEq");
    expect(!contains(reader, "NoteEqProcessor"),
           "PlaybackAudioReader no longer includes or uses NoteEqProcessor");
    const auto readPlayback = functionBlock(reader, "inline int readPlaybackAudio");
    expect(!contains(readPlayback, "Eq"),
           "readPlaybackAudio contains no EQ processing");

    // 契约 §4/§5：Stage1 唯一发布辅助——completeChunkRenderWithAudio 在
    // ProcessRenderRuntime 内只有 publishChunkWithPerNoteEq 一个调用点，
    // raw/light/vocoder 三条最终路径全部经该单一辅助
    expect(countOccurrences(runtime, "completeChunkRenderWithAudio") == 1,
           "completeChunkRenderWithAudio has exactly one call site in ProcessRenderRuntime");
    expect(countOccurrences(runtime, "publishChunkWithPerNoteEq") == 4,
           "publishChunkWithPerNoteEq has one definition and covers raw/light/vocoder publishes");

    // 契约 §6：三个 Blank 分支保留，且每个都被 active-EQ 相交排除；
    // 原始发布覆盖 pitchCurve 缺失与三个 Blank 条件
    expect(countOccurrences(runtime, "markChunkAsBlank") == 3,
           "the three blank guards remain");
    expect(countOccurrences(runtime, "publishRawWithEq") == 5,
           "raw publish covers the pitch-curve-missing guard and the three blank guards");

    // 契约 §6：三个 Blank 分支严格二选一单次 settle —— 无 active EQ 时
    // markBlank + notify 一次；有 active EQ 时只调 publishRawWithEq（Published
    // 才内部 notify，InvalidInput 只 failure，Stale 不通知），其调用后不再
    // 无条件 notify，杜绝 active-EQ raw 发布双通知
    {
        std::size_t searchFrom = 0;
        for (int i = 0; i < 3; ++i)
        {
            const char* marker = "markChunkAsBlank(relChunkStartSec, coreJob.targetRevision);";
            const auto blankPos = runtime.find(marker, searchFrom);
            expect(blankPos != std::string::npos, "blank guard marker remains");
            if (blankPos == std::string::npos)
                break;
            const auto returnPos = runtime.find("return;", blankPos);
            expect(returnPos != std::string::npos, "blank guard ends with a return");
            if (returnPos == std::string::npos)
                break;
            const auto segment = runtime.substr(blankPos, returnPos - blankPos);
            expect(countOccurrences(segment, "notifyChunkSettled") == 1,
                   "blank guard settles exactly once, inside the no-EQ arm");
            expect(countOccurrences(segment, "publishRawWithEq") == 1
                       && segment.find("notifyChunkSettled") < segment.find("publishRawWithEq"),
                   "active-EQ arm calls publishRawWithEq only; no notify follows it");
            searchFrom = returnPos + 7;
        }
    }

    // 契约 §4：Planner 保护范围——silent-gap 分割与最大块分割都不得落入
    // active-EQ Note 保护范围内部
    expect(contains(plannerHeader, "struct ProtectedRange"),
           "RenderChunkPlanner declares a sample-domain ProtectedRange");
    expect(contains(planner, "normalizeProtectedRanges")
               && contains(planner, "insideProtectedRange")
               && contains(planner, "advancePastProtectedRanges"),
           "planner normalizes protected ranges once and advances every split past them");

    // 契约 §1：两个 enqueueRender 调用点把已有 snapshot->notes 传入；
    // stale-generation 不再走旧 snapshot 重规划：requeueRenderChunk 只回退状态机
    expect(contains(processor, "enqueueRender(std::move(job), snap->notes)"),
           "Standalone enqueueRender passes snap->notes");
    expect(contains(controller, "enqueueRender(std::move(job), snap->notes)"),
           "ARA enqueueRender passes snap->notes");
    expect(contains(runtime, "crs->requeueRenderChunk(coreJob);"),
           "stale-generation requeue routes through requeueRenderChunk, not the notes snapshot replan");
    expect(!contains(runtime, "enqueueRender"),
           "ProcessRenderRuntime keeps zero residual of the notes-snapshot enqueueRender replan");
    expect(contains(serviceHeader,
                    "void enqueueRender(RenderJob job, const std::vector<Note>& notes)"),
           "enqueueRender takes notes by const reference (read-only, no copy/storage)");

    // 契约 §4：RenderCache / Stage2 无 EQ 处理
    expect(!contains(renderCacheHeader, "NoteEqProcessor")
               && !contains(renderCacheSource, "NoteEqProcessor"),
           "RenderCache has no EQ processing");
    expect(!contains(stage2, "NoteEqProcessor"),
           "Stage2 has no EQ processing");

    // 契约 §7：EQ 采样率唯一性——Stage1 音频固定 44.1kHz（canonical）。
    // Capture binding 是播放源唯一汇聚点：44.1k 输入直接共享原 buffer，非同率
    // 经既有 upsampleForHost 重采样（时长守恒方式与 prepareImport/ARA 一致），
    // readSource 恒以 TimeCoordinate::kRenderSampleRate 发布；
    // 两个 EQ helper 的 Note 边界与 prepare 固定用 RenderCache::kSampleRate，
    // 无 audioSampleRate 参数；执行读取点成对刷新 audioBuffer/audioSampleRate。
    const auto publishBinding = functionBlock(
        processor, "bindings.publishPlaybackSource = [this](const ContentKey& key,");
    expect(contains(publishBinding, "const double targetRate = TimeCoordinate::kRenderSampleRate;")
               && contains(publishBinding, "readSource.audioSampleRate = targetRate;"),
           "capture binding derives the canonical read source at the fixed render rate");
    expect(contains(publishBinding, "upsampleForHost")
               && contains(publishBinding, "TimeCoordinate::secondsToSamples(")
               && contains(publishBinding, "TimeCoordinate::samplesToSeconds(originalLen, sampleRate)"),
           "non-44.1k capture reuses upsampleForHost with the duration-conserving length");
    expect(contains(publishBinding, "canonicalAudio = std::move(audio);")
               && contains(publishBinding, "readSource.audioBuffer = std::move(canonicalAudio);"),
           "44.1k input shares the original buffer; the binding publishes the canonical buffer once");

    const auto intersects = functionBlock(
        runtime, "bool chunkIntersectsActiveEqNote");
    const auto publish = functionBlock(
        runtime, "RenderCache::ChunkRenderResult publishChunkWithPerNoteEq");
    expect(!contains(intersects, "audioSampleRate") && !contains(publish, "audioSampleRate"),
           "the per-note EQ helpers take no audioSampleRate parameter");
    expect(contains(intersects, "RenderCache::kSampleRate")
               && contains(publish, "RenderCache::kSampleRate"),
           "the per-note EQ helpers convert note boundaries with the fixed RenderCache::kSampleRate");
    expect(contains(publish, "processor.prepare(RenderCache::kSampleRate, *note.eq);"),
           "NoteEqProcessor::prepare binds coefficients at the fixed render rate");
    expect(contains(runtime, "job.audioBuffer = readSource.audioBuffer;")
               && contains(runtime, "job.audioSampleRate = readSource.audioSampleRate;"),
           "the execution read point refreshes audioBuffer and audioSampleRate as a pair");
    expect(countOccurrences(runtime, "coreJob.audioSampleRate") == 0,
           "coreJob.audioSampleRate has zero residual in ProcessRenderRuntime");

    // 契约 §7：project-open 恢复走同一 canonical 链——applySnapshot 经
    // prepareImport（"project-open" tag）重建 Source，落库固定
    // kRenderSampleRate；clip 恢复与 PlaybackReadSource 发布都从该
    // canonical 快照读取，无第二重采样路径
    const auto projectSession = readSource("Source/Utils/ProjectSession.cpp");
    const auto applySnapshot = functionBlock(
        projectSession, "Result<void> ProjectSession::applySnapshot(const ProjectSnapshot& snapshot)");
    expect(contains(applySnapshot, "prepareImport(std::move(*audioBuffer), loadedSampleRate,")
               && contains(applySnapshot, "\"project-open\""),
           "project-open rebuilds sources through the single prepareImport canonical chain");
    expect(contains(applySnapshot, "req.sampleRate = TimeCoordinate::kRenderSampleRate;"),
           "restored sources are stored at the canonical kRenderSampleRate");
    expect(contains(applySnapshot, "readSource.audioBuffer = snap->audioBuffer;")
               && contains(applySnapshot, "readSource.audioSampleRate = snap->audioSampleRate;")
               && contains(applySnapshot, "crs->publishPlaybackSource(key, std::move(readSource));"),
           "applySnapshot republishes PlaybackReadSource from the canonical content snapshot");

    // 契约 §8：唯一原子计划入口——RenderCache 暴露 PlannedChunk 与
    // reconcileFullPlanAndRequest(fullPlan, requestStart, requestEnd)，
    // 旧 requestRenderPending 调度在 RenderCache 与 Service 中零残留
    expect(contains(renderCacheHeader, "struct PlannedChunk {")
           && contains(renderCacheHeader,
                       "std::size_t reconcileFullPlanAndRequest(const std::vector<PlannedChunk>& fullPlan,"),
           "RenderCache exposes PlannedChunk and the single atomic plan-reconcile entry");
    expect(!contains(renderCacheHeader, "requestRenderPending")
           && !contains(renderCacheSource, "requestRenderPending")
           && !contains(service, "requestRenderPending"),
           "requestRenderPending has zero residual in RenderCache and ContentRenderService");
    expect(countOccurrences(service, "reconcileFullPlanAndRequest") == 1,
           "ContentRenderService calls the plan-reconcile entry exactly once per enqueueRender");

    // 契约 §8：ContentRenderService 以 0..contentSampleCount 生成完整计划、
    // 原子 reconcile 一次，并按返回的 jobTokenCount 投递 worker jobs
    const auto enqueue = functionBlock(
        service, "void ContentRenderService::enqueueRender");
    const auto planPos = enqueue.find("RenderChunkPlanner::selectChunksIntersectingRange(");
    const auto planEnd = enqueue.find("kRenderHopSize);", planPos);
    const auto planArgs = (planPos != std::string::npos && planEnd != std::string::npos)
        ? enqueue.substr(planPos, planEnd - planPos) : std::string();
    expect(!planArgs.empty() && countOccurrences(planArgs, "contentSampleCount") == 2
           && contains(planArgs, "0,"),
           "the full plan and the request window span the whole content 0..contentSampleCount");
    expect(contains(enqueue, "const std::size_t jobTokenCount = job.renderCache->reconcileFullPlanAndRequest(")
           && contains(enqueue, "fullPlan, job.startSample, job.endSampleExclusive);"),
           "the reconcile call folds the full plan with the local request window");
    expect(contains(enqueue, "for (std::size_t i = 0; i < jobTokenCount; ++i)")
           && contains(enqueue, "renderWorker_.enqueue(std::move(subJob));"),
           "the service dispatches exactly the returned job token count to the worker");

    // 契约 §8：completeChunkRenderWithAudio 做完整身份校验——key 缺失、
    // span 不符、revision 不符三个失败路径统一返回 Stale（不落 audio）
    const auto complete = functionBlock(
        renderCacheSource,
        "RenderCache::ChunkRenderResult RenderCache::completeChunkRenderWithAudio");
    expect(contains(complete, "if (chunk.startSample != startSample || chunk.endSampleExclusive != endSampleExclusive)")
           && contains(complete, "if (chunk.runningRevision != revision)"),
           "complete validates both the chunk span and the running revision identity");
    expect(countOccurrences(complete, "return ChunkRenderResult::Stale;") == 3,
           "unknown keys, mismatched spans and mismatched revisions all settle as Stale");

    // 契约 §8：stale-generation 只回退状态机——requeueRunningChunk 仅做
    // Running→Pending 与 revision 清零，不 bump desired、不改几何、不发布快照
    const auto requeueChunk = functionBlock(
        renderCacheSource, "bool RenderCache::requeueRunningChunk");
    expect(contains(requeueChunk, "chunk.status != Chunk::Status::Running")
               && contains(requeueChunk, "chunk.runningRevision != runningRevision"),
           "requeueRunningChunk requeues only a chunk still Running whose revision matches");
    expect(contains(requeueChunk, "chunk.status = Chunk::Status::Pending;")
               && contains(requeueChunk, "chunk.runningRevision = 0;")
               && contains(requeueChunk, "pendingChunks_.insert(startSeconds);"),
           "requeueRunningChunk performs only the Running to Pending transition and re-enqueues the span");
    expect(!contains(requeueChunk, "desiredRevision")
               && !contains(requeueChunk, "reconcile")
               && !contains(requeueChunk, "publishLocked"),
           "requeueRunningChunk never bumps desired, reconciles the plan or republishes a snapshot");

    // 契约 §8：requeueRenderChunk 成功回退后只投递一个 job token，worker 下轮
    // 从 PendingJob 重拉 span/revision，owner 回调抓当前 snapshot
    const auto requeueJob = functionBlock(
        service, "void ContentRenderService::requeueRenderChunk");
    expect(contains(requeueJob, "requeueRunningChunk(job.startSeconds, job.targetRevision)")
               && countOccurrences(requeueJob, "renderWorker_.enqueue") == 1,
           "requeueRenderChunk enqueues exactly one worker job token after the state rollback");

    // 契约 §8：hop 单一来源——RenderChunkPlanner::kRenderHopSize 是 planner、service
    // 与 runtime freeze 的唯一渲染 hop；无局部 512、无 workerHopSize、无 cfg hop 兜底
    expect(contains(plannerHeader, "kRenderHopSize = 512"),
           "RenderChunkPlanner owns the single render hop size 512");
    expect(!contains(service, "kHopSize")
               && contains(service, "RenderChunkPlanner::kRenderHopSize"),
           "ContentRenderService consumes the planner hop; its local 512 constant is removed");
    expect(contains(runtime, "RenderChunkPlanner::kRenderHopSize")
               && !contains(runtime, "workerHopSize"),
           "Runtime freeze uses the planner hop; workerHopSize and the cfg hop fallback are removed");

    // 契约 §8：VocoderConfig 不再携带 hopSize；真实 mel/vocoder 分支前唯一一次
    // acquireVocoderConfig（raw 四分支与 light 路径不加载模型）
    const auto renderJobBlock = functionBlock(
        runtime, "void ProcessRenderRuntime::processChunkRenderJob");
    expect(countOccurrences(renderJobBlock, "acquireVocoderConfig(") == 1,
           "acquireVocoderConfig is called exactly once in the render job path");
    const auto rawPos = renderJobBlock.find("publishRawWithEq");
    const auto lightPos = renderJobBlock.find(
        "lightPitchEnabled && contentSnap->pitchShiftSettings.isIdentity()");
    const auto acquirePos = renderJobBlock.find("acquireVocoderConfig(");
    const auto melPos = renderJobBlock.find("computeLogMelSpectrogram");
    expect(rawPos != std::string::npos && lightPos != std::string::npos
               && acquirePos != std::string::npos && melPos != std::string::npos
               && rawPos < acquirePos && lightPos < acquirePos && acquirePos < melPos,
           "acquireVocoderConfig runs after the raw branches and the light path, "
           "and before the real mel/vocoder branch");
    const auto acquireBlock = functionBlock(
        runtime, "bool ProcessRenderRuntime::acquireVocoderConfig");
    expect(!contains(acquireBlock, "hopSize"),
           "VocoderConfig no longer carries hopSize; the freeze hop is the planner constant");
}

// ── 阶段 D：Per-note EQ 持久化契约 ──
// 三个载体（Project ValueTree v5 / Capture 二进制流 v8 / ARA XML payload v5）
// 统一只读写 Note.eq：写端 9 个 scalar 字段、读端逐项 schema 门控 + 字段恢复，
// 历史 Band 草稿（kNumBands/EqBandSettings/"bands"）零残留，无独立 eqSettings 内存字段。
void testPerNoteEqPersistenceContract()
{
    static const char* const eqFields[] = {
        "active", "lowCutFrequencyHz", "lowShelfFrequencyHz", "lowShelfGainDb",
        "peakFrequencyHz", "peakGainDb", "highShelfFrequencyHz", "highShelfGainDb",
        "highCutFrequencyHz",
    };
    static const char* const eqFloatFields[] = {
        "lowCutFrequencyHz", "lowShelfFrequencyHz", "lowShelfGainDb",
        "peakFrequencyHz", "peakGainDb", "highShelfFrequencyHz", "highShelfGainDb",
        "highCutFrequencyHz",
    };

    // ── Project（ValueTree，v5）──
    const auto projectHeader = readSource("Source/Utils/ProjectPersistence.h");
    const auto project = readSource("Source/Utils/ProjectPersistence.cpp");
    const auto projectModel = readSource("Source/Utils/ProjectModel.h");

    expect(contains(projectHeader, "kCurrentProjectFormatVersion = 5"),
           "Project format v5 carries the per-note EQ contract");
    expect(contains(projectHeader, "kMinimumProjectFormatVersion = 3"),
           "Project minimum format version stays 3");

    const auto notesToTree = functionBlock(
        project, "juce::ValueTree ProjectPersistence::notesToValueTree");
    const auto notesFromTree = functionBlock(
        project, "std::vector<Note> ProjectPersistence::notesFromValueTree");
    expect(contains(notesToTree, "if (note.eq.has_value())"),
           "Project EQ write branches on note.eq");
    for (const char* field : eqFields) {
        const std::string token = std::string("eqTree.setProperty(\"") + field + "\"";
        expect(contains(notesToTree, token.c_str()),
               "Project EQ write emits the nine scalar properties");
    }
    for (const char* field : eqFields) {
        const std::string token = std::string("eqTree.hasProperty(\"") + field + "\")";
        expect(contains(notesFromTree, token.c_str()),
               "Project EQ read gates each field with hasProperty");
    }
    for (const char* field : eqFields) {
        const std::string token = std::string("eqTree.getProperty(\"") + field + "\"";
        expect(contains(notesFromTree, token.c_str()),
               "Project EQ read restores each field value");
    }
    expect(!contains(notesFromTree, "getNumProperties"),
           "Project EQ read gate is per-property hasProperty, not a property count");
    expect(contains(notesFromTree, "note.eq = eq;"),
           "Project EQ restore lands on note.eq");

    // ── 版本契约：formatVersion 从根节点显式穿透，EQ 恢复门控 formatVersion>=5 ──
    expect(contains(projectHeader,
                    "contentFromValueTree(const juce::ValueTree& tree, int formatVersion)"),
           "contentFromValueTree receives the project format version");
    expect(contains(projectHeader,
                    "notesFromValueTree(const juce::ValueTree& tree, int formatVersion)"),
           "notesFromValueTree receives the project format version");
    expect(contains(projectHeader,
                    "referenceFeaturesFromValueTree(const juce::ValueTree& tree, int formatVersion)"),
           "referenceFeaturesFromValueTree receives the project format version");
    expect(contains(notesFromTree, "formatVersion >= 5"),
           "Project EQ restore gates on the project format version >= 5");
    const auto contentFromTree = functionBlock(
        project, "ProjectContentEntry ProjectPersistence::contentFromValueTree");
    expect(contains(contentFromTree, "notesFromValueTree(tree.getChildWithName(\"Notes\"), formatVersion)"),
           "Content EQ restore threads the format version into notes");
    expect(contains(contentFromTree, "referenceFeaturesFromValueTree(rfTree, formatVersion)"),
           "Content EQ restore threads the format version into reference features");
    const auto rfFromTree = functionBlock(
        project,
        "ProjectContentEntry::ReferenceFeatureEntry ProjectPersistence::referenceFeaturesFromValueTree");
    expect(contains(rfFromTree, "notesFromValueTree(tree.getChildWithName(\"PitchNotes\"), formatVersion)"),
           "Reference-feature EQ restore threads the format version into pitch notes");

    expect(!contains(project, "kNumBands") && !contains(project, "EqBandSettings")
               && !contains(project, "\"bands\""),
           "Project persistence has zero band-array draft structures");
    expect(contains(projectModel, "std::vector<Note> notes"),
           "ProjectContentEntry carries EQ only through Note.eq");
    expect(!contains(projectModel, "eqSettings"),
           "ProjectContentEntry has no standalone eqSettings memory field");

    // ── Capture（二进制流，v8）──
    const auto capture = readSource("Source/Plugin/Capture/CapturePersistence.cpp");
    expect(contains(capture, "kCaptureArchiveVersion = 8"),
           "Capture v8 adds the per-note EQ stream");
    expect(contains(capture, "hasPerNoteEq = (fileVersion >= 8)"),
           "Capture EQ gate derives from fileVersion >= 8");
    const auto captureSerialize = functionBlock(
        capture, "juce::MemoryBlock CapturePersistence::serialize");
    const auto captureDeserialize = functionBlock(
        capture, "bool CapturePersistence::deserialize");

    // presence 标记写读 + v7 短路：hasPerNoteEq==false 时 readInt 都不执行
    expect(contains(captureSerialize, "stream.writeInt(note.eq.has_value() ? 1 : 0)")
               && contains(captureDeserialize, "if (hasPerNoteEq && stream.readInt() == 1)"),
           "Capture EQ presence is written from note.eq and read behind the hasPerNoteEq short-circuit");

    // presence + 9 字段写读严格同序：写端与读端 token 位置序列逐项同序
    bool writeOrder = true;
    bool readOrder = true;
    std::size_t writePos = captureSerialize.find("stream.writeInt(note.eq.has_value() ? 1 : 0)");
    std::size_t readPos = captureDeserialize.find("if (hasPerNoteEq && stream.readInt() == 1)");
    if (writePos == std::string::npos) writeOrder = false;
    if (readPos == std::string::npos) readOrder = false;
    for (const char* field : eqFields) {
        const std::string token = std::string("eq.") + field;
        const auto w = captureSerialize.find(token, writePos);
        const auto r = captureDeserialize.find(token, readPos);
        if (w == std::string::npos) writeOrder = false;
        if (r == std::string::npos) readOrder = false;
        if (w != std::string::npos) writePos = w + token.size();
        if (r != std::string::npos) readPos = r + token.size();
    }
    expect(writeOrder,
           "Capture EQ write order is presence then the nine fields in contract order");
    expect(readOrder,
           "Capture EQ read order is presence then the nine fields in contract order");
    expect(contains(captureDeserialize, "note.eq = eq;"),
           "Capture EQ restore lands on note.eq");
    expect(!contains(capture, "kNumBands") && !contains(capture, "EqBandSettings")
               && !contains(capture, "\"bands\""),
           "Capture persistence has zero band-array draft structures");

    // ── ARA（XML payload，v5 / min3）──
    const auto ara = readSource("Source/ARA/OpenTuneDocumentController.cpp");
    expect(contains(ara, "kContentPayloadArchiveVersion = 5"),
           "ARA archive v5 carries the per-note EQ contract");
    expect(contains(ara, "kContentPayloadArchiveVersionMin = 3"),
           "ARA archive minimum version stays 3");
    expect(contains(ara, "restoreAudioModificationContent(*xml, filter, version)"),
           "ARA restore receives the archive version for schema gating");
    const auto restoreContent = functionBlock(
        ara, "std::optional<AudioModificationContentState> restoreAudioModificationContent");
    expect(contains(restoreContent, "int archiveVersion"),
           "restoreAudioModificationContent accepts the archive version parameter");
    expect(contains(restoreContent, "if (archiveVersion >= 5)"),
           "ARA EQ restore is gated on archiveVersion >= 5");
    for (const char* field : eqFields) {
        const std::string token = std::string("eqEl->hasAttribute(\"") + field + "\")";
        expect(contains(restoreContent, token.c_str()),
               "ARA EQ read gates each field with hasAttribute");
    }
    expect(!contains(restoreContent, "getNumAttributes"),
           "ARA EQ schema gate is per-attribute hasAttribute, not an attribute count");
    expect(contains(restoreContent, "eqEl->getIntAttribute(\"active\")"),
           "ARA EQ restore reads active without a default value");
    for (const char* field : eqFloatFields) {
        const std::string token = std::string("eqEl->getDoubleAttribute(\"") + field + "\")";
        expect(contains(restoreContent, token.c_str()),
               "ARA EQ restore reads each float field");
    }
    expect(contains(restoreContent, "note.eq = eq;"),
           "ARA EQ restore lands on note.eq");
    const auto serializeContent = functionBlock(
        ara, "void serializeAudioModificationContent");
    expect(contains(serializeContent, "eqEl->setAttribute(\"active\""),
           "ARA EQ write emits the active attribute");
    for (const char* field : eqFloatFields) {
        const std::string token = std::string("eqEl->setAttribute(\"") + field + "\"";
        expect(contains(serializeContent, token.c_str()),
               "ARA EQ write emits the eight float attributes");
    }
    expect(!contains(ara, "kNumBands") && !contains(ara, "EqBandSettings")
               && !contains(ara, "\"bands\""),
           "ARA archive has zero band-array draft structures");

    // ── 三载体统一：EQ 只挂 Note.eq，无独立 eqSettings 内存字段 ──
    expect(!contains(project, "eqSettings") && !contains(capture, "eqSettings")
               && !contains(ara, "eqSettings"),
           "All three carriers persist EQ only through Note.eq; no standalone eqSettings field");

    // ── PluginProcessor：OTST 状态版本保持 9（EQ 随 Note 走，无独立状态块）──
    const auto processor = readSource("Source/PluginProcessor.cpp");
    expect(contains(processor, "kProcessorStateMagic = 0x4F545354"),
           "OTST processor state magic stays 0x4F545354");
    expect(contains(processor, "kProcessorStateVersion = 9"),
           "OTST processor state version stays 9 for the per-note EQ contract");
}

// ── 阶段 E：Per-note EQ 工具契约 ──
// E 键工具：ShortcutId::Eq 可配置默认 E，两模式共享；ToolHandler 点击/框选两入口
// 各打开一次 EQ 预览弹窗；PianoRollComponent 唯一持有弹窗并直通提交链；
// 提交只走秒域 local-mutation 调度，Undo/Redo 仍只调 commitNoteTopologyPatch。
void testPerNoteEqToolContract()
{
    // ── 1. ShortcutId::Eq 枚举位置与默认绑定（ToolODScissors 后 / Count 前）──
    const auto keyShortcut = readSource("Source/Utils/KeyShortcutConfig.h");
    const auto toolODPos = keyShortcut.find("ToolODScissors");
    const auto eqEnumPos = keyShortcut.find("Eq,");
    const auto countPos = keyShortcut.find("Count");
    expect(toolODPos != std::string::npos && eqEnumPos != std::string::npos
               && countPos != std::string::npos && toolODPos < eqEnumPos && eqEnumPos < countPos,
           "ShortcutId::Eq is appended after ToolODScissors and before Count");
    expect(contains(keyShortcut, "{ ShortcutId::Eq, Loc::Keys::kToolEq, { KeyBinding('E', {}) } }"),
           "Eq defaults to the E key in KeyShortcutConfig");
    const auto prefs = readSource("Source/Utils/AppPreferences.cpp");
    const auto prefsHeader = readSource("Source/Utils/AppPreferences.h");
    expect(!contains(prefs, "KeyBinding('E'") && !contains(prefsHeader, "KeyBinding('E'"),
           "The E default binding lives only in KeyShortcutConfig");

    // ── 2. AppPreferences：storage key / suppression key / load / write / setter ──
    const auto toolODKeyPos = prefs.find("\"shared.shortcuts.toolODScissors\"");
    const auto toolEqKeyPos = prefs.find("\"shared.shortcuts.toolEq\"");
    expect(toolODKeyPos != std::string::npos && toolEqKeyPos != std::string::npos
               && toolODKeyPos < toolEqKeyPos,
           "AppPreferences Eq storage key follows the Scissors key in the shared shortcut array");
    expect(contains(prefs, "kSharedEqSuppressRemoveConfirmationKey = \"shared.eq.suppressRemoveConfirmation\""),
           "EQ remove confirmation has its own suppression storage key");
    expect(contains(prefs, "state.shared.suppressEqRemoveConfirmation = properties.getBoolValue("),
           "AppPreferences load restores the EQ suppression flag");
    expect(contains(prefs, "properties.setValue(kSharedEqSuppressRemoveConfirmationKey,"),
           "AppPreferences write persists the EQ suppression flag");
    expect(contains(prefsHeader, "void setSuppressEqRemoveConfirmation(bool suppress);")
               && contains(prefs, "void AppPreferences::setSuppressEqRemoveConfirmation(bool suppress)"),
           "The EQ suppression flag has a single AppPreferences setter");

    // ── 3. SharedPreferencePages generalIds 与 Localization ──
    const auto sharedPages = readSource("Source/Editor/Preferences/SharedPreferencePages.cpp");
    const auto generalStart = sharedPages.find("generalIds_ = {");
    const auto generalEnd = sharedPages.find("};", generalStart);
    const auto generalIds = (generalStart != std::string::npos
                             && generalEnd != std::string::npos)
        ? sharedPages.substr(generalStart, generalEnd - generalStart)
        : std::string();
    expect(contains(generalIds, "KeyShortcutConfig::ShortcutId::Eq,"),
           "Shortcut preference General group includes Eq");
    const auto localization = readSource("Source/Utils/LocalizationManager.h");
    expect(contains(localization, "kToolEq = \"Tool: EQ\""),
           "Localization exposes the Eq tool display name as Tool: EQ");

    // ── 4. ToolHandler：Context 回调 / 两模式共享 / 交互分派 ──
    const auto toolHandlerHeader = readSource(
        "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h");
    const auto toolHandler = readSource(
        "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    expect(contains(toolHandlerHeader, "std::function<void(int)> openEqPreview;")
               && contains(toolHandlerHeader, "std::function<juce::MouseCursor()> getEqCursor;"),
           "ToolHandler Context carries the Eq preview-open and cursor callbacks");
    const auto keyPressed = functionBlock(
        toolHandler, "bool PianoRollToolHandler::keyPressed");
    expect(contains(keyPressed, "KeyShortcutConfig::ShortcutId::Eq")
               && contains(keyPressed, "ctx_.setCurrentTool(ToolId::Eq)"),
           "ToolHandler Eq shortcut switches to the Eq tool");
    // 两模式共享：Eq 分支位于 !isOpenDyne gate 之外
    {
        const auto gatePos = keyPressed.find("if (!isOpenDyne)");
        std::string gateBlock;
        if (gatePos != std::string::npos) {
            const auto bracePos = keyPressed.find('{', gatePos);
            if (bracePos != std::string::npos) {
                int depth = 0;
                for (std::size_t index = bracePos; index < keyPressed.size(); ++index) {
                    if (keyPressed[index] == '{')
                        ++depth;
                    else if (keyPressed[index] == '}' && --depth == 0) {
                        gateBlock = keyPressed.substr(bracePos, index - bracePos + 1);
                        break;
                    }
                }
            }
        }
        expect(!gateBlock.empty() && contains(gateBlock, "ShortcutId::ToolDrawNote")
                   && !contains(gateBlock, "ShortcutId::Eq"),
               "The Eq shortcut stays outside the !isOpenDyne gate and is shared by both schemes");
    }
    const auto mouseMove = functionBlock(
        toolHandler, "void PianoRollToolHandler::mouseMove");
    expect(contains(mouseMove, "currentTool_ == ToolId::Eq")
               && contains(mouseMove, "ctx_.setMouseCursor(ctx_.getEqCursor())"),
           "mouseMove applies the Eq cursor via getEqCursor");
    // empty-space 手势与 Select 复用：mouseDown 前置空区意图、drag/up 开头消费
    const auto mouseDown = functionBlock(
        toolHandler, "void PianoRollToolHandler::mouseDown");
    const auto mouseDrag = functionBlock(
        toolHandler, "void PianoRollToolHandler::mouseDrag");
    const auto mouseUp = functionBlock(
        toolHandler, "void PianoRollToolHandler::mouseUp");
    const auto isEmptySpace = functionBlock(
        toolHandler, "bool PianoRollToolHandler::isEmptySpaceMouseDown");
    expect(contains(mouseDown, "if (isEmptySpaceMouseDown(e))")
               && contains(mouseDown, "beginEmptySpaceIntent(e);")
               && mouseDown.find("isEmptySpaceMouseDown(e)") < mouseDown.find("switch (currentTool_)"),
           "Eq empty-space mouseDown enters the shared empty-space intent before tool dispatch");
    expect(contains(mouseDrag, "consumeEmptySpaceIntentDrag(e)")
               && contains(mouseUp, "consumeEmptySpaceIntentUp(e)"),
           "Eq empty-space drag and up reuse the Select empty-space intent paths");
    expect(!contains(isEmptySpace, "ToolId::Eq"),
           "isEmptySpaceMouseDown does not exclude the Eq tool from empty-space gestures");
    // 点击记 pending 主音符；拖拽超阈值转框选；mouseUp 与框选完成各打开一次
    const auto eqDown = functionBlock(
        toolHandler, "void PianoRollToolHandler::handleEqToolMouseDown");
    const auto eqDrag = functionBlock(
        toolHandler, "void PianoRollToolHandler::handleEqToolMouseDrag");
    const auto eqUp = functionBlock(
        toolHandler, "void PianoRollToolHandler::handleEqToolMouseUp");
    expect(contains(eqDown, "noteSelection.setSingle(clickedNoteIndex, noteCount)")
               && contains(eqDown, "pendingEqPrimaryIndex_ = clickedNoteIndex;"),
           "Eq click records the pending primary note without opening the popup");
    expect(contains(eqDrag, "pendingEqPrimaryIndex_ = -1;")
               && contains(eqDrag, "beginAreaSelection(startEvent);")
               && contains(eqDrag, "handleSelectDrag(e);"),
           "Eq drag past the threshold clears the pending note and becomes an area selection");
    expect(contains(eqUp, "if (pendingEqPrimaryIndex_ >= 0)")
               && contains(eqUp, "ctx_.openEqPreview(primaryIndex);")
               && countOccurrences(eqUp, "ctx_.openEqPreview") == 1,
           "Eq mouseUp opens the preview exactly once from the pending primary note");
    const auto selectUp = functionBlock(
        toolHandler, "void PianoRollToolHandler::handleSelectUp");
    expect(contains(selectUp, "currentTool_ == ToolId::Eq && !ctx_.getState().noteSelection.empty()")
               && contains(selectUp, "ctx_.openEqPreview(ctx_.getState().noteSelection.anchorIndex);")
               && countOccurrences(selectUp, "ctx_.openEqPreview") == 1,
           "Area-selection completion opens the Eq preview once from the anchor note");
    // 无 EQ 日志空壳：EQ 分派直接进真实 handler
    expect(!contains(eqDown, "AppLogger") && !contains(eqDrag, "AppLogger")
               && !contains(eqUp, "AppLogger"),
           "Eq tool handlers carry no logging stubs");
    {
        const auto casePos = mouseDown.find("case ToolId::Eq:");
        const auto breakPos = casePos != std::string::npos
            ? mouseDown.find("break;", casePos) : std::string::npos;
        const auto eqCase = (casePos != std::string::npos && breakPos != std::string::npos)
            ? mouseDown.substr(casePos, breakPos - casePos) : std::string();
        expect(contains(eqCase, "handleEqToolMouseDown(e);") && !contains(eqCase, "AppLogger"),
               "Eq mouseDown dispatches to the real handler, not a logging stub");
    }

    // ── 5. PianoRollComponent：唯一持有者 / 直通打开 / 提交链 ──
    const auto pianoRollHeader = readSource("Source/Standalone/UI/PianoRollComponent.h");
    const auto pianoRoll = readSource("Source/Standalone/UI/PianoRollComponent.cpp");
    expect(contains(pianoRollHeader, "std::unique_ptr<EqPopupComponent> eqPopup_;"),
           "PianoRollComponent is the single owner of the Eq popup");
    expect(countOccurrences(pianoRoll, "std::make_unique<EqPopupComponent>") == 1,
           "The Eq popup is instantiated exactly once, owned by PianoRollComponent");
    const auto buildContext = functionBlock(
        pianoRoll, "PianoRollToolHandler::Context PianoRollComponent::buildToolHandlerContext");
    expect(contains(buildContext,
                    "toolCtx.openEqPreview = [this](int primaryIndex) { openEqPopupForSelection(primaryIndex); };")
               && contains(buildContext, "toolCtx.getEqCursor = [this]() { return getEqCursor(); };"),
           "The ToolHandler primary index passes straight through to openEqPopupForSelection");
    const auto openPopup = functionBlock(
        pianoRoll, "void PianoRollComponent::openEqPopupForSelection");
    expect(contains(openPopup, "const Note& primary = notes[primaryIndex];")
               && contains(openPopup, "primary.eq.value_or(EqSettings{})"),
           "The popup opens from the primary note eq with a default fallback");
    expect(!contains(openPopup, "findNoteIndexAt") && !contains(openPopup, "beginNoteDraft"),
           "Opening the popup re-hits nothing by coordinates and starts no draft");
    const auto applyEq = functionBlock(
        pianoRoll, "void PianoRollComponent::applyEqSettingsToSelection");
    const auto removeEq = functionBlock(
        pianoRoll, "void PianoRollComponent::removeEqFromSelection");
    const auto eqChainOrder = contains(applyEq, "beginNoteDraft()")
        && contains(applyEq, "working[idx].eq = settings;")
        && contains(applyEq, "contentDirty = true;")
        && contains(applyEq, "commitNoteDraft();")
        && applyEq.find("beginNoteDraft()") < applyEq.find("working[idx].eq = settings;")
        && applyEq.find("working[idx].eq = settings;") < applyEq.find("contentDirty = true;")
        && applyEq.find("contentDirty = true;") < applyEq.find("commitNoteDraft();");
    expect(eqChainOrder,
           "Eq commit runs the draft chain beginNoteDraft -> eq assign -> contentDirty -> commitNoteDraft");
    const auto eqRemoveChainOrder = contains(removeEq, "beginNoteDraft()")
        && contains(removeEq, "working[idx].eq = std::nullopt;")
        && contains(removeEq, "contentDirty = true;")
        && contains(removeEq, "commitNoteDraft();")
        && removeEq.find("beginNoteDraft()") < removeEq.find("working[idx].eq = std::nullopt;")
        && removeEq.find("working[idx].eq = std::nullopt;") < removeEq.find("contentDirty = true;")
        && removeEq.find("contentDirty = true;") < removeEq.find("commitNoteDraft();");
    expect(eqRemoveChainOrder,
           "Eq removal runs the draft chain beginNoteDraft -> eq reset -> contentDirty -> commitNoteDraft");
    const auto commitDraft = functionBlock(
        pianoRoll, "bool PianoRollComponent::commitNoteDraft");
    expect(contains(commitDraft, "a.eq == b.eq"),
           "notesContentEqual compares a.eq == b.eq");
    expect(contains(commitDraft, "lastKnownNotesRevision_ = committedSnap->notesRevision;")
               && contains(commitDraft, "listener.contentEdited();"),
           "The Eq commit updates notesRevision and flags content edited");
    const auto commands = readSource("Source/Content/ContentEditCommands.h");
    expect(!contains(commands, "Eq") && !contains(commands, "eq"),
           "ContentEditCommands exposes no EQ-specific commit interface");

    // ── 6. EQ cursor：ToolbarIcons 图标 + CursorThemeManager 主题化 ──
    const auto getCursor = functionBlock(
        pianoRoll, "juce::MouseCursor PianoRollComponent::getEqCursor");
    expect(contains(getCursor, "CursorThemeManager::getInstance().resolveCursor(")
               && contains(getCursor, "ToolbarIcons::createEqIconImage()"),
           "The Eq cursor builds from ToolbarIcons::createEqIconImage through CursorThemeManager");
    const auto setCurrentTool = functionBlock(
        pianoRoll, "void PianoRollComponent::setCurrentTool");
    {
        const auto eqCasePos = setCurrentTool.find("case ToolId::Eq:");
        const auto eqBreakPos = eqCasePos != std::string::npos
            ? setCurrentTool.find("break;", eqCasePos) : std::string::npos;
        const auto eqCase = (eqCasePos != std::string::npos && eqBreakPos != std::string::npos)
            ? setCurrentTool.substr(eqCasePos, eqBreakPos - eqCasePos) : std::string();
        expect(contains(eqCase, "setMouseCursor(getEqCursor());")
                   && !contains(eqCase, "CrosshairCursor"),
               "setCurrentTool applies the themed Eq cursor, never Crosshair");
    }

    // ── 7. 两个 PluginEditor：工具 id 上界与偏好注入；两模式工具入口共享 ──
    const auto pluginEditor = readSource("Source/Plugin/PluginEditor.cpp");
    const auto standaloneEditor = readSource("Source/Standalone/PluginEditor.cpp");
    expect(contains(pluginEditor, "toolId > static_cast<int>(ToolId::Eq)")
               && contains(standaloneEditor, "toolId > static_cast<int>(ToolId::Eq)"),
           "Both editors bound tool ids at ToolId::Eq");
    expect(contains(pluginEditor, "pianoRoll_.setAppPreferences(&appPreferences_);")
               && contains(standaloneEditor, "pianoRoll_.setAppPreferences(&appPreferences_);"),
           "Both editors inject AppPreferences into the piano roll");
    const auto parameterPanel = readSource("Source/Standalone/UI/ParameterPanel.cpp");
    const auto setOpenDyneMode = functionBlock(
        parameterPanel, "void ParameterPanel::setOpenDyneMode");
    expect(!contains(setOpenDyneMode, "eqToolButton_->setVisible")
               && contains(parameterPanel, "eqToolButton_ = std::make_unique<ToolIconButton>(11,"),
           "The EQ toolbar entry is shared by both schemes with the Eq tool id");

    // ── 8. 提交调度链：秒域 local-mutation helper，无显式 republish ──
    const auto processor = readSource("Source/PluginProcessor.cpp");
    const auto commitTopology = functionBlock(
        processor, "OpenTuneAudioProcessor::commitContentNoteTopologyPatch");
    expect(contains(commitTopology, "onContentLocalMutationCompletedSeconds(")
               && contains(commitTopology, "patch.affectedRange.startSeconds")
               && contains(commitTopology, "patch.affectedRange.endSeconds")
               && !contains(commitTopology, "republishPlaybackSource"),
           "Note topology commit schedules only the seconds-domain local-mutation helper");
    const auto secondsHelper = functionBlock(
        processor, "void OpenTuneAudioProcessor::onContentLocalMutationCompletedSeconds");
    expect(contains(secondsHelper, "dc->refreshModificationCRSMetadata(key)")
               && contains(secondsHelper, "dc->requestModificationRender(key, startSeconds, endSeconds)")
               && contains(secondsHelper, "refreshCRSMetadata(key)")
               && contains(secondsHelper, "requestRenderForLocalMutationRange(key, startSeconds, endSeconds)"),
           "The seconds helper reuses the ARA and non-ARA Stage1 range scheduling");
    const auto notePatchAction = readSource("Source/Utils/PianoRollNotePatchAction.cpp");
    const auto undoBlock = functionBlock(notePatchAction, "void PianoRollNotePatchAction::undo");
    const auto redoBlock = functionBlock(notePatchAction, "void PianoRollNotePatchAction::redo");
    expect(contains(undoBlock, "commands_->commitNoteTopologyPatch(contentKey_, beforePatch_);")
               && contains(redoBlock, "commands_->commitNoteTopologyPatch(contentKey_, afterPatch_);")
               && !contains(undoBlock, "enqueueRender") && !contains(redoBlock, "enqueueRender"),
           "PianoRollNotePatchAction undo/redo route only through commitNoteTopologyPatch");
}

// ── 阶段 F：Per-note EQ UI/SRC 源契约 ──
// Designer 六文件（EqGraphRenderer / EqPopupComponent / EqBandInteraction）严格复刻
// SRC 视觉数学（filterResponseDb 近似公式，非 DSP magnitude）；弹窗两态
// （预览 180×80 / 完整 600×400）共享四控制按钮，图区命中返回 ButtonId::None；
// Bypass 不早退只降透明度；提交只发生在拖拽 mouseUp / 数值 OK / Bypass 三路径，
// 无 onSettingsChanged 逐帧提交；交互直接消费 renderer xToFreq/yToGain；
// 工具链（ParameterPanel 两模式 eqToolButton、drawNotes 双分支 EQ 指示、
// ToolbarIcons EQ cursor 图像）共享既有结构。
void testPerNoteEqUiSrcContract()
{
    const auto rendererHeader = readSource("Source/Standalone/UI/PianoRoll/EqGraphRenderer.h");
    const auto renderer = readSource("Source/Standalone/UI/PianoRoll/EqGraphRenderer.cpp");
    const auto popupHeader = readSource("Source/Standalone/UI/PianoRoll/EqPopupComponent.h");
    const auto popup = readSource("Source/Standalone/UI/PianoRoll/EqPopupComponent.cpp");
    const auto interactionHeader = readSource("Source/Standalone/UI/PianoRoll/EqBandInteraction.h");
    const auto interaction = readSource("Source/Standalone/UI/PianoRoll/EqBandInteraction.cpp");

    // ── 1. 视觉层独立：无 DSP 依赖与 magnitude 机制 ──
    expect(!contains(rendererHeader, "NoteEqProcessor::") && !contains(rendererHeader, "NoteEqProcessor.h")
               && !contains(renderer, "NoteEqProcessor::") && !contains(renderer, "NoteEqProcessor.h"),
           "EqGraphRenderer never includes or calls NoteEqProcessor");
    expect(!contains(renderer, "juce::dsp") && !contains(renderer, "FilterDesign")
               && !contains(renderer, "getMagnitude"),
           "EqGraphRenderer keeps no DSP magnitude machinery");

    // ── 2. SRC 视觉公式 token（logGaussian / shelf ratio^2 / cut -10log10 ratio^4）──
    const auto filterResponse = functionBlock(renderer, "double EqGraphRenderer::filterResponseDb");
    expect(contains(filterResponse, "std::pow(ratio, 2.0 * kFixedShelfCutRatio)")
               && contains(filterResponse,
                           "-10.0 * std::log10(1.0 + std::pow(ratio, 2.0 * kFixedShelfCutRatio))"),
           "cut bands use the SRC -10log10(1+ratio^(2*kFixedShelfCutRatio)) formula");
    expect(contains(filterResponse,
                    "std::pow(frequencyHz / settings_.lowShelfFrequencyHz, kFixedShelfCutRatio)")
               && contains(filterResponse,
                           "std::pow(settings_.highShelfFrequencyHz / frequencyHz, kFixedShelfCutRatio)"),
           "shelves use the SRC ratio^kFixedShelfCutRatio shelf formula");
    expect(contains(filterResponse, "logGaussian(frequencyHz, settings_.peakFrequencyHz, peakWidthOctaves)")
               && contains(filterResponse, "0.42 / std::sqrt(kPeakQ)"),
           "peak uses the SRC logGaussian with the 0.42/sqrt(Q) width");
    expect(contains(rendererHeader, "kFixedShelfCutRatio = 2.0") && contains(rendererHeader, "kPeakQ = 2.0"),
           "SRC ratio exponent 2.0 and fixed Q 2.0 stay in the visual layer");
    expect(contains(renderer, "std::log(frequencyHz / kMinFrequencyHz) / std::log(1000.0)")
               && contains(rendererHeader, "kMinFrequencyHz = 20.0"),
           "log X axis maps ln(f/20)/ln(1000)");
    expect(contains(rendererHeader, "kBaseSegments = 240") && contains(rendererHeader, "kCurvatureDb = 0.22")
               && contains(rendererHeader, "kMaxSubdivisionDepth = 3"),
           "adaptive sampling stays at 240 base segments, 0.22 dB curvature, depth 3");
    const auto monotonic = functionBlock(renderer, "void EqGraphRenderer::monotonicCubicPath");
    expect(contains(monotonic, "const double limit = 3.0 * std::min(std::abs(left), std::abs(right));")
               && contains(monotonic, "if (magnitude > 3.0)")
               && contains(monotonic, "const double scale = 3.0 / magnitude;"),
           "Fritsch-Carlson clamps slope magnitude at the 3x secant limit");
    const auto combined = functionBlock(renderer, "double EqGraphRenderer::combinedResponseDb");
    expect(contains(combined, "for (int i = 0; i < 5; ++i)")
               && contains(combined, "total += filterResponseDb(i, frequencyHz);")
               && !contains(combined, "std::clamp"),
           "combined curve is the real five-band sum, never clamped inside the sum");
    const auto combinedPath = functionBlock(renderer, "juce::Path EqGraphRenderer::buildCombinedPath");
    expect(contains(combinedPath, "std::clamp(combinedResponseDb(freq), -gainRangeDb_, gainRangeDb_)")
               && contains(combinedPath, "monotonicCubicPath(path, samples, graphBounds_, gainRangeDb_)"),
           "combined path clips to the live view gain range");
    const auto singlePath = functionBlock(renderer, "juce::Path EqGraphRenderer::buildSingleBandPath");
    expect(contains(singlePath, "std::clamp(filterResponseDb(bandIndex, freq), -gainRangeDb_, gainRangeDb_)"),
           "single-band paths clip to the live view gain range");
    expect(contains(rendererHeader, "kCombinedCurveWidth = 2.25f")
               && contains(rendererHeader, "kSingleCurveWidth = 1.25f"),
           "curve widths are 2.25 combined / 1.25 single");
    expect(contains(renderer, "juce::Colour::fromRGB(255, 200, 72)"),
           "combined curve color is the SRC gold 255,200,72");
    expect(contains(renderer, "static const std::array<juce::Colour, 5> palette"),
           "single bands use the five-color palette");
    expect(contains(renderer, "PathStrokeType(kCombinedCurveWidth")
               && contains(renderer, "PathStrokeType(kSingleCurveWidth"),
           "curve strokes consume the fixed 2.25/1.25 widths");

    // ── 3. 弹窗两态：尺寸 / 共享四控制 / 图区 None / 自管理 bounds ──
    expect(contains(popupHeader, "kPreviewWidth = 180") && contains(popupHeader, "kPreviewHeight = 80")
               && contains(popupHeader, "kFullWidth = 600") && contains(popupHeader, "kFullHeight = 400"),
           "preview is 180x80 and full is 600x400");
    const auto paint = functionBlock(popup, "void EqPopupComponent::paint");
    const auto previewBranch = functionBlock(paint, "if (isPreview_)");
    const auto afterPreview = paint.find("if (isPreview_)") + previewBranch.size();
    expect(contains(previewBranch, "renderer_.drawPreview(g)") && !contains(previewBranch, "paintButton"),
           "preview branch draws only curves; buttons live outside it");
    expect(contains(paint, "paintButton(g, ButtonId::Maximize,")
               && contains(paint, "paintButton(g, ButtonId::Bypass,")
               && contains(paint, "paintButton(g, ButtonId::Remove,")
               && contains(paint, "paintButton(g, ButtonId::Close,"),
           "both modes paint Maximize, Bypass, Remove and Close");
    expect(paint.find("paintButton(g, ButtonId::Maximize,") > afterPreview
               && paint.find("paintButton(g, ButtonId::Bypass,") > afterPreview
               && paint.find("paintButton(g, ButtonId::Remove,") > afterPreview
               && paint.find("paintButton(g, ButtonId::Close,") > afterPreview,
           "the four controls paint in the shared section after the preview/full branch");
    expect(contains(popupHeader, "enum class ButtonId { None = -1, Maximize = 0, Bypass, Remove, Close };")
               && contains(functionBlock(popup, "EqPopupComponent::ButtonId EqPopupComponent::hitTestButton"),
                           "return ButtonId::None;"),
           "graph area hit-testing returns ButtonId::None and never toggles mode");
    const auto toggle = functionBlock(popup, "void EqPopupComponent::toggleMaximize");
    expect(contains(toggle, "savedPreviewBounds_ = getBounds();")
               && contains(toggle, "setBounds(savedPreviewBounds_)")
               && contains(toggle, "const int targetW = kFullWidth;")
               && contains(toggle, "const int targetH = kFullHeight;"),
           "toggleMaximize saves and restores the preview bounds by itself");

    // ── 4. 预览/完整元素差异 ──
    const auto drawPreview = functionBlock(renderer, "void EqGraphRenderer::drawPreview");
    expect(!contains(drawPreview, "drawGrid(") && !contains(drawPreview, "drawAxisLabels(")
               && !contains(drawPreview, "drawCoordReadout(") && !contains(drawPreview, "drawLegend(")
               && !contains(drawPreview, "drawViewRangeButtons("),
           "preview draws no grid, axis labels, readout, legend or view-range buttons");
    expect(contains(drawPreview, "buildSingleBandPath(i)") && contains(drawPreview, "buildCombinedPath()")
               && contains(drawPreview, "drawAnchors(g, -1)"),
           "preview keeps the five curves, the combined curve and the anchors");
    const auto drawFull = functionBlock(renderer, "void EqGraphRenderer::drawFull");
    expect(contains(drawFull, "drawGrid(g)") && contains(drawFull, "drawAxisLabels(g)")
           && !contains(drawFull, "drawCoordReadout(")
           && contains(drawFull, "drawLegend(g, legendItems, hoveredLegend)")
           && contains(drawFull, "drawViewRangeButtons(g, hoveredViewRangeControl, pressedViewRangeControl)"),
           "full mode draws grid, axis labels, legend and both view-range buttons; "
           "the coord readout never renders inside drawFull");
    expect(contains(paint, "if (!isPreview_ && !interaction_.isDragging()")
           && contains(paint, "renderer_.drawCoordReadout(g, activeMousePos_, true, coordAnimOpacity_)"),
           "the coord readout is full-mode only and lives in the popup paint animation path");

    // ── 4b. 背景/坐标映射/锚点/完整模式布局 ──
    const auto drawBackground = functionBlock(renderer, "void EqGraphRenderer::drawBackground");
    expect(contains(drawBackground, "g.fillRect(graphBounds_)") && !contains(drawBackground, "fillAll"),
           "drawBackground fills only the graph bounds, never the whole surface");
    const auto gainToY = functionBlock(renderer, "float EqGraphRenderer::gainToY");
    expect(contains(gainToY, "std::clamp((gainDb + gainRangeDb_) / (2.0 * gainRangeDb_), 0.0, 1.0)"),
           "gainToY clamps the normalized gain to 0..1 before the pixel projection");
    const auto anchorPosition = functionBlock(
        renderer, "juce::Point<float> EqGraphRenderer::anchorPosition");
    expect(contains(anchorPosition, "gain = filterResponseDb(bandIndex, freq);"),
           "Cut anchors project the filter response at the cutoff frequency");
    expect(contains(anchorPosition, "gain = settings_.lowShelfGainDb;")
           && contains(anchorPosition, "gain = settings_.peakGainDb;")
           && contains(anchorPosition, "gain = settings_.highShelfGainDb;"),
           "Shelf/Peak anchors project the three stored gainDb values directly");
    expect(!contains(anchorPosition, "std::clamp"),
           "anchor gains never clamp by bandIndex");
    const auto resizedBlock = functionBlock(popup, "void EqPopupComponent::resized");
    expect(contains(resizedBlock, "leftMargin = 54.0f") && contains(resizedBlock, "rightMargin = 44.0f")
           && contains(resizedBlock, "bottomMargin = 42.0f") && contains(resizedBlock, "topMargin = 28.0f"),
           "full mode reserves the SRC left/right/top/bottom margins for the axes");
    const auto legendBlock = functionBlock(renderer, "void EqGraphRenderer::drawLegend");
    expect(contains(legendBlock, "graphBounds_.getRight() - totalWidth - 14.0f")
           && contains(legendBlock, "graphBounds_.getY() + 12.0f"),
           "legend anchors to the top-right corner of the graph area");
    expect(contains(legendBlock, "juce::Colour::fromRGBA(7, 12, 18, 126)")
           && contains(legendBlock, "fillRoundedRectangle(box, 8.0f)"),
           "legend draws a semi-transparent rounded background");

    // ── 5. Bypass 不早退 / Remove / Close / 三路径提交 ──
    expect(contains(drawPreview, "const float bypassAlpha = settings_.active ? 1.0f : 0.28f;")
               && !contains(drawPreview, "return;"),
           "preview dims under bypass without early return");
    expect(contains(drawFull, "const float bypassAlpha = settings_.active ? 1.0f : 0.28f;")
               && !contains(drawFull, "return;"),
           "full mode dims under bypass without early return");
    expect(contains(popupHeader, "void setRemoveConfirmationSuppressed(bool suppress);")
               && contains(functionBlock(popup, "void EqPopupComponent::setRemoveConfirmationSuppressed"),
                           "suppressRemoveConfirmation_ = suppress;"),
           "Remove confirmation suppression has a public setter");
    expect(contains(popupHeader, "std::function<void(bool)> onRemoveConfirmationSuppressed;"),
           "the suppression flag syncs through the onRemoveConfirmationSuppressed callback");
    const auto mouseDown = functionBlock(popup, "void EqPopupComponent::mouseDown");
    expect(contains(mouseDown, "settings_.active = !settings_.active;")
               && contains(mouseDown, "commitSettings();"),
           "Bypass toggles active and commits once");
    expect(contains(mouseDown, "if (suppressRemoveConfirmation_)")
               && contains(mouseDown, "if (onRemoveEq) onRemoveEq();"),
           "Remove skips the dialog when suppressed and fires onRemoveEq");
    expect(contains(mouseDown, "case ButtonId::Close:") && contains(mouseDown, "if (onClose) onClose();"),
           "Close fires the onClose callback");
    const auto mouseDrag = functionBlock(popup, "void EqPopupComponent::mouseDrag");
    expect(!contains(mouseDrag, "commitSettings")
           && contains(mouseDrag, "interaction_.hasDragThreshold(pendingDragStartPos_, event.position)")
           && contains(mouseDrag, "interaction_.startDrag(pendingDragBand_, pendingDragStartPos_, settings_)")
           && contains(mouseDrag, "interaction_.updateDrag(event.position, settings_)")
           && contains(mouseDrag, "renderer_.setSettings(settings_)"),
           "drag starts only past the threshold, then updates the preview; no per-frame commit");
    const auto mouseUp = functionBlock(popup, "void EqPopupComponent::mouseUp");
    expect(contains(mouseUp, "if (wasDragging_ && !dragCommitted_)")
           && contains(mouseUp, "commitSettings();")
           && contains(mouseUp, "dragCommitted_ = true;")
           && countOccurrences(mouseUp, "commitSettings();") == 1,
           "started anchor drags commit exactly once on mouseUp");
    expect(contains(mouseDown, "pendingDragBand_ = bandIdx;")
           && contains(mouseDown, "pendingDragStartPos_ = pos;"),
           "anchor mouseDown only records the pending band and its start position");
    expect(contains(mouseUp, "showValueInputPopup(band);"),
           "unstarted pending clicks open the numeric value input for the recorded band");
    expect(countOccurrences(popup, "commitSettings();") == 3
               && !contains(popup, "onSettingsChanged") && !contains(popupHeader, "onSettingsChanged"),
           "commit fires only on anchor mouseUp, numeric OK and Bypass; no onSettingsChanged stream");

    // ── 6. 数值输入：freq/gain 编辑 + Q 只读 2.0 + 实际 setBounds ──
    const auto valueInput = functionBlock(popup, "void EqPopupComponent::showValueInputPopup");
    expect(contains(valueInput, "\"Q: 2.0\""),
           "Q is a read-only 2.0 label in the value input");
    expect(contains(valueInput, "commitSettings();") && countOccurrences(valueInput, "commitSettings();") == 1,
           "numeric OK commits exactly once");
    const auto valueOverlay = functionBlock(popup, "void EqPopupComponent::layoutValueInputOverlay");
    expect(contains(valueOverlay, "freqEditor_->setBounds(") && contains(valueOverlay, "gainEditor_->setBounds(")
               && contains(valueOverlay, "qLabel_->setBounds("),
           "value input lays out freq/gain/Q fields with real setBounds");

    // ── 7. 图例 / 视图范围 / 动画 / 锚点 / HUD ──
    const auto legendItems = functionBlock(
        renderer, "std::array<EqGraphRenderer::LegendItem, 6> EqGraphRenderer::buildLegendItems");
    expect(contains(legendItems, "\"Combined\"") && contains(legendItems, "\"LowCut\"")
               && contains(legendItems, "\"LowShelf\"") && contains(legendItems, "\"Peak\"")
               && contains(legendItems, "\"HighShelf\"") && contains(legendItems, "\"HighCut\""),
           "legend has exactly the six curve entries");
    expect(!contains(legendItems, "Source") && !contains(legendItems, "Target"),
           "legend has no Source/Target entries");
    expect(contains(rendererHeader, "kViewRange6 = 6.0") && contains(rendererHeader, "kViewRange12 = 12.0")
               && contains(rendererHeader, "kViewRange30 = 30.0")
               && contains(rendererHeader, "kViewRangeCircleRadius = 16.0f")
               && contains(rendererHeader, "std::clamp(rangeDb, 6.0, 30.0)"),
           "view ranges are 6/12/30 dB with a 32px circle button clamped to 6..30");
    expect(contains(rendererHeader, "enum class ViewRangeButton { None, Decrease, Increase };"),
           "view-range controls are the None/Decrease/Increase enum");
    expect(contains(mouseDown, "pressedViewRange_ = 0;")
           && contains(mouseDown, "pressedViewRange_ = 1;")
           && !contains(mouseDown, "setViewGainRangeDb"),
           "view-range mouseDown only records the pressed control, never changes the range");
    expect(contains(mouseUp, "currentRange >= 30.0 ? 12.0")
           && contains(mouseUp, "currentRange >= 12.0 ? 6.0")
           && contains(mouseUp, "currentRange <= 6.0 ? 12.0")
           && contains(mouseUp, "currentRange <= 12.0 ? 30.0"),
           "view-range mouseUp cycles Decrease 30->12->6 and Increase 6->12->30 on same-button release");
    expect(contains(mouseUp, "pressedViewRange_ = -1;")
           && contains(functionBlock(popup, "void EqPopupComponent::mouseExit"),
                       "pressedViewRange_ = -1;"),
           "mouseUp and mouseExit clear the pressed view-range control");
    expect(contains(popupHeader, "kCoordFadeInTime") && contains(popupHeader, "0.120")
               && contains(popupHeader, "kCoordActiveEntry") && contains(popupHeader, "0.140")
               && contains(popupHeader, "kCoordActiveHold") && contains(popupHeader, "2.360")
               && contains(popupHeader, "kCoordFadeOutTime") && contains(popupHeader, "0.500"),
           "coord animation rhythm is 120/140/2360/500 ms");
    expect(contains(rendererHeader, "kAnchorNormalRadius") && contains(rendererHeader, "4.75f")
               && contains(rendererHeader, "kAnchorHoverRadius") && contains(rendererHeader, "4.9f")
               && contains(rendererHeader, "kAnchorActiveRadius") && contains(rendererHeader, "5.2f")
               && contains(rendererHeader, "kHaloNormalRadius") && contains(rendererHeader, "5.6f")
               && contains(rendererHeader, "kHaloHoverRadius") && contains(rendererHeader, "7.1f"),
           "anchor radii 4.75/4.9/5.2 and halos 5.6/7.1 match SRC");
    expect(contains(rendererHeader, "void drawCrosshairAndHud(")
               && contains(rendererHeader, "void drawCoordReadout("),
            "crosshair/HUD and coord readout renderer entries exist");
    expect(contains(drawFull, "if (settings_.active && isDragging && hoveredBand >= 0 && hoveredBand < 5)")
               && contains(drawFull, "drawCrosshairAndHud(g, mousePos, hoveredBand, hudText)"),
           "crosshair and HUD show during drags in full mode");

    // ── 8. EqBandInteraction 直连 renderer 坐标映射 ──
    const auto updateDrag = functionBlock(interaction, "EqSettings EqBandInteraction::updateDrag");
    expect(contains(updateDrag, "const double freq = renderer_->xToFreq(currentPos.getX());"),
           "drag frequency maps through renderer xToFreq");
    expect(countOccurrences(updateDrag, "renderer_->yToGain(currentPos.getY())") == 3,
           "shelf/peak gains map through renderer yToGain");
    expect(contains(updateDrag, "std::clamp(freq, 500.0, 12000.0)"),
           "Peak frequency range is 500-12000 Hz");
    expect(contains(updateDrag, "std::clamp(")
               && countOccurrences(updateDrag, "-12.0f, 12.0f)") == 3,
           "shelf/peak gains clamp to ±12 dB");
    {
        const auto cutStart = updateDrag.find("case 0: // LowCut");
        const auto shelfStart = updateDrag.find("case 1: // LowShelf");
        const auto cutRegion = (cutStart != std::string::npos && shelfStart != std::string::npos)
            ? updateDrag.substr(cutStart, shelfStart - cutStart) : std::string();
        expect(contains(cutRegion, "result.lowCutFrequencyHz") && !contains(cutRegion, "GainDb"),
               "LowCut drag writes frequency only, never gain");
        const auto highCutStart = updateDrag.find("case 4: // HighCut");
        const auto returnPos = updateDrag.find("return result;", highCutStart);
        const auto highCutRegion = (highCutStart != std::string::npos && returnPos != std::string::npos)
            ? updateDrag.substr(highCutStart, returnPos - highCutStart) : std::string();
        expect(contains(highCutRegion, "result.highCutFrequencyHz") && !contains(highCutRegion, "GainDb"),
               "HighCut drag writes frequency only, never gain");
    }

    // ── 9. 工具链：ParameterPanel 两模式 / drawNotes 双分支 / ToolbarIcons ──
    const auto parameterPanel = readSource("Source/Standalone/UI/ParameterPanel.cpp");
    expect(contains(parameterPanel, "eqToolButton_ = std::make_unique<ToolIconButton>(11, \"EQ\"")
               && contains(parameterPanel, "addAndMakeVisible(*eqToolButton_);"),
           "ParameterPanel builds the EQ button visible for both schemes");
    expect(!contains(functionBlock(parameterPanel, "void ParameterPanel::setOpenDyneMode"), "eqToolButton_"),
           "setOpenDyneMode never hides the EQ button (shared by both schemes)");
    const auto rendererSource = readSource("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");
    const auto drawNotesBlock = functionBlock(rendererSource, "void PianoRollRenderer::drawNotes");
    expect(countOccurrences(drawNotesBlock, "drawNoteEqIndicators(g, ctx, item);") == 2,
           "drawNotes calls drawNoteEqIndicators in both actual branches");
    const auto dyneBranch = functionBlock(drawNotesBlock, "if (item.notesPrimaryScheme)");
    expect(contains(dyneBranch, "drawNoteEqIndicators(g, ctx, item);"),
           "the OpenDyne blob branch draws EQ indicators");
    const auto afterDyne = drawNotesBlock.find("if (item.notesPrimaryScheme)") + dyneBranch.size();
    expect(countOccurrences(drawNotesBlock.substr(afterDyne), "drawNoteEqIndicators(g, ctx, item);") == 1,
           "the OpenTune note branch draws EQ indicators too");
    const auto indicators = functionBlock(rendererSource, "void PianoRollRenderer::drawNoteEqIndicators");
    expect(contains(indicators, "if (!note.eq.has_value())")
               && contains(indicators, "const bool isActive = note.eq->active;"),
           "indicators gate on note.eq and read the active flag");
    expect(contains(indicators, "juce::Colour::fromRGB(255, 200, 72).withAlpha(0.75f)")
               && contains(indicators, "juce::Colour::fromRGB(255, 200, 72).withAlpha(0.30f)"),
           "active vs bypass notes differ by marker alpha");
    const auto toolbarIcons = readSource("Source/Standalone/UI/ToolbarIcons.h");
    expect(contains(toolbarIcons, "static juce::Image createEqIconImage()"),
           "ToolbarIcons owns the single EQ icon/cursor source image");

    // ── 10. Designer 文件无 band 数组草稿 / DSP 依赖 / 排除功能 ──
    static const char* const designerFiles[] = {
        "Source/Standalone/UI/PianoRoll/EqGraphRenderer.h",
        "Source/Standalone/UI/PianoRoll/EqGraphRenderer.cpp",
        "Source/Standalone/UI/PianoRoll/EqPopupComponent.h",
        "Source/Standalone/UI/PianoRoll/EqPopupComponent.cpp",
        "Source/Standalone/UI/PianoRoll/EqBandInteraction.h",
        "Source/Standalone/UI/PianoRoll/EqBandInteraction.cpp",
    };
    for (const char* path : designerFiles) {
        const auto text = readSource(path);
        expect(!contains(text, "kNumBands") && !contains(text, "bands[") && !contains(text, ".bands")
                   && !contains(text, "EqBandSettings") && !contains(text, "NoteEqProcessor::")
                   && !contains(text, "juce::dsp") && !contains(text, "FilterDesign")
                   && !contains(text, "getMagnitude") && !contains(text, "preset")
                   && !contains(text, "spectrum") && !contains(text, "per-band")
                   && !contains(text, "wheel") && !contains(text, "Source")
                   && !contains(text, "Target"),
               "Designer EQ files carry no band-array drafts, DSP magnitude or excluded features");
    }
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
    testParameterPanelLayoutContract();
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
    testNoteEqDataContract();
    testPerNoteEqStage1Contract();
    testPerNoteEqPersistenceContract();
    testPerNoteEqToolContract();
    testPerNoteEqUiSrcContract();

    if (failures != 0) {
        std::cerr << failures << " reference contract test(s) failed\n";
        return 1;
    }

    std::cout << "Reference contracts passed\n";
    return 0;
}
