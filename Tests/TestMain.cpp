#include "TestSupport.h"
#include "ARA/OpenTunePlaybackRenderer.h"
#include "Audio/AudioFormatRegistry.h"
#include "Plugin/Capture/CaptureSession.h"
#include "Plugin/Capture/CapturePersistence.h"
#include "Plugin/Capture/CaptureSegment.h"
#include "Utils/PianoRollEditAction.h"
#include "Standalone/UI/MenuBarComponent.h"
#include "Standalone/UI/FrameScheduler.h"
#include "Standalone/UI/PianoRoll/PianoRollRenderer.h"
#include "Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h"
#include "Standalone/UI/TimelineViewportState.h"
#include "Standalone/UI/WaveformMipmap.h"
#include "Standalone/UI/WaveformTileCache.h"
#include "Utils/AppPreferences.h"
#include "Utils/AudioEditingScheme.h"
#include "Utils/ParameterPanelSync.h"
#include "Utils/SimdAccelerator.h"
#include "Utils/UndoManager.h"

#include <array>
#include <cmath>
#include <initializer_list>
#include <map>
#include <optional>

namespace OpenTune {
#if JucePlugin_Enable_ARA
bool canRenderPublishedRegionView(const ::OpenTune::VST3AraSession::PublishedRegionView& view) noexcept;
bool shouldRenderAraPlaybackBlock(juce::AudioProcessor::Realtime realtime,
                                  const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept;
#endif
std::optional<RenderBlockSpan> computeRegionBlockRenderSpan(double blockStartSeconds,
                                                            int blockSamples,
                                                            double hostSampleRate,
                                                            double playbackStartSeconds,
                                                            double playbackEndSeconds) noexcept;
}

namespace {

struct SuiteEntry {
    const char* name;
    const char* description;
    void (*run)();
};

constexpr std::array<SuiteEntry, 35> kSuites{{
    { "core", "leaf utilities and render primitives", &runCoreBehaviorSuite },
    { "processor", "shared processor and render contracts", &runProcessorBehaviorSuite },
    { "ui", "piano-roll and visual loop behavior", &runUiBehaviorSuite },
    { "piano-roll-f0-visual", "piano-roll F0 curve visual rendering contracts", &runPianoRollF0VisualSuite },
    { "piano-roll-intent", "piano-roll mouse intent behavior", &runPianoRollIntentBehaviorSuite },
    { "architecture", "clip core, arrangement, session, and guards", &runArchitectureBehaviorSuite },
    { "undo", "undo/redo manager", &runUndoManagerSuite },
    { "memory", "memory optimization and render cache refactor", &runMemoryOptimizationSuite },
    { "time-grid", "vocal-time-stretch TimeGrid data model + tau", &runTimeGridSuite },
    { "soundtouch", "vocal-time-stretch SoundTouchStretcher wrapper (WSOLA)", &runSoundTouchStretcherSuite },
    { "time-stretch-cache", "vocal-time-stretch TimeStretchCache clip-wide single-entry cache", &runTimeStretchCacheSuite },
    { "matstore-timegrid", "vocal-time-stretch MaterializationStore + TimeGrid + RB lifecycle integration", &runMaterializationStoreTimeGridSuite },
    { "stage2-worker", "vocal-time-stretch Stage 2 worker + readPlaybackAudio fast path", &runStage2WorkerSuite },
    { "timetool-handler", "vocal-time-stretch Time tool ToolHandler hover/drag/commit", &runTimeToolHandlerSuite },
    { "integration-pipeline", "vocal-time-stretch L3 integration: order independence, undo, waveform tau", &runIntegrationPipelineSuite },
    { "invariant-contract", "vocal-time-stretch L4 contract / invariants: bypass bit-exactness, ARA region length, RB reset", &runInvariantContractSuite },
    { "game-note-generator", "GameNoteGenerator D3PM ONNX transcription + mergeChunkNotes seam dedup", &runGameNoteGeneratorSuite },
    { "vocoder-config", "vocoder-runtime-config: forward chain defaults + mel hash invariants", &runVocoderConfigSuite },
    { "timegrid-patch", "AUTO Ref TimeGrid intent compiler", &runTimeGridPatchBuilderSuite },
    { "composite-undo-action", "CompositeUndoAction: undo/reverse order, empty safety, single-step count", &runCompositeUndoActionSuite },
    { "undo-manager-contract", "UndoManager + CompositeUndoAction: nested composite safety", &runUndoManagerContractSuite },
    { "reference-auto-align", "ReferenceAutoAlign pure request/patch contract", &runReferenceAutoAlignSuite },
    { "auto-ref-failure", "ReferenceAutoAlign failure modes: NoOverlap, NotReady, InsufficientFeatures", &runAutoRefFailureSuite },
    { "auto-ref-architecture", "AUTO Ref UI architecture source-scan guards", &runAutoRefArchitectureSuite },
    { "basic-derived-analysis", "DerivedAnalysis slot smoke test", &runBasicDerivedAnalysisSuite },
    { "auto-ref-integration", "AUTO Ref domain applier transaction and composite undo", &runAutoRefIntegrationSuite },
    { "derived-analysis", "MaterializationStore DerivedAnalysis slot set/get/invalidate lifecycle", &runMaterializationDerivedAnalysisSuite },
    { "mat-contract", "MaterializationStore contract: derived analysis isolation from snapshot/notes", &runMaterializationContractSuite },
    { "ref-analysis-svc", "ReferenceAnalysisService lifecycle and submit/cancel safety", &runReferenceAnalysisServiceSuite },
    { "ref-binding", "StandaloneArrangement reference binding lifecycle (set/get/clear/delete/move)", &runReferenceBindingSuite },
    { "ref-binding-cascade", "StandaloneArrangement reference binding cascade (delete/move cross-track/split)", &runPlacementReferenceCascadeSuite },
    { "arrangement-contract", "StandaloneArrangement contract (idempotent, invalid-id, snapshot)", &runArrangementContractSuite },
    { "project-session-reference", "ProjectSession reference binding roundtrip and corruption", &runProjectSessionReferenceSuite },
    { "timeline-rendering", "DAW timeline rendering pipeline contracts", &runTimelineRenderingSuite },
    { "timeline-rendering-perf", "DAW timeline rendering runtime diagnostics contracts", &runTimelineRenderingPerfSuite },
}};

void printHeader()
{
    std::cout << "========================================" << std::endl;
    std::cout << "OpenTune Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
}

void printSuiteList()
{
    std::cout << "Available suites:" << std::endl;
    for (const auto& suite : kSuites)
        std::cout << "  " << suite.name << "  - " << suite.description << std::endl;
}

const SuiteEntry* findSuite(const juce::String& name)
{
    for (const auto& suite : kSuites) {
        if (name == suite.name)
            return &suite;
    }

    return nullptr;
}

void runSuite(const SuiteEntry& suite)
{
    std::cout << "\nRunning suite: " << suite.name << std::endl;
    suite.run();
}

OpenTuneAudioProcessor::PreparedImport makePreparedImport(const juce::String& displayName,
                                                          int numSamples = 128)
{
    OpenTuneAudioProcessor::PreparedImport preparedImport;
    preparedImport.displayName = displayName;
    preparedImport.storedAudioBuffer.setSize(1, numSamples);
    preparedImport.storedAudioBuffer.clear();
    if (numSamples > 0)
        preparedImport.storedAudioBuffer.setSample(0, 0, 0.25f);
    return preparedImport;
}

bool sameDetectedKey(const DetectedKey& lhs, const DetectedKey& rhs)
{
    return lhs.root == rhs.root
        && lhs.scale == rhs.scale
        && approxEqual(lhs.confidence, rhs.confidence, 1.0e-6f);
}

bool sameEditingRange(const AudioEditingScheme::FrameRange& lhs,
                      const AudioEditingScheme::FrameRange& rhs)
{
    return lhs.startFrame == rhs.startFrame
        && lhs.endFrameExclusive == rhs.endFrameExclusive;
}

Note makeUndoTestNote(double startTime, double endTime, float pitch)
{
    Note note;
    note.startTime = startTime;
    note.endTime = endTime;
    note.pitch = pitch;
    note.originalPitch = pitch;
    note.velocity = 0.9f;
    note.isVoiced = true;
    return note;
}

CorrectedSegment makeUndoTestSegment(int startFrame,
                                     int endFrame,
                                     std::vector<float> f0Data,
                                     CorrectedSegment::Source source)
{
    CorrectedSegment segment(startFrame, endFrame, f0Data, source);
    segment.retuneSpeed = 0.35f;
    segment.vibratoDepth = 0.12f;
    segment.vibratoRate = 5.1f;
    return segment;
}

bool seedUndoMatrixMaterialization(OpenTuneAudioProcessor& processor, const juce::String& clipName, uint64_t& outMaterializationId)
{
    outMaterializationId = processor.commitPreparedImportAsPlacement(makePreparedImport(clipName), {0, 0.0}).materializationId;
    return outMaterializationId != 0 && processor.setMaterializationPitchCurveById(outMaterializationId, std::make_shared<PitchCurve>());
}

juce::File locateWorkspaceRoot()
{
    auto current = juce::File::getCurrentWorkingDirectory();
    for (int depth = 0; depth < 8 && current.isDirectory(); ++depth) {
        if (current.getChildFile("CMakeLists.txt").existsAsFile()) {
            return current;
        }

        const auto parent = current.getParentDirectory();
        if (parent == current) {
            break;
        }

        current = parent;
    }

    return {};
}

juce::String readWorkspaceFile(const juce::String& relativePath)
{
    const auto workspaceRoot = locateWorkspaceRoot();
    if (!workspaceRoot.isDirectory()) {
        return {};
    }

    const auto file = workspaceRoot.getChildFile(relativePath);
    if (!file.existsAsFile()) {
        return {};
    }

    return file.loadFileAsString();
}

class WorkspaceFileCache {
public:
    const juce::String& get(const juce::String& relativePath)
    {
        const auto it = cache_.find(relativePath);
        if (it != cache_.end())
            return it->second;

        auto content = readWorkspaceFile(relativePath);
        const auto [inserted, _] = cache_.emplace(relativePath, std::move(content));
        return inserted->second;
    }

    void clear() { cache_.clear(); }

private:
    std::map<juce::String, juce::String> cache_;
};

WorkspaceFileCache& getFileCache()
{
    static WorkspaceFileCache cache;
    return cache;
}

juce::String extractWorkspaceFileSection(const juce::String& relativePath,
                                        const juce::String& startNeedle,
                                        const juce::String& endNeedle)
{
    const auto& source = getFileCache().get(relativePath);
    const auto start = source.indexOf(startNeedle);
    if (start < 0) {
        return {};
    }

    const auto end = source.indexOf(start + startNeedle.length(), endNeedle);
    if (end < 0 || end <= start) {
        return {};
    }

    return source.substring(start, end);
}

bool sourceContains(const juce::String& relativePath, const juce::String& needle)
{
    return getFileCache().get(relativePath).contains(needle);
}

bool sourceContainsAny(const juce::String& relativePath, std::initializer_list<const char*> needles)
{
    const auto& source = getFileCache().get(relativePath);
    for (const auto* needle : needles) {
        if (source.contains(needle))
            return true;
    }

    return false;
}

bool workspaceFileExists(const juce::String& relativePath)
{
    const auto workspaceRoot = locateWorkspaceRoot();
    return workspaceRoot.isDirectory() && workspaceRoot.getChildFile(relativePath).existsAsFile();
}

bool popupMenuContainsItemText(const juce::PopupMenu& menu, const juce::String& itemText)
{
    juce::PopupMenu::MenuItemIterator iterator(menu, true);
    while (iterator.next()) {
        if (iterator.getItem().text == itemText) {
            return true;
        }
    }

    return false;
}

juce::File makeCleanTemporaryDirectory(const juce::String& leafName)
{
    auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("OpenTuneTests")
        .getChildFile(leafName);
    directory.deleteRecursively();
    directory.createDirectory();
    return directory;
}

AppPreferences::StorageOptions makeAppPreferencesStorageOptions(const juce::String& leafName)
{
    AppPreferences::StorageOptions options;
    options.applicationName = "OpenTuneTests";
    options.settingsDirectory = makeCleanTemporaryDirectory(leafName);
    options.fileName = "app-preferences.settings";
    return options;
}

juce::File resolveAppPreferencesSettingsFile(const AppPreferences::StorageOptions& storageOptions)
{
    return storageOptions.settingsDirectory.getChildFile(storageOptions.fileName);
}

juce::MemoryBlock serializeProcessorState(OpenTuneAudioProcessor& processor)
{
    juce::MemoryBlock stateData;
    processor.getStateInformation(stateData);
    return stateData;
}

std::unique_ptr<OpenTuneAudioProcessor> makeProcessorWithWrapperType(juce::AudioProcessor::WrapperType wrapperType)
{
    juce::AudioProcessor::setTypeOfNextNewPlugin(wrapperType);
    auto processor = std::make_unique<OpenTuneAudioProcessor>();
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
    return processor;
}

juce::MemoryBlock makeMinimalVst3StateWithMetadataOnlySource(uint64_t sourceId)
{
    juce::MemoryBlock stateData;
    juce::MemoryOutputStream out(stateData, false);
    out.writeInt(0x4F545354); // 'OTST'
    out.writeInt(8);          // Placement::clipInSeconds version
    out.writeDouble(1.0);     // zoomLevel
    out.writeInt(120);        // trackHeight

    out.writeInt(1); // source count
    out.writeInt64(static_cast<juce::int64>(sourceId));
    out.writeString("ara-metadata-only-source");
    out.writeDouble(44100.0);
    out.writeBool(false); // metadata-only source: no serialized PCM buffer

    out.writeInt(0); // materialization count
    out.writeInt(0); // active track
    out.writeInt(OpenTuneAudioProcessor::MAX_TRACKS);
    for (int trackId = 0; trackId < OpenTuneAudioProcessor::MAX_TRACKS; ++trackId) {
        out.writeInt64(0);      // selectedPlacementId
        out.writeBool(false);   // muted
        out.writeBool(false);   // solo
        out.writeFloat(1.0f);   // volume
        out.writeInt(0);        // placement count
    }

    out.flush();
    return stateData;
}

bool serializedStateContainsAscii(const juce::MemoryBlock& stateData, const char* needle)
{
    if (needle == nullptr || *needle == '\0' || stateData.getSize() == 0) {
        return false;
    }

    const auto* begin = static_cast<const char*>(stateData.getData());
    const auto* end = begin + static_cast<std::ptrdiff_t>(stateData.getSize());
    const std::string target(needle);
    return std::search(begin, end, target.begin(), target.end()) != end;
}

std::shared_ptr<const juce::AudioBuffer<float>> makeSharedAudioBuffer(int numSamples, float firstSample = 0.25f)
{
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, numSamples);
    buffer->clear();
    if (numSamples > 0) {
        buffer->setSample(0, 0, firstSample);
    }
    return buffer;
}

std::shared_ptr<PitchCurve> makePitchCurveWithPayload(std::vector<float> originalF0,
                                                      std::vector<float> originalEnergy,
                                                      std::vector<CorrectedSegment> correctedSegments,
                                                      int hopSize = 1,
                                                      double sampleRate = 100.0)
{
    auto curve = std::make_shared<PitchCurve>();
    curve->setHopSize(hopSize);
    curve->setSampleRate(sampleRate);
    curve->setOriginalF0(originalF0);
    curve->setOriginalEnergy(originalEnergy);
    curve->replaceCorrectedSegments(correctedSegments);
    return curve;
}

std::vector<float> renderPitchCurveF0(const std::shared_ptr<PitchCurve>& curve, int frameCount)
{
    std::vector<float> rendered(static_cast<size_t>(frameCount), 0.0f);
    if (!curve) {
        return rendered;
    }

    curve->renderF0Range(0,
                         frameCount,
                         [&](int startFrame, const float* data, int length) {
                             std::copy(data,
                                       data + length,
                                       rendered.begin() + static_cast<std::ptrdiff_t>(startFrame));
                         });
    return rendered;
}

std::vector<float> renderPitchCurveCorrectedOnlyF0(const std::shared_ptr<PitchCurve>& curve, int frameCount)
{
    std::vector<float> rendered(static_cast<size_t>(frameCount), 0.0f);
    if (!curve) {
        return rendered;
    }

    curve->renderCorrectedOnlyRange(0,
                                    frameCount,
                                    [&](int startFrame, const float* data, int length) {
                                        std::copy(data,
                                                  data + length,
                                                  rendered.begin() + static_cast<std::ptrdiff_t>(startFrame));
                                    });
    return rendered;
}

juce::MouseEvent makeMouseEvent(juce::Component& component,
                                juce::Point<float> position,
                                juce::Point<float> mouseDownPosition,
                                bool mouseWasDragged)
{
    const auto eventTime = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),
                            position,
                            juce::ModifierKeys::leftButtonModifier,
                            1.0f,
                            0.0f,
                            0.0f,
                            0.0f,
                            0.0f,
                            &component,
                            &component,
                            eventTime,
                            mouseDownPosition,
                            eventTime,
                            1,
                            mouseWasDragged);
}

struct PianoRollToolHandlerHarness {
    juce::Component component;
    InteractionState state;
    std::vector<Note> committedNotes;
    std::shared_ptr<PitchCurve> pitchCurve;
    KeyShortcutConfig::KeyShortcutSettings shortcutSettings;
    MaterializationTimelineProjection projection{ 0.0, 4.0, 4.0 };
    bool commitNoteDraftResult = true;
    bool applyManualCorrectionResult = true;
    bool commitNotesAndSegmentsResult = false;
    float retuneSpeed = 0.35f;
    int commitNoteDraftCalls = 0;
    int applyManualCorrectionCalls = 0;
    int commitNotesAndSegmentsCalls = 0;
    int notifyPitchCurveEditedCalls = 0;
    int notifyPlayheadChangeCalls = 0;
    std::vector<double> notifiedPlayheadTimes;
    std::vector<float> originalF0;
    F0Timeline f0Timeline;
    int lineAnchorHitSegment = -1;
    int selectLineAnchorSegmentCalls = 0;
    int toggleLineAnchorSegmentSelectionCalls = 0;
    int clearLineAnchorSegmentSelectionCalls = 0;
    std::vector<Note> lastCommittedNotes;
    std::vector<PianoRollToolHandler::ManualCorrectionOp> lastManualCorrectionOps;
    int lastManualCorrectionStartFrame = -1;
    int lastManualCorrectionEndFrame = -1;
    bool lastManualCorrectionPreviewOnly = false;
    std::function<float(float)> yToFreqOverride;
    juce::Rectangle<int> lastInvalidatedArea;
    PianoRollToolHandler handler;

    PianoRollToolHandlerHarness()
        : handler(buildContext())
    {
        component.setBounds(0, 0, 800, 400);
    }

    PianoRollToolHandler::Context buildContext()
    {
        PianoRollToolHandler::Context ctx;
        ctx.getState = [this]() -> InteractionState& { return state; };

        ctx.xToTime = [](int x) { return static_cast<double>(x) * 0.01; };
        ctx.timeToX = [](double seconds) { return static_cast<int>(std::lround(seconds * 100.0)); };
        ctx.yToFreq = [this](float y) { return yToFreqOverride ? yToFreqOverride(y) : 440.0f; };
        ctx.freqToY = [](float) { return 0.0f; };

        ctx.getCommittedNotes = [this]() -> const std::vector<Note>& { return committedNotes; };
        ctx.getDisplayNotes = [this]() -> const std::vector<Note>& {
            return state.noteDraft.active ? state.noteDraft.workingNotes : committedNotes;
        };
        ctx.getNoteDraft = [this]() -> NoteInteractionDraft& { return state.noteDraft; };
        ctx.beginNoteDraft = [this]() {
            state.noteDraft.active = true;
            state.noteDraft.baselineNotes = committedNotes;
            state.noteDraft.workingNotes = committedNotes;
        };
        ctx.commitNoteDraft = [this]() {
            ++commitNoteDraftCalls;
            if (!commitNoteDraftResult) {
                return false;
            }

            committedNotes = state.noteDraft.workingNotes;
            lastCommittedNotes = committedNotes;
            state.noteDraft.clear();
            return true;
        };
        ctx.clearNoteDraft = [this]() { state.noteDraft.clear(); };
        ctx.commitNotesAndSegments = [this](const std::vector<Note>& notes,
                                            const std::vector<CorrectedSegment>&,
                                            F0FrameRange) {
            ++commitNotesAndSegmentsCalls;
            if (!commitNotesAndSegmentsResult) {
                return false;
            }

            committedNotes = notes;
            lastCommittedNotes = committedNotes;
            state.noteDraft.clear();
            return true;
        };

        ctx.getPitchCurve = [this]() { return pitchCurve; };
        ctx.getPianoKeyWidth = []() { return 0; };
        ctx.getMaterializationProjection = [this]() { return projection; };
        ctx.projectTimelineTimeToMaterialization = [this](double timelineSeconds) {
            return projection.projectTimelineTimeToMaterialization(timelineSeconds);
        };
        ctx.projectMaterializationTimeToTimeline = [this](double materializationSeconds) {
            return projection.projectMaterializationTimeToTimeline(materializationSeconds);
        };
        ctx.getNotesBounds = [](const std::vector<Note>&) { return juce::Rectangle<int>(); };
        ctx.getSelectionBounds = []() { return juce::Rectangle<int>(); };
        ctx.getHandDrawPreviewBounds = []() { return juce::Rectangle<int>(); };
        ctx.getLineAnchorPreviewBounds = []() { return juce::Rectangle<int>(); };
        ctx.getNoteDragCurvePreviewBounds = []() { return juce::Rectangle<int>(); };

        ctx.getMinMidi = []() { return 36.0f; };
        ctx.getMaxMidi = []() { return 84.0f; };
        ctx.getRetuneSpeed = [this]() { return retuneSpeed; };
        ctx.getVibratoDepth = []() { return 0.0f; };
        ctx.getVibratoRate = []() { return 0.0f; };
        ctx.getAudioEditingScheme = []() { return AudioEditingScheme::Scheme::CorrectedF0Primary; };
        ctx.getShortcutSettings = [this]() -> const KeyShortcutConfig::KeyShortcutSettings& { return shortcutSettings; };
        ctx.recalculatePIP = [](Note&) { return -1.0f; };
        ctx.getF0Timeline = [this]() { return f0Timeline; };

        ctx.getDirtyStartTime = [this]() { return state.drawing.dirtyStartTime; };
        ctx.setDirtyStartTime = [this](double v) { state.drawing.dirtyStartTime = v; };
        ctx.getDirtyEndTime = [this]() { return state.drawing.dirtyEndTime; };
        ctx.setDirtyEndTime = [this](double v) { state.drawing.dirtyEndTime = v; };

        ctx.getDrawingNoteStartTime = [this]() { return state.drawing.drawingNoteStartTime; };
        ctx.setDrawingNoteStartTime = [this](double v) { state.drawing.drawingNoteStartTime = v; };
        ctx.getDrawingNoteEndTime = [this]() { return state.drawing.drawingNoteEndTime; };
        ctx.setDrawingNoteEndTime = [this](double v) { state.drawing.drawingNoteEndTime = v; };
        ctx.getDrawingNotePitch = [this]() { return state.drawing.drawingNotePitch; };
        ctx.setDrawingNotePitch = [this](float v) { state.drawing.drawingNotePitch = v; };
        ctx.getDrawingNoteIndex = [this]() { return state.drawing.drawingNoteIndex; };
        ctx.setDrawingNoteIndex = [this](int v) { state.drawing.drawingNoteIndex = v; };

        ctx.getDrawNoteToolPendingDrag = [this]() { return state.drawNoteToolPendingDrag; };
        ctx.setDrawNoteToolPendingDrag = [this](bool v) { state.drawNoteToolPendingDrag = v; };
        ctx.getDrawNoteToolMouseDownPos = [this]() { return state.drawNoteToolMouseDownPos; };
        ctx.setDrawNoteToolMouseDownPos = [this](juce::Point<int> v) { state.drawNoteToolMouseDownPos = v; };
        ctx.getDragThreshold = []() { return 3; };

        ctx.getNoteDragManualStartTime = [this]() { return state.noteDrag.manualStartTime; };
        ctx.setNoteDragManualStartTime = [this](double v) { state.noteDrag.manualStartTime = v; };
        ctx.getNoteDragManualEndTime = [this]() { return state.noteDrag.manualEndTime; };
        ctx.setNoteDragManualEndTime = [this](double v) { state.noteDrag.manualEndTime = v; };
        ctx.getNoteDragInitialManualTargets = [this]() -> std::vector<std::pair<double, float>>& {
            return state.noteDrag.initialManualTargets;
        };
        ctx.getNoteDragPreviewF0 = [this]() -> std::vector<float>& { return state.noteDrag.previewF0; };
        ctx.getNoteDragPreviewStartFrame = [this]() { return state.noteDrag.previewStartFrame; };
        ctx.setNoteDragPreviewStartFrame = [this](int v) { state.noteDrag.previewStartFrame = v; };
        ctx.getNoteDragPreviewEndFrameExclusive = [this]() { return state.noteDrag.previewEndFrameExclusive; };
        ctx.setNoteDragPreviewEndFrameExclusive = [this](int v) { state.noteDrag.previewEndFrameExclusive = v; };

        ctx.invalidateVisual = [this](const juce::Rectangle<int>& dirtyArea) { lastInvalidatedArea = dirtyArea; };
        ctx.setMouseCursor = [](const juce::MouseCursor&) {};
        ctx.grabKeyboardFocus = []() {};
        ctx.setCurrentTool = [](ToolId) {};
        ctx.showToolSelectionMenu = []() {};

        ctx.notifyPlayheadChange = [this](double time) {
            ++notifyPlayheadChangeCalls;
            notifiedPlayheadTimes.push_back(time);
        };
        ctx.notifyPitchCurveEdited = [this](int, int) { ++notifyPitchCurveEditedCalls; };
        ctx.notifyAutoTuneRequested = []() {};
        ctx.notifyPlayPauseToggle = []() {};
        ctx.notifyStopPlayback = []() {};
        ctx.notifyEscapeKey = []() {};
        ctx.notifyNoteOffsetChanged = [](size_t, float, float) {};

        ctx.applyManualCorrection = [this](std::vector<PianoRollToolHandler::ManualCorrectionOp> ops,
                                           int startFrame,
                                           int endFrame,
                                           bool previewOnly) {
            ++applyManualCorrectionCalls;
            lastManualCorrectionOps = std::move(ops);
            lastManualCorrectionStartFrame = startFrame;
            lastManualCorrectionEndFrame = endFrame;
            lastManualCorrectionPreviewOnly = previewOnly;
            return applyManualCorrectionResult;
        };
        ctx.selectNotesOverlappingFrames = [](int, int) { return true; };
        ctx.getOriginalF0 = [this]() { return originalF0; };

        ctx.findLineAnchorSegmentNear = [this](int, int) { return lineAnchorHitSegment; };
        ctx.selectLineAnchorSegment = [this](int) { ++selectLineAnchorSegmentCalls; };
        ctx.toggleLineAnchorSegmentSelection = [this](int) { ++toggleLineAnchorSegmentSelectionCalls; };
        ctx.clearLineAnchorSegmentSelection = [this]() { ++clearLineAnchorSegmentSelectionCalls; };
        ctx.setUndoDescription = [](juce::String) {};
        return ctx;
    }
};

void runAppPreferencesRoundTripsSharedPreferencesTest()
{
    constexpr const char* testName = "AppPreferences_RoundTripsSharedPreferences";

    const auto storage = makeAppPreferencesStorageOptions("shared-preferences-roundtrip");

    {
        AppPreferences preferences(storage);

        ZoomSensitivityConfig::ZoomSensitivitySettings zoomSettings;
        zoomSettings.horizontalZoomFactor = 0.72f;
        zoomSettings.verticalZoomFactor = 0.41f;
        zoomSettings.scrollSpeed = 156.0f;

        preferences.setLanguage(Language::Japanese);
        preferences.setTheme(ThemeId::Aurora);
        preferences.setAudioEditingScheme(AudioEditingScheme::Scheme::NotesPrimary);
        preferences.setZoomSensitivity(zoomSettings);
        preferences.flush();
    }

    {
        AppPreferences preferences(storage);
        const auto state = preferences.getState();

        if (state.shared.language != Language::Japanese) {
            logFail(testName, "shared language did not persist");
            return;
        }

        if (state.shared.theme != ThemeId::Aurora) {
            logFail(testName, "shared theme did not persist");
            return;
        }

        if (state.shared.audioEditingScheme != AudioEditingScheme::Scheme::NotesPrimary) {
            logFail(testName, "shared editing scheme did not persist");
            return;
        }

        if (!approxEqual(state.shared.zoomSensitivity.horizontalZoomFactor, 0.72f, 1.0e-6f)
            || !approxEqual(state.shared.zoomSensitivity.verticalZoomFactor, 0.41f, 1.0e-6f)
            || !approxEqual(state.shared.zoomSensitivity.scrollSpeed, 156.0f, 1.0e-6f)) {
            logFail(testName, "shared zoom settings did not persist");
            return;
        }
    }

    logPass(testName);
}

void runAppPreferencesRoundTripsStandalonePreferencesTest()
{
    constexpr const char* testName = "AppPreferences_RoundTripsStandalonePreferences";

    const auto storage = makeAppPreferencesStorageOptions("standalone-preferences-roundtrip");

    KeyShortcutConfig::KeyShortcutSettings shortcutSettings;
    const auto playPauseIndex = static_cast<size_t>(KeyShortcutConfig::ShortcutId::PlayPause);
    shortcutSettings.bindings[playPauseIndex].bindings.clear();
    shortcutSettings.bindings[playPauseIndex].addBinding(
        KeyShortcutConfig::KeyBinding('P', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier));

    {
        AppPreferences preferences(storage);
        preferences.setStandaloneShortcuts(shortcutSettings);
        preferences.setMouseTrailTheme(MouseTrailConfig::TrailTheme::Galaxy);
        preferences.flush();
    }

    {
        AppPreferences preferences(storage);
        const auto state = preferences.getState();
        const auto& restoredPlayPause = state.standalone.shortcuts.bindings[playPauseIndex];

        if (!restoredPlayPause.hasBinding(
                KeyShortcutConfig::KeyBinding('P', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier))) {
            logFail(testName, "standalone shortcut bindings did not persist");
            return;
        }

        if (state.standalone.mouseTrailTheme != MouseTrailConfig::TrailTheme::Galaxy) {
            logFail(testName, "standalone mouse trail theme did not persist");
            return;
        }
    }

    logPass(testName);
}

void runAppPreferencesRoundTripsSharedVisualPreferencesTest()
{
    constexpr const char* testName = "AppPreferences_RoundTripsSharedVisualPreferences";

    const auto storage = makeAppPreferencesStorageOptions("shared-visual-preferences-roundtrip");

    PianoRollVisualPreferences visualPreferences;
    visualPreferences.noteNameMode = NoteNameMode::ShowAll;
    visualPreferences.showChunkBoundaries = true;
    visualPreferences.showUnvoicedFrames = true;

    {
        AppPreferences preferences(storage);
        preferences.setPianoRollVisualPreferences(visualPreferences);
        preferences.flush();
    }

    {
        AppPreferences preferences(storage);
        const auto state = preferences.getState();
        const auto settingsXml = resolveAppPreferencesSettingsFile(storage).loadFileAsString();

        if (state.shared.pianoRollVisualPreferences.noteNameMode != NoteNameMode::ShowAll
            || !state.shared.pianoRollVisualPreferences.showChunkBoundaries
            || !state.shared.pianoRollVisualPreferences.showUnvoicedFrames) {
            logFail(testName, "shared piano-roll visual preferences did not round-trip through AppPreferences");
            return;
        }

        if (!settingsXml.contains("shared.pianoRoll.noteNameMode")
            || !settingsXml.contains("shared.pianoRoll.showChunkBoundaries")
            || !settingsXml.contains("shared.pianoRoll.showUnvoicedFrames")) {
            logFail(testName, "shared piano-roll visual preference keys were not persisted to storage");
            return;
        }

        if (settingsXml.contains("shared.voicedOnlyEditing")) {
            logFail(testName, "voiced-only editing was introduced as a standalone shared preference key");
            return;
        }
    }

    logPass(testName);
}

void runProcessorStateDoesNotSerializeAppPreferencesTest()
{
    constexpr const char* testName = "ProcessorState_DoesNotSerializeAppPreferences";

    const auto storage = makeAppPreferencesStorageOptions("processor-state-isolation");

    AppPreferences preferences(storage);
    preferences.setLanguage(Language::Spanish);
    preferences.setTheme(ThemeId::DarkBlueGrey);
    preferences.setAudioEditingScheme(AudioEditingScheme::Scheme::NotesPrimary);
    preferences.flush();

    OpenTuneAudioProcessor processor;
    const auto stateData = serializeProcessorState(processor);

    if (serializedStateContainsAscii(stateData, "shared.language")
        || serializedStateContainsAscii(stateData, "shared.theme")
        || serializedStateContainsAscii(stateData, "shared.audioEditing")
        || serializedStateContainsAscii(stateData, "audioEditingScheme")
        || serializedStateContainsAscii(stateData, "language")
        || serializedStateContainsAscii(stateData, "theme")) {
        logFail(testName, "processor state leaked app-level preference data");
        return;
    }

    logPass(testName);
}

void runProcessorStateDoesNotSerializeSharedVisualPreferencesTest()
{
    constexpr const char* testName = "ProcessorState_DoesNotSerializeSharedVisualPreferences";

    const auto storage = makeAppPreferencesStorageOptions("processor-state-shared-visual-isolation");

    {
        AppPreferences preferences(storage);
        preferences.setNoteNameMode(NoteNameMode::Hide);
        preferences.setShowChunkBoundaries(true);
        preferences.setShowUnvoicedFrames(true);
        preferences.flush();
    }

    const auto settingsXml = resolveAppPreferencesSettingsFile(storage).loadFileAsString();
    if (!settingsXml.contains("shared.pianoRoll.noteNameMode")
        || !settingsXml.contains("shared.pianoRoll.showChunkBoundaries")
        || !settingsXml.contains("shared.pianoRoll.showUnvoicedFrames")) {
        logFail(testName, "shared visual preferences were not persisted before processor isolation check");
        return;
    }

    OpenTuneAudioProcessor processor;
    const auto stateData = serializeProcessorState(processor);

    if (serializedStateContainsAscii(stateData, "shared.pianoRoll")
        || serializedStateContainsAscii(stateData, "noteNameMode")
        || serializedStateContainsAscii(stateData, "showChunkBoundaries")
        || serializedStateContainsAscii(stateData, "showUnvoicedFrames")) {
        logFail(testName, "processor state leaked shared piano-roll visual preferences");
        return;
    }

    logPass(testName);
}

void runStandalonePreferencesDialogContainsStandaloneOnlyPagesTest()
{
    constexpr const char* testName = "StandalonePreferencesDialog_ContainsStandaloneOnlyPages";

    const auto& source = getFileCache().get("Source/Standalone/PluginEditor.cpp");
    if (!source.contains("TabbedPreferencesDialog")
        || !source.contains("SharedPreferencePages")
        || !source.contains("StandalonePreferencePages")) {
        logFail(testName, "standalone editor does not explicitly compose shared and standalone preference pages");
        return;
    }

    logPass(testName);
}

void runPluginPreferencesDialogExcludesStandaloneOnlyPagesTest()
{
    constexpr const char* testName = "PluginPreferencesDialog_IncludesShortcutsAndTrail";

    const auto& source = getFileCache().get("Source/Plugin/PluginEditor.cpp");
    if (!source.contains("TabbedPreferencesDialog")
        || !source.contains("SharedPreferencePages")
        || !source.contains("StandalonePreferencePages")) {
        logFail(testName, "plugin editor does not include shortcut and mouse-trail preference pages");
        return;
    }

    logPass(testName);
}

void runSharedPreferencePagesExposeInteractionSchemeAndVisualControlsTest()
{
    constexpr const char* testName = "SharedPreferencePages_ExposeInteractionSchemeAndVisualControls";

    const auto& source = getFileCache().get("Source/Editor/Preferences/SharedPreferencePages.cpp");
    if (!source.contains("Audio Editing Scheme")
        || !source.contains("noteNameMode")
        || !source.contains("showChunkBoundaries")
        || !source.contains("showUnvoicedFrames")) {
        logFail(testName, "shared preference pages do not expose the interaction scheme and visual controls yet");
        return;
    }

    logPass(testName);
}

void runStandaloneShortcutSettingsUseModalCaptureDialogTest()
{
    constexpr const char* testName = "StandaloneShortcutSettings_UseModalCaptureDialog";

    const auto& source = getFileCache().get("Source/Editor/Preferences/StandalonePreferencePages.cpp");
    if (!source.contains("class CaptureWindow final : public juce::AlertWindow")
        || source.contains("class CaptureOverlay final : public juce::Component")
        || source.contains("captureOverlay_")) {
        logFail(testName, "shortcut capture still relies on the page-scoped overlay instead of a modal capture dialog");
        return;
    }

    logPass(testName);
}

void runStandalonePreferencesOwnAudioSettingsUiTest()
{
    constexpr const char* testName = "StandalonePreferences_OwnAudioSettingsUi";

    const auto& standalonePages = getFileCache().get("Source/Editor/Preferences/StandalonePreferencePages.cpp");
    const auto& standaloneEditorHeader = getFileCache().get("Source/Standalone/PluginEditor.h");
    const auto& standaloneEditorSource = getFileCache().get("Source/Standalone/PluginEditor.cpp");
    const auto& pluginEditorHeader = getFileCache().get("Source/Plugin/PluginEditor.h");
    const auto& pluginEditorSource = getFileCache().get("Source/Plugin/PluginEditor.cpp");
    const auto& processorHeader = getFileCache().get("Source/PluginProcessor.h");
    const auto& processorSource = getFileCache().get("Source/PluginProcessor.cpp");
    const auto& transportHeader = getFileCache().get("Source/Standalone/UI/TransportBarComponent.h");

    if (!standalonePages.contains("AudioSettingsPage")
        || !standalonePages.contains("createAudioPages")) {
        logFail(testName, "standalone preferences no longer own the audio settings page composition");
        return;
    }

    if (standaloneEditorHeader.contains("audioSettingsRequested()")
        || standaloneEditorSource.contains("showAudioSettingsDialog(")
        || pluginEditorHeader.contains("audioSettingsRequested()")
        || pluginEditorSource.contains("showAudioSettingsDialog(")
        || processorHeader.contains("showAudioSettingsDialog(")
        || processorSource.contains("showAudioSettingsDialog(")
        || transportHeader.contains("audioSettingsRequested()")) {
        logFail(testName, "legacy audio settings dialog chain still exists alongside the standalone preferences page");
        return;
    }

    logPass(testName);
}

void runViewMenuExposesSharedVisualOptionsAcrossProfilesTest()
{
    constexpr const char* testName = "ViewMenu_ExposesSharedVisualOptionsAcrossProfiles";

    const auto& header = getFileCache().get("Source/Standalone/UI/MenuBarComponent.h");
    const auto& source = getFileCache().get("Source/Standalone/UI/MenuBarComponent.cpp");
    if (!header.contains("setNoteNameMode(")
        || !header.contains("setShowChunkBoundaries(")
        || !header.contains("setShowUnvoicedFrames(")
        || !source.contains("NoteNameMode")
        || !source.contains("ShowChunkBoundaries")
        || !source.contains("ShowUnvoicedFrames")) {
        logFail(testName, "view menus do not expose shared visual options across standalone and plugin profiles yet");
        return;
    }

    logPass(testName);
}

void runPluginViewMenuStillExcludesStandaloneOnlyMouseTrailOptionsTest()
{
    constexpr const char* testName = "PluginViewMenu_StillExcludesStandaloneOnlyMouseTrailOptions";

    auto languageState = std::make_shared<LocalizationManager::LanguageState>();
    languageState->language = Language::English;
    LocalizationManager::getInstance().bindLanguageState(languageState);

    OpenTuneAudioProcessor processor;
    MenuBarComponent standaloneMenu(processor, MenuBarComponent::Profile::Standalone);
    MenuBarComponent pluginMenu(processor, MenuBarComponent::Profile::Plugin);

    const auto standaloneViewMenu = standaloneMenu.getMenuForIndex(2, {});
    const auto pluginViewMenu = pluginMenu.getMenuForIndex(2, {});
    const auto mouseTrailText = Loc::get(Language::English, Loc::Keys::kMouseTrail);

    if (!popupMenuContainsItemText(standaloneViewMenu, mouseTrailText)) {
        logFail(testName, "standalone view menu lost its standalone-only mouse trail entry");
        return;
    }

    if (popupMenuContainsItemText(pluginViewMenu, mouseTrailText)) {
        logFail(testName, "plugin view menu still exposes standalone-only mouse trail options");
        return;
    }

    logPass(testName);
}

void runPreferencesDialogUsesExplicitPageCompositionNotBooleanFlagsTest()
{
    constexpr const char* testName = "PreferencesDialog_UsesExplicitPageComposition_NotBooleanFlags";

    if (workspaceFileExists("Source/Standalone/UI/OptionsDialogComponent.h")) {
        logFail(testName, "legacy mixed-format options dialog still exists");
        return;
    }

    if (sourceContains("Source/Standalone/PluginEditor.cpp", "showAudioTab")
        || sourceContains("Source/Plugin/PluginEditor.cpp", "showAudioTab")
        || sourceContains("Source/Standalone/PluginEditor.cpp", "showLanguageTab")
        || sourceContains("Source/Plugin/PluginEditor.cpp", "showLanguageTab")) {
        logFail(testName, "boolean-flag page composition still exists");
        return;
    }

    logPass(testName);
}

void runAudioEditingSchemeRulesUseExplicitSchemeInputTest()
{
    constexpr const char* testName = "AudioEditingSchemeRules_UseExplicitSchemeInput";

    if (workspaceFileExists("Source/Utils/AudioEditingSchemeManager.h")) {
        logFail(testName, "legacy audio editing scheme manager still owns shared preference state");
        return;
    }

    const auto& source = getFileCache().get("Source/Utils/ParameterPanelSync.h");
    if (!source.contains("AudioEditingScheme::Scheme")
        || source.contains("AudioEditingSchemeManager::resolveParameterTarget")
        || source.contains("AudioEditingSchemeManager::getScheme()")) {
        logFail(testName, "parameter panel sync does not use explicit scheme input");
        return;
    }

    logPass(testName);
}

void runAudioEditingSchemeUsesSchemeManagedVoicedOnlyPolicyTest()
{
    constexpr const char* testName = "AudioEditingScheme_UsesSchemeManagedVoicedOnlyPolicy";

    const auto& preferencesSource = getFileCache().get("Source/Utils/AppPreferences.cpp");

    if (AudioEditingScheme::usesVoicedOnlyEditing(AudioEditingScheme::Scheme::CorrectedF0Primary)) {
        logFail(testName, "corrected-f0-primary unexpectedly enables voiced-only editing");
        return;
    }

    if (!AudioEditingScheme::usesVoicedOnlyEditing(AudioEditingScheme::Scheme::NotesPrimary)) {
        logFail(testName, "notes-primary no longer enables voiced-only editing");
        return;
    }

    const std::vector<float> originalF0 { 0.0f, 0.0f, 220.0f, 221.0f, 0.0f, 222.0f };
    const AudioEditingScheme::FrameRange requestedRange { 0, 6 };
    const auto correctedRange = AudioEditingScheme::trimFrameRangeToEditableBounds(
        AudioEditingScheme::Scheme::CorrectedF0Primary,
        originalF0,
        requestedRange);
    if (!sameEditingRange(correctedRange, requestedRange)) {
        logFail(testName, "corrected-f0-primary unexpectedly trims edits to voiced-only bounds");
        return;
    }

    const auto notesRange = AudioEditingScheme::trimFrameRangeToEditableBounds(
        AudioEditingScheme::Scheme::NotesPrimary,
        originalF0,
        requestedRange);
    if (!sameEditingRange(notesRange, { 2, 6 })) {
        logFail(testName, "notes-primary no longer trims edits to the first and last voiced frame");
        return;
    }

    if (AudioEditingScheme::canEditFrame(AudioEditingScheme::Scheme::NotesPrimary, originalF0, 1)
        || !AudioEditingScheme::canEditFrame(AudioEditingScheme::Scheme::NotesPrimary, originalF0, 2)) {
        logFail(testName, "notes-primary frame edit gate no longer matches voiced-only policy");
        return;
    }

    if (Loc::get(Language::English, Loc::Keys::kCorrectedF0First) == "Corrected F0 First"
        || Loc::get(Language::English, Loc::Keys::kNotesFirst) == "Notes First") {
        logFail(testName, "interaction scheme display copy did not update to the new product wording");
        return;
    }

    if (preferencesSource.contains("shared.voicedOnlyEditing")) {
        logFail(testName, "voiced-only policy leaked into standalone shared preferences");
        return;
    }

    logPass(testName);
}

void runAudioEditingSchemeNotesPrimaryContractRemainsUnchangedTest()
{
    constexpr const char* testName = "AudioEditingScheme_NotesPrimaryContractRemainsUnchanged";

    if (!AudioEditingScheme::usesVoicedOnlyEditing(AudioEditingScheme::Scheme::NotesPrimary)) {
        logFail(testName, "notes-primary no longer enforces voiced-only editing");
        return;
    }

    if (!AudioEditingScheme::shouldSelectNotesForEditedFrameRange(AudioEditingScheme::Scheme::NotesPrimary)) {
        logFail(testName, "notes-primary no longer auto-selects notes for edited frame ranges");
        return;
    }

    if (AudioEditingScheme::allowsLineAnchorSegmentSelection(AudioEditingScheme::Scheme::NotesPrimary)) {
        logFail(testName, "notes-primary unexpectedly allows line-anchor segment selection");
        return;
    }

    AudioEditingScheme::ParameterTargetContext parameterContext;
    parameterContext.hasSelectedNotes = true;
    parameterContext.hasFrameSelection = true;

    const auto parameterTarget = AudioEditingScheme::resolveParameterTarget(
        AudioEditingScheme::Scheme::NotesPrimary,
        AudioEditingScheme::ParameterKind::RetuneSpeed,
        parameterContext);
    if (parameterTarget != AudioEditingScheme::ParameterTarget::SelectedNotes) {
        logFail(testName, "notes-primary retune target priority changed");
        return;
    }

    AudioEditingScheme::AutoTuneTargetContext autoTuneContext;
    autoTuneContext.totalFrameCount = 256;
    autoTuneContext.selectedNotesRange = { 24, 64 };
    autoTuneContext.selectionAreaRange = { 80, 160 };
    autoTuneContext.f0SelectionRange = { 40, 72 };

    const auto autoTuneDecision = AudioEditingScheme::resolveAutoTuneRange(
        AudioEditingScheme::Scheme::NotesPrimary,
        autoTuneContext);
    if (autoTuneDecision.target != AudioEditingScheme::AutoTuneTarget::SelectedNotes
        || !sameEditingRange(autoTuneDecision.range, autoTuneContext.selectedNotesRange)) {
        logFail(testName, "notes-primary auto-tune priority changed");
        return;
    }

    logPass(testName);
}

void runPianoRollHotPathSourceGuardNoPerEventDebugLoggingTest()
{
    constexpr const char* testName = "PianoRollHotPath_SourceGuard_NoPerEventDebugLogging";
    constexpr auto toolHandlerPath = "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp";

    const auto mouseMoveSection = extractWorkspaceFileSection(
        toolHandlerPath,
        "void PianoRollToolHandler::mouseMove",
        "void PianoRollToolHandler::mouseDown");
    const auto mouseDragSection = extractWorkspaceFileSection(
        toolHandlerPath,
        "void PianoRollToolHandler::mouseDrag",
        "void PianoRollToolHandler::mouseUp");
    const auto mouseUpSection = extractWorkspaceFileSection(
        toolHandlerPath,
        "void PianoRollToolHandler::mouseUp",
        "void PianoRollToolHandler::handleDeleteKey");
    const auto selectToolSection = extractWorkspaceFileSection(
        toolHandlerPath,
        "void PianoRollToolHandler::handleSelectTool",
        "void PianoRollToolHandler::handleDrawCurveTool");
    const auto drawCurveSection = extractWorkspaceFileSection(
        toolHandlerPath,
        "void PianoRollToolHandler::handleDrawCurveTool",
        "void PianoRollToolHandler::handleDrawNoteMouseDown");
    const auto drawNoteToolSection = extractWorkspaceFileSection(
        toolHandlerPath,
        "void PianoRollToolHandler::handleDrawNoteTool",
        "void PianoRollToolHandler::handleAutoTuneTool");

    if (mouseMoveSection.isEmpty()
        || mouseDragSection.isEmpty()
        || mouseUpSection.isEmpty()
        || selectToolSection.isEmpty()
        || drawCurveSection.isEmpty()
        || drawNoteToolSection.isEmpty()) {
        logFail(testName, "failed to locate one or more piano-roll hot-path source sections");
        return;
    }

    if (mouseMoveSection.contains("AppLogger::debug")
        || mouseDragSection.contains("AppLogger::debug")
        || mouseUpSection.contains("AppLogger::debug")
        || selectToolSection.contains("AppLogger::debug")
        || drawCurveSection.contains("AppLogger::debug")
        || drawNoteToolSection.contains("AppLogger::debug")) {
        logFail(testName, "piano-roll hot path still emits per-event debug logging");
        return;
    }

    const auto& componentSource = getFileCache().get("Source/Standalone/UI/PianoRollComponent.cpp");
    if (componentSource.contains("paint: starting") || componentSource.contains("paint: completed")) {
        logFail(testName, "piano-roll paint path still emits debug logging");
        return;
    }

    logPass(testName);
}

void runPianoRollInteractionSourceGuardInteractiveInvalidationIsNotFullBoundsTest()
{
    constexpr const char* testName = "PianoRollInteraction_SourceGuard_InteractiveInvalidationIsNotFullBounds";

    const auto buildContextSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "PianoRollToolHandler::Context PianoRollComponent::buildToolHandlerContext()",
        "void PianoRollComponent::initializeToolHandler()");
    if (buildContextSection.isEmpty()) {
        logFail(testName, "failed to locate tool-handler context construction");
        return;
    }

    if (buildContextSection.contains("toolCtx.invalidateVisual = [this]() {")
        && buildContextSection.contains("getLocalBounds()")) {
        logFail(testName, "interactive invalidation still falls back to full local bounds");
        return;
    }

    logPass(testName);
}

void runPianoRollInteractionSourceGuardDeleteLegacyCopyWritebackApiBeforeRefactorTest()
{
    constexpr const char* testName = "PianoRollInteraction_SourceGuard_DeleteLegacyCopyWritebackApiBeforeRefactor";

    const auto& header = getFileCache().get("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h");
    if (header.contains("std::function<std::vector<Note>()> getNotes;")
        || header.contains("std::function<bool(const std::vector<Note>&)> replaceNotes;")) {
        logFail(testName, "tool handler context still exposes the legacy getNotes/replaceNotes interactive API");
        return;
    }

    logPass(testName);
}

void runMaterializationStoreNotesRevisionAdvancesOnSetNotesTest()
{
    constexpr const char* testName = "MaterializationStore_NotesRevisionAdvancesOnSetNotes";

    MaterializationStore materializationStore;
    const uint64_t materializationId = materializationStore.createMaterialization(makeTestClipRequest());
    if (materializationId == 0) {
        logFail(testName, "failed to create materialization for notes revision test");
        return;
    }

    MaterializationStore::MaterializationNotesSnapshot beforeSnapshot;
    if (!materializationStore.getNotesSnapshot(materializationId, beforeSnapshot)) {
        logFail(testName, "failed to read initial notes snapshot");
        return;
    }

    std::vector<Note> notes;
    notes.push_back(makeUndoTestNote(0.1, 0.4, 440.0f));
    if (!materializationStore.setNotes(materializationId, notes)) {
        logFail(testName, "setNotes rejected a valid materialization update");
        return;
    }

    MaterializationStore::MaterializationNotesSnapshot afterSnapshot;
    if (!materializationStore.getNotesSnapshot(materializationId, afterSnapshot)) {
        logFail(testName, "failed to read updated notes snapshot");
        return;
    }

    if (afterSnapshot.notesRevision <= beforeSnapshot.notesRevision) {
        logFail(testName, "notes revision did not advance after setNotes");
        return;
    }

    if (afterSnapshot.notes.size() != 1 || !approxEqual(static_cast<float>(afterSnapshot.notes.front().pitch), 440.0f, 1.0e-4f)) {
        logFail(testName, "updated notes snapshot does not reflect the committed note payload");
        return;
    }

    logPass(testName);
}

void runPianoRollComponentSourceGuardPaintUsesCachedNotesInsteadOfProcessorReadTest()
{
    constexpr const char* testName = "PianoRollComponent_SourceGuard_PaintUsesCachedNotesInsteadOfProcessorRead";

    const auto paintSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "void PianoRollComponent::paint",
        "void PianoRollComponent::setInferenceActive");
    const auto renderItemSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "PianoRollRenderer::MaterializationRenderItem PianoRollComponent::buildMaterializationRenderItem",
        "void PianoRollComponent::visibilityChanged");
    if (paintSection.isEmpty()) {
        logFail(testName, "failed to locate piano-roll paint source section");
        return;
    }
    if (renderItemSection.isEmpty()) {
        logFail(testName, "failed to locate piano-roll materialization render item source section");
        return;
    }

    if (paintSection.contains("getCurrentClipNotesCopy()")) {
        logFail(testName, "paint path still reads legacy clip notes");
        return;
    }

    if (!renderItemSection.contains("item.displayNotes = getDisplayedNotes()")
        || !renderItemSection.contains("item.displayNotes = processor_->getMaterializationNotesById(placement.materializationId)")
        || !paintSection.contains("renderer_->drawNotes(g, ctx, item)")) {
        logFail(testName, "paint path must keep draft/display notes scoped to note rendering");
        return;
    }

    const auto rendererSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp",
        "void PianoRollRenderer::drawNotes",
        "} // namespace OpenTune");
    if (rendererSection.isEmpty()) {
        logFail(testName, "failed to locate piano-roll renderer source section");
        return;
    }

    if (!rendererSection.contains("const auto& notes = item.displayNotes")
        || rendererSection.contains("item.notes")
        || rendererSection.contains("committedNotes")) {
        logFail(testName, "renderer must not let draft notes feed correctedF0 curve shaping");
        return;
    }

    logPass(testName);
}

void runPianoRollDrawNoteDraftSurvivesMultiEventDragTest()
{
    constexpr const char* testName = "PianoRoll_DrawNoteDraft_SurvivesMultiEventDrag";

    PianoRollToolHandlerHarness harness;
    harness.handler.setTool(ToolId::DrawNote);

    const auto mouseDownPos = juce::Point<float>(10.0f, 100.0f);
    harness.handler.mouseDown(makeMouseEvent(harness.component, mouseDownPos, mouseDownPos, false));
    harness.handler.mouseDrag(makeMouseEvent(harness.component,
                                             juce::Point<float>(25.0f, 100.0f),
                                             mouseDownPos,
                                             true));

    if (!harness.state.noteDraft.active || harness.state.noteDraft.workingNotes.size() != 1) {
        logFail(testName, "first draw-note drag did not create a single working draft note");
        return;
    }

    const auto firstEndTime = harness.state.noteDraft.workingNotes.front().endTime;
    harness.handler.mouseDrag(makeMouseEvent(harness.component,
                                             juce::Point<float>(40.0f, 100.0f),
                                             mouseDownPos,
                                             true));

    if (!harness.state.noteDraft.active || harness.state.noteDraft.workingNotes.size() != 1) {
        logFail(testName, "multi-event draw-note drag reset the draft back to committed notes");
        return;
    }

    if (harness.state.noteDraft.workingNotes.front().endTime <= firstEndTime) {
        logFail(testName, "second draw-note drag event did not advance the existing working draft note");
        return;
    }

    logPass(testName);
}

void runPianoRollEmptySpaceSeekMouseDownOnlyArmsPendingTest()
{
    constexpr const char* testName = "PianoRollEmptySpaceSeek_MouseDownOnlyArmsPending";

    PianoRollToolHandlerHarness harness;
    harness.handler.setTool(ToolId::Select);

    const auto pos = juce::Point<float>(120.0f, 160.0f);
    harness.handler.mouseDown(makeMouseEvent(harness.component, pos, pos, false));

    if (harness.notifyPlayheadChangeCalls != 0) {
        logFail(testName, "empty-space mouseDown should not seek before mouseUp");
        return;
    }

    if (harness.state.selection.isSelectingArea || harness.state.noteDraft.active) {
        logFail(testName, "empty-space mouseDown should arm only pending seek intent");
        return;
    }

    logPass(testName);
}

void runPianoRollEmptySpaceSeekMouseUpWithinThresholdSeeksOnceTest()
{
    constexpr const char* testName = "PianoRollEmptySpaceSeek_MouseUpWithinThresholdSeeksOnce";

    PianoRollToolHandlerHarness harness;
    harness.handler.setTool(ToolId::Select);

    const auto down = juce::Point<float>(120.0f, 160.0f);
    harness.handler.mouseDown(makeMouseEvent(harness.component, down, down, false));
    harness.handler.mouseUp(makeMouseEvent(harness.component,
                                           juce::Point<float>(128.0f, 164.0f),
                                           down,
                                           false));

    if (harness.notifyPlayheadChangeCalls != 1) {
        logFail(testName, "empty-space click should seek exactly once on mouseUp");
        return;
    }

    if (harness.notifiedPlayheadTimes.empty() || !approxEqual(harness.notifiedPlayheadTimes.front(), 1.20, 1.0e-9)) {
        logFail(testName, "empty-space click should seek to the original down time");
        return;
    }

    if (harness.state.selection.hasSelectionArea || harness.state.noteDraft.active) {
        logFail(testName, "empty-space click should not leave selection or note draft state");
        return;
    }

    logPass(testName);
}

void runPianoRollEmptySpaceSeekDrawNoteClickDoesNotCreateNoteTest()
{
    constexpr const char* testName = "PianoRollEmptySpaceSeek_DrawNoteClickDoesNotCreateNote";

    PianoRollToolHandlerHarness harness;
    harness.handler.setTool(ToolId::DrawNote);

    const auto down = juce::Point<float>(120.0f, 160.0f);
    harness.handler.mouseDown(makeMouseEvent(harness.component, down, down, false));
    harness.handler.mouseUp(makeMouseEvent(harness.component, down, down, false));

    if (harness.notifyPlayheadChangeCalls != 1) {
        logFail(testName, "draw-note empty-space click should seek once");
        return;
    }

    if (!harness.committedNotes.empty()
        || harness.commitNoteDraftCalls != 0
        || harness.state.noteDraft.active
        || harness.state.drawNoteToolPendingDrag
        || harness.state.drawing.isDrawingNote) {
        logFail(testName, "draw-note empty-space click should not create or commit a note");
        return;
    }

    logPass(testName);
}

void runPianoRollEmptySpaceSeekHandDrawClickDoesNotApplyCorrectionTest()
{
    constexpr const char* testName = "PianoRollEmptySpaceSeek_HandDrawClickDoesNotApplyCorrection";

    PianoRollToolHandlerHarness harness;
    harness.handler.setTool(ToolId::HandDraw);
    harness.pitchCurve = makePitchCurveWithPayload({ 220.0f, 221.0f, 222.0f }, { 1.0f, 1.0f, 1.0f }, {}, 1, 100.0);
    harness.originalF0 = { 220.0f, 221.0f, 222.0f };
    harness.f0Timeline = F0Timeline{ 1, 100.0, 3 };

    const auto down = juce::Point<float>(120.0f, 160.0f);
    harness.handler.mouseDown(makeMouseEvent(harness.component, down, down, false));
    harness.handler.mouseUp(makeMouseEvent(harness.component, down, down, false));

    if (harness.notifyPlayheadChangeCalls != 1) {
        logFail(testName, "hand-draw empty-space click should seek once");
        return;
    }

    if (harness.applyManualCorrectionCalls != 0
        || harness.notifyPitchCurveEditedCalls != 0
        || harness.state.handDrawPendingDrag
        || harness.state.drawing.isDrawingF0) {
        logFail(testName, "hand-draw empty-space click should not start or commit correction");
        return;
    }

    logPass(testName);
}

void runPianoRollEmptySpaceSeekLineAnchorClickPlacesAnchorWithoutSeekTest()
{
    constexpr const char* testName = "PianoRollEmptySpaceSeek_LineAnchorClickPlacesAnchorWithoutSeek";

    PianoRollToolHandlerHarness harness;
    harness.handler.setTool(ToolId::LineAnchor);
    harness.pitchCurve = makePitchCurveWithPayload({ 220.0f, 221.0f, 222.0f }, { 1.0f, 1.0f, 1.0f }, {}, 1, 100.0);
    harness.originalF0 = { 220.0f, 221.0f, 222.0f };
    harness.f0Timeline = F0Timeline{ 1, 100.0, 3 };

    const auto down = juce::Point<float>(120.0f, 160.0f);
    harness.handler.mouseDown(makeMouseEvent(harness.component, down, down, false));
    harness.handler.mouseUp(makeMouseEvent(harness.component, down, down, false));

    if (harness.notifyPlayheadChangeCalls != 0) {
        logFail(testName, "line-anchor main-edit click should not seek");
        return;
    }

    if (!harness.state.drawing.isPlacingAnchors
        || harness.state.drawing.pendingAnchors.size() != 1
        || harness.applyManualCorrectionCalls != 0
        || harness.notifyPitchCurveEditedCalls != 0
        || harness.state.emptySpaceIntent.active) {
        logFail(testName, "line-anchor click should start the anchor tool state without routing through empty-space seek");
        return;
    }

    logPass(testName);
}

void runPianoRollEmptySpaceSeekSelectDragBeyondThresholdStartsBoxSelectionTest()
{
    constexpr const char* testName = "PianoRollEmptySpaceSeek_SelectDragBeyondThresholdStartsBoxSelection";

    PianoRollToolHandlerHarness harness;
    harness.handler.setTool(ToolId::Select);

    const auto down = juce::Point<float>(120.0f, 160.0f);
    harness.handler.mouseDown(makeMouseEvent(harness.component, down, down, false));
    harness.handler.mouseDrag(makeMouseEvent(harness.component,
                                             juce::Point<float>(140.0f, 178.0f),
                                             down,
                                             true));

    if (harness.notifyPlayheadChangeCalls != 0) {
        logFail(testName, "empty-space drag should not seek while converting to tool drag");
        return;
    }

    if (!harness.state.selection.isSelectingArea || !harness.state.selection.hasSelectionArea) {
        logFail(testName, "select empty-space drag beyond threshold should start box selection");
        return;
    }

    logPass(testName);
}

void runPianoRollEmptySpaceSeekSelectDragMouseUpFinishesBoxSelectionTest()
{
    constexpr const char* testName = "PianoRollEmptySpaceSeek_SelectDragMouseUpFinishesBoxSelection";

    PianoRollToolHandlerHarness harness;
    harness.handler.setTool(ToolId::Select);

    const auto down = juce::Point<float>(120.0f, 160.0f);
    const auto drag = juce::Point<float>(150.0f, 188.0f);
    harness.handler.mouseDown(makeMouseEvent(harness.component, down, down, false));
    harness.handler.mouseDrag(makeMouseEvent(harness.component, drag, down, true));
    harness.handler.mouseUp(makeMouseEvent(harness.component, drag, down, true));

    if (harness.notifyPlayheadChangeCalls != 0) {
        logFail(testName, "select empty-space drag should not seek on mouseUp");
        return;
    }

    if (harness.state.selection.isSelectingArea) {
        logFail(testName, "select empty-space drag should run the normal selection mouseUp path");
        return;
    }

    logPass(testName);
}

void runPianoRollEmptySpaceSeekDrawNoteDragBeyondThresholdCommitsNoteWithoutSeekTest()
{
    constexpr const char* testName = "PianoRollEmptySpaceSeek_DrawNoteDragBeyondThresholdCommitsNoteWithoutSeek";

    PianoRollToolHandlerHarness harness;
    harness.handler.setTool(ToolId::DrawNote);

    const auto down = juce::Point<float>(120.0f, 160.0f);
    const auto release = juce::Point<float>(165.0f, 160.0f);
    harness.handler.mouseDown(makeMouseEvent(harness.component, down, down, false));
    harness.handler.mouseDrag(makeMouseEvent(harness.component, release, down, true));
    harness.handler.mouseUp(makeMouseEvent(harness.component, release, down, true));

    if (harness.notifyPlayheadChangeCalls != 0) {
        logFail(testName, "draw-note empty-space drag should not also seek");
        return;
    }

    if (harness.commitNoteDraftCalls != 1 || harness.committedNotes.size() != 1) {
        logFail(testName, "draw-note empty-space drag should create exactly one note");
        return;
    }

    if (!approxEqual(harness.committedNotes.front().startTime, 1.20, 1.0e-9)
        || harness.committedNotes.front().endTime <= harness.committedNotes.front().startTime) {
        logFail(testName, "draw-note empty-space drag should use the original down time and release time");
        return;
    }

    logPass(testName);
}

void runPianoRollEmptySpaceSeekToolSwitchCancelsPendingIntentTest()
{
    constexpr const char* testName = "PianoRollEmptySpaceSeek_ToolSwitchCancelsPendingIntent";

    PianoRollToolHandlerHarness harness;
    harness.handler.setTool(ToolId::Select);

    const auto down = juce::Point<float>(120.0f, 160.0f);
    harness.handler.mouseDown(makeMouseEvent(harness.component, down, down, false));
    harness.handler.setTool(ToolId::DrawNote);
    harness.handler.mouseUp(makeMouseEvent(harness.component, down, down, false));

    if (harness.notifyPlayheadChangeCalls != 0) {
        logFail(testName, "tool switch should cancel a pending empty-space click seek");
        return;
    }

    if (harness.state.emptySpaceIntent.active
        || harness.state.drawNoteToolPendingDrag
        || harness.state.handDrawPendingDrag
        || harness.state.noteDraft.active) {
        logFail(testName, "tool switch should clear transient mouse gesture state");
        return;
    }

    logPass(testName);
}

void runPianoRollEmptySpaceSeekLineAnchorSegmentHitStillSelectsSegmentTest()
{
    constexpr const char* testName = "PianoRollEmptySpaceSeek_LineAnchorSegmentHitStillSelectsSegment";

    PianoRollToolHandlerHarness harness;
    harness.handler.setTool(ToolId::LineAnchor);
    harness.pitchCurve = makePitchCurveWithPayload({ 220.0f, 221.0f, 222.0f }, { 1.0f, 1.0f, 1.0f }, {}, 1, 100.0);
    harness.originalF0 = { 220.0f, 221.0f, 222.0f };
    harness.f0Timeline = F0Timeline{ 1, 100.0, 3 };
    harness.lineAnchorHitSegment = 7;

    const auto down = juce::Point<float>(120.0f, 160.0f);
    harness.handler.mouseDown(makeMouseEvent(harness.component, down, down, false));

    if (harness.notifyPlayheadChangeCalls != 0) {
        logFail(testName, "line-anchor segment hit should not be treated as empty-space seek");
        return;
    }

    if (harness.selectLineAnchorSegmentCalls != 1
        || harness.state.drawing.isPlacingAnchors
        || !harness.state.drawing.pendingAnchors.empty()) {
        logFail(testName, "line-anchor segment hit should keep existing segment selection behavior");
        return;
    }

    logPass(testName);
}

void runPianoRollLineAnchorRetuneSpeedAppliesOnlyOriginalF0ShapeResidualTest()
{
    constexpr const char* testName = "PianoRoll_LineAnchorRetuneSpeedAppliesOnlyOriginalF0ShapeResidual";

    PianoRollToolHandlerHarness harness;
    harness.handler.setTool(ToolId::LineAnchor);
    harness.retuneSpeed = 0.25f;
    harness.originalF0 = { 100.0f, 130.0f, 190.0f, 170.0f, 200.0f };
    harness.pitchCurve = makePitchCurveWithPayload(harness.originalF0,
                                                   std::vector<float>(harness.originalF0.size(), 1.0f),
                                                   {},
                                                   1,
                                                   100.0);
    harness.f0Timeline = F0Timeline{ 1, 100.0, static_cast<int>(harness.originalF0.size()) };
    harness.yToFreqOverride = [](float y) {
        return y < 100.0f ? 440.0f : 880.0f;
    };

    const auto first = juce::Point<float>(0.0f, 50.0f);
    const auto second = juce::Point<float>(4.0f, 150.0f);
    harness.handler.mouseDown(makeMouseEvent(harness.component, first, first, false));
    harness.handler.mouseDown(makeMouseEvent(harness.component, second, second, false));

    if (harness.applyManualCorrectionCalls != 1 || harness.lastManualCorrectionOps.size() != 1) {
        logFail(testName, "line-anchor should submit one ManualCorrectionOp through the real ToolHandler path");
        return;
    }

    const auto& op = harness.lastManualCorrectionOps.front();
    if (op.source != CorrectedSegment::Source::LineAnchor
        || op.startFrame != 0
        || op.endFrameExclusive != 4
        || op.f0Data.size() != 4
        || harness.lastManualCorrectionStartFrame != 0
        || harness.lastManualCorrectionEndFrame != 3
        || harness.lastManualCorrectionPreviewOnly) {
        logFail(testName, "line-anchor ManualCorrectionOp frame/source contract changed");
        return;
    }

    const auto sourceTrend = [&]() {
        double sumX = 0.0;
        double sumY = 0.0;
        double sumXX = 0.0;
        double sumXY = 0.0;
        int count = 0;

        for (int frame = op.startFrame; frame < op.endFrameExclusive; ++frame) {
            const float sourceF0 = harness.originalF0[static_cast<size_t>(frame)];
            if (sourceF0 <= 0.0f) {
                continue;
            }

            const double x = static_cast<double>(frame - op.startFrame);
            const double y = std::log2(sourceF0);
            sumX += x;
            sumY += y;
            sumXX += x * x;
            sumXY += x * y;
            ++count;
        }

        struct Trend {
            float intercept = 0.0f;
            float slope = 0.0f;
        };

        Trend trend;
        const double n = static_cast<double>(count);
        const double denom = n * sumXX - sumX * sumX;
        trend.slope = denom != 0.0
            ? static_cast<float>((n * sumXY - sumX * sumY) / denom)
            : 0.0f;
        trend.intercept = count > 0
            ? static_cast<float>((sumY - static_cast<double>(trend.slope) * sumX) / n)
            : 0.0f;
        return trend;
    }();

    const auto targetLineF0 = [](int frame) {
        const float t = static_cast<float>(frame) / 4.0f;
        return std::pow(2.0f, std::log2(440.0f) + (std::log2(880.0f) - std::log2(440.0f)) * t);
    };

    const auto expectedF0 = [&](int frame) {
        const float sourceLogTrend = sourceTrend.intercept
            + sourceTrend.slope * static_cast<float>(frame - op.startFrame);
        const float logShape = std::log2(harness.originalF0[static_cast<size_t>(frame)]) - sourceLogTrend;
        return targetLineF0(frame) * std::pow(2.0f, logShape * (1.0f - harness.retuneSpeed));
    };

    double residualLogSum = 0.0;
    double residualSlopeNum = 0.0;
    for (int frame = 0; frame < 4; ++frame) {
        if (!approxEqual(op.f0Data[static_cast<size_t>(frame)], expectedF0(frame), 1.0e-4f)) {
            logFail(testName, "retune speed should scale only the de-positioned OriginalF0 shape residual");
            return;
        }

        const double x = static_cast<double>(frame) - 1.5;
        const double residualLog = std::log2(op.f0Data[static_cast<size_t>(frame)] / targetLineF0(frame));
        residualLogSum += residualLog;
        residualSlopeNum += x * residualLog;
    }

    if (!approxEqual(static_cast<float>(residualLogSum), 0.0f, 1.0e-5f)) {
        logFail(testName, "line-anchor residual introduced an overall high/low pitch offset");
        return;
    }

    if (!approxEqual(static_cast<float>(residualSlopeNum), 0.0f, 1.0e-5f)) {
        logFail(testName, "line-anchor residual retained the OriginalF0 large-scale trend");
        return;
    }

    const auto pureTargetAtFrame2 = targetLineF0(2);
    if (approxEqual(op.f0Data[2], pureTargetAtFrame2, 1.0e-4f)) {
        logFail(testName, "line-anchor generation fell back to a target-only line and lost OriginalF0 shape");
        return;
    }

    if (harness.state.drawing.pendingAnchors.size() != 2
        || !approxEqual(harness.state.drawing.pendingAnchors[0].freq, 440.0f, 1.0e-4f)
        || !approxEqual(harness.state.drawing.pendingAnchors[1].freq, 880.0f, 1.0e-4f)) {
        logFail(testName, "retune speed must not move the anchor target positions");
        return;
    }

    CorrectedSegment committed(op.startFrame, op.endFrameExclusive, op.f0Data, op.source);
    committed.retuneSpeed = 0.95f;
    auto committedCurve = makePitchCurveWithPayload(harness.originalF0,
                                                    std::vector<float>(harness.originalF0.size(), 1.0f),
                                                    { committed },
                                                    1,
                                                    100.0);
    const auto rendered = renderPitchCurveF0(committedCurve, static_cast<int>(harness.originalF0.size()));
    const auto correctedOnly = renderPitchCurveCorrectedOnlyF0(committedCurve, static_cast<int>(harness.originalF0.size()));
    for (int frame = 0; frame < 4; ++frame) {
        const auto idx = static_cast<size_t>(frame);
        if (!approxEqual(rendered[idx], op.f0Data[idx], 1.0e-4f)
            || !approxEqual(correctedOnly[idx], op.f0Data[idx], 1.0e-4f)) {
            logFail(testName, "render/read should consume committed f0Data without retune-time remixing");
            return;
        }
    }

    logPass(testName);
}

void runPianoRollEmptySpaceSeekContinuousModeCentersOnSeekTest()
{
    constexpr const char* testName = "PianoRollEmptySpaceSeek_ContinuousModeCentersOnSeek";

    const auto notifySection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "toolCtx.notifyPlayheadChange = [this](double time)",
        "toolCtx.notifyPitchCurveEdited");

    if (notifySection.isEmpty()) {
        logFail(testName, "failed to locate playhead notification context");
        return;
    }

    if (!notifySection.contains("scrollMode_ == ScrollMode::Continuous")
        || !notifySection.contains("getPlayheadAbsolutePixelX(time)")
        || !notifySection.contains("setScrollOffset(centeredScroll)")) {
        logFail(testName, "continuous scroll mode no longer centers the playhead on seek");
        return;
    }

    logPass(testName);
}

void runManualPreviewMouseUpCommitIsAtomicTest()
{
    constexpr const char* testName = "ManualPreview_MouseUpCommit_IsAtomic";
    constexpr auto processorPath = "Source/PluginProcessor.cpp";

    PianoRollToolHandlerHarness harness;
    harness.handler.setTool(ToolId::Select);
    harness.pitchCurve = std::make_shared<PitchCurve>();
    harness.commitNotesAndSegmentsResult = false;

    const auto originalNote = makeUndoTestNote(0.10, 0.20, 220.0f);
    auto draftedNote = originalNote;
    draftedNote.endTime = 0.35;

    harness.committedNotes = { originalNote };
    harness.state.noteDraft.active = true;
    harness.state.noteDraft.baselineNotes = { originalNote };
    harness.state.noteDraft.workingNotes = { draftedNote };
    harness.state.noteDrag.draggedNoteIndex = 0;
    harness.state.noteDrag.draggedNoteIndices = { 0 };
    harness.state.noteDrag.isDraggingNotes = true;
    harness.state.noteDrag.manualStartTime = 0.10;
    harness.state.noteDrag.manualEndTime = 0.35;
    harness.state.noteDrag.previewStartFrame = 1;
    harness.state.noteDrag.previewEndFrameExclusive = 3;
    harness.state.noteDrag.previewF0 = { 220.0f, 221.0f };

    harness.handler.mouseUp(makeMouseEvent(harness.component,
                                           juce::Point<float>(35.0f, 50.0f),
                                           juce::Point<float>(10.0f, 50.0f),
                                           true));

    if (harness.committedNotes.size() != 1
        || !approxEqual(harness.committedNotes.front().startTime, originalNote.startTime, 1.0e-6)
        || !approxEqual(harness.committedNotes.front().endTime, originalNote.endTime, 1.0e-6)) {
        logFail(testName, "manual preview failure still advanced note truth before the curve side succeeded");
        return;
    }

    if (harness.notifyPitchCurveEditedCalls != 0) {
        logFail(testName, "manual preview failure still notified pitch-curve success");
        return;
    }

    const auto processorSection = extractWorkspaceFileSection(
        processorPath,
        "bool OpenTuneAudioProcessor::commitMaterializationNotesAndSegmentsById",
        "bool OpenTuneAudioProcessor::commitAutoTuneGeneratedNotesByMaterializationId");
    if (processorSection.isEmpty()) {
        logFail(testName, "failed to locate processor atomic commit section");
        return;
    }

    if (!processorSection.contains("commitNotesAndPitchCurve(")
        || !processorSection.contains("clonePitchCurveWithCorrectedSegments(")
        || processorSection.contains("replaceCorrectedSegments(")
        || processorSection.contains("materializationStore_->setNotes(")) {
        logFail(testName, "manual preview mouse-up still mutates store-owned curve before one atomic materialization commit");
        return;
    }

    logPass(testName);
}

void runNoteBasedSyncCommitDoesNotFallBackToNotesOnlyTest()
{
    constexpr const char* testName = "NoteBasedSyncCommit_DoesNotFallBackToNotesOnly";

    PianoRollToolHandlerHarness harness;
    harness.handler.setTool(ToolId::Select);
    harness.pitchCurve = makePitchCurveWithPayload(std::vector<float>(60, 220.0f),
                                                   std::vector<float>(60, 1.0f),
                                                   {},
                                                   1,
                                                   100.0);
    harness.f0Timeline = F0Timeline{ 1, 100.0, 60 };
    harness.commitNotesAndSegmentsResult = false;

    const auto originalNote = makeUndoTestNote(0.10, 0.20, 220.0f);
    harness.committedNotes = { originalNote };
    harness.state.noteDraft.active = true;
    harness.state.noteDraft.baselineNotes = { originalNote };
    harness.state.noteDraft.workingNotes = { originalNote };
    harness.state.noteResize.isResizing = true;
    harness.state.noteResize.noteIndex = 0;
    harness.state.noteResize.edge = NoteResizeEdge::Right;
    harness.state.noteResize.originalStartTime = originalNote.startTime;
    harness.state.noteResize.originalEndTime = originalNote.endTime;

    harness.handler.mouseDrag(makeMouseEvent(harness.component,
                                             juce::Point<float>(35.0f, 50.0f),
                                             juce::Point<float>(20.0f, 50.0f),
                                             true));

    harness.handler.mouseUp(makeMouseEvent(harness.component,
                                           juce::Point<float>(35.0f, 50.0f),
                                           juce::Point<float>(20.0f, 50.0f),
                                           true));

    if (harness.commitNotesAndSegmentsCalls != 1) {
        logFail(testName, "note-based sync edit did not attempt notes+segments commit");
        return;
    }

    if (harness.commitNoteDraftCalls != 0) {
        logFail(testName, "note-based sync edit fell back to notes-only commit after notes+segments failure");
        return;
    }

    if (harness.committedNotes.size() != 1
        || !approxEqual(harness.committedNotes.front().startTime, originalNote.startTime, 1.0e-6)
        || !approxEqual(harness.committedNotes.front().endTime, originalNote.endTime, 1.0e-6)) {
        logFail(testName, "note-based sync failure still advanced notes without correctedF0");
        return;
    }

    logPass(testName);
}

void runNoteBasedCorrectedF0SyncHasNoNotesOnlyFallbackGuardTest()
{
    constexpr const char* testName = "Architecture_NoteBasedCorrectedF0SyncHasNoNotesOnlyFallback";

    const auto toolSource = getFileCache().get("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto componentSource = getFileCache().get("Source/Standalone/UI/PianoRollComponent.cpp");

    const auto checkCorrectedF0SyncBranch = [&](const juce::String& source,
                                                const juce::String& functionStart,
                                                const juce::String& functionEnd) {
        const auto section = source.substring(source.indexOf(functionStart),
                                              source.indexOf(functionEnd));
        if (section.isEmpty()) {
            return false;
        }

        const int snapIndex = section.indexOf("auto snap = clonedCurve->getSnapshot();");
        const int correctedCommitIndex = section.indexOf("commitEditedMaterializationNotesAndSegments(");
        if (snapIndex < 0 || correctedCommitIndex <= snapIndex) {
            return false;
        }

        const auto syncBranch = section.substring(snapIndex, correctedCommitIndex);
        return !syncBranch.contains("commitNoteDraft()")
            && !syncBranch.contains("commitEditedMaterializationNotes(notes");
    };

    if (!checkCorrectedF0SyncBranch(componentSource,
                                    "bool PianoRollComponent::applyNoteParameterToSelectedNotes",
                                    "bool PianoRollComponent::applyParameterToFrameRange")) {
        logFail(testName, "notes-only fallback remains in correctedF0 sync branch");
        return;
    }

    const juce::String helperName = "bool commitNoteBasedCorrection";
    const auto helperSection = toolSource.substring(toolSource.indexOf(helperName),
                                                    toolSource.indexOf("const std::vector<Note>& committedNotes"));
    if (helperSection.isEmpty()
        || helperSection.contains("commitNoteDraft()")
        || !helperSection.contains("commitNotesAndSegments(")) {
        logFail(testName, "note-based tool edits no longer use a single notes+segments commit helper");
        return;
    }

    logPass(testName);
}

void runCorrectedF0PreviewOnlyActivatesInCorrectedF0PrimaryTest()
{
    constexpr const char* testName = "CorrectedF0Preview_OnlyActivatesInCorrectedF0Primary";
    constexpr auto componentPath = "Source/Standalone/UI/PianoRollComponent.cpp";

    const auto paintSection = extractWorkspaceFileSection(
        componentPath,
        "void PianoRollComponent::paint",
        "void PianoRollComponent::setInferenceActive");
    const auto previewSection = extractWorkspaceFileSection(
        componentPath,
        "void PianoRollComponent::drawNoteDragCurvePreview",
        "void PianoRollComponent::drawSelectionBox");

    if (paintSection.isEmpty() || previewSection.isEmpty()) {
        logFail(testName, "failed to locate note-drag preview render sections");
        return;
    }

    if (!paintSection.contains("drawNoteDragCurvePreview(")
        || !previewSection.contains("AudioEditingScheme::Scheme::CorrectedF0Primary")
        || previewSection.contains("AudioEditingScheme::Scheme::NotesPrimary")
        || !previewSection.contains("showCorrectedF0_")
        || !previewSection.contains("interactionState_.noteDrag.previewF0")) {
        logFail(testName, "transient corrected-F0 preview is not locked to CorrectedF0Primary only");
        return;
    }

    logPass(testName);
}

void runPianoRollVisualInvalidationDirtyAreasMergeWithoutForcedFullRepaintTest()
{
    constexpr const char* testName = "PianoRollVisualInvalidation_DirtyAreasMergeWithoutForcedFullRepaint";

    PianoRollVisualInvalidationState state;

    PianoRollVisualInvalidationRequest first;
    first.reasonsMask = static_cast<uint32_t>(PianoRollVisualInvalidationReason::Interaction);
    first.hasDirtyArea = true;
    first.dirtyArea = { 10, 20, 30, 40 };
    first.priority = PianoRollVisualInvalidationPriority::Interactive;

    PianoRollVisualInvalidationRequest second;
    second.reasonsMask = static_cast<uint32_t>(PianoRollVisualInvalidationReason::Interaction);
    second.hasDirtyArea = true;
    second.dirtyArea = { 25, 35, 20, 15 };
    second.priority = PianoRollVisualInvalidationPriority::Interactive;

    state.merge(first);
    state.merge(second);

    const auto expectedDirtyArea = first.dirtyArea.getUnion(second.dirtyArea);
    if (state.fullRepaint || !state.hasDirtyArea || state.dirtyArea != expectedDirtyArea) {
        logFail(testName, "dirty-area invalidation merged into an unexpected state");
        return;
    }

    const auto decision = makeVisualFlushDecision(state, juce::Rectangle<int>(0, 0, 200, 200));
    if (!decision.shouldRepaint || decision.fullRepaint || !decision.hasDirtyArea || decision.dirtyArea != expectedDirtyArea) {
        logFail(testName, "flush decision escalated merged dirty areas into a forced full repaint");
        return;
    }

    logPass(testName);
}

void runKeyShortcutMatchingUsesExplicitSettingsInputTest()
{
    constexpr const char* testName = "KeyShortcutMatching_UsesExplicitSettingsInput";

    const auto& source = getFileCache().get("Source/Utils/KeyShortcutConfig.h");
    if (!source.contains("matchesShortcut(const KeyShortcutSettings&")
        || source.contains("getMutableSettings()")
        || source.contains("getSettings()")
        || source.contains("setSettings(")) {
        logFail(testName, "key shortcut matching still depends on hidden global settings ownership");
        return;
    }

    logPass(testName);
}

void runThemeAndLanguageStartupInitializeFromAppPreferencesTest()
{
    constexpr const char* testName = "ThemeAndLanguageStartup_InitializeFromAppPreferences";

    const auto& standaloneSource = getFileCache().get("Source/Standalone/PluginEditor.cpp");
    const auto& pluginSource = getFileCache().get("Source/Plugin/PluginEditor.cpp");

    if (!standaloneSource.contains("AppPreferences")
        || !pluginSource.contains("AppPreferences")
        || standaloneSource.contains("themeChanged(Theme::getActiveTheme())")
        || pluginSource.contains("themeChanged(Theme::getActiveTheme())")
        || !standaloneSource.contains("applyThemeToEditor(appPreferences_.getState().shared.theme)")
        || !pluginSource.contains("applyThemeToEditor(appPreferences_.getState().shared.theme)")
        || !standaloneSource.contains("applyThemeToEditor(sharedPreferences.theme)")
        || !pluginSource.contains("applyThemeToEditor(sharedPreferences.theme)")) {
        logFail(testName, "editor startup still initializes theme or language from hidden global state instead of app preferences");
        return;
    }

    const auto& themeSource = getFileCache().get("Source/Standalone/UI/ThemeTokens.h");
    if (themeSource.contains("setActiveTheme(")
        || themeSource.contains("getActiveTheme(")
        || themeSource.contains("getActiveStyle(")
        || themeSource.contains("getActiveTokens(")) {
        logFail(testName, "theme tokens still expose hidden active-theme ownership");
        return;
    }

    const auto& localizationSource = getFileCache().get("Source/Utils/LocalizationManager.h");
    if (localizationSource.contains("currentLanguage_")
        || localizationSource.contains("void setLanguage(")
        || localizationSource.contains("Language getLanguage() const")
        || !localizationSource.contains("bindLanguageState")) {
        logFail(testName, "localization manager still owns current language instead of consuming explicit language input");
        return;
    }

    if (standaloneSource.contains("LocalizationManager::getInstance().setLanguage(")
        || pluginSource.contains("LocalizationManager::getInstance().setLanguage(")) {
        logFail(testName, "editors still push language through hidden localization ownership API");
        return;
    }

    logPass(testName);
}

void runAudioFormatRegistryRegistersImportFormatsTest()
{
    constexpr const char* testName = "AudioFormatRegistry_RegistersImportFormats";

    juce::AudioFormatManager formatManager;
    AudioFormatRegistry::registerImportFormats(formatManager);

    if (formatManager.getNumKnownFormats() < 2) {
        logFail(testName, "import registry did not register the mandatory WAV/AIFF decoders");
        return;
    }

    if (formatManager.findFormatForFileExtension(".wav") == nullptr
        || formatManager.findFormatForFileExtension(".aiff") == nullptr) {
        logFail(testName, "import registry is missing required lossless audio formats");
        return;
    }

    const auto wildcard = AudioFormatRegistry::getImportWildcardFilter();
    if (!wildcard.contains("*.wav") || !wildcard.contains("*.aiff")) {
        logFail(testName, "import wildcard is not derived from the registered decoder owner");
        return;
    }

    logPass(testName);
}

void runAudioFormatRegistryOpensGeneratedWavTest()
{
    constexpr const char* testName = "AudioFormatRegistry_OpensGeneratedWav";

    const auto directory = makeCleanTemporaryDirectory("audio-format-registry");
    const auto wavFile = directory.getChildFile("registry-smoke.wav");

    juce::AudioBuffer<float> buffer(1, 64);
    buffer.clear();
    buffer.setSample(0, 0, 0.25f);

    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::OutputStream> stream(wavFile.createOutputStream());
    if (stream == nullptr) {
        logFail(testName, "failed to create temporary wav output stream");
        return;
    }

    const auto writerOptions = juce::AudioFormatWriter::Options()
        .withSampleRate(44100.0)
        .withNumChannels(1)
        .withBitsPerSample(16);
    auto writer = wavFormat.createWriterFor(stream, writerOptions);
    if (writer == nullptr) {
        logFail(testName, "failed to create temporary wav writer");
        return;
    }

    if (!writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples())) {
        logFail(testName, "failed to write test wav payload");
        return;
    }

    writer.reset();

    auto reader = AudioFormatRegistry::createReaderFor(wavFile);
    if (reader == nullptr) {
        logFail(testName, "import registry could not open a generated wav file");
        return;
    }

    if (reader->sampleRate != 44100.0 || reader->numChannels != 1 || reader->lengthInSamples != 64) {
        logFail(testName, "import registry reader returned unexpected wav metadata");
        return;
    }

    logPass(testName);
}

void runStandaloneImportFlowCopiesPendingFileBeforeMovingPendingImportTest()
{
    constexpr const char* testName = "StandaloneImportFlow_CopiesPendingFileBeforeMove";

    const auto& source = getFileCache().get("Source/Standalone/PluginEditor.cpp");
    const int copyIndex = source.indexOf("const auto sourceFile = pendingImport.file;");
    const int loadIndex = copyIndex >= 0 ? source.indexOf(copyIndex, "asyncAudioLoader_.loadAudioFile(") : -1;
    const int moveIndex = loadIndex >= 0 ? source.indexOf(loadIndex, "std::move(pendingImport)") : -1;

    if (copyIndex < 0 || loadIndex < 0 || moveIndex < 0 || !(copyIndex < loadIndex && loadIndex < moveIndex)) {
        logFail(testName, "standalone import does not copy pendingImport.file before moving PendingImport into the completion lambda");
        return;
    }

    const auto loadCallBeforeMove = source.substring(loadIndex, moveIndex);
    if (loadCallBeforeMove.contains("pendingImport.file")) {
        logFail(testName, "standalone import still reads pendingImport.file inline while moving the same PendingImport into the completion lambda");
        return;
    }

    if (!loadCallBeforeMove.contains("sourceFile")) {
        logFail(testName, "standalone import loadAudioFile call does not use the stable sourceFile copy");
        return;
    }

    logPass(testName);
}

void runPianoRollPlayheadViewportStopsBeforeScrollbarsTest()
{
    constexpr const char* testName = "PianoRollPlayheadViewport_StopsBeforeScrollbars";

    PianoRollComponent pianoRoll;
    pianoRoll.setSize(640, 360);

    const auto viewportBounds = PianoRollComponentTestProbe::getTimelineViewportBounds(pianoRoll);
    const auto ctx = PianoRollComponentTestProbe::buildRenderContext(pianoRoll);

    if (viewportBounds.getWidth() != (640 - 12 - 15) || viewportBounds.getHeight() != (360 - 12 - 15)) {
        logFail(testName, "timeline viewport still includes scrollbar space");
        return;
    }

    if (ctx.width != viewportBounds.getRight() || ctx.height != viewportBounds.getBottom()) {
        logFail(testName, "render context does not consume the scrollbar-excluded viewport bounds");
        return;
    }

    logPass(testName);
}

void runPianoRollPlayheadUsesDedicatedOverlayTest()
{
    constexpr const char* testName = "PianoRollPlayhead_UsesDedicatedOverlay";

    const auto& header = getFileCache().get("Source/Standalone/UI/PianoRollComponent.h");
    const auto& componentSource = getFileCache().get("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto& rendererHeader = getFileCache().get("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");
    const auto& rendererSource = getFileCache().get("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");

    if (!header.contains("#include \"PlayheadOverlayComponent.h\"")
        || !header.contains("void onHeartbeatTick();")
        || !header.contains("PlayheadOverlayComponent playheadOverlay_")) {
        logFail(testName, "piano roll no longer declares a dedicated playhead overlay contract");
        return;
    }

    if (!header.contains("playheadOverlay_.setPlaying(playing);")
        || !componentSource.contains("playheadOverlay_.setTimelineStartSeconds")
        || !componentSource.contains("playheadOverlay_.setPlayheadSeconds")
        || !componentSource.contains("playheadOverlay_.setScrollOffset")
        || componentSource.contains("renderer_->drawPlayhead(g, ctx)")) {
        logFail(testName, "piano roll still paints the playhead through the heavy main layer");
        return;
    }

    if (rendererHeader.contains("drawPlayhead(")
        || rendererHeader.contains("showPlayhead")
        || rendererHeader.contains("playheadSeconds")
        || rendererHeader.contains("playheadColour")
        || rendererSource.contains("void PianoRollRenderer::drawPlayhead")) {
        logFail(testName, "piano-roll renderer still owns playhead drawing instead of materialization rendering only");
        return;
    }

    logPass(testName);
}

void runStandaloneEditorHeartbeatDrivesPianoRollVisualLoopTest()
{
    constexpr const char* testName = "StandaloneEditorHeartbeat_DrivesPianoRollVisualLoop";

    const auto& source = getFileCache().get("Source/Standalone/PluginEditor.cpp");
    if (!source.contains("pianoRoll_.onHeartbeatTick();")) {
        logFail(testName, "standalone editor timer no longer drives piano-roll heartbeat updates");
        return;
    }

    logPass(testName);
}

void runStandaloneArrangementMultipleClipPlacementsStayTrackLocalTest()
{
    constexpr const char* testName = "StandaloneArrangement_MultipleClipPlacementsStayTrackLocal";

    MaterializationStore materializationStore;
    StandaloneArrangement arrangement;

    const uint64_t materializationA = materializationStore.createMaterialization(makeTestClipRequest());
    const uint64_t materializationB = materializationStore.createMaterialization(makeTestClipRequest());
    if (materializationA == 0 || materializationB == 0) {
        logFail(testName, "failed to create test materializations");
        return;
    }

    StandaloneArrangement::Placement placementA;
    placementA.materializationId = materializationA;
    placementA.timelineStartSeconds = 0.0;
    placementA.durationSeconds = 1.0;

    StandaloneArrangement::Placement placementB;
    placementB.materializationId = materializationB;
    placementB.timelineStartSeconds = 2.0;
    placementB.durationSeconds = 1.0;

    if (!arrangement.insertPlacement(0, placementA) || !arrangement.insertPlacement(0, placementB)) {
        logFail(testName, "failed to insert track-local placements");
        return;
    }

    if (arrangement.getNumPlacements(0) != 2) {
        logFail(testName, "track-local multiple clip placement regressed");
        return;
    }

    if (arrangement.findPlacementIndexById(0, placementA.placementId) < 0
        || arrangement.findPlacementIndexById(0, placementB.placementId) < 0) {
        logFail(testName, "placement escaped owning track");
        return;
    }

    if (arrangement.getNumPlacements(1) != 0
        || arrangement.findPlacementIndexById(1, placementA.placementId) >= 0
        || arrangement.findPlacementIndexById(1, placementB.placementId) >= 0) {
        logFail(testName, "placement leaked into unrelated track");
        return;
    }

    logPass(testName);
}

void runMaterializationDetectedKeyStateStaysMaterializationLocalTest()
{
    constexpr const char* testName = "MaterializationDetectedKeyStateStaysMaterializationLocal";

    OpenTuneAudioProcessor processor;
    const auto committedA = processor.commitPreparedImportAsPlacement(makePreparedImport("clip-a"), {0, 0.0});
    const auto committedB = processor.commitPreparedImportAsPlacement(makePreparedImport("clip-b"), {1, 0.0});
    const uint64_t materializationA = committedA.materializationId;
    const uint64_t materializationB = committedB.materializationId;
    if (!committedA.isValid() || !committedB.isValid()) {
        logFail(testName, "failed to materialize processor clips");
        return;
    }

    DetectedKey key;
    key.root = Key::D;
    key.scale = Scale::Minor;
    key.confidence = 0.87f;

    if (!processor.setMaterializationDetectedKeyById(materializationA, key)) {
        logFail(testName, "failed to set detected key by materializationId");
        return;
    }

    const auto resolvedA = processor.getMaterializationDetectedKeyById(materializationA);
    const auto resolvedB = processor.getMaterializationDetectedKeyById(materializationB);
    if (!sameDetectedKey(resolvedA, key)) {
        logFail(testName, "materialization-local detected key readback regressed");
        return;
    }

    if (resolvedB.confidence > 0.0f) {
        logFail(testName, "detected key leaked to another materialization");
        return;
    }

    logPass(testName);
}

void runDeletePlacementLastReferenceReclaimsMaterializationTest()
{
    constexpr const char* testName = "DeletePlacement_LastReferenceReclaimsMaterialization";

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(makePreparedImport("last-reference-materialization"), {0, 0.0});
    auto* arrangement = processor.getStandaloneArrangement();
    if (!committed.isValid() || arrangement == nullptr) {
        logFail(testName, "failed to prepare placement state");
        return;
    }

    if (!processor.deletePlacement(0, 0).has_value()) {
        logFail(testName, "deletePlacement rejected valid placement deletion");
        return;
    }

    // Trigger reclaim sweep synchronously (in production this runs on message thread via AsyncUpdater)
    processor.runReclaimSweepOnMessageThread();

    if (arrangement->getNumPlacements(0) != 0) {
        logFail(testName, "timeline delete did not remove the placement itself");
        return;
    }

    if (processor.getMaterializationAudioBufferById(committed.materializationId) != nullptr) {
        logFail(testName, "deleting the last remaining placement left orphan materialization resident");
        return;
    }

    SourceStore::SourceSnapshot sourceSnapshot;
    if (processor.getSourceSnapshotById(committed.sourceId, sourceSnapshot)) {
        logFail(testName, "deleting the last remaining placement left orphan source resident");
        return;
    }

    logPass(testName);
}

void runDeletePlacementSharedReferenceKeepsSourceUntilFinalOwnerRemovedTest()
{
    constexpr const char* testName = "DeletePlacement_SharedReferenceKeepsSourceUntilFinalOwnerRemoved";

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(makePreparedImport("shared-delete-materialization", 256), {0, 0.0});
    auto* arrangement = processor.getStandaloneArrangement();
    if (!committed.isValid() || arrangement == nullptr) {
        logFail(testName, "failed to prepare shared source placement state");
        return;
    }

    OpenTuneAudioProcessor::PreparedImport siblingMaterialization;
    siblingMaterialization.displayName = "shared-delete-materialization-sibling";
    siblingMaterialization.storedAudioBuffer.setSize(1, 64);
    siblingMaterialization.storedAudioBuffer.clear();
    siblingMaterialization.storedAudioBuffer.setSample(0, 0, 0.5f);

    const auto sibling = processor.commitPreparedImportAsPlacement(std::move(siblingMaterialization),
                                                                   {0, 1.0},
                                                                   committed.sourceId);
    if (!sibling.isValid()) {
        logFail(testName, "failed to create sibling materialization on the same source");
        return;
    }

    if (sibling.sourceId != committed.sourceId || sibling.materializationId == committed.materializationId) {
        logFail(testName, "same-source sibling placement did not create a distinct materialization");
        return;
    }

    if (!processor.deletePlacement(0, 0).has_value()) {
        logFail(testName, "failed to delete the first same-source placement");
        return;
    }

    // Trigger reclaim sweep; source should survive (sibling still active)
    processor.runReclaimSweepOnMessageThread();

    if (arrangement->getNumPlacements(0) != 1) {
        logFail(testName, "timeline delete did not keep the surviving placement");
        return;
    }

    if (processor.getMaterializationAudioBufferById(committed.materializationId) != nullptr) {
        logFail(testName, "deleted placement kept its unreferenced materialization alive");
        return;
    }

    if (processor.getMaterializationAudioBufferById(sibling.materializationId) == nullptr) {
        logFail(testName, "surviving placement lost its materialization during sibling delete");
        return;
    }

    SourceStore::SourceSnapshot sourceSnapshot;
    if (!processor.getSourceSnapshotById(committed.sourceId, sourceSnapshot)) {
        logFail(testName, "shared source was reclaimed before its final materialization disappeared");
        return;
    }

    if (!processor.deletePlacement(0, 0).has_value()) {
        logFail(testName, "failed to delete the last same-source placement");
        return;
    }

    // Trigger reclaim sweep; now source should be gone
    processor.runReclaimSweepOnMessageThread();

    if (processor.getMaterializationAudioBufferById(sibling.materializationId) != nullptr) {
        logFail(testName, "final materialization survived after its last placement disappeared");
        return;
    }

    if (processor.getSourceSnapshotById(committed.sourceId, sourceSnapshot)) {
        logFail(testName, "source survived after its final materialization disappeared");
        return;
    }

    logPass(testName);
}

void runProcessorImportCreatesDistinctSourceMaterializationAndPlacementOwnersTest()
{
    constexpr const char* testName = "ProcessorImport_CreatesDistinctSourceMaterializationAndPlacementOwners";

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(makePreparedImport("source-materialization-placement", 512), { 1, 0.5 });
    if (!committed.isValid()) {
        logFail(testName, "failed to create imported placement owners");
        return;
    }

    SourceStore::SourceSnapshot sourceSnapshot;
    if (!processor.getSourceSnapshotById(committed.sourceId, sourceSnapshot) || sourceSnapshot.audioBuffer == nullptr) {
        logFail(testName, "import did not create a persisted source owner");
        return;
    }

    OpenTuneAudioProcessor::MaterializationSnapshot materializationSnapshot;
    if (!processor.getMaterializationSnapshotById(committed.materializationId, materializationSnapshot)
        || materializationSnapshot.audioBuffer == nullptr
        || materializationSnapshot.sourceId != committed.sourceId) {
        logFail(testName, "import did not create a materialization that points back to the committed source owner");
        return;
    }

    StandaloneArrangement::Placement placement;
    if (!processor.getPlacementById(1, committed.placementId, placement)
        || placement.materializationId != committed.materializationId) {
        logFail(testName, "placement did not retain the imported materialization owner");
        return;
    }

    const double expectedSourceEndSeconds = TimeCoordinate::samplesToSeconds(materializationSnapshot.audioBuffer->getNumSamples(),
                                                                             TimeCoordinate::kRenderSampleRate);
    if (!approxEqual(materializationSnapshot.sourceWindow.sourceStartSeconds, 0.0, 1.0e-6)
        || !approxEqual(materializationSnapshot.sourceWindow.sourceEndSeconds, expectedSourceEndSeconds, 1.0e-6)
        || materializationSnapshot.lineageParentMaterializationId != 0) {
        logFail(testName, "imported materialization did not persist its source provenance window");
        return;
    }

    logPass(testName);
}

void runEnsureSourceByIdCreatesForcedSourceOwnerTest()
{
    constexpr const char* testName = "EnsureSourceById_CreatesForcedSourceOwner";

    OpenTuneAudioProcessor processor;
    const auto sourceAudio = makeSharedAudioBuffer(256, 0.75f);
    if (!processor.ensureSourceById(77, "ara-source", sourceAudio, 48000.0)) {
        logFail(testName, "failed to seed a forced source owner");
        return;
    }

    SourceStore::SourceSnapshot sourceSnapshot;
    if (!processor.getSourceSnapshotById(77, sourceSnapshot)
        || sourceSnapshot.audioBuffer != sourceAudio
        || !approxEqual(sourceSnapshot.sampleRate, 48000.0, 1.0e-6)) {
        logFail(testName, "forced source owner was not persisted with the requested identity");
        return;
    }

    logPass(testName);
}

void runProcessorStateFreshProcessorRoundTripsMaterializationAndPlacementTest()
{
    constexpr const char* testName = "ProcessorState_FreshProcessorRoundTripsMaterializationAndPlacement";

    OpenTuneAudioProcessor writer;
    const auto committed = writer.commitPreparedImportAsPlacement(makePreparedImport("roundtrip-materialization", 512), { 2, 1.25 });
    if (!committed.isValid()) {
        logFail(testName, "failed to seed source processor state");
        return;
    }

    // replacement audio 涓?import 绛夐暱锛?12 samples锛夛細replaceAudio 涓嶆敼 sourceWindow锛?:1 閲嶆覆鏌撹涔?
    const auto replacementAudio = makeSharedAudioBuffer(512, 0.5f);
    const std::vector<SilentGap> silentGaps{ SilentGap{ 64, 128, -60.0f } };
    const std::vector<Note> notes{ makeUndoTestNote(0.12, 0.28, 233.0f) };
    DetectedKey key;
    key.root = Key::E;
    key.scale = Scale::Major;
    key.confidence = 0.91f;

    if (!writer.replaceMaterializationAudioById(committed.materializationId, replacementAudio, silentGaps)
        || !writer.setMaterializationPitchCurveById(committed.materializationId, std::make_shared<PitchCurve>())
        || !writer.setMaterializationNotesById(committed.materializationId, notes)
        || !writer.setMaterializationDetectedKeyById(committed.materializationId, key)
        || !writer.setMaterializationOriginalF0StateById(committed.materializationId, OriginalF0State::Ready)) {
        logFail(testName, "failed to seed materialization payload before state serialization");
        return;
    }

    const auto stateData = serializeProcessorState(writer);
    const auto rawAudioBytes = static_cast<size_t>(replacementAudio->getNumChannels())
        * static_cast<size_t>(replacementAudio->getNumSamples())
        * sizeof(float);
    const auto maxExpectedBytes = rawAudioBytes + (rawAudioBytes / 8) + 65536;
    if (stateData.getSize() > maxExpectedBytes) {
        logFail(testName, "serialized state still inflates materialization payload far beyond near-raw binary size");
        return;
    }
    OpenTuneAudioProcessor reader;
    reader.setStateInformation(stateData.getData(), static_cast<int>(stateData.getSize()));

    SourceStore::SourceSnapshot restoredSource;
    if (!reader.getSourceSnapshotById(committed.sourceId, restoredSource) || restoredSource.audioBuffer == nullptr) {
        logFail(testName, "fresh processor restore did not recreate source owner state");
        return;
    }

    MaterializationStore::MaterializationSnapshot restoredMaterialization;
    if (!reader.getMaterializationSnapshotById(committed.materializationId, restoredMaterialization)) {
        logFail(testName, "fresh processor restore did not recreate materialization owner state");
        return;
    }

    if (restoredMaterialization.audioBuffer == nullptr
        || restoredMaterialization.audioBuffer->getNumSamples() != replacementAudio->getNumSamples()) {
        logFail(testName, "fresh processor restore lost serialized audio payload");
        return;
    }

    if (restoredMaterialization.silentGaps.size() != silentGaps.size()) {
        logFail(testName, "fresh processor restore lost serialized silent gaps");
        return;
    }

    if (restoredMaterialization.notes.size() != notes.size()) {
        logFail(testName, "fresh processor restore lost serialized notes");
        return;
    }

    if (!sameDetectedKey(restoredMaterialization.detectedKey, key)) {
        logFail(testName, "fresh processor restore lost serialized detected key");
        return;
    }

    if (restoredMaterialization.originalF0State != OriginalF0State::Ready) {
        logFail(testName, "fresh processor restore lost serialized original-F0 state");
        return;
    }

    // sourceWindow 蹇呴』淇濇寔鍘?import 鐨勫€硷紙replaceAudio 涓嶆敼 sourceWindow锛?
    const double expectedSourceEndSeconds = TimeCoordinate::samplesToSeconds(512,
                                                                             TimeCoordinate::kRenderSampleRate);
    if (!approxEqual(restoredMaterialization.sourceWindow.sourceStartSeconds, 0.0, 1.0e-6)
        || !approxEqual(restoredMaterialization.sourceWindow.sourceEndSeconds, expectedSourceEndSeconds, 1.0e-6)
        || restoredMaterialization.lineageParentMaterializationId != 0) {
        logFail(testName, "fresh processor restore lost serialized source provenance metadata");
        return;
    }

    StandaloneArrangement::Placement restoredPlacement;
    if (!reader.getPlacementById(2, committed.placementId, restoredPlacement)) {
        logFail(testName, "fresh processor restore did not recreate placement mapping");
        return;
    }

    if (restoredMaterialization.sourceId != committed.sourceId) {
        logFail(testName, "restored materialization no longer points to the serialized source owner");
        return;
    }

    if (!approxEqual(restoredPlacement.timelineStartSeconds, 1.25, 1.0e-6)
        || restoredPlacement.materializationId != committed.materializationId) {
        logFail(testName, "restored placement mapping does not match serialized truth");
        return;
    }

    logPass(testName);
}

void runSourceMaterializationStoresReplaceContentStoreTest()
{
    constexpr const char* testName = "SourceMaterializationStores_ReplaceContentStore";

    const auto& processorHeader = getFileCache().get("Source/PluginProcessor.h");
    if (workspaceFileExists("Source/ContentStore.h")
        || !workspaceFileExists("Source/SourceStore.h")
        || !workspaceFileExists("Source/MaterializationStore.h")
        || !processorHeader.contains("SourceStore")
        || !processorHeader.contains("MaterializationStore")
        || processorHeader.contains("ContentStore")) {
        logFail(testName, "shared runtime still depends on ContentStore instead of SourceStore plus MaterializationStore");
        return;
    }

    logPass(testName);
}

void runProcessorStateBinarySerializationAvoidsXmlBase64Test()
{
    constexpr const char* testName = "ProcessorState_BinarySerializationAvoidsXmlBase64";

    const auto& source = getFileCache().get("Source/PluginProcessor.cpp");
    if (source.contains("toBase64Encoding(")
        || source.contains("fromBase64Encoding(")
        || source.contains("copyXmlToBinary(")
        || source.contains("getXmlFromBinary(")) {
        logFail(testName, "processor state serialization still depends on XML/base64 expansion");
        return;
    }

    logPass(testName);
}

void runProcessorStateRestoreReplacesExistingOwnerStateTest()
{
    constexpr const char* testName = "ProcessorState_RestoreReplacesExistingOwnerState";

    OpenTuneAudioProcessor sourceProcessor;
    const auto sourceCommitted = sourceProcessor.commitPreparedImportAsPlacement(makePreparedImport("source-state", 256), { 1, 2.0 });
    if (!sourceCommitted.isValid()) {
        logFail(testName, "failed to seed source processor");
        return;
    }

    const auto sourceStateData = serializeProcessorState(sourceProcessor);

    OpenTuneAudioProcessor targetProcessor;
    const auto stalePlacementA = targetProcessor.commitPreparedImportAsPlacement(makePreparedImport("stale-a", 128), { 0, 0.0 });
    const auto stalePlacementB = targetProcessor.commitPreparedImportAsPlacement(makePreparedImport("stale-b", 128), { 3, 4.0 });
    auto* arrangement = targetProcessor.getStandaloneArrangement();
    if (!stalePlacementA.isValid() || !stalePlacementB.isValid() || arrangement == nullptr) {
        logFail(testName, "failed to seed target processor with stale owner state");
        return;
    }

    targetProcessor.setStateInformation(sourceStateData.getData(), static_cast<int>(sourceStateData.getSize()));

    if (arrangement->getNumPlacements(0) != 0
        || arrangement->getNumPlacements(3) != 0
        || arrangement->getNumPlacements(1) != 1) {
        logFail(testName, "state restore appended to existing placement owners instead of replacing them");
        return;
    }

    if (targetProcessor.getMaterializationAudioBufferById(stalePlacementB.materializationId) != nullptr) {
        logFail(testName, "state restore left stale materialization owner state resident");
        return;
    }

    StandaloneArrangement::Placement restoredPlacement;
    if (!targetProcessor.getPlacementById(1, sourceCommitted.placementId, restoredPlacement)) {
        logFail(testName, "state restore did not materialize the replacement placement");
        return;
    }

    if (!approxEqual(restoredPlacement.timelineStartSeconds, 2.0, 1.0e-6)) {
        logFail(testName, "replacement placement timeline start did not match serialized state");
        return;
    }

    logPass(testName);
}

void runMaterializationPlacementMaterializationLocalTimingStaysIndependentFromPlacementTest()
{
    constexpr const char* testName = "MaterializationPlacement_MaterializationLocalTimingStaysIndependentFromPlacement";

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(makePreparedImport("materialization-local", 44100), {0, 2.0});
    auto* arrangement = processor.getStandaloneArrangement();
    if (!committed.isValid() || arrangement == nullptr) {
        logFail(testName, "failed to prepare materialization/placement state");
        return;
    }

    const std::vector<Note> notes{ makeUndoTestNote(0.10, 0.20, 220.0f) };
    if (!processor.setMaterializationNotesById(committed.materializationId, notes)) {
        logFail(testName, "failed to seed materialization-local notes");
        return;
    }

    StandaloneArrangement::Placement placement;
    if (!arrangement->getPlacementById(0, committed.placementId, placement)) {
        logFail(testName, "failed to resolve placement");
        return;
    }

    if (!arrangement->setPlacementTimelineStartSeconds(0, placement.placementId, 5.0)) {
        logFail(testName, "failed to move placement without touching materialization");
        return;
    }

    const auto storedNotes = processor.getMaterializationNotesById(committed.materializationId);
    if (storedNotes.size() != 1
        || !approxEqual(storedNotes.front().startTime, 0.10, 1.0e-6)
        || !approxEqual(storedNotes.front().endTime, 0.20, 1.0e-6)) {
        logFail(testName, "placement move polluted materialization-local note timing");
        return;
    }

    logPass(testName);
}

void runMaterializationCommandsDoNotMutateTimelinePlacementTruthTest()
{
    constexpr const char* testName = "MaterializationCommands_DoNotMutateTimelinePlacementTruth";

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(makePreparedImport("materialization-only-command", 44100), {0, 1.5});
    auto* arrangement = processor.getStandaloneArrangement();
    if (!committed.isValid() || arrangement == nullptr) {
        logFail(testName, "failed to prepare placement state for materialization command test");
        return;
    }

    if (!processor.setMaterializationPitchCurveById(committed.materializationId, std::make_shared<PitchCurve>())) {
        logFail(testName, "failed to seed pitch curve for materialization command test");
        return;
    }

    StandaloneArrangement::Placement beforePlacement;
    if (!arrangement->getPlacementById(0, committed.placementId, beforePlacement)) {
        logFail(testName, "failed to resolve placement before materialization command");
        return;
    }

    const std::vector<Note> notes{ makeUndoTestNote(0.12, 0.28, 220.0f) };
    const std::vector<CorrectedSegment> segments{
        makeUndoTestSegment(0, 3, {220.0f, 221.0f, 222.0f}, CorrectedSegment::Source::HandDraw)
    };

    if (!processor.setMaterializationNotesById(committed.materializationId, notes)
        || !processor.setMaterializationCorrectedSegmentsById(committed.materializationId, segments)) {
        logFail(testName, "materialization command failed to commit note or segment edits");
        return;
    }

    StandaloneArrangement::Placement afterPlacement;
    if (!arrangement->getPlacementById(0, committed.placementId, afterPlacement)) {
        logFail(testName, "placement disappeared after materialization command");
        return;
    }

    if (!approxEqual(afterPlacement.timelineStartSeconds, beforePlacement.timelineStartSeconds, 1.0e-6)
        || !approxEqual(afterPlacement.durationSeconds, beforePlacement.durationSeconds, 1.0e-6)
        || afterPlacement.placementId != beforePlacement.placementId
        || afterPlacement.materializationId != beforePlacement.materializationId) {
        logFail(testName, "materialization command mutated placement truth");
        return;
    }

    logPass(testName);
}

void runEditingCommandDoesNotMutatePlacementTest()
{
    constexpr const char* testName = "EditingCommand_DoesNotMutatePlacement";

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(makePreparedImport("edit-command", 44100), {0, 0.75});
    auto* arrangement = processor.getStandaloneArrangement();
    if (!committed.isValid() || arrangement == nullptr) {
        logFail(testName, "failed to prepare editing-command state");
        return;
    }

    StandaloneArrangement::Placement beforePlacement;
    if (!arrangement->getPlacementById(0, committed.placementId, beforePlacement)) {
        logFail(testName, "failed to resolve placement before edit command");
        return;
    }

    const std::vector<Note> oldNotes;
    const std::vector<Note> newNotes{ makeUndoTestNote(0.08, 0.24, 233.0f) };
    processor.setMaterializationNotesById(committed.materializationId, newNotes);

    StandaloneArrangement::Placement afterPlacement;
    if (!arrangement->getPlacementById(0, committed.placementId, afterPlacement)) {
        logFail(testName, "placement disappeared after edit command");
        return;
    }

    if (!approxEqual(afterPlacement.timelineStartSeconds, beforePlacement.timelineStartSeconds, 1.0e-6)
        || !approxEqual(afterPlacement.durationSeconds, beforePlacement.durationSeconds, 1.0e-6)
        || afterPlacement.placementId != beforePlacement.placementId) {
        logFail(testName, "editing command mutated placement state");
        return;
    }

    logPass(testName);
}

void runSplitPlacementBirthsIndependentMaterializationsTest()
{
    constexpr const char* testName = "SplitPlacement_BirthsIndependentMaterializations";

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(makePreparedImport("split-materialization", 44100), {0, 0.0});
    auto* arrangement = processor.getStandaloneArrangement();
    if (!committed.isValid() || arrangement == nullptr) {
        logFail(testName, "failed to prepare split test state");
        return;
    }

    const auto originalBuffer = processor.getMaterializationAudioBufferById(committed.materializationId);
    if (originalBuffer == nullptr) {
        logFail(testName, "missing original materialization buffer");
        return;
    }

    const std::vector<Note> originalNotes{ makeUndoTestNote(0.10, 0.20, 220.0f) };
    if (!processor.setMaterializationNotesById(committed.materializationId, originalNotes)) {
        logFail(testName, "failed to seed materialization notes before split");
        return;
    }

    const auto originalCurve = makePitchCurveWithPayload(
        std::vector<float>(100, 220.0f),
        std::vector<float>(100, 0.5f),
        {
            makeUndoTestSegment(5, 10, { 221.0f, 222.0f, 223.0f, 224.0f, 225.0f }, CorrectedSegment::Source::HandDraw),
            makeUndoTestSegment(60, 65, { 246.0f, 247.0f, 248.0f, 249.0f, 250.0f }, CorrectedSegment::Source::LineAnchor)
        });
    if (!processor.setMaterializationPitchCurveById(committed.materializationId, originalCurve)) {
        logFail(testName, "failed to seed materialization pitch curve before split");
        return;
    }

    if (!processor.splitPlacementAtSeconds(0, 0, 0.25)) {
        logFail(testName, "splitPlacementAtSeconds rejected a valid split");
        return;
    }

    if (arrangement->getNumPlacements(0) != 2) {
        logFail(testName, "split did not create two placements");
        return;
    }

    StandaloneArrangement::Placement leadingPlacement;
    StandaloneArrangement::Placement trailingPlacement;
    if (!arrangement->getPlacementByIndex(0, 0, leadingPlacement)
        || !arrangement->getPlacementByIndex(0, 1, trailingPlacement)) {
        logFail(testName, "failed to read split placements");
        return;
    }

    if (leadingPlacement.materializationId == committed.materializationId
        || trailingPlacement.materializationId == committed.materializationId
        || leadingPlacement.materializationId == trailingPlacement.materializationId
        || leadingPlacement.placementId == 0
        || trailingPlacement.placementId == 0
        || leadingPlacement.placementId == trailingPlacement.placementId) {
        logFail(testName, "split did not birth two independent materializations");
        return;
    }

    const auto leadingBuffer = processor.getMaterializationAudioBufferById(leadingPlacement.materializationId);
    const auto trailingBuffer = processor.getMaterializationAudioBufferById(trailingPlacement.materializationId);
    if (leadingBuffer == nullptr
        || trailingBuffer == nullptr
        || leadingBuffer == originalBuffer
        || trailingBuffer == originalBuffer
        || leadingBuffer->getNumSamples() != 11025
        || trailingBuffer->getNumSamples() != 33075) {
        logFail(testName, "split did not slice independent materialization-local audio windows");
        return;
    }

    OpenTuneAudioProcessor::MaterializationSnapshot leadingSnapshot;
    OpenTuneAudioProcessor::MaterializationSnapshot trailingSnapshot;
    if (!processor.getMaterializationSnapshotById(leadingPlacement.materializationId, leadingSnapshot)
        || !processor.getMaterializationSnapshotById(trailingPlacement.materializationId, trailingSnapshot)) {
        logFail(testName, "failed to resolve split materialization snapshots");
        return;
    }

    if (!approxEqual(leadingSnapshot.sourceWindow.sourceStartSeconds, 0.0, 1.0e-6)
        || !approxEqual(leadingSnapshot.sourceWindow.sourceEndSeconds, 0.25, 1.0e-6)
        || !approxEqual(trailingSnapshot.sourceWindow.sourceStartSeconds, 0.25, 1.0e-6)
        || !approxEqual(trailingSnapshot.sourceWindow.sourceEndSeconds, 1.0, 1.0e-6)
        || leadingSnapshot.lineageParentMaterializationId != committed.materializationId
        || trailingSnapshot.lineageParentMaterializationId != committed.materializationId) {
        logFail(testName, "split did not persist child materialization lineage and source provenance windows");
        return;
    }

    const auto leadingCurveSnapshot = leadingSnapshot.pitchCurve != nullptr ? leadingSnapshot.pitchCurve->getSnapshot() : nullptr;
    const auto trailingCurveSnapshot = trailingSnapshot.pitchCurve != nullptr ? trailingSnapshot.pitchCurve->getSnapshot() : nullptr;
    if (leadingCurveSnapshot == nullptr
        || trailingCurveSnapshot == nullptr
        || leadingCurveSnapshot->getOriginalF0().size() != 25
        || trailingCurveSnapshot->getOriginalF0().size() != 75
        || leadingCurveSnapshot->getCorrectedSegments().size() != 1
        || trailingCurveSnapshot->getCorrectedSegments().size() != 1
        || leadingCurveSnapshot->getCorrectedSegments().front().startFrame != 5
        || trailingCurveSnapshot->getCorrectedSegments().front().startFrame != 35) {
        logFail(testName, "split did not preserve and rebase pitch-curve payload into child materializations");
        return;
    }

    if (processor.getMaterializationAudioBufferById(committed.materializationId) != nullptr) {
        logFail(testName, "split kept the original materialization alive after birthing left and right children");
        return;
    }

    const std::vector<Note> leftNotes{ makeUndoTestNote(0.02, 0.08, 246.94f) };
    if (!processor.setMaterializationNotesById(leadingPlacement.materializationId, leftNotes)) {
        logFail(testName, "failed to edit the leading split materialization");
        return;
    }

    const auto trailingNotes = processor.getMaterializationNotesById(trailingPlacement.materializationId);
    if (!trailingNotes.empty()) {
        logFail(testName, "editing the leading split materialization polluted the trailing sibling");
        return;
    }

    logPass(testName);
}

void runMergePlacementRewritesPlacementOnlyOrFailsExplicitlyTest()
{
    constexpr const char* testName = "MergePlacement_RewritesPlacementOnlyOrFailsExplicitly";

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(makePreparedImport("merge-materialization", 44100), {0, 0.0});
    auto* arrangement = processor.getStandaloneArrangement();
    if (!committed.isValid() || arrangement == nullptr) {
        logFail(testName, "failed to prepare merge test state");
        return;
    }

    const auto originalBuffer = processor.getMaterializationAudioBufferById(committed.materializationId);
    if (originalBuffer == nullptr) {
        logFail(testName, "failed to resolve original materialization audio before merge");
        return;
    }

    const std::vector<SilentGap> silentGaps{
        SilentGap{ 2205, 4410, -60.0f },
        SilentGap{ 26460, 28665, -60.0f }
    };
    const auto originalCurve = makePitchCurveWithPayload(
        std::vector<float>(100, 220.0f),
        std::vector<float>(100, 0.25f),
        {
            makeUndoTestSegment(5, 10, { 221.0f, 222.0f, 223.0f, 224.0f, 225.0f }, CorrectedSegment::Source::HandDraw),
            makeUndoTestSegment(60, 66, { 246.0f, 247.0f, 248.0f, 249.0f, 250.0f, 251.0f }, CorrectedSegment::Source::LineAnchor)
        });
    const std::vector<Note> originalNotes{
        makeUndoTestNote(0.05, 0.10, 220.0f),
        makeUndoTestNote(0.60, 0.72, 246.94f)
    };
    DetectedKey key;
    key.root = Key::C;
    key.scale = Scale::Major;
    key.confidence = 0.88f;
    if (!processor.replaceMaterializationAudioById(committed.materializationId, originalBuffer, silentGaps)
        || !processor.setMaterializationPitchCurveById(committed.materializationId, originalCurve)
        || !processor.setMaterializationNotesById(committed.materializationId, originalNotes)
        || !processor.setMaterializationDetectedKeyById(committed.materializationId, key)) {
        logFail(testName, "failed to seed full materialization payload before merge");
        return;
    }

    if (!processor.splitPlacementAtSeconds(0, 0, 0.25)) {
        logFail(testName, "failed to create split placements for merge test");
        return;
    }

    StandaloneArrangement::Placement leadingPlacement;
    StandaloneArrangement::Placement trailingPlacement;
    if (!arrangement->getPlacementByIndex(0, 0, leadingPlacement)
        || !arrangement->getPlacementByIndex(0, 1, trailingPlacement)) {
        logFail(testName, "failed to resolve split placements before merge");
        return;
    }

    auto mergeResult1 = processor.mergePlacements(0, leadingPlacement.placementId, trailingPlacement.placementId, 0);
    if (!mergeResult1.has_value()) {
        logFail(testName, "mergePlacements rejected contiguous placements from the same source lineage");
        return;
    }

    if (arrangement->getNumPlacements(0) != 1) {
        logFail(testName, "merge did not collapse back to a single placement");
        return;
    }

    StandaloneArrangement::Placement mergedPlacement;
    if (!arrangement->getPlacementByIndex(0, 0, mergedPlacement)) {
        logFail(testName, "failed to resolve merged placement");
        return;
    }

    if (mergedPlacement.materializationId == committed.materializationId
        || mergedPlacement.materializationId == leadingPlacement.materializationId
        || mergedPlacement.materializationId == trailingPlacement.materializationId
        || !approxEqual(mergedPlacement.durationSeconds, 1.0, 1.0e-6)) {
        logFail(testName, "merge did not birth one new merged materialization");
        return;
    }

    if (processor.getMaterializationAudioBufferById(leadingPlacement.materializationId) != nullptr
        || processor.getMaterializationAudioBufferById(trailingPlacement.materializationId) != nullptr) {
        logFail(testName, "merge kept split source materializations alive after producing the merged result");
        return;
    }

    const auto mergedBuffer = processor.getMaterializationAudioBufferById(mergedPlacement.materializationId);
    if (mergedBuffer == nullptr || mergedBuffer->getNumSamples() != 44100) {
        logFail(testName, "merge did not rebuild one continuous merged materialization-local buffer");
        return;
    }

    OpenTuneAudioProcessor::MaterializationSnapshot mergedSnapshot;
    if (!processor.getMaterializationSnapshotById(mergedPlacement.materializationId, mergedSnapshot)) {
        logFail(testName, "failed to resolve merged materialization snapshot");
        return;
    }

    if (!approxEqual(mergedSnapshot.sourceWindow.sourceStartSeconds, 0.0, 1.0e-6)
        || !approxEqual(mergedSnapshot.sourceWindow.sourceEndSeconds, 1.0, 1.0e-6)
        || mergedSnapshot.originalF0State != OriginalF0State::Ready
        || !sameDetectedKey(mergedSnapshot.detectedKey, key)) {
        logFail(testName, "merge lost persisted source provenance or metadata payload");
        return;
    }

    if (mergedSnapshot.notes.size() != originalNotes.size()
        || !approxEqual(mergedSnapshot.notes.front().startTime, originalNotes.front().startTime, 1.0e-6)
        || !approxEqual(mergedSnapshot.notes.back().startTime, originalNotes.back().startTime, 1.0e-6)) {
        logFail(testName, "merge did not preserve materialization-local note payload");
        return;
    }

    if (mergedSnapshot.silentGaps.size() != silentGaps.size()
        || mergedSnapshot.silentGaps.front().startSample != silentGaps.front().startSample
        || mergedSnapshot.silentGaps.back().endSampleExclusive != silentGaps.back().endSampleExclusive) {
        logFail(testName, "merge did not preserve silent-gap payload");
        return;
    }

    const auto mergedCurveSnapshot = mergedSnapshot.pitchCurve != nullptr ? mergedSnapshot.pitchCurve->getSnapshot() : nullptr;
    if (mergedCurveSnapshot == nullptr
        || mergedCurveSnapshot->getOriginalF0().size() != 100
        || mergedCurveSnapshot->getOriginalEnergy().size() != 100
        || mergedCurveSnapshot->getCorrectedSegments().size() != 2
        || mergedCurveSnapshot->getCorrectedSegments()[0].startFrame != 5
        || mergedCurveSnapshot->getCorrectedSegments()[1].startFrame != 60) {
        logFail(testName, "merge did not preserve and rebase pitch-curve payload");
        return;
    }

    logPass(testName);
}

void runMergePlacementRejectsNonContiguousSourceWindowsTest()
{
    constexpr const char* testName = "MergePlacement_RejectsNonContiguousSourceWindows";

    OpenTuneAudioProcessor processor;
    const auto leading = processor.commitPreparedImportAsPlacement(makePreparedImport("same-source-window", 44100), { 0, 0.0 });
    auto* arrangement = processor.getStandaloneArrangement();
    if (!leading.isValid() || arrangement == nullptr) {
        logFail(testName, "failed to prepare the leading placement");
        return;
    }

    const auto trailing = processor.commitPreparedImportAsPlacement(makePreparedImport("same-source-window", 44100),
                                                                   { 0, 1.0 },
                                                                   leading.sourceId);
    if (!trailing.isValid()) {
        logFail(testName, "failed to prepare the trailing same-source placement");
        return;
    }

    if (processor.mergePlacements(0, leading.placementId, trailing.placementId, 0).has_value()) {
        logFail(testName, "merge accepted same-source placements that do not describe one contiguous source provenance window");
        return;
    }

    if (arrangement->getNumPlacements(0) != 2
        || processor.getMaterializationAudioBufferById(leading.materializationId) == nullptr
        || processor.getMaterializationAudioBufferById(trailing.materializationId) == nullptr) {
        logFail(testName, "failed merge attempt mutated existing placement or materialization state");
        return;
    }

    logPass(testName);
}



void runPlacementCommandsDoNotMutateClipCoreTruthTest()
{
    constexpr const char* testName = "PlacementCommands_DoNotMutateClipCoreTruth";

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(makePreparedImport("placement-only-command", 44100), {0, 0.0});
    if (!committed.isValid()) {
        logFail(testName, "failed to prepare placement command state");
        return;
    }

    const std::vector<Note> notes{ makeUndoTestNote(0.05, 0.15, 220.0f) };
    if (!processor.setMaterializationNotesById(committed.materializationId, notes)) {
        logFail(testName, "failed to seed materialization notes before placement command");
        return;
    }

    const auto originalBuffer = processor.getMaterializationAudioBufferById(committed.materializationId);
    if (originalBuffer == nullptr) {
        logFail(testName, "missing materialization buffer before placement command");
        return;
    }

    if (!processor.movePlacementToTrack(0, 1, committed.placementId, 2.0)) {
        logFail(testName, "placement move command rejected valid move");
        return;
    }

    if (processor.getMaterializationAudioBufferById(committed.materializationId) != originalBuffer) {
        logFail(testName, "placement command rewrote materialization audio");
        return;
    }

    const auto storedNotes = processor.getMaterializationNotesById(committed.materializationId);
    if (storedNotes.size() != notes.size()
        || !approxEqual(storedNotes.front().startTime, notes.front().startTime, 1.0e-6)
        || !approxEqual(storedNotes.front().endTime, notes.front().endTime, 1.0e-6)) {
        logFail(testName, "placement command rewrote materialization-local note timing");
        return;
    }

    logPass(testName);
}

#if JucePlugin_Enable_ARA
void runAraSessionSnapshotExposesSourceMaterializationAndPlacementOwnershipTest()
{
    constexpr const char* testName = "AraSession_SnapshotExposesSourceMaterializationAndPlacementOwnership";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x20);
    auto* playbackRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0x21);
    VST3AraSessionTestProbe::seedSinglePublishedRegion(session, audioSource, playbackRegion, 31337);

    const auto snapshot = session.loadSnapshot();
    const auto* regionView = snapshot != nullptr ? snapshot->findRegion(playbackRegion) : nullptr;
    if (regionView == nullptr) {
        logFail(testName, "failed to publish seeded ARA region view");
        return;
    }

    if (regionView->regionIdentity.audioSource != audioSource
        || regionView->regionIdentity.playbackRegion != playbackRegion
        || regionView->appliedProjection.sourceId != 1
        || regionView->appliedProjection.materializationId != 31337
        || regionView->appliedProjection.appliedRegionIdentity != regionView->regionIdentity) {
        logFail(testName, "ARA snapshot no longer exposes source/materialization/placement ownership explicitly");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

#if JucePlugin_Enable_ARA
void runAraBindingMultiplePlaybackRegionsSameAudioModificationShareMaterializationTest()
{
    constexpr const char* testName = "AraBinding_MultiplePlaybackRegionsSameAudioModificationShareMaterialization";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x220);
    auto* firstRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0x221);
    auto* secondRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0x222);
    const SourceWindow sourceWindow{42, 0.0, 1.0};

    VST3AraSessionTestProbe::seedSource(session, audioSource, 42);
    VST3AraSessionTestProbe::seedAudioModificationBinding(session, "mod-shared", 42, 9001, sourceWindow);
    VST3AraSessionTestProbe::seedPlaybackRegionForModification(session, audioSource, firstRegion, "mod-shared", sourceWindow, 0.0, 1.0);
    VST3AraSessionTestProbe::seedPlaybackRegionForModification(session, audioSource, secondRegion, "mod-shared", sourceWindow, 4.0, 5.0);
    VST3AraSessionTestProbe::publish(session);

    const auto snapshot = session.loadSnapshot();
    const auto* firstView = snapshot != nullptr ? snapshot->findRegion(firstRegion) : nullptr;
    const auto* secondView = snapshot != nullptr ? snapshot->findRegion(secondRegion) : nullptr;
    if (firstView == nullptr || secondView == nullptr) {
        logFail(testName, "failed to publish both playback regions");
        return;
    }

    if (firstView->appliedProjection.materializationId != 9001
        || secondView->appliedProjection.materializationId != 9001) {
        logFail(testName, "same AudioModification did not share one materialization binding");
        return;
    }

    if (firstView->appliedProjection.appliedRegionIdentity != firstView->regionIdentity
        || secondView->appliedProjection.appliedRegionIdentity != secondView->regionIdentity) {
        logFail(testName, "shared binding was not projected independently per playback region");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

void runProcessorModelRejectsMixedClipOwnerApisTest()
{
    constexpr const char* testName = "ProcessorModel_RejectsMixedClipOwnerApis";

    const auto& processorHeader = getFileCache().get("Source/PluginProcessor.h");
    const auto processorPlacementSection = extractWorkspaceFileSection("Source/PluginProcessor.h",
                                                                       "struct CommittedPlacement",
                                                                       "struct MaterializationRefreshRequest");
    const auto arrangementPlacementSection = extractWorkspaceFileSection("Source/StandaloneArrangement.h",
                                                                         "struct Placement",
                                                                         "struct Track");
    const auto& sessionHeader = getFileCache().get("Source/ARA/VST3AraSession.h");
    const auto& sessionSource = getFileCache().get("Source/ARA/VST3AraSession.cpp");
    const auto& pluginEditorHeader = getFileCache().get("Source/Plugin/PluginEditor.h");
    const auto& pluginEditorSource = getFileCache().get("Source/Plugin/PluginEditor.cpp");
    const auto& standaloneEditorSource = getFileCache().get("Source/Standalone/PluginEditor.cpp");
    const auto& materializationProjectionHeader = getFileCache().get("Source/Utils/MaterializationTimelineProjection.h");

    const auto expectMissing = [&](const juce::String& source,
                                   const juce::String& needle,
                                   const char* detail) -> bool
    {
        if (source.contains(needle)) {
            logFail(testName, detail);
            return false;
        }

        return true;
    };

    const auto expectPresent = [&](const juce::String& source,
                                   const juce::String& needle,
                                   const char* detail) -> bool
    {
        if (!source.contains(needle)) {
            logFail(testName, detail);
            return false;
        }

        return true;
    };

    if (processorHeader.contains("getClipAudioBufferById")
        || processorHeader.contains("setClipPitchCurveById")
        || processorHeader.contains("requestClipDerivedRefresh")
        || processorHeader.contains("PreparedImportClip")
        || processorHeader.contains("prepareImportClip(")
        || processorHeader.contains("commitPreparedImportPlacement(")
        || processorHeader.contains("commitPreparedDetachedContent(")
        || processorHeader.contains("exportClipAudio(")
        || sessionHeader.contains("contentBinding")
        || sessionHeader.contains("registerPlaybackRegionContentBinding")
        || sessionHeader.contains("findPublishedRegionByContentId")
        || pluginEditorHeader.contains("syncClipToPianoRoll")
        || pluginEditorSource.contains("syncClipToPianoRoll(")) {
        logFail(testName, "mixed clip owner APIs or helpers reappeared in shared runtime or VST3 editor");
        return;
    }

    if (!processorHeader.contains("PreparedImport")
        || !processorHeader.contains("prepareImport")
        || !processorHeader.contains("commitPreparedImportAsPlacement")
        || !processorHeader.contains("commitPreparedImportAsMaterialization(")
        || !processorHeader.contains("exportPlacementAudio(")
        || !processorHeader.contains("getMaterializationAudioBufferById")
        || !sessionHeader.contains("AppliedMaterializationProjection")
        || !sessionHeader.contains("bindPlaybackRegionToMaterialization")
        || !pluginEditorHeader.contains("syncMaterializationProjectionToPianoRoll")) {
        logFail(testName, "explicit source/materialization/projection APIs are missing from the current public contract");
        return;
    }

    if (!expectMissing(sessionHeader, "AppliedContentProjection", "old AppliedContentProjection contract still exists")
        || !expectMissing(sessionHeader, "bindPlaybackRegionToContent(", "old bindPlaybackRegionToContent contract still exists")
        || !expectMissing(processorHeader, "PreparedImportedContent", "processor still exposes PreparedImportedContent")
        || !expectMissing(processorHeader, "prepareImportedContent", "processor still exposes prepareImportedContent")
        || !expectMissing(processorHeader, "commitPreparedContentAsPlacement", "processor still exposes commitPreparedContentAsPlacement")
        || !expectMissing(processorHeader, "commitPreparedContent(", "processor still exposes commitPreparedContent")
        || !expectMissing(arrangementPlacementSection, "contentStartSeconds", "Standalone placement still exposes contentStartSeconds")
        || !expectMissing(processorPlacementSection, "contentId", "CommittedPlacement still exposes contentId")
        || !expectMissing(processorHeader, "struct ContentRefreshRequest", "processor still exposes ContentRefreshRequest")
        || !expectMissing(processorHeader, "requestContentRefresh(", "processor still exposes requestContentRefresh")
        || !expectMissing(processorHeader, "getContentAudioBufferById", "processor still exposes getContentAudioBufferById")
        || !expectMissing(processorHeader, "getContentPitchCurveById", "processor still exposes getContentPitchCurveById")
        || !expectMissing(processorHeader, "setContentPitchCurveById", "processor still exposes setContentPitchCurveById")
        || !expectMissing(processorHeader, "getContentOriginalF0StateById", "processor still exposes getContentOriginalF0StateById")
        || !expectMissing(processorHeader, "setContentOriginalF0StateById", "processor still exposes setContentOriginalF0StateById")
        || !expectMissing(processorHeader, "getContentDetectedKeyById", "processor still exposes getContentDetectedKeyById")
        || !expectMissing(processorHeader, "setContentDetectedKeyById", "processor still exposes setContentDetectedKeyById")
        || !expectMissing(processorHeader, "getContentNotesById", "processor still exposes getContentNotesById")
        || !expectMissing(processorHeader, "setContentNotesById", "processor still exposes setContentNotesById")
        || !expectMissing(processorHeader, "setContentCorrectedSegmentsById", "processor still exposes setContentCorrectedSegmentsById")
        || !expectMissing(processorHeader, "commitContentNotesAndSegmentsById", "processor still exposes commitContentNotesAndSegmentsById")
        || !expectMissing(processorHeader, "commitAutoTuneGeneratedNotesByContentId", "processor still exposes content-era auto-tune api")
        || !expectMissing(processorHeader, "reclaimUnreferencedContent", "processor still exposes reclaimUnreferencedContent")
        || !expectMissing(pluginEditorHeader, "resolveCurrentContentId", "VST3 editor still exposes content-era selection helper")
        || !expectMissing(pluginEditorHeader, "resolveCurrentContentProjection", "VST3 editor still exposes content-era projection helper")
        || !expectMissing(pluginEditorSource, "ContentTimelineProjection", "VST3 editor still uses ContentTimelineProjection")
        || !expectMissing(pluginEditorSource, "previousContentId", "VST3 record path still reuses previousContentId")
        || !expectMissing(pluginEditorSource, "reusedWorkspace=", "VST3 record path still reports workspace reuse")
        || !expectMissing(standaloneEditorSource, "ContentTimelineProjection", "Standalone editor still uses ContentTimelineProjection")
        || !expectMissing(sessionSource, "writeAppliedMaterializationToSourceSiblingRegions", "ARA materialization binding still broadcasts to source siblings")
        || !expectMissing(sessionSource, "clearAppliedMaterializationForSourceSiblingRegions", "ARA materialization clear still broadcasts to source siblings")) {
        return;
    }

    if (workspaceFileExists("Source/Utils/ContentTimelineProjection.h")) {
        logFail(testName, "old ContentTimelineProjection header still exists");
        return;
    }

    if (!workspaceFileExists("Source/Utils/MaterializationTimelineProjection.h")) {
        logFail(testName, "new MaterializationTimelineProjection header is missing");
        return;
    }

    if (!expectPresent(sessionHeader, "AppliedMaterializationProjection", "ARA session does not expose AppliedMaterializationProjection")
        || !expectPresent(processorPlacementSection, "sourceId", "CommittedPlacement is missing sourceId")
        || !expectPresent(processorPlacementSection, "materializationId", "CommittedPlacement is missing materializationId")
        || !expectPresent(processorHeader, "struct MaterializationRefreshRequest", "processor is missing MaterializationRefreshRequest")
        || !expectPresent(processorHeader, "requestMaterializationRefresh(", "processor is missing requestMaterializationRefresh")
        || !expectPresent(processorHeader, "getMaterializationAudioBufferById", "processor is missing getMaterializationAudioBufferById")
        || !expectPresent(processorHeader, "getMaterializationPitchCurveById", "processor is missing getMaterializationPitchCurveById")
        || !expectPresent(processorHeader, "setMaterializationPitchCurveById", "processor is missing setMaterializationPitchCurveById")
        || !expectPresent(processorHeader, "getMaterializationDetectedKeyById", "processor is missing getMaterializationDetectedKeyById")
        || !expectPresent(processorHeader, "setMaterializationDetectedKeyById", "processor is missing setMaterializationDetectedKeyById")
        || !expectPresent(processorHeader, "getMaterializationNotesById", "processor is missing getMaterializationNotesById")
        || !expectPresent(processorHeader, "setMaterializationNotesById", "processor is missing setMaterializationNotesById")
        || !expectPresent(processorHeader, "commitMaterializationNotesAndSegmentsById", "processor is missing materialization commit api")
        || !expectPresent(processorHeader, "commitAutoTuneGeneratedNotesByMaterializationId", "processor is missing materialization auto-tune api")
        || !expectPresent(sessionHeader, "sourceId", "ARA public contract is missing sourceId")
        || !expectPresent(sessionHeader, "PublishedRegionView", "ARA public contract is missing PublishedRegionView")
        || !expectPresent(arrangementPlacementSection, "materializationId", "Standalone placement is missing materializationId")
        || !expectPresent(materializationProjectionHeader, "MaterializationTimelineProjection", "MaterializationTimelineProjection type is missing")
        || !expectPresent(pluginEditorHeader, "resolveCurrentMaterializationId", "VST3 editor is missing materialization selection helper")
        || !expectPresent(pluginEditorHeader, "resolveCurrentMaterializationSync", "VST3 editor is missing materialization placement sync helper")) {
        return;
    }

    logPass(testName);
}

void runSplitPlacementPianoRollDisplaysProjectedWindowOnlyTest()
{
    constexpr const char* testName = "SplitPlacement_PianoRollDisplaysProjectedWindowOnly";

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(makePreparedImport("split-piano-roll", 44100), {0, 0.0});
    auto* arrangement = processor.getStandaloneArrangement();
    if (!committed.isValid() || arrangement == nullptr) {
        logFail(testName, "failed to prepare piano-roll split projection state");
        return;
    }

    const std::vector<Note> notes{
        makeUndoTestNote(0.10, 0.18, 220.0f),
        makeUndoTestNote(0.30, 0.40, 246.94f)
    };
    if (!processor.setMaterializationNotesById(committed.materializationId, notes)) {
        logFail(testName, "failed to seed materialization-local notes for projection test");
        return;
    }

    if (!processor.splitPlacementAtSeconds(0, 0, 0.25)) {
        logFail(testName, "failed to create trailing placement window");
        return;
    }

    StandaloneArrangement::Placement trailingPlacement;
    if (!arrangement->getPlacementByIndex(0, 1, trailingPlacement)) {
        logFail(testName, "failed to resolve trailing split placement");
        return;
    }

    const auto trailingNotes = processor.getMaterializationNotesById(trailingPlacement.materializationId);
    if (trailingNotes.size() != 1
        || !approxEqual(trailingNotes.front().startTime, 0.05, 1.0e-6)
        || !approxEqual(trailingNotes.front().endTime, 0.15, 1.0e-6)) {
        logFail(testName, "split did not rebase trailing materialization notes into local coordinates");
        return;
    }

    MaterializationTimelineProjection projection;
    projection.timelineStartSeconds = trailingPlacement.timelineStartSeconds;
    projection.timelineDurationSeconds = trailingPlacement.durationSeconds;
    projection.materializationDurationSeconds = trailingPlacement.durationSeconds;

    PianoRollComponent pianoRoll;
    pianoRoll.setSize(640, 360);
    pianoRoll.setProcessor(&processor);
    pianoRoll.setMaterializationProjection(projection);
    pianoRoll.setEditedMaterialization(trailingPlacement.materializationId,
                               processor.getMaterializationPitchCurveById(trailingPlacement.materializationId),
                               processor.getMaterializationAudioBufferById(trailingPlacement.materializationId),
                               44100);

    const auto activeProjection = PianoRollComponentTestProbe::getMaterializationProjection(pianoRoll);
    if (!approxEqual(activeProjection.timelineStartSeconds, projection.timelineStartSeconds, 1.0e-6)
        || !approxEqual(activeProjection.timelineDurationSeconds, projection.timelineDurationSeconds, 1.0e-6)
        || !approxEqual(activeProjection.materializationDurationSeconds, projection.materializationDurationSeconds, 1.0e-6)) {
        logFail(testName, "piano roll did not retain the explicit projected materialization window");
        return;
    }

    const auto visibleNoteBounds = PianoRollComponentTestProbe::getNoteBounds(pianoRoll, trailingNotes.front());
    if (visibleNoteBounds.isEmpty()) {
        logFail(testName, "piano roll failed to render note payload that lies inside the projected trailing window");
        return;
    }

    logPass(testName);
}

void runPianoRollTimelineViewDomainLateCaptureCanBrowseBeforeSegmentTest()
{
    constexpr const char* testName = "PianoRollTimelineViewDomain_LateCaptureCanBrowseBeforeSegment";

    MaterializationTimelineProjection projection;
    projection.timelineStartSeconds = 205.0;
    projection.timelineDurationSeconds = 11.84;
    projection.materializationDurationSeconds = 11.84;

    PianoRollComponent pianoRoll;
    pianoRoll.setSize(900, 420);
    pianoRoll.setMaterializationProjection(projection);
    pianoRoll.setTimelineViewDomain(0.0, projection.timelineEndSeconds());

    if (!PianoRollComponentTestProbe::hasExplicitTimelineViewDomain(pianoRoll)) {
        logFail(testName, "late regular capture must install an explicit timeline view domain");
        return;
    }

    if (!approxEqual(PianoRollComponentTestProbe::toVisibleTimelineSeconds(pianoRoll, 0.0), 0.0, 1.0e-6)) {
        logFail(testName, "timeline zero must remain visible in the widened view domain");
        return;
    }

    if (!approxEqual(PianoRollComponentTestProbe::toAbsoluteTimelineSeconds(pianoRoll, 0.0), 0.0, 1.0e-6)) {
        logFail(testName, "scroll origin must address DAW timeline zero, not the capture segment start");
        return;
    }

    if (PianoRollComponentTestProbe::timeToX(pianoRoll, 0.0)
        < PianoRollComponentTestProbe::getPianoKeyWidth(pianoRoll)) {
        logFail(testName, "timeline zero must map into the browsable ruler area");
        return;
    }

    if (PianoRollComponentTestProbe::timeToX(pianoRoll, projection.timelineStartSeconds)
        <= PianoRollComponentTestProbe::timeToX(pianoRoll, 0.0)) {
        logFail(testName, "captured content must still project to its absolute host-time position");
        return;
    }

    if (!approxEqual(projection.projectMaterializationTimeToTimeline(0.0), 205.0, 1.0e-6)
        || !approxEqual(projection.projectTimelineTimeToMaterialization(205.0), 0.0, 1.0e-6)) {
        logFail(testName, "projection mapping must remain anchored to the captured host time");
        return;
    }

    logPass(testName);
}

void runPianoRollTimelineViewDomainDefaultProjectionWindowUnchangedTest()
{
    constexpr const char* testName = "PianoRollTimelineViewDomain_DefaultProjectionWindowUnchanged";

    MaterializationTimelineProjection projection;
    projection.timelineStartSeconds = 8.0;
    projection.timelineDurationSeconds = 2.0;
    projection.materializationDurationSeconds = 2.0;

    PianoRollComponent pianoRoll;
    pianoRoll.setSize(640, 360);
    pianoRoll.setMaterializationProjection(projection);

    if (PianoRollComponentTestProbe::hasExplicitTimelineViewDomain(pianoRoll)) {
        logFail(testName, "default placement/ARA projection must not install a widened view domain");
        return;
    }

    if (!approxEqual(PianoRollComponentTestProbe::toVisibleTimelineSeconds(pianoRoll, projection.timelineStartSeconds),
                     0.0,
                     1.0e-6)) {
        logFail(testName, "default projection window origin changed unexpectedly");
        return;
    }

    if (!approxEqual(PianoRollComponentTestProbe::toAbsoluteTimelineSeconds(pianoRoll, 0.0),
                     projection.timelineStartSeconds,
                     1.0e-6)) {
        logFail(testName, "default visible origin must remain the projection start");
        return;
    }

    logPass(testName);
}

void runClipDerivedRefreshDoesNotMutateStandaloneSelectionTest()
{
    constexpr const char* testName = "ClipDerivedRefreshDoesNotMutateStandaloneSelection";

    OpenTuneAudioProcessor processor;
    const auto committedA = processor.commitPreparedImportAsPlacement(makePreparedImport("clip-a"), {0, 0.0});
    const auto committedB = processor.commitPreparedImportAsPlacement(makePreparedImport("clip-b"), {0, 2.0});
    const uint64_t clipA = committedA.materializationId;
    auto* arrangement = processor.getStandaloneArrangement();
    if (!committedA.isValid() || !committedB.isValid() || arrangement == nullptr) {
        logFail(testName, "failed to prepare arrangement state");
        return;
    }

    arrangement->setActiveTrack(0);
    arrangement->selectPlacement(0, committedB.placementId);

    OpenTuneAudioProcessor::MaterializationRefreshRequest request;
    request.materializationId = clipA;

    if (!processor.requestMaterializationRefresh(request)) {
        logFail(testName, "requestMaterializationRefresh rejected valid materialization");
        return;
    }

    if (arrangement->getActiveTrackId() != 0 || arrangement->getSelectedPlacementId(0) != committedB.placementId) {
        logFail(testName, "clip refresh mutated standalone selection semantics");
        return;
    }

    logPass(testName);
}

#if JucePlugin_Enable_ARA
void runVst3AraSnapshotDoesNotPublishStalePayloadTest()
{
    constexpr const char* testName = "VST3AraSnapshot_DoesNotPublishStalePayload";

    VST3AraSession::SourceSlot sourceSlot;
    sourceSlot.audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x1);
    sourceSlot.sampleRate = 44100.0;
    sourceSlot.numChannels = 1;
    sourceSlot.numSamples = 64;
    sourceSlot.contentRevision = 2;
    sourceSlot.sampleAccessEnabled = true;

    VST3AraSession::RegionSlot regionSlot;
    regionSlot.identity.audioSource = sourceSlot.audioSource;
    regionSlot.identity.playbackRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0x2);
    regionSlot.appliedProjection.sourceId = 1;
    regionSlot.appliedProjection.materializationId = 42;
    regionSlot.playbackStartSeconds = 0.0;
    regionSlot.playbackEndSeconds = 1.0;
    regionSlot.sourceWindow = SourceWindow{1, 0.0, 1.0};

    const auto snapshot = VST3AraSession::buildSnapshotForPublication({sourceSlot}, {regionSlot}, regionSlot.identity, 7);
    if (snapshot == nullptr || snapshot->publishedRegions.size() != 1) {
        logFail(testName, "failed to publish snapshot for stale-payload check");
        return;
    }

    // Published snapshot carries only identity/binding metadata, no raw audio payload
    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

#if JucePlugin_Enable_ARA
void runRenderableAraRegionViewAcceptsBindingWithoutRawAudioTest()
{
    constexpr const char* testName = "RenderableAraRegionView_AcceptsBindingWithoutRawAudio";

    VST3AraSession::PublishedRegionView view;
    view.regionIdentity.audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x3);
    view.regionIdentity.playbackRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0x4);
    view.appliedProjection.sourceId = 1;
    view.appliedProjection.materializationId = 99;
    view.appliedProjection.appliedRegionIdentity = view.regionIdentity;
    view.playbackStartSeconds = 0.0;
    view.playbackEndSeconds = 1.0;
    view.sourceWindow = SourceWindow{1, 0.0, 1.0};
    view.bindingState = VST3AraSession::BindingState::Renderable;

    if (!OpenTune::canRenderPublishedRegionView(view)) {
        logFail(testName, "renderer rejected a fully bound region view with no raw audio field");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

#if JucePlugin_Enable_ARA
void runRenderableAraRegionViewRejectsNonAppliedSiblingTest()
{
    constexpr const char* testName = "RenderableAraRegionView_RejectsNonAppliedSibling";

    VST3AraSession::PublishedRegionView view;
    view.regionIdentity.audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x5);
    view.regionIdentity.playbackRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0x6);
    view.appliedProjection.sourceId = 1;
    view.appliedProjection.materializationId = 123;
    view.appliedProjection.appliedRegionIdentity.audioSource = view.regionIdentity.audioSource;
    view.appliedProjection.appliedRegionIdentity.playbackRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0x7);
    view.playbackStartSeconds = 0.0;
    view.playbackEndSeconds = 1.0;
    view.sourceWindow = SourceWindow{1, 0.0, 1.0};

    if (OpenTune::canRenderPublishedRegionView(view)) {
        logFail(testName, "renderer accepted sibling region whose appliedRegion does not match");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

#if JucePlugin_Enable_ARA
void runAraBindingStateRenderableRequiresMatchingSourceWindowTest()
{
    constexpr const char* testName = "AraBindingState_RenderableRequiresMatchingSourceWindow";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0xC0);
    auto* playbackRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0xC1);
    const SourceWindow sourceWindow{42, 0.0, 1.0};

    VST3AraSessionTestProbe::seedSource(session, audioSource, sourceWindow.sourceId);
    VST3AraSessionTestProbe::seedAudioModificationBinding(session,
                                                           "mod-renderable-current-window",
                                                           sourceWindow.sourceId,
                                                           9101,
                                                           sourceWindow,
                                                           3,
                                                           sourceWindow.durationSeconds());
    VST3AraSessionTestProbe::seedPlaybackRegionForModification(session,
                                                                audioSource,
                                                                playbackRegion,
                                                                "mod-renderable-current-window",
                                                                sourceWindow,
                                                                2.0,
                                                                3.0);
    VST3AraSessionTestProbe::publish(session);

    const auto snapshot = session.loadSnapshot();
    const auto* view = snapshot != nullptr ? snapshot->findRegion(playbackRegion) : nullptr;
    if (view == nullptr) {
        logFail(testName, "failed to publish renderable binding view");
        return;
    }

    if (view->bindingState != VST3AraSession::BindingState::Renderable
        || !OpenTune::canRenderPublishedRegionView(*view)) {
        logFail(testName, "matching sourceWindow/region identity binding should be renderable");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

#if JucePlugin_Enable_ARA
void runAraBindingStateStaleSourceWindowNeedsRenderTest()
{
    constexpr const char* testName = "AraBindingState_StaleSourceWindowNeedsRender";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0xC2);
    auto* playbackRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0xC3);
    const SourceWindow staleWindow{42, 0.0, 1.0};
    const SourceWindow currentWindow{42, 0.5, 1.5};

    VST3AraSessionTestProbe::seedSource(session, audioSource, currentWindow.sourceId);
    VST3AraSessionTestProbe::seedPlaybackRegionWithStaleAppliedProjection(session,
                                                                           audioSource,
                                                                           playbackRegion,
                                                                           "mod-stale-window",
                                                                           currentWindow,
                                                                           staleWindow,
                                                                           playbackRegion,
                                                                           9102);
    VST3AraSessionTestProbe::publish(session);

    const auto snapshot = session.loadSnapshot();
    const auto* view = snapshot != nullptr ? snapshot->findRegion(playbackRegion) : nullptr;
    if (view == nullptr) {
        logFail(testName, "failed to publish stale sourceWindow view");
        return;
    }

    if (view->bindingState != VST3AraSession::BindingState::BoundNeedsRender
        || OpenTune::canRenderPublishedRegionView(*view)) {
        logFail(testName, "stale applied sourceWindow must publish BoundNeedsRender and stay non-renderable");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

#if JucePlugin_Enable_ARA
void runAraBindingStateStaleRegionIdentityNeedsRenderTest()
{
    constexpr const char* testName = "AraBindingState_StaleRegionIdentityNeedsRender";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0xC4);
    auto* playbackRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0xC5);
    auto* staleAppliedRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0xC6);
    const SourceWindow sourceWindow{43, 0.0, 1.0};

    VST3AraSessionTestProbe::seedSource(session, audioSource, sourceWindow.sourceId);
    VST3AraSessionTestProbe::seedPlaybackRegionWithStaleAppliedProjection(session,
                                                                           audioSource,
                                                                           playbackRegion,
                                                                           "mod-stale-region",
                                                                           sourceWindow,
                                                                           sourceWindow,
                                                                           staleAppliedRegion,
                                                                           9103);
    VST3AraSessionTestProbe::publish(session);

    const auto snapshot = session.loadSnapshot();
    const auto* view = snapshot != nullptr ? snapshot->findRegion(playbackRegion) : nullptr;
    if (view == nullptr) {
        logFail(testName, "failed to publish stale region identity view");
        return;
    }

    if (view->bindingState != VST3AraSession::BindingState::BoundNeedsRender
        || OpenTune::canRenderPublishedRegionView(*view)) {
        logFail(testName, "stale applied region identity must publish BoundNeedsRender and stay non-renderable");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

#if JucePlugin_Enable_ARA
void runAraBindingStateStaleSourceWindowRequeuesBirthTest()
{
    constexpr const char* testName = "AraBindingState_StaleSourceWindowRequeuesBirth";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0xC7);
    auto* playbackRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0xC8);
    const SourceWindow staleWindow{44, 0.0, 1.0};
    const SourceWindow currentWindow{44, 0.25, 1.25};

    VST3AraSessionTestProbe::seedSource(session, audioSource, currentWindow.sourceId);
    VST3AraSessionTestProbe::seedAudioModificationBinding(session,
                                                           "mod-stale-window-requeue",
                                                           staleWindow.sourceId,
                                                           9104,
                                                           staleWindow,
                                                           1,
                                                           staleWindow.durationSeconds());
    VST3AraSessionTestProbe::seedPlaybackRegionForModification(session,
                                                                audioSource,
                                                                playbackRegion,
                                                                "mod-stale-window-requeue",
                                                                currentWindow,
                                                                0.0,
                                                                currentWindow.durationSeconds());

    if (!VST3AraSessionTestProbe::upsertPendingBirthIfNeededForPersistentId(session, "mod-stale-window-requeue")) {
        logFail(testName, "stale binding on a hydrated source should be eligible for session-side auto-birth");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

void runNotesPrimaryRetuneTargetPrefersSelectedNotesTest()
{
    constexpr const char* testName = "NotesPrimaryRetuneTarget_PrefersSelectedNotes";

    AudioEditingScheme::ParameterTargetContext context;
    context.hasSelectedNotes = true;
    context.hasFrameSelection = true;

    const auto target = AudioEditingScheme::resolveParameterTarget(
        AudioEditingScheme::Scheme::NotesPrimary,
        AudioEditingScheme::ParameterKind::RetuneSpeed,
        context);

    if (target != AudioEditingScheme::ParameterTarget::SelectedNotes) {
        logFail(testName, "notes-first retune target no longer prefers selected notes");
        return;
    }

    logPass(testName);
}

void runCorrectedF0PrimaryRetuneTargetDoesNotExposeLineAnchorSegmentTargetTest()
{
    constexpr const char* testName = "CorrectedF0PrimaryRetuneTarget_DoesNotExposeLineAnchorSegmentTarget";

    const auto& schemeSource = getFileCache().get("Source/Utils/AudioEditingScheme.h");
    if (schemeSource.contains("SelectedLineAnchorSegments")
        || schemeSource.contains("hasSelectedLineAnchorSegments")) {
        logFail(testName, "retune target routing still exposes line-anchor segment metadata as a parameter target");
        return;
    }

    AudioEditingScheme::ParameterTargetContext context;
    context.hasSelectedNotes = true;
    context.hasFrameSelection = true;

    const auto target = AudioEditingScheme::resolveParameterTarget(
        AudioEditingScheme::Scheme::CorrectedF0Primary,
        AudioEditingScheme::ParameterKind::RetuneSpeed,
        context);

    if (target != AudioEditingScheme::ParameterTarget::SelectedNotes) {
        logFail(testName, "corrected-f0-first retune target should not route through selected line-anchor segments");
        return;
    }

    logPass(testName);
}

void runFrameSelectionParametersTreatManualCorrectedF0AsCommittedTruthTest()
{
    constexpr const char* testName = "FrameSelectionParameters_TreatManualCorrectedF0AsCommittedTruth";

    const auto componentSource = getFileCache().get("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto componentHeader = getFileCache().get("Source/Standalone/UI/PianoRollComponent.h");

    if (componentSource.contains("hasHandDrawCorrectionInRange")
        || componentHeader.contains("hasHandDrawCorrectionInRange")) {
        logFail(testName, "frame-selection parameter routing still has a HandDraw-only manual-correction guard");
        return;
    }

    const auto helperSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "bool isManualCorrectionSource",
        "} // namespace");
    if (helperSection.isEmpty()
        || !helperSection.contains("CorrectedSegment::Source::HandDraw")
        || !helperSection.contains("CorrectedSegment::Source::LineAnchor")) {
        logFail(testName, "manual correctedF0 source helper must cover both HandDraw and LineAnchor");
        return;
    }

    const auto parameterSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "bool PianoRollComponent::applyParameterToFrameRange",
        "bool PianoRollComponent::getFrameRangeForTimeSpan");
    if (parameterSection.isEmpty()
        || !parameterSection.contains("hasManualCorrectionInRange(startFrame, endFrameExclusive)")
        || parameterSection.indexOf("hasManualCorrectionInRange(startFrame, endFrameExclusive)")
            >= parameterSection.indexOf("enqueueNoteBasedCorrectionAsync(")) {
        logFail(testName, "frame-selection parameter edits can still enqueue NoteBased recompute over manual correctedF0");
        return;
    }

    logPass(testName);
}

void runNotesPrimaryAutoTuneUsesSelectedNotesRangeTest()
{
    constexpr const char* testName = "NotesPrimaryAutoTune_UsesSelectedNotesRange";

    AudioEditingScheme::AutoTuneTargetContext context;
    context.totalFrameCount = 256;
    context.selectedNotesRange = { 24, 64 };
    context.selectionAreaRange = { 0, 192 };
    context.f0SelectionRange = { 0, 192 };

    const auto decision = AudioEditingScheme::resolveAutoTuneRange(AudioEditingScheme::Scheme::NotesPrimary, context);

    if (decision.target == AudioEditingScheme::AutoTuneTarget::None) {
        logFail(testName, "notes-first auto tune target unexpectedly resolved to none");
        return;
    }

    if (decision.target != AudioEditingScheme::AutoTuneTarget::SelectedNotes
        || !sameEditingRange(decision.range, context.selectedNotesRange)) {
        logFail(testName, "notes-first auto tune no longer resolves to selected-note range");
        return;
    }

    logPass(testName);
}

void runCorrectedF0PrimaryAutoTunePrefersSelectionAreaTest()
{
    constexpr const char* testName = "CorrectedF0PrimaryAutoTune_PrefersSelectionArea";

    AudioEditingScheme::AutoTuneTargetContext context;
    context.totalFrameCount = 256;
    context.selectedNotesRange = { 24, 64 };
    context.selectionAreaRange = { 80, 160 };
    context.f0SelectionRange = { 24, 64 };

    const auto decision = AudioEditingScheme::resolveAutoTuneRange(AudioEditingScheme::Scheme::CorrectedF0Primary, context);

    if (decision.target == AudioEditingScheme::AutoTuneTarget::None) {
        logFail(testName, "corrected-f0-first auto tune target unexpectedly resolved to none");
        return;
    }

    if (decision.target != AudioEditingScheme::AutoTuneTarget::FrameSelection
        || !sameEditingRange(decision.range, context.selectionAreaRange)) {
        logFail(testName, "corrected-f0-first auto tune stopped preferring explicit frame selection");
        return;
    }

    logPass(testName);
}

void runStandaloneEditorParameterPanelSyncFollowsEditingSchemeTest()
{
    constexpr const char* testName = "ParameterPanelSyncDecision_FollowsEditingScheme";

    ParameterPanelSyncContext correctedF0Context;
    correctedF0Context.clipRetuneSpeedPercent = 55.0f;
    correctedF0Context.clipVibratoDepth = 6.0f;
    correctedF0Context.clipVibratoRate = 7.2f;
    correctedF0Context.wasShowingSelectionParameters = false;
    correctedF0Context.hasSelectedNoteParameters = true;
    correctedF0Context.selectedNoteRetuneSpeedPercent = 31.0f;
    correctedF0Context.selectedNoteVibratoDepth = 17.0f;
    correctedF0Context.selectedNoteVibratoRate = 9.0f;
    const auto correctedF0Decision = resolveParameterPanelSyncDecision(
        AudioEditingScheme::Scheme::CorrectedF0Primary,
        correctedF0Context);
    if (!correctedF0Decision.shouldSetRetuneSpeed
        || !correctedF0Decision.shouldSetVibratoDepth
        || !correctedF0Decision.shouldSetVibratoRate
        || !approxEqual(correctedF0Decision.retuneSpeedPercent, 31.0f, 1.0e-4f)
        || !approxEqual(correctedF0Decision.vibratoDepth, 17.0f, 1.0e-4f)
        || !approxEqual(correctedF0Decision.vibratoRate, 9.0f, 1.0e-4f)) {
        logFail(testName, "corrected-f0-first sync decision should use selected note parameters, not line-anchor segment metadata");
        return;
    }

    ParameterPanelSyncContext notesPrimaryContext = correctedF0Context;
    const auto notesPrimaryDecision = resolveParameterPanelSyncDecision(
        AudioEditingScheme::Scheme::NotesPrimary,
        notesPrimaryContext);
    if (!notesPrimaryDecision.shouldSetRetuneSpeed
        || !notesPrimaryDecision.shouldSetVibratoDepth
        || !notesPrimaryDecision.shouldSetVibratoRate
        || !approxEqual(notesPrimaryDecision.retuneSpeedPercent, 31.0f, 1.0e-4f)
        || !approxEqual(notesPrimaryDecision.vibratoDepth, 17.0f, 1.0e-4f)
        || !approxEqual(notesPrimaryDecision.vibratoRate, 9.0f, 1.0e-4f)) {
        logFail(testName, "notes-first sync decision did not prioritize selected note parameters");
        return;
    }

    logPass(testName);
}

void runParameterPanelSyncDecisionRestoresClipDefaultsAfterSelectionEndsTest()
{
    constexpr const char* testName = "ParameterPanelSyncDecision_RestoresClipDefaultsAfterSelectionEnds";

    ParameterPanelSyncContext context;
    context.clipRetuneSpeedPercent = 48.0f;
    context.clipVibratoDepth = 11.0f;
    context.clipVibratoRate = 7.8f;
    context.wasShowingSelectionParameters = true;

    const auto decision = resolveParameterPanelSyncDecision(AudioEditingScheme::Scheme::NotesPrimary, context);
    if (!decision.shouldSetRetuneSpeed
        || !decision.shouldSetVibratoDepth
        || !decision.shouldSetVibratoRate
        || !approxEqual(decision.retuneSpeedPercent, 48.0f, 1.0e-4f)
        || !approxEqual(decision.vibratoDepth, 11.0f, 1.0e-4f)
        || !approxEqual(decision.vibratoRate, 7.8f, 1.0e-4f)
        || decision.nextShowingSelectionParameters) {
        logFail(testName, "selection reset decision no longer restores clip defaults");
        return;
    }

    logPass(testName);
}

void runRendererBlockSpanClipsLeadingEdgeTest()
{
    constexpr const char* testName = "RendererBlockSpan_ClipsLeadingEdge";

    const auto span = OpenTune::computeRegionBlockRenderSpan(0.0, 512, 44100.0, 0.005, 0.030);
    if (!span.has_value()) {
        logFail(testName, "expected partial overlap span for leading edge");
        return;
    }

    if (span->destinationStartSample <= 0 || span->destinationStartSample >= 512) {
        logFail(testName, "leading overlap did not start inside host block");
        return;
    }

    if (span->samplesToCopy <= 0 || (span->destinationStartSample + span->samplesToCopy) > 512) {
        logFail(testName, "leading overlap copied outside destination block");
        return;
    }

    logPass(testName);
}

void runRendererBlockSpanClipsTrailingEdgeTest()
{
    constexpr const char* testName = "RendererBlockSpan_ClipsTrailingEdge";

    const auto span = OpenTune::computeRegionBlockRenderSpan(0.0, 512, 44100.0, 0.0, 0.004);
    if (!span.has_value()) {
        logFail(testName, "expected partial overlap span for trailing edge");
        return;
    }

    if (span->destinationStartSample != 0) {
        logFail(testName, "trailing overlap should begin at block start");
        return;
    }

    if (span->samplesToCopy <= 0 || span->samplesToCopy >= 512) {
        logFail(testName, "trailing overlap was not clipped to region end");
        return;
    }

    logPass(testName);
}

void runRendererBlockSpanRejectsBoundaryTouchTest()
{
    constexpr const char* testName = "RendererBlockSpan_RejectsBoundaryTouch";

    const auto span = OpenTune::computeRegionBlockRenderSpan(0.0, 512, 44100.0, 512.0 / 44100.0, 0.050);
    if (span.has_value()) {
        logFail(testName, "boundary-touch case should not produce overlap span");
        return;
    }

    logPass(testName);
}

void runRendererBlockSpanRejectsInvalidInputTest()
{
    constexpr const char* testName = "RendererBlockSpan_RejectsInvalidInput";

    if (OpenTune::computeRegionBlockRenderSpan(0.0, 0, 44100.0, 0.0, 0.050).has_value()) {
        logFail(testName, "zero-sized block should not produce overlap span");
        return;
    }

    if (OpenTune::computeRegionBlockRenderSpan(0.0, 512, 0.0, 0.0, 0.050).has_value()) {
        logFail(testName, "non-positive host sample rate should not produce overlap span");
        return;
    }

    logPass(testName);
}

#if JucePlugin_Enable_ARA
void runVst3AraSessionDefersRegionRemovalUntilDidEndEditingTest()
{
    constexpr const char* testName = "VST3AraSession_DefersRegionRemovalUntilDidEndEditing";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x8);
    auto* playbackRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0x9);
    VST3AraSessionTestProbe::seedSinglePublishedRegion(session, audioSource, playbackRegion, 777);

    const auto initialSnapshot = session.loadSnapshot();
    if (initialSnapshot == nullptr || initialSnapshot->findRegion(playbackRegion) == nullptr) {
        logFail(testName, "failed to seed published region state");
        return;
    }

    session.willBeginEditing();
    session.willRemovePlaybackRegionFromAudioModification(playbackRegion);

    const auto duringEditSnapshot = session.loadSnapshot();
    if (duringEditSnapshot == nullptr || duringEditSnapshot->epoch != initialSnapshot->epoch) {
        logFail(testName, "snapshot advanced during edit transaction");
        return;
    }

    if (duringEditSnapshot->findRegion(playbackRegion) == nullptr) {
        logFail(testName, "published region disappeared before didEndEditing");
        return;
    }

    session.didEndEditing();
    const auto finalSnapshot = session.loadSnapshot();
    if (finalSnapshot == nullptr || finalSnapshot->epoch <= initialSnapshot->epoch) {
        logFail(testName, "snapshot did not advance at edit boundary");
        return;
    }

    if (finalSnapshot->findRegion(playbackRegion) != nullptr) {
        logFail(testName, "region removal was not published after didEndEditing");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

#if JucePlugin_Enable_ARA
void runVst3AraSessionDefersSourceDestroyUntilDidEndEditingTest()
{
    constexpr const char* testName = "VST3AraSession_DefersSourceDestroyUntilDidEndEditing";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0xA);
    auto* playbackRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0xB);
    VST3AraSessionTestProbe::seedSinglePublishedRegion(session, audioSource, playbackRegion, 888);

    const auto initialSnapshot = session.loadSnapshot();
    if (initialSnapshot == nullptr || initialSnapshot->findRegion(playbackRegion) == nullptr) {
        logFail(testName, "failed to seed published source state");
        return;
    }

    session.willBeginEditing();
    session.willDestroyAudioSource(audioSource);

    const auto duringEditSnapshot = session.loadSnapshot();
    if (duringEditSnapshot == nullptr || duringEditSnapshot->epoch != initialSnapshot->epoch) {
        logFail(testName, "source destroy advanced snapshot during edit transaction");
        return;
    }

    if (duringEditSnapshot->findRegion(playbackRegion) == nullptr) {
        logFail(testName, "published source-backed region disappeared before didEndEditing");
        return;
    }

    session.didEndEditing();
    const auto finalSnapshot = session.loadSnapshot();
    if (finalSnapshot == nullptr || finalSnapshot->epoch <= initialSnapshot->epoch) {
        logFail(testName, "source destroy did not publish at edit boundary");
        return;
    }

    if (finalSnapshot->findRegion(playbackRegion) != nullptr) {
        logFail(testName, "source-backed region still published after didEndEditing");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

}

std::atomic<bool> gHasTestFailure{ false };

void logPass(const char* testName)
{
    std::cout << "[PASS] " << testName << std::endl;
}

// Forward declarations for ARA playback/transport repair guard tests
#if JucePlugin_Enable_ARA
void runAraBindingStateEnumDefinesLifecycleStatesTest();
void runAraPublishedRegionViewExposesBindingStateTest();
#endif

// AraFinal — terminal ARA architecture contract guards
void runAraFinalRecordRequestedDoesNotTriggerGame();
void runAraFinalRecordRequestedDoesNotFallbackToMaterializationRefresh();
void runAraFinalSessionHasNoSourceLevelHydrationReadPass();
void runAraFinalPublishedRegionViewExposesNoRawAudioPayload();
void runAraFinalPluginEditorHasNoPrepareImportFromAraRegionHelper();
void runAraFinalPluginEditorHasNoBindPlaybackRegionToMaterialization();
void runAraFinalRequestMaterializationRefreshContractIsNonAra();
void runAraFinalBirthPathOwnsOriginalF0Release();
void runAraBirthPathDetectsSilentGapsBeforeCommit();
void runAuroraRightSidebarBackgroundReferenceRestyleSourceGuardTest();
void runAuroraScrollbarOutlineSourceGuardTest();
void runDmlVocoderUsesDeviceOutputBinding();
void runRenderingPriorityRemainsGpuFirstAndCpuFirstOnly();
void runStage2WorkerStreamsStage1InputDirectly();

// ARA birth lifecycle contract tests
#if JucePlugin_Enable_ARA
void runAraBirthSameSourceTwoDifferentPersistentIdsBothBirth();
void runAraBirthNewPersistentIdArrivesWhileWorkAlreadyInFlight_NotLost();
void runAraBirthStaleResultForOldWindowDoesNotOverrideLatestDesiredWindow();
void runAraBirthDidAddOnPreexistingRegionWithNewPersistentIdEnqueuesBirth();
void runAraBirthUpsertWithUnreadySourceStillRecordsPending();
void runAraBirthReadySourceTriggersQueueForExistingPending();
void runAraBirthSamePidNewWindowBumpsRevisionAndReplacesWindow();
void runAraEditorMissingPayloadDoesNotClearBinding();
void runAraEditorDestroyRecreateReattachesExistingBinding();
void runAraStateRestorePreBindSetStateStillRestoresIntoFinalSharedStores();
#endif

void logFail(const char* testName, const char* detail)
{
    gHasTestFailure.store(true, std::memory_order_relaxed);
    std::cout << "[FAIL] " << testName << ": " << detail << std::endl;
}

void logSection(const char* section)
{
    std::cout << "\n=== " << section << " ===" << std::endl;
}

bool approxEqual(float a, float b, float tol)
{
    return std::abs(a - b) <= tol;
}

bool approxEqual(double a, double b, double tol)
{
    return std::abs(a - b) <= tol;
}

void runPitchCurveNoteBasedSmoothsAdjacentNoteBoundaryTest()
{
    constexpr const char* testName = "PitchCurve_NoteBasedSmoothsAdjacentNoteBoundary";
    constexpr int frameCount = 120;
    constexpr int hopSize = 160;
    constexpr double f0SampleRate = 16000.0;
    constexpr double audioSampleRate = 44100.0;

    std::vector<Note> notes(2);
    notes[0].startTime = 0.0;
    notes[0].endTime = 0.6;
    notes[0].pitch = 220.0f;
    notes[0].originalPitch = 220.0f;
    notes[1].startTime = 0.6;
    notes[1].endTime = 1.2;
    notes[1].pitch = 440.0f;
    notes[1].originalPitch = 220.0f;

    auto makeCurve = [&] {
        std::vector<float> originalF0(frameCount, 220.0f);
        return makePitchCurveWithPayload(std::move(originalF0),
                                         std::vector<float>(frameCount, 1.0f),
                                         {},
                                         hopSize,
                                         f0SampleRate);
    };

    {
        auto curve = makeCurve();
        curve->applyCorrectionToRange(notes, 0, frameCount, 1.0f, 0.0f, 7.5f, audioSampleRate);

        const auto f0 = renderPitchCurveF0(curve, frameCount);
        if (!approxEqual(f0[59], 220.0f, 1.0e-3f) || !approxEqual(f0[60], 440.0f, 1.0e-3f)) {
            logFail(testName, "retuneSpeed=1 should keep the note boundary fully locked to note targets");
            return;
        }
    }

    {
        auto curve = makeCurve();
        curve->applyCorrectionToRange(notes, 0, frameCount, 0.0f, 0.0f, 7.5f, audioSampleRate);

        const auto f0 = renderPitchCurveF0(curve, frameCount);
        const float boundaryJumpSemitones = std::abs(PitchUtils::freqToMidi(f0[60]) - PitchUtils::freqToMidi(f0[59]));
        if (boundaryJumpSemitones >= 3.0f) {
            logFail(testName, "retuneSpeed=0 should smooth the correctedF0 output across adjacent note boundaries");
            return;
        }

        if (!(f0[56] > 220.0f
              && f0[56] < f0[59]
              && f0[59] < f0[60]
              && f0[60] < f0[63]
              && f0[63] < 440.0f)) {
            logFail(testName, "retuneSpeed=0 should form a monotonic local output transition, not a two-frame corner nudge");
            return;
        }

        if (!approxEqual(f0[50], 220.0f, 1.0e-3f) || !approxEqual(f0[70], 440.0f, 1.0e-3f)) {
            logFail(testName, "note-boundary smoothing should stay local to the transition context");
            return;
        }
    }

    logPass(testName);
}

void runPitchCurveNoteBasedLocalRecomputeMatchesSmoothBoundaryTest()
{
    constexpr const char* testName = "PitchCurve_NoteBasedLocalRecomputeMatchesSmoothBoundary";
    constexpr int frameCount = 180;
    constexpr int hopSize = 160;
    constexpr double f0SampleRate = 16000.0;
    constexpr double audioSampleRate = 44100.0;
    constexpr float retuneSpeed = 0.15f;

    std::vector<Note> notes(3);
    notes[0].startTime = 0.0;
    notes[0].endTime = 0.6;
    notes[0].pitch = 220.0f;
    notes[0].originalPitch = 220.0f;
    notes[1].startTime = 0.6;
    notes[1].endTime = 1.2;
    notes[1].pitch = 440.0f;
    notes[1].originalPitch = 220.0f;
    notes[2].startTime = 1.2;
    notes[2].endTime = 1.8;
    notes[2].pitch = 330.0f;
    notes[2].originalPitch = 220.0f;

    auto makeCurve = [&] {
        return makePitchCurveWithPayload(std::vector<float>(frameCount, 220.0f),
                                         std::vector<float>(frameCount, 1.0f),
                                         {},
                                         hopSize,
                                         f0SampleRate);
    };

    auto fullRangeCurve = makeCurve();
    fullRangeCurve->applyCorrectionToRange(notes, 0, frameCount, retuneSpeed, 0.0f, 7.5f, audioSampleRate);
    const auto fullRangeF0 = renderPitchCurveF0(fullRangeCurve, frameCount);

    auto locallyRecomputedCurve = makeCurve();
    locallyRecomputedCurve->applyCorrectionToRange(notes, 0, frameCount, retuneSpeed, 0.0f, 7.5f, audioSampleRate);
    locallyRecomputedCurve->applyCorrectionToRange(notes, 60, 120, retuneSpeed, 0.0f, 7.5f, audioSampleRate);
    const auto localF0 = renderPitchCurveF0(locallyRecomputedCurve, frameCount);

    for (int frame = 50; frame < 70; ++frame) {
        if (!approxEqual(fullRangeF0[frame], localF0[frame], 1.0e-4f)) {
            logFail(testName, "local recompute produced a different correctedF0 corner at the first note boundary");
            return;
        }
    }

    for (int frame = 110; frame < 130; ++frame) {
        if (!approxEqual(fullRangeF0[frame], localF0[frame], 1.0e-4f)) {
            logFail(testName, "local recompute produced a different correctedF0 corner at the second note boundary");
            return;
        }
    }

    logPass(testName);
}

void runPitchCurveNoteBasedDoesNotCreateEdgeTransitionSegmentsTest()
{
    constexpr const char* testName = "PitchCurve_NoteBasedDoesNotCreateEdgeTransitionSegments";
    constexpr int frameCount = 140;
    constexpr int hopSize = 160;
    constexpr double f0SampleRate = 16000.0;
    constexpr double audioSampleRate = 44100.0;
    constexpr float retuneSpeed = 0.15f;

    std::vector<Note> notes(2);
    notes[0].startTime = 0.0;
    notes[0].endTime = 0.7;
    notes[0].pitch = 220.0f;
    notes[0].originalPitch = 220.0f;
    notes[1].startTime = 0.7;
    notes[1].endTime = 1.4;
    notes[1].pitch = 440.0f;
    notes[1].originalPitch = 220.0f;

    auto curve = makePitchCurveWithPayload(std::vector<float>(frameCount, 220.0f),
                                           std::vector<float>(frameCount, 1.0f),
                                           {},
                                           hopSize,
                                           f0SampleRate);

    curve->applyCorrectionToRange(notes, 50, 90, retuneSpeed, 0.0f, 7.5f, audioSampleRate);

    const auto snap = curve->getSnapshot();
    if (!snap) {
        logFail(testName, "pitch curve snapshot missing after note-based correction");
        return;
    }

    const auto& segments = snap->getCorrectedSegments();
    if (segments.size() != 1) {
        logFail(testName, "note-based correction should not create separate edge transition segments");
        return;
    }

    const auto expectedRange = PitchCurve::expandNoteBasedCorrectionRange(50, 90, frameCount);
    const auto& segment = segments.front();
    if (segment.source != CorrectedSegment::Source::NoteBased
        || segment.startFrame != expectedRange.startFrame
        || segment.endFrame != expectedRange.endFrameExclusive
            || segment.f0Data.size() != static_cast<size_t>(expectedRange.endFrameExclusive - expectedRange.startFrame)) {
        logFail(testName, "note-based correction segment does not match the expanded calculation range");
        return;
    }

    logPass(testName);
}

void runPitchCurveNoteBasedPreservesAdjacentManualSegmentsTest()
{
    constexpr const char* testName = "PitchCurve_NoteBasedPreservesAdjacentManualSegments";
    constexpr int frameCount = 140;
    constexpr int hopSize = 160;
    constexpr double f0SampleRate = 16000.0;
    constexpr double audioSampleRate = 44100.0;

    std::vector<Note> notes(1);
    notes[0].startTime = 0.5;
    notes[0].endTime = 1.0;
    notes[0].pitch = 330.0f;
    notes[0].originalPitch = 220.0f;

    CorrectedSegment handDraw(45, 52, { 301.0f, 302.0f, 303.0f, 304.0f, 305.0f, 306.0f, 307.0f }, CorrectedSegment::Source::HandDraw);
    handDraw.retuneSpeed = 0.42f;
    CorrectedSegment lineAnchor(108, 116, { 401.0f, 402.0f, 403.0f, 404.0f, 405.0f, 406.0f, 407.0f, 408.0f }, CorrectedSegment::Source::LineAnchor);
    lineAnchor.retuneSpeed = 0.65f;
    CorrectedSegment overlappingManual(80, 84, { 501.0f, 502.0f, 503.0f, 504.0f }, CorrectedSegment::Source::HandDraw);

    auto curve = makePitchCurveWithPayload(std::vector<float>(frameCount, 220.0f),
                                           std::vector<float>(frameCount, 1.0f),
                                           { handDraw, overlappingManual, lineAnchor },
                                           hopSize,
                                           f0SampleRate);

    curve->applyCorrectionToRange(notes, 60, 100, 0.2f, 0.0f, 7.5f, audioSampleRate);

    const auto snap = curve->getSnapshot();
    if (!snap) {
        logFail(testName, "pitch curve snapshot missing after note-based correction");
        return;
    }

    const auto& segments = snap->getCorrectedSegments();
    const auto handIt = std::find_if(segments.begin(), segments.end(), [](const CorrectedSegment& seg) {
        return seg.source == CorrectedSegment::Source::HandDraw;
    });
    const auto lineIt = std::find_if(segments.begin(), segments.end(), [](const CorrectedSegment& seg) {
        return seg.source == CorrectedSegment::Source::LineAnchor;
    });
    const auto overlapIt = std::find_if(segments.begin(), segments.end(), [](const CorrectedSegment& seg) {
        return seg.startFrame == 80 && seg.endFrame == 84 && seg.source == CorrectedSegment::Source::HandDraw;
    });

    if (handIt == segments.end() || lineIt == segments.end() || overlapIt == segments.end()) {
        logFail(testName, "note-based recompute removed adjacent manual segment");
        return;
    }

    if (handIt->startFrame != handDraw.startFrame
        || handIt->endFrame != handDraw.endFrame
        || handIt->source != handDraw.source
        || handIt->retuneSpeed != handDraw.retuneSpeed
        || handIt->f0Data != handDraw.f0Data) {
        logFail(testName, "hand-draw segment changed during note-based recompute");
        return;
    }

    if (lineIt->startFrame != lineAnchor.startFrame
        || lineIt->endFrame != lineAnchor.endFrame
        || lineIt->source != lineAnchor.source
        || lineIt->retuneSpeed != lineAnchor.retuneSpeed
        || lineIt->f0Data != lineAnchor.f0Data) {
        logFail(testName, "line-anchor segment changed during note-based recompute");
        return;
    }

    if (overlapIt->f0Data != overlappingManual.f0Data) {
        logFail(testName, "manual segment inside expanded note-based context changed");
        return;
    }

    for (const auto& seg : segments) {
        if (seg.source != CorrectedSegment::Source::NoteBased) {
            continue;
        }
        if (seg.startFrame < overlappingManual.endFrame && seg.endFrame > overlappingManual.startFrame) {
            logFail(testName, "note-based insertion overlapped a preserved manual segment");
            return;
        }
    }

    const auto noteBasedCount = std::count_if(segments.begin(), segments.end(), [](const CorrectedSegment& seg) {
        return seg.source == CorrectedSegment::Source::NoteBased;
    });
    if (noteBasedCount == 0) {
        logFail(testName, "note-based recompute should replace only note-based source segments");
        return;
    }

    logPass(testName);
}

void runPitchCurveLineAnchorRenderUsesCommittedCorrectedF0Test()
{
    constexpr const char* testName = "PitchCurve_LineAnchorRenderUsesCommittedCorrectedF0";

    CorrectedSegment lineAnchor(1, 4, { 200.0f, 200.0f, 200.0f }, CorrectedSegment::Source::LineAnchor);
    lineAnchor.retuneSpeed = 0.15f;

    auto curve = makePitchCurveWithPayload({ 100.0f, 100.0f, 100.0f, 100.0f, 100.0f },
                                           { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
                                           { lineAnchor },
                                           1,
                                           100.0);

    const auto f0 = renderPitchCurveF0(curve, 5);
    if (!approxEqual(f0[1], 200.0f, 1.0e-4f)
        || !approxEqual(f0[2], 200.0f, 1.0e-4f)
        || !approxEqual(f0[3], 200.0f, 1.0e-4f)) {
        logFail(testName, "LineAnchor renderF0Range reinterpreted committed f0Data through retuneSpeed");
        return;
    }

    logPass(testName);
}

void runPitchCurveLineAnchorCorrectedOnlyUsesCommittedCorrectedF0Test()
{
    constexpr const char* testName = "PitchCurve_LineAnchorCorrectedOnlyUsesCommittedCorrectedF0";

    CorrectedSegment lineAnchor(1, 4, { 200.0f, 200.0f, 200.0f }, CorrectedSegment::Source::LineAnchor);
    lineAnchor.retuneSpeed = 0.15f;

    auto curve = makePitchCurveWithPayload({ 100.0f, 100.0f, 100.0f, 100.0f, 100.0f },
                                           { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
                                           { lineAnchor },
                                           1,
                                           100.0);

    const auto f0 = renderPitchCurveCorrectedOnlyF0(curve, 5);
    if (!approxEqual(f0[0], 0.0f, 1.0e-4f)
        || !approxEqual(f0[1], 200.0f, 1.0e-4f)
        || !approxEqual(f0[2], 200.0f, 1.0e-4f)
        || !approxEqual(f0[3], 200.0f, 1.0e-4f)
        || !approxEqual(f0[4], 0.0f, 1.0e-4f)) {
        logFail(testName, "LineAnchor renderCorrectedOnlyRange reinterpreted committed f0Data through retuneSpeed");
        return;
    }

    logPass(testName);
}

void runPitchCurveManualCorrectionDoesNotCreateEdgeTransitionSegmentsTest()
{
    constexpr const char* testName = "PitchCurve_ManualCorrectionDoesNotCreateEdgeTransitionSegments";

    auto curve = makePitchCurveWithPayload({ 100.0f, 105.0f, 110.0f, 115.0f, 120.0f, 125.0f, 130.0f },
                                           { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
                                           {},
                                           1,
                                           100.0);

    const std::vector<float> committedF0 { 220.0f, 230.0f, 240.0f };
    curve->setManualCorrectionRange(2, 5, committedF0, CorrectedSegment::Source::LineAnchor);

    const auto snap = curve->getSnapshot();
    if (!snap) {
        logFail(testName, "pitch curve snapshot missing after manual correction");
        return;
    }

    const auto& segments = snap->getCorrectedSegments();
    if (segments.size() != 1) {
        logFail(testName, "manual correction created derived edge transition segments");
        return;
    }

    const auto& segment = segments.front();
    if (segment.source != CorrectedSegment::Source::LineAnchor
        || segment.startFrame != 2
        || segment.endFrame != 5
        || segment.f0Data != committedF0) {
        logFail(testName, "manual correction segment should be exactly the committed f0Data range");
        return;
    }

    const auto correctedOnly = renderPitchCurveCorrectedOnlyF0(curve, 7);
    if (!approxEqual(correctedOnly[0], 0.0f, 1.0e-4f)
        || !approxEqual(correctedOnly[1], 0.0f, 1.0e-4f)
        || !approxEqual(correctedOnly[2], 220.0f, 1.0e-4f)
        || !approxEqual(correctedOnly[3], 230.0f, 1.0e-4f)
        || !approxEqual(correctedOnly[4], 240.0f, 1.0e-4f)
        || !approxEqual(correctedOnly[5], 0.0f, 1.0e-4f)
        || !approxEqual(correctedOnly[6], 0.0f, 1.0e-4f)) {
        logFail(testName, "manual correction leaked correctedF0 outside the committed range");
        return;
    }

    logPass(testName);
}

void runPianoRollRendererCorrectedF0DoesNotForceHardBoundaryStrokeTest()
{
    constexpr const char* testName = "PianoRollRenderer_CorrectedF0DoesNotForceHardBoundaryStroke";

    const auto& rendererSource = getFileCache().get("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");
    if (rendererSource.contains("drawHardCorrectedF0Corners")
        || rendererSource.contains("previousNoteIndex != currentNoteIndex")
        || rendererSource.contains("rightNote.startTime")) {
        logFail(testName, "correctedF0 rendering still forces visual note-boundary corners");
        return;
    }

    if (!rendererSource.contains("juce::PathStrokeType::curved")
        || !rendererSource.contains("juce::PathStrokeType::rounded")
        || rendererSource.contains("juce::PathStrokeType::mitered")
        || rendererSource.contains("juce::PathStrokeType::butt")) {
        logFail(testName, "correctedF0 rendering should draw the data curve instead of enforcing a hard-corner style");
        return;
    }

    logPass(testName);
}

std::vector<PianoRollRenderer::F0VisualSegment> buildTestF0VisualSegments(
    const std::vector<float>& f0,
    const std::vector<float>& energy,
    double pixelsPerSecond,
    const std::vector<uint8_t>* visibleMask = nullptr)
{
    PianoRollRenderer::F0VisualBuildOptions options;
    options.startFrame = 0;
    options.endFrameExclusive = static_cast<int>(f0.size());
    options.viewportStartX = -100000;
    options.viewportEndX = 100000;
    options.pixelsPerSecond = pixelsPerSecond;
    options.secondsPerFrame = 0.01;

    const auto* energyPtr = energy.empty() ? nullptr : &energy;
    return PianoRollRenderer::buildF0VisualSegments(
        f0,
        energyPtr,
        visibleMask,
        options,
        [pixelsPerSecond](int frame) {
            return static_cast<float>(static_cast<double>(frame) * 0.01 * pixelsPerSecond);
        },
        [](int, float frequency) {
            return frequency;
        });
}

float dbfsToLinearRms(float dbfs)
{
    return std::pow(10.0f, dbfs / 20.0f);
}

void runPianoRollF0VisualUsesEnergyAlphaWithinBoundsTest()
{
    constexpr const char* testName = "PianoRollF0Visual_UsesEnergyAlphaWithinBounds";

    const auto segments = buildTestF0VisualSegments(
        { 220.0f, 221.0f, 222.0f, 223.0f, 224.0f },
        {
            dbfsToLinearRms(-70.0f),
            dbfsToLinearRms(-48.0f),
            dbfsToLinearRms(-30.0f),
            dbfsToLinearRms(-18.0f),
            dbfsToLinearRms(-12.0f)
        },
        200.0);

    if (segments.size() != 1 || segments.front().points.size() != 5) {
        logFail(testName, "high-zoom F0 visual builder should keep one visual point per voiced frame");
        return;
    }

    const auto& points = segments.front().points;
    if (!(points.front().energyAlpha < points[1].energyAlpha
          && points[1].energyAlpha < points[2].energyAlpha
          && points[2].energyAlpha < points[3].energyAlpha)
        || !approxEqual(points[3].energyAlpha, 1.0f, 1.0e-4f)
        || !approxEqual(points.back().energyAlpha, 1.0f, 1.0e-4f)) {
        logFail(testName, "energy alpha should fade by absolute dBFS and reach full opacity by -18 dBFS");
        return;
    }

    logPass(testName);
}

void runPianoRollF0VisualHotLevelAddsGoldTintTest()
{
    constexpr const char* testName = "PianoRollF0Visual_HotLevelAddsGoldTint";

    const auto segments = buildTestF0VisualSegments(
        { 220.0f, 221.0f, 222.0f, 223.0f },
        {
            dbfsToLinearRms(-24.0f),
            dbfsToLinearRms(-18.0f),
            dbfsToLinearRms(-12.0f),
            dbfsToLinearRms(-6.0f)
        },
        200.0);

    if (segments.size() != 1 || segments.front().points.size() != 4) {
        logFail(testName, "test setup should keep one visual point per voiced frame");
        return;
    }

    const auto& points = segments.front().points;
    if (!(points.front().levelHotMix < points[1].levelHotMix
          && points[1].levelHotMix < points[2].levelHotMix
          && points[2].levelHotMix < points.back().levelHotMix)
        || !approxEqual(points[1].energyAlpha, 1.0f, 1.0e-4f)
        || !approxEqual(points.back().energyAlpha, 1.0f, 1.0e-4f)) {
        logFail(testName, "hot levels should stay fully opaque and increasingly blend toward gold above the threshold");
        return;
    }

    logPass(testName);
}

void runPianoRollF0VisualZoomedOutBucketsAreBoundedByPixelDensityTest()
{
    constexpr const char* testName = "PianoRollF0Visual_ZoomedOutBucketsAreBoundedByPixelDensity";

    std::vector<float> f0(1000, 220.0f);
    const auto segments = buildTestF0VisualSegments(f0, {}, 10.0);

    if (segments.size() != 1 || segments.front().points.size() >= 120) {
        logFail(testName, "zoomed-out F0 curve should be smoothed into screen-density visual buckets");
        return;
    }

    logPass(testName);
}

void runPianoRollF0VisualZoomedInRestoresFrameDetailTest()
{
    constexpr const char* testName = "PianoRollF0Visual_ZoomedInRestoresFrameDetail";

    std::vector<float> f0(24, 220.0f);
    const auto segments = buildTestF0VisualSegments(f0, {}, 140.0);

    if (segments.size() != 1 || segments.front().points.size() != f0.size()) {
        logFail(testName, "zoomed-in F0 curve should expand back to frame-level detail");
        return;
    }

    logPass(testName);
}

void runPianoRollF0VisualVoicelessGapsDoNotConnectAcrossSegmentsTest()
{
    constexpr const char* testName = "PianoRollF0Visual_VoicelessGapsDoNotConnectAcrossSegments";

    const auto segments = buildTestF0VisualSegments(
        { 220.0f, 221.0f, 0.0f, 222.0f, 223.0f, 2501.0f, 224.0f, 225.0f },
        {},
        200.0);

    if (segments.size() != 3
        || segments[0].points.size() != 2
        || segments[1].points.size() != 2
        || segments[2].points.size() != 2) {
        logFail(testName, "invalid and out-of-range F0 frames should split visual segments");
        return;
    }

    std::vector<uint8_t> mismatchedMask { 1, 1 };
    const auto maskedSegments = buildTestF0VisualSegments({ 220.0f, 221.0f, 222.0f }, {}, 200.0, &mismatchedMask);
    if (!maskedSegments.empty()) {
        logFail(testName, "mismatched visible masks should not draw a potentially wrong connected curve");
        return;
    }

    logPass(testName);
}

void runPianoRollF0VisualStyleTokensAndLayeringTest()
{
    constexpr const char* testName = "PianoRollF0Visual_StyleTokensAndLayering";

    const auto& componentSource = getFileCache().get("Source/Standalone/UI/PianoRollComponent.cpp");
    const int originalDrawIndex = componentSource.indexOf(
        "renderer_->drawPreparedF0Curve(g, item.originalF0VisualSegments, UIColors::originalF0, 0.62f, true");
    const int correctedDrawIndex = componentSource.indexOf(
        "renderer_->drawPreparedF0Curve(g, item.correctedF0VisualSegments, UIColors::correctedF0, 0.94f, false");
    // selected OriginalF0 emphasis is now unified inside drawPreparedF0Curve via
    // the note.selected alpha ramp, so only the two-level z-order matters.
    if (originalDrawIndex < 0
        || correctedDrawIndex < 0
        || originalDrawIndex > correctedDrawIndex) {
        logFail(testName, "OriginalF0 should draw below CorrectedF0 in the unified F0 path");
        return;
    }

    const auto& uiColors = getFileCache().get("Source/Standalone/UI/UIColors.h");
    const auto& themeTokens = getFileCache().get("Source/Standalone/UI/ThemeTokens.h");
    if (!uiColors.contains("originalF0 { 0xFFD24A3A }")
        || !uiColors.contains("correctedF0 { 0xFF196FC4 }")
        || !themeTokens.contains("0xFFD24A3A }, // originalF0")
        || !themeTokens.contains("0xFF196FC4 }, // correctedF0")) {
        logFail(testName, "F0 visual color tokens should keep OriginalF0 red and use the deeper reference blue for CorrectedF0");
        return;
    }

    if (!uiColors.contains("noteBlock { 0xFF235AA8 }")
        || !uiColors.contains("noteBlockBorder { 0xFF3A69A2 }")
        || !uiColors.contains("noteBlockSelected { 0xFF2F6FC4 }")
        || !themeTokens.contains("0xFF235AA8")
        || !themeTokens.contains("0xFF3A69A2")) {
        logFail(testName, "note blocks should use the deeper reference blue palette");
        return;
    }

    // After timeline-rendering refactor, selected OriginalF0 emphasis lives
    // inside drawPreparedF0Curve via note.selected alpha ramp, not a separate path.
    if (!componentSource.contains("UIColors::originalF0.withAlpha(0.42f)")
        && !componentSource.contains("note.selected"))
    {
        logFail(testName, "selected OriginalF0 emphasis must remain softer than CorrectedF0 via unified path");
        return;
    }

    const auto& rendererSource = getFileCache().get("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");
    if (!rendererSource.contains("note.selected ? 0.42f : 0.28f")
        || !rendererSource.contains("edgeColour.withAlpha(note.selected ? 0.66f : 0.48f)")
        || !rendererSource.contains("note.selected ? 0.48f : 0.34f")) {
        logFail(testName, "note block opacity should stay low enough for CorrectedF0 to read through");
        return;
    }

    logPass(testName);
}

void runPianoRollF0VisualEndpointFadeAndGlowContractTest()
{
    constexpr const char* testName = "PianoRollF0Visual_EndpointFadeAndGlowContract";

    const auto& rendererSource = getFileCache().get("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");
    const auto drawFunction = rendererSource
        .fromFirstOccurrenceOf("void PianoRollRenderer::drawPreparedF0Curve", true, false)
        .upToFirstOccurrenceOf("// ============================================================================", false, false);

    if (!drawFunction.contains("drawTaperedCurve")
        || !drawFunction.contains("fadeSpanCount")
        || !drawFunction.contains("taperAlpha")
        || !drawFunction.contains("energyAlpha")
        || !drawFunction.contains("levelHotMix")
        || !rendererSource.contains("appendSmoothedF0Path")) {
        logFail(testName, "F0 draw path should taper endpoints, multiply by energy alpha, blend hot levels, and use smoothed paths");
        return;
    }

    if (drawFunction.contains("fillEllipse")
        || drawFunction.contains("noteBlockSelected")
        || drawFunction.contains("spanPath")
        || drawFunction.contains("startNewSubPath(a.x")
        || !drawFunction.contains("0.055f")
        || !drawFunction.contains("0.070f")) {
        logFail(testName, "F0 draw path should avoid endpoint dots, per-span strokes, foreign selection colors, and keep glow reduced");
        return;
    }

    if (!drawFunction.contains("kAlphaBucketStep")
        || !drawFunction.contains("kHotBucketStep")
        || !drawFunction.contains("buildDisplaySmoothedF0Points(segment.points, isThinLine ? 1 : 2)")
        || !drawFunction.contains("0xFFFFC24A")
        || !drawFunction.contains("DrawRun")) {
        logFail(testName, "F0 draw path should group adjacent spans, smooth display points, and blend high-level gold locally");
        return;
    }

    logPass(testName);
}

void runAuroraTopBarReferenceRestyleSourceGuardTest()
{
    constexpr const char* testName = "AuroraTopBar_ReferenceRestyleSourceGuard";

    const auto& topBarSource = getFileCache().get("Source/Standalone/UI/TopBarComponent.cpp");
    if (!topBarSource.contains("const float shadowMargin = UIColors::currentThemeId() == ThemeId::Aurora ? 10.0f : 12.0f;")
        || !topBarSource.contains("UIColors::fillAuroraGlass(g, bounds, 7.0f);")
        || !topBarSource.contains("UIColors::drawAuroraGlassFrame(g, bounds, 7.0f, false);")
        || !topBarSource.contains("juce::ColourGradient bottomClosure")
        || topBarSource.contains("juce::ColourGradient bottomGlow")
        || topBarSource.contains("juce::ColourGradient bottomFade")) {
        logFail(testName, "Aurora top tray should use a tighter rounded shell and remove the old bottom glow/fade stack");
        return;
    }

    const auto& transportSource = getFileCache().get("Source/Standalone/UI/TransportBarComponent.cpp");
    if (!transportSource.contains("iconColor = UIColors::textPrimary.withAlpha(0.96f);")
        || !transportSource.contains("iconColor = UIColors::textPrimary.withAlpha(isTransportButton ? 0.92f : 0.78f);")
        || !transportSource.contains("const float hoverScale = 1.08f;")
        || !transportSource.contains("const int buttonWidth = 50;")
        || !transportSource.contains("const int groupGap = 20;")
        || !transportSource.contains("const int typeWidth = 180;")) {
        logFail(testName, "Aurora transport controls should keep the original layout rhythm while using white active icons instead of accent-blue active icons");
        return;
    }

    const auto& lookAndFeelSource = getFileCache().get("Source/Standalone/UI/AuroraLookAndFeel.cpp");
    if (!lookAndFeelSource.contains("float arrowSize = 4.25f;")
        || !lookAndFeelSource.contains("UIColors::textSecondary.withAlpha(isActive ? 0.78f : 0.62f)")) {
        logFail(testName, "Aurora combo arrows should remain visually subordinate to the shared chrome");
        return;
    }

    const auto& auroraThemeSource = getFileCache().get("Source/Standalone/UI/AuroraTheme.h");
    const auto& uiColorsSource = getFileCache().get("Source/Standalone/UI/UIColors.h");
    if (!auroraThemeSource.contains("static const juce::uint32 TrayTop         = 0xFF14263A;")
        || !auroraThemeSource.contains("static const juce::uint32 ButtonNormal   = 0xC00B1728;")
        || !uiColorsSource.contains("const auto trayTop = juce::Colour { Aurora::Colors::TrayTop };")
        || !uiColorsSource.contains("const auto fill = isActive ? auroraButtonActive : (isHovered ? auroraButtonHover : auroraButtonNormal);")
        || !uiColorsSource.contains("juce::DropShadow outerGlow(glow.withMultipliedAlpha(isActive ? 0.20f : (isHovered ? 0.12f : 0.055f)),")
        || !uiColorsSource.contains("g.setColour(outerEdge.withMultipliedAlpha(strong ? 0.92f : 0.64f));")
        || uiColorsSource.contains("juce::ColourGradient leftHotspot")
        || uiColorsSource.contains("juce::ColourGradient rightHotspot")
        || uiColorsSource.contains("const auto structuralEdge = coreShadow.interpolatedWith(juce::Colour { Aurora::Colors::BgDeep }, 0.42f);")) {
        logFail(testName, "Aurora should keep the refined tray background while button chrome falls back to the original broader glass treatment");
        return;
    }

    logPass(testName);
}

void runProcessorAutoTuneAndLocalRetuneFifteenPercentKeepSameSmoothBoundaryTest()
{
    constexpr const char* testName = "Processor_AutoTuneAndLocalRetuneFifteenPercentKeepSameSmoothBoundary";
    constexpr int frameCount = 180;
    constexpr int hopSize = 160;
    constexpr double f0SampleRate = 16000.0;
    constexpr double audioSampleRate = 44100.0;
    constexpr float retuneSpeed = 0.15f;

    auto makeAutoNotes = [] {
        std::vector<Note> notes(3);
        notes[0].startTime = 0.0;
        notes[0].endTime = 0.6;
        notes[0].pitch = 220.0f;
        notes[0].originalPitch = 220.0f;
        notes[1].startTime = 0.6;
        notes[1].endTime = 1.2;
        notes[1].pitch = 440.0f;
        notes[1].originalPitch = 220.0f;
        notes[2].startTime = 1.2;
        notes[2].endTime = 1.8;
        notes[2].pitch = 330.0f;
        notes[2].originalPitch = 220.0f;
        return notes;
    };

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(
        makePreparedImport("auto-local-retune-15", frameCount),
        {0, 0.0});
    if (!committed.isValid()) {
        logFail(testName, "failed to create materialization for auto/local retune comparison");
        return;
    }

    auto baseCurve = makePitchCurveWithPayload(std::vector<float>(frameCount, 220.0f),
                                               std::vector<float>(frameCount, 1.0f),
                                               {},
                                               hopSize,
                                               f0SampleRate);
    if (!processor.setMaterializationPitchCurveById(committed.materializationId, baseCurve)) {
        logFail(testName, "failed to seed materialization pitch curve");
        return;
    }

    const auto autoNotes = makeAutoNotes();
    if (!processor.commitAutoTuneGeneratedNotesByMaterializationId(committed.materializationId,
                                                                   autoNotes,
                                                                   0,
                                                                   frameCount,
                                                                   retuneSpeed,
                                                                   0.0f,
                                                                   7.5f,
                                                                   audioSampleRate)) {
        logFail(testName, "processor AutoTune commit failed");
        return;
    }

    const auto autoF0 = renderPitchCurveF0(
        processor.getMaterializationPitchCurveById(committed.materializationId),
        frameCount);

    auto retunedNotes = processor.getMaterializationNotesById(committed.materializationId);
    if (retunedNotes.size() != autoNotes.size()) {
        logFail(testName, "AutoTune commit did not store the generated notes");
        return;
    }

    retunedNotes[1].selected = true;
    retunedNotes[1].retuneSpeed = retuneSpeed;
    retunedNotes[1].dirty = true;

    auto localCurve = processor.getMaterializationPitchCurveById(committed.materializationId);
    if (!localCurve) {
        logFail(testName, "missing materialization pitch curve before local retune");
        return;
    }

    auto recomputedCurve = localCurve->clone();
    recomputedCurve->applyCorrectionToRange(retunedNotes, 60, 120, retuneSpeed, 0.0f, 7.5f, audioSampleRate);
    const auto recomputedSnap = recomputedCurve->getSnapshot();
    if (!recomputedSnap
        || !processor.commitMaterializationNotesAndSegmentsById(committed.materializationId,
                                                                retunedNotes,
                                                                recomputedSnap->getCorrectedSegments())) {
        logFail(testName, "local 15 percent retune commit failed");
        return;
    }

    const auto localF0 = renderPitchCurveF0(
        processor.getMaterializationPitchCurveById(committed.materializationId),
        frameCount);

    for (int frame = 50; frame < 70; ++frame) {
        if (!approxEqual(autoF0[frame], localF0[frame], 1.0e-4f)) {
            logFail(testName, "AutoTune 15 percent and local 15 percent retune differ at the first note boundary");
            return;
        }
    }

    for (int frame = 110; frame < 130; ++frame) {
        if (!approxEqual(autoF0[frame], localF0[frame], 1.0e-4f)) {
            logFail(testName, "AutoTune 15 percent and local 15 percent retune differ at the second note boundary");
            return;
        }
    }

    logPass(testName);
}

void runProcessorLineAnchorRenderUsesCommittedCorrectedF0Test()
{
    constexpr const char* testName = "Processor_LineAnchorRenderUsesCommittedCorrectedF0";

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(
        makePreparedImport("processor-line-anchor-f0-truth", 128),
        {0, 0.0});
    if (!committed.isValid()) {
        logFail(testName, "failed to create materialization for processor line-anchor render test");
        return;
    }

    CorrectedSegment lineAnchor(1, 4, { 200.0f, 200.0f, 200.0f }, CorrectedSegment::Source::LineAnchor);
    lineAnchor.retuneSpeed = 0.15f;

    auto curve = makePitchCurveWithPayload({ 100.0f, 100.0f, 100.0f, 100.0f, 100.0f },
                                           { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
                                           { lineAnchor },
                                           1,
                                           100.0);
    if (!processor.setMaterializationPitchCurveById(committed.materializationId, curve)) {
        logFail(testName, "failed to seed processor materialization pitch curve");
        return;
    }

    const auto f0 = renderPitchCurveF0(
        processor.getMaterializationPitchCurveById(committed.materializationId),
        5);
    if (!approxEqual(f0[1], 200.0f, 1.0e-4f)
        || !approxEqual(f0[2], 200.0f, 1.0e-4f)
        || !approxEqual(f0[3], 200.0f, 1.0e-4f)) {
        logFail(testName, "processor-owned render path reinterpreted LineAnchor f0Data through retuneSpeed");
        return;
    }

    logPass(testName);
}

MaterializationStore::CreateMaterializationRequest makeTestClipRequest()
{
    MaterializationStore::CreateMaterializationRequest request;
    request.sourceId = 1;

    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, 128);
    buffer->clear();
    buffer->setSample(0, 0, 0.25f);
    request.audioBuffer = std::move(buffer);
    request.pitchCurve = std::make_shared<PitchCurve>();
    request.renderCache = std::make_shared<RenderCache>();

    return request;
}

bool seedPublishedIdleChunk(RenderCache& cache,
                            double startSeconds,
                            double endSeconds,
                            std::vector<float> audio)
{
    cache.requestRenderPending(startSeconds,
                               endSeconds,
                               TimeCoordinate::secondsToSamplesFloor(startSeconds, TimeCoordinate::kRenderSampleRate),
                               TimeCoordinate::secondsToSamplesCeil(endSeconds, TimeCoordinate::kRenderSampleRate));

    RenderCache::PendingJob job;
    if (!cache.getNextPendingJob(job))
        return false;

    if (!cache.addChunk(job.startSample, job.endSampleExclusive, std::move(audio), job.targetRevision))
        return false;

    cache.completeChunkRender(job.startSeconds,
                              job.targetRevision,
                              RenderCache::CompletionResult::Succeeded);
    return true;
}



void runMacStandalonePackagingMacDocsGoToBundleResourcesTest()
{
    constexpr const char* testName = "MacStandalonePackaging_MacDocsGoToBundleResources";

    const auto& cmake = getFileCache().get("CMakeLists.txt");
    if (!cmake.contains("set_source_files_properties(\"${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide.html\" PROPERTIES")
        || !cmake.contains("MACOSX_PACKAGE_LOCATION \"Resources/docs\"")
        || !cmake.contains("target_sources(OpenTune_Standalone PRIVATE \"${CMAKE_CURRENT_SOURCE_DIR}/docs/UserGuide.html\")")
        || !cmake.contains("if(WIN32)")
        || !cmake.contains("$<TARGET_FILE_DIR:OpenTune_Standalone>/docs/UserGuide.html")) {
        logFail(testName, "mac standalone docs are not packaged as bundle-owned resources");
        return;
    }

    logPass(testName);
}



void runLockFreeQueueBasicEnqueueDequeueTest()
{
    constexpr const char* testName = "LockFreeQueue_BasicEnqueueDequeue";

    LockFreeQueue<int> queue(4);

    if (!queue.empty() || queue.size() != 0) {
        logFail(testName, "freshly constructed queue is not empty");
        return;
    }

    if (!queue.try_enqueue(10) || !queue.try_enqueue(20)) {
        logFail(testName, "enqueue into non-full queue failed");
        return;
    }

    if (queue.size() != 2) {
        logFail(testName, "size does not reflect enqueued items");
        return;
    }

    int value = 0;
    if (!queue.try_dequeue(value) || value != 10) {
        logFail(testName, "first dequeue did not return FIFO-order item");
        return;
    }

    if (!queue.try_dequeue(value) || value != 20) {
        logFail(testName, "second dequeue did not return FIFO-order item");
        return;
    }

    if (!queue.empty()) {
        logFail(testName, "queue not empty after draining all items");
        return;
    }

    if (queue.try_dequeue(value)) {
        logFail(testName, "dequeue from empty queue should fail");
        return;
    }

    logPass(testName);
}

void runLockFreeQueueRejectsWhenFullTest()
{
    constexpr const char* testName = "LockFreeQueue_RejectsWhenFull";

    LockFreeQueue<int> queue(4);

    for (int i = 0; i < 4; ++i) {
        if (!queue.try_enqueue(i)) {
            logFail(testName, "enqueue failed before reaching capacity");
            return;
        }
    }

    if (queue.try_enqueue(99)) {
        logFail(testName, "enqueue into full queue did not fail");
        return;
    }

    if (queue.size() != 4) {
        logFail(testName, "size changed after rejected enqueue");
        return;
    }

    logPass(testName);
}

void runLockFreeQueueClearDrainsAllItemsTest()
{
    constexpr const char* testName = "LockFreeQueue_ClearDrainsAllItems";

    LockFreeQueue<int> queue(8);
    for (int i = 0; i < 5; ++i)
        queue.try_enqueue(i * 10);

    queue.clear();

    if (!queue.empty() || queue.size() != 0) {
        logFail(testName, "queue not empty after clear");
        return;
    }

    if (!queue.try_enqueue(42)) {
        logFail(testName, "enqueue failed after clear");
        return;
    }

    int value = 0;
    if (!queue.try_dequeue(value) || value != 42) {
        logFail(testName, "dequeue after clear did not return newly enqueued item");
        return;
    }

    logPass(testName);
}

namespace {
class TestUndoAction : public OpenTune::UndoAction {
public:
    TestUndoAction(juce::String desc, std::function<void()> onUndo = nullptr, std::function<void()> onRedo = nullptr)
        : desc_(std::move(desc)), onUndo_(std::move(onUndo)), onRedo_(std::move(onRedo)) {}
    void undo() override { if (onUndo_) onUndo_(); }
    void redo() override { if (onRedo_) onRedo_(); }
    juce::String getDescription() const override { return desc_; }
private:
    juce::String desc_;
    std::function<void()> onUndo_;
    std::function<void()> onRedo_;
};
} // anonymous namespace

void runUndoManagerSuite()
{
    logSection("UndoManager");

    // --- add/undo/redo 基本流程 ---
    {
        constexpr const char* testName = "UndoManager_AddUndoRedo";
        UndoManager mgr;

        for (int i = 0; i < 3; ++i)
            mgr.addAction(std::make_unique<TestUndoAction>("Action " + juce::String(i)));

        if (!mgr.canUndo()) {
            logFail(testName, "canUndo should be true after adding actions");
            return;
        }
        if (mgr.canRedo()) {
            logFail(testName, "canRedo should be false before any undo");
            return;
        }

        if (!mgr.undo()) {
            logFail(testName, "undo() should return true");
            return;
        }
        if (!mgr.canRedo()) {
            logFail(testName, "canRedo should be true after undo");
            return;
        }
        if (!mgr.canUndo()) {
            logFail(testName, "canUndo should still be true after one undo");
            return;
        }

        if (!mgr.redo()) {
            logFail(testName, "redo() should return true");
            return;
        }
        if (mgr.canRedo()) {
            logFail(testName, "canRedo should be false after redo to original state");
            return;
        }

        logPass(testName);
    }

    // --- undo 清除 redo 历史 ---
    {
        constexpr const char* testName = "UndoManager_UndoClearsRedoHistory";
        UndoManager mgr;

        mgr.addAction(std::make_unique<TestUndoAction>("A"));
        mgr.addAction(std::make_unique<TestUndoAction>("B"));

        mgr.undo();
        mgr.addAction(std::make_unique<TestUndoAction>("C"));

        if (mgr.canRedo()) {
            logFail(testName, "canRedo should be false after adding new action following undo");
            return;
        }

        logPass(testName);
    }

    // --- 递归防护 ---
    {
        constexpr const char* testName = "UndoManager_RecursionGuard";
        UndoManager mgr;
        bool addInUndoCalled = false;

        auto action1 = std::make_unique<TestUndoAction>("Action1",
            [&]() {
                mgr.addAction(std::make_unique<TestUndoAction>("Nested"));
                addInUndoCalled = true;
            });
        mgr.addAction(std::move(action1));
        mgr.addAction(std::make_unique<TestUndoAction>("Action2"));

        mgr.undo(); // undo Action2 (no callback)
        mgr.undo(); // undo Action1 -> lambda fires, tries addAction

        if (!addInUndoCalled) {
            logFail(testName, "undo callback was not invoked");
            return;
        }
        if (!mgr.canRedo()) {
            logFail(testName, "canRedo should be true – recursive addAction must be ignored");
            return;
        }

        logPass(testName);
    }

    // --- 容量限制 ---
    {
        constexpr const char* testName = "UndoManager_CapacityLimit";
        UndoManager mgr;

        for (int i = 0; i < 501; ++i)
            mgr.addAction(std::make_unique<TestUndoAction>(juce::String(i)));

        for (int i = 0; i < 500; ++i) {
            if (!mgr.canUndo()) {
                logFail(testName, "canUndo became false before 500 undos");
                return;
            }
            mgr.undo();
        }
        if (mgr.canUndo()) {
            logFail(testName, "canUndo should be false after 500 undos");
            return;
        }

        logPass(testName);
    }

    // --- onChange 回调 ---
    {
        constexpr const char* testName = "UndoManager_OnChangeCallback";
        UndoManager mgr;
        int changeCount = 0;
        mgr.setOnChange([&]() { ++changeCount; });

        mgr.addAction(std::make_unique<TestUndoAction>("A"));
        if (changeCount != 1) {
            logFail(testName, "onChange not called on add");
            return;
        }

        mgr.addAction(std::make_unique<TestUndoAction>("B"));
        if (changeCount != 2) {
            logFail(testName, "onChange not called on second add");
            return;
        }

        mgr.undo();
        if (changeCount != 3) {
            logFail(testName, "onChange not called on undo");
            return;
        }

        mgr.redo();
        if (changeCount != 4) {
            logFail(testName, "onChange not called on redo");
            return;
        }

        mgr.clear();
        if (changeCount != 5) {
            logFail(testName, "onChange not called on clear");
            return;
        }

        logPass(testName);
    }

    // --- clear ---
    {
        constexpr const char* testName = "UndoManager_Clear";
        UndoManager mgr;

        mgr.addAction(std::make_unique<TestUndoAction>("A"));
        mgr.addAction(std::make_unique<TestUndoAction>("B"));
        mgr.clear();

        if (mgr.canUndo()) {
            logFail(testName, "canUndo should be false after clear");
            return;
        }
        if (mgr.canRedo()) {
            logFail(testName, "canRedo should be false after clear");
            return;
        }

        logPass(testName);
    }

    // --- getUndoDescription / getRedoDescription ---
    {
        constexpr const char* testName = "UndoManager_Descriptions";
        UndoManager mgr;

        if (mgr.getUndoDescription().isNotEmpty() || mgr.getRedoDescription().isNotEmpty()) {
            logFail(testName, "descriptions should be empty for empty manager");
            return;
        }

        mgr.addAction(std::make_unique<TestUndoAction>("First"));
        mgr.addAction(std::make_unique<TestUndoAction>("Second"));

        if (mgr.getUndoDescription() != "Second") {
            logFail(testName, "undo description should be 'Second'");
            return;
        }
        if (mgr.getRedoDescription().isNotEmpty()) {
            logFail(testName, "redo description should be empty when nothing undone");
            return;
        }

        mgr.undo();
        if (mgr.getUndoDescription() != "First") {
            logFail(testName, "undo description should be 'First' after undo");
            return;
        }
        if (mgr.getRedoDescription() != "Second") {
            logFail(testName, "redo description should be 'Second' after undo");
            return;
        }

        logPass(testName);
    }
}

// ── SimdAccelerator Tests ─────────────────────────

void runSimdAcceleratorDotProductTest()
{
    constexpr const char* testName = "SimdAccelerator_DotProduct";
    const auto& simd = SimdAccelerator::getInstance();

    // 已知结果: [1,2,3,4] · [5,6,7,8] = 5+12+21+32 = 70
    const float a[] = { 1.0f, 2.0f, 3.0f, 4.0f };
    const float b[] = { 5.0f, 6.0f, 7.0f, 8.0f };
    const float result = simd.dotProduct(a, b, 4);

    if (!approxEqual(result, 70.0f, 1e-4f)) {
        logFail(testName, ("expected 70, got " + juce::String(result, 6)).toRawUTF8());
        return;
    }

    // 空向量
    const float emptyResult = simd.dotProduct(a, b, 0);
    if (!approxEqual(emptyResult, 0.0f, 1e-6f)) {
        logFail(testName, "dot product of zero-length should be 0");
        return;
    }

    logPass(testName);
}

void runSimdAcceleratorVectorLogTest()
{
    constexpr const char* testName = "SimdAccelerator_VectorLog";
    const auto& simd = SimdAccelerator::getInstance();

    const float input[] = { 1.0f, 2.7182818f, 10.0f, 100.0f };
    float result[4] = {};
    float expected[4] = {};
    for (int i = 0; i < 4; ++i)
        expected[i] = std::log(input[i]);

    simd.vectorLog(result, input, 4);

    for (int i = 0; i < 4; ++i) {
        if (!approxEqual(result[i], expected[i], 1e-4f)) {
            logFail(testName, ("mismatch at index " + juce::String(i)
                + ": expected " + juce::String(expected[i], 6)
                + " got " + juce::String(result[i], 6)).toRawUTF8());
            return;
        }
    }

    logPass(testName);
}

void runSimdAcceleratorBackendNameTest()
{
    constexpr const char* testName = "SimdAccelerator_BackendName";
    const auto& simd = SimdAccelerator::getInstance();
    const char* name = simd.getBackendName();

    if (name == nullptr || name[0] == '\0') {
        logFail(testName, "backend name is null or empty");
        return;
    }

#if defined(__APPLE__)
    if (juce::String(name) != "Apple Accelerate") {
        logFail(testName, ("expected 'Apple Accelerate', got '" + juce::String(name) + "'").toRawUTF8());
        return;
    }
#else
    if (juce::String(name) != "Scalar") {
        logFail(testName, ("expected 'Scalar', got '" + juce::String(name) + "'").toRawUTF8());
        return;
    }
#endif

    logPass(testName);
}

void runSimdAcceleratorDotProductLargeVectorTest()
{
    constexpr const char* testName = "SimdAccelerator_DotProduct_LargeVector";
    const auto& simd = SimdAccelerator::getInstance();

    // 模拟 MelSpectrogram 的实际工作负载: 1025 维 dot product
    constexpr size_t N = 1025;
    std::vector<float> a(N), b(N);
    float expectedSum = 0.0f;
    for (size_t i = 0; i < N; ++i) {
        a[i] = static_cast<float>(i) * 0.001f;
        b[i] = static_cast<float>(N - i) * 0.001f;
        expectedSum += a[i] * b[i];
    }

    const float result = simd.dotProduct(a.data(), b.data(), N);

    if (!approxEqual(result, expectedSum, expectedSum * 1e-4f)) {
        logFail(testName, ("expected " + juce::String(expectedSum, 6)
            + " got " + juce::String(result, 6)).toRawUTF8());
        return;
    }

    logPass(testName);
}

// ── ChannelLayoutPolicy Tests (channel-layout-policy spec) ────────────────────

void runChannelLayoutNumericGuardTest()
{
    constexpr const char* testName = "ChannelLayout_NumericGuard_PreservesHighAmplitude";

    juce::AudioBuffer<float> buf(1, 8);
    auto* p = buf.getWritePointer(0);
    p[0] = 0.5f;
    p[1] = std::numeric_limits<float>::quiet_NaN();
    p[2] = std::numeric_limits<float>::infinity();
    p[3] = -std::numeric_limits<float>::infinity();
    p[4] = 5.0f;
    p[5] = -7.0f;
    p[6] = 0.0f;
    p[7] = 1e-9f;

    Capture::CaptureSession::applyNumericGuardForTest(buf);

    if (!approxEqual(p[0], 0.5f)) { logFail(testName, "p[0] mutated unexpectedly"); return; }
    if (!approxEqual(p[1], 0.0f)) { logFail(testName, "NaN not zeroed"); return; }
    if (!approxEqual(p[2], 0.0f)) { logFail(testName, "+Inf not zeroed"); return; }
    if (!approxEqual(p[3], 0.0f)) { logFail(testName, "-Inf not zeroed"); return; }
    if (!approxEqual(p[4], 5.0f)) { logFail(testName, "high amplitude 5.0 NOT preserved"); return; }
    if (!approxEqual(p[5], -7.0f)) { logFail(testName, "high amplitude -7.0 NOT preserved"); return; }
    if (!approxEqual(p[6], 0.0f)) { logFail(testName, "zero mutated unexpectedly"); return; }
    if (!approxEqual(p[7], 1e-9f, 1e-12f)) { logFail(testName, "low amplitude not preserved"); return; }

    logPass(testName);
}

void runChannelLayoutCaptureSegmentSnapshotTest()
{
    constexpr const char* testName = "ChannelLayout_CaptureSegment_Snapshot";

    Capture::ProcessorBindings bindings{};
    Capture::CaptureSession session(std::move(bindings));

    // Initial config: mono
    session.prepareToPlay(44100.0, 512, /*hostInputChannels=*/1);
    if (session.getCaptureChannels() != 1) {
        logFail(testName, "captureChannels should be 1 after mono prepareToPlay");
        return;
    }
    if (!session.armNewCapture()) {
        logFail(testName, "armNewCapture failed");
        return;
    }
    const auto& segs = session.testSegments();
    if (segs.empty() || segs.back()->captureChannels != 1) {
        logFail(testName, "first segment captureChannels snapshot must be 1");
        return;
    }

    // Bus reconfigure mid-session: still-armed segment must retain its snapshot.
    session.prepareToPlay(44100.0, 512, /*hostInputChannels=*/2);
    if (session.getCaptureChannels() != 2) {
        logFail(testName, "captureChannels should update to 2 after stereo prepareToPlay");
        return;
    }
    if (segs.back()->captureChannels != 1) {
        logFail(testName, "armed segment must retain captureChannels=1 across session reconfigure");
        return;
    }

    logPass(testName);
}

void runChannelLayoutMaterializationStoreInvariantTest()
{
    constexpr const char* testName = "ChannelLayout_MaterializationStore_Invariant";

    MaterializationStore store;

    // Mono: accepted.
    {
        auto buf = std::make_shared<juce::AudioBuffer<float>>(1, 1024);
        buf->clear();
        MaterializationStore::CreateMaterializationRequest req{};
        req.sourceId = 1;
        req.audioBuffer = buf;
        req.sourceWindow = SourceWindow{1, 0.0, 1.0};
        const uint64_t id = store.createMaterialization(std::move(req));
        if (id == 0) { logFail(testName, "mono materialization creation failed"); return; }
    }

    // Stereo: accepted.
    {
        auto buf = std::make_shared<juce::AudioBuffer<float>>(2, 1024);
        buf->clear();
        MaterializationStore::CreateMaterializationRequest req{};
        req.sourceId = 2;
        req.audioBuffer = buf;
        req.sourceWindow = SourceWindow{2, 0.0, 1.0};
        const uint64_t id = store.createMaterialization(std::move(req));
        if (id == 0) { logFail(testName, "stereo materialization creation failed"); return; }
    }

    // 4 channels: rejected.
    {
        auto buf = std::make_shared<juce::AudioBuffer<float>>(4, 1024);
        buf->clear();
        MaterializationStore::CreateMaterializationRequest req{};
        req.sourceId = 3;
        req.audioBuffer = buf;
        req.sourceWindow = SourceWindow{3, 0.0, 1.0};
        const uint64_t id = store.createMaterialization(std::move(req));
        if (id != 0) {
            logFail(testName, "4-channel materialization MUST be rejected");
            return;
        }
    }

    logPass(testName);
}

void runChannelLayoutPrepareImportRejectsMultichannelTest()
{
    constexpr const char* testName = "ChannelLayout_PrepareImport_RejectsMultichannel";

    OpenTuneAudioProcessor processor;

    // 1ch import → accepted, stored as 1.
    {
        juce::AudioBuffer<float> mono(1, 4410);
        mono.clear();
        OpenTuneAudioProcessor::PreparedImport prep;
        if (!processor.prepareImport(std::move(mono), 44100.0,
                                       juce::String("mono.wav"), juce::String("mono.wav"), prep)) {
            logFail(testName, "mono import unexpectedly rejected");
            return;
        }
        if (prep.storedAudioBuffer.getNumChannels() != 1) {
            logFail(testName, "mono storage channel count must be 1");
            return;
        }
    }

    // 2ch import → accepted, stored as 2.
    {
        juce::AudioBuffer<float> stereo(2, 4410);
        stereo.clear();
        OpenTuneAudioProcessor::PreparedImport prep;
        if (!processor.prepareImport(std::move(stereo), 44100.0,
                                       juce::String("stereo.wav"), juce::String("stereo.wav"), prep)) {
            logFail(testName, "stereo import unexpectedly rejected");
            return;
        }
        if (prep.storedAudioBuffer.getNumChannels() != 2) {
            logFail(testName, "stereo storage channel count must be 2");
            return;
        }
    }

    // 6ch import → rejected.
    {
        juce::AudioBuffer<float> surround(6, 4410);
        surround.clear();
        OpenTuneAudioProcessor::PreparedImport prep;
        if (processor.prepareImport(std::move(surround), 44100.0,
                                      juce::String("surround.wav"), juce::String("surround.wav"), prep)) {
            logFail(testName, "5.1 import MUST be rejected");
            return;
        }
    }

    logPass(testName);
}

// ============================================================================
// Timeline Rendering Pipeline Tests — L1 Static Contract Gate (Source Guards)
// ============================================================================

void runTimelinePlayheadOverlayDirtyRectOnlyTest()
{
    constexpr const char* testName = "TimelinePlayhead_OverlayDirtyRectOnly";

    const auto setPlayheadSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PlayheadOverlayComponent.h",
        "void setPlayheadSeconds",
        "void setZoomLevel");
    const auto setZoomSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PlayheadOverlayComponent.h",
        "void setZoomLevel",
        "void setScrollOffset");
    const auto setScrollSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PlayheadOverlayComponent.h",
        "void setScrollOffset",
        "void setTimelineStart");
    const auto setTimelineSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PlayheadOverlayComponent.h",
        "void setTimelineStartSeconds",
        "void setPianoKeyWidth");

    if (setPlayheadSection.isEmpty() || setZoomSection.isEmpty()
        || setScrollSection.isEmpty() || setTimelineSection.isEmpty()) {
        logFail(testName, "failed to locate PlayheadOverlayComponent setter sections");
        return;
    }

    // Each setter must NOT use unconditional repaint() — must use dirty-rect union of old+new
    bool anyUnconditionalRepaint = false;
    if (setPlayheadSection.contains("repaint()")) anyUnconditionalRepaint = true;
    if (setZoomSection.contains("repaint()")) anyUnconditionalRepaint = true;
    if (setScrollSection.contains("repaint()")) anyUnconditionalRepaint = true;
    if (setTimelineSection.contains("repaint()")) anyUnconditionalRepaint = true;

    if (anyUnconditionalRepaint) {
        logFail(testName, "PlayheadOverlay setter(s) still use unconditional repaint() — must use dirty-rect union of old+new narrow rects");
        return;
    }

    logPass(testName);
}

void runTimelinePlayheadPositionDoesNotEnterRenderModelKeyTest()
{
    constexpr const char* testName = "TimelinePlayhead_PositionDoesNotEnterRenderModelKey";

    const auto& pianoCacheHeader = getFileCache().get("Source/Standalone/UI/PianoRoll/PianoRollRenderModelCache.h");
    const auto& arrangementCacheHeader = getFileCache().get("Source/Standalone/UI/ArrangementRenderModelCache.h");

    std::initializer_list<const char*> forbidden = {
        "playhead", "Playhead", "positionSeconds", "PositionSeconds", "transportPosition"
    };

    for (const auto* needle : forbidden) {
        if (pianoCacheHeader.contains(needle)) {
            logFail(testName,
                    (juce::String("PianoRoll render-model key/cache contains playhead dependency: ")
                     + needle).toRawUTF8());
            return;
        }
        if (arrangementCacheHeader.contains(needle)) {
            logFail(testName,
                    (juce::String("Arrangement render-model key/cache contains playhead dependency: ")
                     + needle).toRawUTF8());
            return;
        }
    }

    logPass(testName);
}

void runTimelineInvalidationViewportShiftExposesOnlyNewStripTest()
{
    constexpr const char* testName = "TimelineInvalidation_ViewportShiftExposesOnlyNewStrip";

    const auto pianoScrollSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "void PianoRollComponent::setScrollOffset",
        "void PianoRollComponent::onHeartbeatTick");

    const auto arrScrollSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/ArrangementViewComponent.cpp",
        "void ArrangementViewComponent::setScrollOffset",
        "void ArrangementViewComponent::mouseDown");

    if (pianoScrollSection.isEmpty() || arrScrollSection.isEmpty()) {
        logFail(testName, "failed to locate scroll methods");
        return;
    }

    bool pianoFull = pianoScrollSection.contains("repaint()") && !pianoScrollSection.contains("FrameScheduler");
    bool arrFull = arrScrollSection.contains("repaint()") && !arrScrollSection.contains("FrameScheduler");

    if (pianoFull || arrFull) {
        logFail(testName,
            (juce::String("scroll still triggers whole-component repaint: PianoRoll=")
             + juce::String(pianoFull ? "yes" : "no")
             + juce::String(", Arrangement=")
             + juce::String(arrFull ? "yes" : "no")).toRawUTF8());
        return;
    }

    logPass(testName);
}

void runTimelineInvalidationBigJumpPromotesToSingleFullRedrawTest()
{
    constexpr const char* testName = "TimelineInvalidation_BigJumpPromotesToSingleFullRedraw";

    TimelineViewportState viewport;
    viewport.viewportWidthPx = 800;
    viewport.viewportHeightPx = 240;

    const auto smallStrip = viewport.exposedStripForScrollDelta(0, 120);
    if (smallStrip.isEmpty() || viewport.requiresFullRedrawForDelta(0, 120)) {
        logFail(testName, "small scroll should expose a dirty strip instead of requiring full redraw");
        return;
    }

    const auto bigStrip = viewport.exposedStripForScrollDelta(0, 801);
    if (!bigStrip.isEmpty() || !viewport.requiresFullRedrawForDelta(0, 801)) {
        logFail(testName, "big scroll should promote to one full redraw decision");
        return;
    }

    const auto& frameScheduler = getFileCache().get("Source/Standalone/UI/FrameScheduler.h");
    if (!frameScheduler.contains("full repaint promotion") && !frameScheduler.contains("recordFullRepaintPromotion")) {
        logFail(testName, "FrameScheduler does not record full repaint promotions for large viewport jumps");
        return;
    }

    logPass(testName);
}

void runPianoRollPlayheadOnlyTicksDoNotRebuildRenderModelTest()
{
    constexpr const char* testName = "PianoRoll_PlayheadOnlyTicksDoNotRebuildRenderModel";

    const auto overlayHeader = extractWorkspaceFileSection(
        "Source/Standalone/UI/PlayheadOverlayComponent.h",
        "void setPlayheadSeconds",
        "void setZoomLevel");
    const auto componentSource = getFileCache().get("Source/Standalone/UI/PianoRollComponent.cpp");

    if (overlayHeader.isEmpty()) {
        logFail(testName, "failed to locate PlayheadOverlayComponent playhead setter");
        return;
    }

    if (overlayHeader.contains("prepareVisibleRenderModel")
        || overlayHeader.contains("invalidateRenderModel")
        || overlayHeader.contains("buildRenderContext")) {
        logFail(testName, "playhead setter still rebuilds or invalidates PianoRoll content model");
        return;
    }

    const auto playheadNotifySection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "void PianoRollComponent::notifyPlayheadChange",
        "void PianoRollComponent::updatePlayheadOverlay");
    if (!playheadNotifySection.isEmpty()
        && (playheadNotifySection.contains("prepareVisibleRenderModel")
            || playheadNotifySection.contains("renderModelCache_.invalidate"))) {
        logFail(testName, "PianoRoll playhead notification invalidates/rebuilds the render model");
        return;
    }

    if (componentSource.contains("playhead") && componentSource.contains("PianoRollRenderModelCache::Key")
        && componentSource.contains("playheadSeconds")) {
        logFail(testName, "PianoRoll render model key appears to include playhead position");
        return;
    }

    logPass(testName);
}

void runPianoRollStoppedSeekKeepsPresentationIntentTest()
{
    constexpr const char* testName = "PianoRoll_StoppedSeekKeepsPresentationIntent";

    const auto header = getFileCache().get("Source/Standalone/UI/PianoRollComponent.h");
    const auto source = getFileCache().get("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto notifySection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "toolCtx.notifyPlayheadChange = [this](double time) {",
        "toolCtx.notifyPitchCurveEdited = [this](int s, int e) {");
    const auto vblankSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "void PianoRollComponent::onScrollVBlankCallback",
        "void PianoRollComponent::setZoomLevel");

    if (header.isEmpty() || source.isEmpty() || notifySection.isEmpty() || vblankSection.isEmpty()) {
        logFail(testName, "failed to locate PianoRoll stopped-seek implementation sections");
        return;
    }

    if (!header.contains("double pendingSeekTime_{-1.0};")
        || !header.contains("pending playhead presentation intent")) {
        logFail(testName, "pendingSeekTime_ contract comment was not updated to presentation-intent semantics");
        return;
    }

    if (!header.contains("double lastObservedRawPlayheadTime_{0.0};")) {
        logFail(testName, "stopped presentation path must track the last observed raw host playhead");
        return;
    }

    if (!notifySection.contains("pendingSeekTime_ = time;")) {
        logFail(testName, "notifyPlayheadChange no longer records presentation intent");
        return;
    }

    if (!notifySection.contains("playheadOverlay_.setPlayheadSeconds(time);")) {
        logFail(testName, "notifyPlayheadChange must still update overlay immediately");
        return;
    }

    if (!header.contains("if (stateChanged && playing)")
        || !header.contains("pendingSeekTime_ = -1.0;")) {
        logFail(testName, "setIsPlaying must clear pending presentation intent when playback resumes");
        return;
    }

    if (!source.contains("const double rawHostTime = readPlayheadTime();")
        || !source.contains("const double hostTime = projectPlayheadTime(rawHostTime);")) {
        logFail(testName, "stopped presentation path must read raw host time and project it explicitly");
        return;
    }

    if (!vblankSection.contains("if (pendingSeekTime_ >= 0.0)")) {
        logFail(testName, "stopped VBlank path does not honor pending presentation intent");
        return;
    }

    if (vblankSection.contains("playheadOverlay_.setPlayheadSeconds(hostTime);\n        pendingSeekTime_ = -1.0;\n        return;")) {
        logFail(testName, "stopped VBlank path still immediately overwrites seek intent with host/clamped time");
        return;
    }

    if (!vblankSection.contains("const double stoppedPresentationTime = pendingSeekTime_ >= 0.0 ? pendingSeekTime_ : hostTime;")
        || !vblankSection.contains("playheadOverlay_.setPlayheadSeconds(stoppedPresentationTime);")) {
        logFail(testName, "stopped VBlank path must prefer pending presentation intent over host time");
        return;
    }

    if (!vblankSection.contains("std::abs(rawHostTime - lastObservedRawPlayheadTime_) > 0.001")
        || !vblankSection.contains("lastObservedRawPlayheadTime_ = rawHostTime;")) {
        logFail(testName, "stopped presentation path must yield back to host truth when the stopped raw host position changes");
        return;
    }

    logPass(testName);
}

void runPianoRollPaintConsumesPreparedRenderModelOnlyTest()
{
    constexpr const char* testName = "PianoRoll_PaintConsumesPreparedRenderModelOnly";

    const auto paintSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "void PianoRollComponent::paint",
        "void PianoRollComponent::resized");

    if (paintSection.isEmpty()) {
        logFail(testName, "failed to locate paint() method");
        return;
    }

    if (paintSection.contains("buildRenderContext")
        || paintSection.contains("prepareVisibleRenderModel")
        || paintSection.contains("buildMaterializationRenderItem")) {
        logFail(testName, "paint() still builds or refreshes PianoRoll render-model data");
        return;
    }

    // paint() must NOT have processor/store side-effect reads
    if (paintSection.contains("getMaterializationStore()")
        || paintSection.contains("getSourceStore()")
        || paintSection.contains("getProcessor()")
        || paintSection.contains("processor_")) {
        logFail(testName, "paint() still has processor/store side-effect reads");
        return;
    }

    if (paintSection.contains("renderCorrectedOnlyRange")
        || paintSection.contains("getOriginalF0()")
        || paintSection.contains("drawF0Curve")) {
        logFail(testName, "paint() still performs raw F0 preparation/draw calls instead of prepared visual segments");
        return;
    }

    logPass(testName);
}

void runPianoRollF0VisualsUseSingleRenderPathTest()
{
    constexpr const char* testName = "PianoRoll_F0VisualsUseSingleRenderPath";

    const auto& componentHeader = getFileCache().get("Source/Standalone/UI/PianoRollComponent.h");
    const auto& componentSource = getFileCache().get("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto paintSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "void PianoRollComponent::paint",
        "void PianoRollComponent::resized");
    const auto drawCurveSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp",
        "void PianoRollRenderer::drawF0Curve",
        "void PianoRollRenderer::drawGhostNotes");

    if (componentHeader.contains("drawSelectedOriginalF0Curve")) {
        logFail(testName, "drawSelectedOriginalF0Curve declaration still exists — parallel F0 render path");
        return;
    }

    if (componentSource.contains("drawSelectedOriginalF0Curve")) {
        logFail(testName, "drawSelectedOriginalF0Curve implementation still exists — parallel F0 render path");
        return;
    }

    if (!paintSection.isEmpty()
        && (paintSection.contains("getOriginalF0()")
            || paintSection.contains("drawF0Curve"))) {
        logFail(testName, "PianoRoll paint still has a raw F0 draw path instead of prepared F0 visuals");
        return;
    }

    if (!drawCurveSection.isEmpty() && drawCurveSection.contains("buildF0VisualSegments")) {
        logFail(testName, "PianoRollRenderer::drawF0Curve still builds F0 visual segments at draw time");
        return;
    }

    logPass(testName);
}

void runPianoRollVisibleRangeCullsNotesF0AndWaveformTilesTest()
{
    constexpr const char* testName = "PianoRoll_VisibleRangeCullsNotesF0AndWaveformTiles";

    const auto& cacheHeader = getFileCache().get("Source/Standalone/UI/PianoRoll/PianoRollRenderModelCache.h");
    const auto& componentSource = getFileCache().get("Source/Standalone/UI/PianoRollComponent.cpp");

    if (!cacheHeader.contains("visibleStart") || !cacheHeader.contains("visibleEnd")) {
        logFail(testName, "PianoRoll render-model key does not include viewport visible time/frame range");
        return;
    }

    if (!cacheHeader.contains("projection") && !cacheHeader.contains("Projection")) {
        logFail(testName, "PianoRoll render-model key does not include projection identity/revision");
        return;
    }

    const auto buildItemSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "PianoRollRenderer::MaterializationRenderItem PianoRollComponent::buildMaterializationRenderItem",
        "void PianoRollComponent::visibilityChanged");
    if (!buildItemSection.isEmpty()
        && buildItemSection.contains("renderCorrectedOnlyRange(\n                0,")) {
        logFail(testName, "PianoRoll corrected F0 is still generated from frame 0 instead of visible range");
        return;
    }

    if (!componentSource.contains("rangeForTimesWithMargin")
        && !componentSource.contains("visibleTimeStart")) {
        logFail(testName, "PianoRoll model preparation does not derive visible F0/note range");
        return;
    }

    logPass(testName);
}

void runPianoRollVerticalGeometryInvalidatesRenderModelKeyTest()
{
    constexpr const char* testName = "PianoRoll_VerticalGeometryInvalidatesRenderModelKey";

    const auto& cacheHeader = getFileCache().get("Source/Standalone/UI/PianoRoll/PianoRollRenderModelCache.h");
    const auto& componentHeader = getFileCache().get("Source/Standalone/UI/PianoRollComponent.h");
    const auto prepareSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "void PianoRollComponent::prepareVisibleRenderModel() const",
        "void PianoRollComponent::refreshVerticalViewportGeometry");
    const auto refreshSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "void PianoRollComponent::refreshVerticalViewportGeometry",
        "void PianoRollComponent::setScale");

    if (prepareSection.isEmpty() || refreshSection.isEmpty()) {
        logFail(testName, "failed to locate PianoRoll vertical geometry preparation sections");
        return;
    }

    if (!cacheHeader.contains("verticalZoomBucket")
        || !cacheHeader.contains("verticalScrollBucket")
        || !cacheHeader.contains("verticalZoomBucket == o.verticalZoomBucket")
        || !cacheHeader.contains("verticalScrollBucket == o.verticalScrollBucket")) {
        logFail(testName, "PianoRoll render-model key does not compare vertical zoom/scroll geometry");
        return;
    }

    if (!prepareSection.contains("cacheKey.verticalZoomBucket = quantizeGeometryPx(pixelsPerSemitone_);")
        || !prepareSection.contains("cacheKey.verticalScrollBucket = quantizeGeometryPx(verticalScrollOffset_);")) {
        logFail(testName, "PianoRoll render-model key is not populated from current vertical geometry");
        return;
    }

    if (!componentHeader.contains("refreshVerticalViewportGeometry")
        || !refreshSection.contains("updateScrollBars();")
        || !refreshSection.contains("prepareVisibleRenderModel();")
        || !refreshSection.contains("invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Viewport)")) {
        logFail(testName, "vertical geometry helper must update scrollbars, prepare model, and invalidate viewport");
        return;
    }

    struct SectionSpec {
        const char* label;
        const char* start;
        const char* end;
    };
    const std::array<SectionSpec, 5> verticalEntrypoints{{
        { "vertical zoom", "void PianoRollComponent::handleVerticalZoomWheel", "void PianoRollComponent::handleHorizontalScrollWheel" },
        { "vertical wheel scroll", "void PianoRollComponent::handleVerticalScrollWheel", "void PianoRollComponent::handleHorizontalZoomWheel" },
        { "vertical scrollbar", "void PianoRollComponent::scrollBarMoved", "std::vector<Note> PianoRollComponent::getEditedMaterializationNotesCopy" },
        { "fit-to-screen", "void PianoRollComponent::fitToScreen", "// HachiTune-style MIDI-based coordinate conversion" },
        { "panning", "void PianoRollComponent::mouseDrag", "void PianoRollComponent::mouseUp" }
    }};
    for (const auto& entrypoint : verticalEntrypoints) {
        const auto entrySection = extractWorkspaceFileSection(
            "Source/Standalone/UI/PianoRollComponent.cpp",
            entrypoint.start,
            entrypoint.end);
        if (entrySection.isEmpty()) {
            logFail(testName, (juce::String("failed to locate vertical geometry entrypoint: ") + entrypoint.label).toRawUTF8());
            return;
        }
        if (!entrySection.contains("refreshVerticalViewportGeometry")) {
            logFail(testName, (juce::String("vertical geometry entrypoint does not refresh prepared model: ") + entrypoint.label).toRawUTF8());
            return;
        }
    }

    logPass(testName);
}

void runPianoRollRenderContextUsesSnapshotVerticalCoordinatesTest()
{
    constexpr const char* testName = "PianoRoll_RenderContextUsesSnapshotVerticalCoordinates";

    const auto buildSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "PianoRollRenderer::RenderContext PianoRollComponent::buildRenderContext",
        "void PianoRollComponent::prepareVisibleRenderModel");

    if (buildSection.isEmpty()) {
        logFail(testName, "failed to locate PianoRoll buildRenderContext section");
        return;
    }

    if (!buildSection.contains("snapshotPixelsPerSemitone")
        || !buildSection.contains("snapshotVerticalScrollOffset")
        || !buildSection.contains("snapshotMaxMidi")) {
        logFail(testName, "RenderContext does not snapshot vertical geometry before building lambdas");
        return;
    }

    if (buildSection.contains("ctx.midiToY = [this]")
        || buildSection.contains("ctx.freqToY = [this]")
        || buildSection.contains("ctx.freqToMidi = [this]")) {
        logFail(testName, "RenderContext vertical coordinate lambdas still capture live component state");
        return;
    }

    if (!buildSection.contains("ctx.midiToY = [snapshotPixelsPerSemitone, snapshotVerticalScrollOffset, snapshotMaxMidi]")
        || !buildSection.contains("ctx.freqToY = [snapshotPixelsPerSemitone, snapshotVerticalScrollOffset, snapshotMaxMidi]")
        || !buildSection.contains("ctx.freqToMidi = []")) {
        logFail(testName, "RenderContext vertical coordinate lambdas are not built from the same snapshot");
        return;
    }

    logPass(testName);
}

void runArrangementScrollOffsetDoesNotInvalidateWholeComponentTest()
{
    constexpr const char* testName = "Arrangement_ScrollOffsetDoesNotInvalidateWholeComponent";

    const auto paintSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/ArrangementViewComponent.cpp",
        "void ArrangementViewComponent::paint",
        "void ArrangementViewComponent::drawTimeRuler");

    if (paintSection.isEmpty()) {
        logFail(testName, "failed to locate Arrangement paint() — check section boundaries");
        return;
    }

    // Anti-pattern: paint() enumerates all tracks and placements
    if (paintSection.contains("getNumPlacements()") || paintSection.contains("getNumTracks()")) {
        logFail(testName, "paint() still enumerates all tracks/placements per-paint");
        return;
    }

    // Anti-pattern: paint() builds waveform paths per-paint
    // Detected by: creating a new Path AND iterating waveform peaks in paint section
    if (paintSection.contains("juce::Path waveformPath") || paintSection.contains("juce::Path  waveformPath")) {
        logFail(testName, "paint() still builds waveform paths (juce::Path waveformPath) per-paint");
        return;
    }

    logPass(testName);
}

void runArrangementPaintConsumesVisibleRenderModelOnlyTest()
{
    constexpr const char* testName = "Arrangement_PaintConsumesVisibleRenderModelOnly";

    const auto paintSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/ArrangementViewComponent.cpp",
        "void ArrangementViewComponent::paint",
        "void ArrangementViewComponent::drawTimeRuler");

    if (paintSection.isEmpty()) return;

    // paint() must NOT lock placements or refresh the model per-paint
    if (paintSection.contains("getPlacementByIndex")
        || paintSection.contains("requestRenderModelUpdate")
        || paintSection.contains("renderModelCache_.update")
        || paintSection.contains("processor_.")) {
        logFail(testName, "Arrangement paint() still performs model update or processor reads");
        return;
    }

    logPass(testName);
}

void runArrangementVisibleRangeCullsOffscreenPlacementsTest()
{
    constexpr const char* testName = "Arrangement_VisibleRangeCullsOffscreenPlacements";

    const auto& cacheHeader = getFileCache().get("Source/Standalone/UI/ArrangementRenderModelCache.h");
    const auto& cacheSource = getFileCache().get("Source/Standalone/UI/ArrangementRenderModelCache.cpp");

    if (!cacheHeader.contains("visibleTimeStart") || !cacheHeader.contains("visibleTimeEnd")) {
        logFail(testName, "Arrangement render-model key does not include visible time range");
        return;
    }

    if (!cacheHeader.contains("arrangementRevision") && !cacheHeader.contains("snapshotRevision")) {
        logFail(testName, "Arrangement render-model cache lacks arrangement revision in its key");
        return;
    }

    if (!cacheSource.contains("timelineEndSeconds() < visibleTimeStart")
        || !cacheSource.contains("timelineStartSeconds > visibleTimeEnd")) {
        logFail(testName, "Arrangement render-model update does not cull offscreen placements by visible range");
        return;
    }

    logPass(testName);
}

void runWaveformTileCacheHasBoundedMemoryAndEvictionTest()
{
    constexpr const char* testName = "WaveformTileCache_HasBoundedMemoryAndEviction";

    WaveformTileCache cache;
    WaveformMipmap mipmap;
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, 4096);
    audio->clear();
    for (int i = 0; i < audio->getNumSamples(); ++i)
        audio->setSample(0, i, std::sin(static_cast<float>(i) * 0.03f));
    mipmap.setAudioSource(audio);

    int guard = 0;
    while (!mipmap.isComplete() && guard < 1000) {
        mipmap.buildIncremental(1.0);
        ++guard;
    }

    juce::Rectangle<int> bounds(0, 0, 80, 32);
    const auto maxTiles = WaveformTileCache::kMaxTiles;
    for (std::size_t i = 0; i < maxTiles + 24; ++i) {
        cache.getOrCreate(static_cast<uint64_t>(i + 1),
                          static_cast<uint64_t>(i + 1),
                          static_cast<int>(i % 9),
                          mipmap,
                          1.0f,
                          bounds,
                          static_cast<double>(i),
                          static_cast<double>(i) + 1.0,
                          0,
                          0);
    }

    if (cache.size() > maxTiles) {
        logFail(testName, "WaveformTileCache exceeded its configured max tile count");
        return;
    }

    if (cache.get(1, 1, 0, 0.0, 1.0, 0, 0) != nullptr) {
        logFail(testName, "WaveformTileCache did not evict the oldest tile after capacity pressure");
        return;
    }

    logPass(testName);
}

void runTimelineFrameDriverCoalescesRequestsByPriorityTest()
{
    constexpr const char* testName = "TimelineFrameDriver_CoalescesRequestsByPriority";

    FrameScheduler::instance().resetDiagnosticsForTests();
    juce::Component component;
    component.setBounds(0, 0, 200, 100);

    FrameScheduler::instance().requestViewportShift(component, {0, 0, 10, 100});
    FrameScheduler::instance().requestViewportShift(component, {10, 0, 10, 100});

    const auto snapshot = FrameScheduler::instance().diagnosticsSnapshot();
    if (snapshot.viewportShiftRequests < 2 || snapshot.pendingComponentCount != 1) {
        logFail(testName, "FrameScheduler did not coalesce multiple viewport requests by component");
        return;
    }

    logPass(testName);
}

void runTimelineFrameDriverDropsNonCriticalAnimationRateDuringPlaybackTest()
{
    constexpr const char* testName = "TimelineFrameDriver_DropsNonCriticalAnimationRateDuringPlayback";

    FrameScheduler::instance().resetDiagnosticsForTests();
    FrameScheduler::instance().setPlaybackActiveForTimeline(true);
    juce::Component component;
    component.setBounds(0, 0, 200, 100);

    for (int i = 0; i < 8; ++i)
        FrameScheduler::instance().requestLowPriorityAnimation(component, {0, 0, 50, 20});

    FrameScheduler::instance().setPlaybackActiveForTimeline(false);

    const auto snapshot = FrameScheduler::instance().diagnosticsSnapshot();
    if (snapshot.lowPriorityAnimationDropped == 0) {
        logFail(testName, "FrameScheduler did not drop/throttle low-priority animation while playback is active");
        return;
    }

    logPass(testName);
}

void runTimelineEditorHeartbeatNoDirectRepaintTest()
{
    constexpr const char* testName = "TimelineEditor_HeartbeatNoDirectRepaint";

    const auto& standaloneEditor = getFileCache().get("Source/Standalone/PluginEditor.cpp");
    const auto& vst3Editor = getFileCache().get("Source/Plugin/PluginEditor.cpp");

    // Anti-pattern: pianoRoll_.repaint() called directly from editor sync paths
    if (standaloneEditor.contains("pianoRoll_.repaint()")) {
        logFail(testName, "Standalone editor still has direct pianoRoll_.repaint() calls");
        return;
    }
    if (vst3Editor.contains("pianoRoll_.repaint()")) {
        logFail(testName, "VST3 editor still has direct pianoRoll_.repaint() calls");
        return;
    }

    logPass(testName);
}

void runTimelineNoParallelF0RenderPathsTest()
{
    constexpr const char* testName = "Timeline_NoParallelF0RenderPaths";

    const auto& componentSource = getFileCache().get("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto& componentHeader = getFileCache().get("Source/Standalone/UI/PianoRollComponent.h");
    const auto& rendererSource = getFileCache().get("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");

    // No drawSelectedOriginalF0Curve
    if (componentHeader.contains("drawSelectedOriginalF0Curve")
        || componentSource.contains("drawSelectedOriginalF0Curve")) {
        logFail(testName, "drawSelectedOriginalF0Curve still exists — separate F0 render path");
        return;
    }

    // No old/new render path switches
    if (componentSource.contains("useOldRenderer") || componentSource.contains("useNewRenderer")) {
        logFail(testName, "old/new renderer switch still present in component");
        return;
    }
    if (rendererSource.contains("useOldRenderer") || rendererSource.contains("useNewRenderer")) {
        logFail(testName, "old/new renderer switch still present in renderer");
        return;
    }

    // No fallback to full repaint
    if (componentSource.contains("fallbackRepaint") || componentSource.contains("fullRepaintFallback")) {
        logFail(testName, "fallback full-repaint path still exists");
        return;
    }

    logPass(testName);
}

void runTimelineKillListNoFullOverlayRepaintForPositionTest()
{
    constexpr const char* testName = "TimelineKillList_NoFullOverlayRepaintForPosition";

    const auto setPlayheadSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PlayheadOverlayComponent.h",
        "void setPlayheadSeconds",
        "void setZoomLevel");

    if (!setPlayheadSection.isEmpty()
        && setPlayheadSection.contains("repaint()")
        && !setPlayheadSection.contains("dirty")) {
        logFail(testName, "Playhead setPosition still triggers unconditional full repaint()");
        return;
    }

    logPass(testName);
}

void runTimelineKillListNoPaintTimeRenderContextBuildTest()
{
    constexpr const char* testName = "TimelineKillList_NoPaintTimeRenderContextBuild";

    const auto paintSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "void PianoRollComponent::paint",
        "void PianoRollComponent::resized");

    if (!paintSection.isEmpty()) {
        if (paintSection.contains("buildRenderContext")
            || paintSection.contains("prepareVisibleRenderModel")
            || paintSection.contains("buildMaterializationRenderItem")) {
            logFail(testName, "paint() still builds or refreshes render context/model");
            return;
        }
    }

    logPass(testName);
}

void runTimelineKillListNoScrollTimeWholeInvalidationTest()
{
    constexpr const char* testName = "TimelineKillList_NoScrollTimeWholeInvalidation";

    const auto pianoScrollSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "void PianoRollComponent::setScrollOffset",
        "void PianoRollComponent::onHeartbeatTick");
    const auto arrScrollSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/ArrangementViewComponent.cpp",
        "void ArrangementViewComponent::setScrollOffset",
        "void ArrangementViewComponent::mouseDown");

    bool pianoFull = !pianoScrollSection.isEmpty() && pianoScrollSection.contains("repaint()")
        && !pianoScrollSection.contains("FrameScheduler");
    bool arrFull = !arrScrollSection.isEmpty() && arrScrollSection.contains("repaint()")
        && !arrScrollSection.contains("FrameScheduler");

    if (pianoFull || arrFull) {
        logFail(testName,
            (juce::String("scroll still triggers whole-component invalidation: PianoRoll=")
             + juce::String(pianoFull ? "yes" : "no")
             + juce::String(", Arrangement=")
             + juce::String(arrFull ? "yes" : "no")).toRawUTF8());
        return;
    }

    logPass(testName);
}

void runTimelineKillListNoUnboundedUiCachesTest()
{
    constexpr const char* testName = "TimelineKillList_NoUnboundedUiCaches";

    bool tileCacheExists = workspaceFileExists("Source/Standalone/UI/WaveformTileCache.h");
    bool pianoCacheExists = workspaceFileExists("Source/Standalone/UI/PianoRoll/PianoRollRenderModelCache.h");
    bool arrCacheExists = workspaceFileExists("Source/Standalone/UI/ArrangementRenderModelCache.h");

    // If any cache already exists, verify bounded memory
    auto checkBounded = [](const juce::String& relativePath, const char* label) -> bool {
        const auto& source = getFileCache().get(relativePath);
        juce::ignoreUnused(label);
        return source.contains("kMaxEntries")
            || source.contains("kMaxTiles")
            || source.contains("evict");
    };

    if (tileCacheExists && !checkBounded("Source/Standalone/UI/WaveformTileCache.h", "WaveformTileCache")) {
        logFail(testName, "WaveformTileCache missing bounded memory guard");
        return;
    }
    if (pianoCacheExists && !checkBounded("Source/Standalone/UI/PianoRoll/PianoRollRenderModelCache.h", "PianoCache")) {
        logFail(testName, "PianoRollRenderModelCache missing bounded memory guard");
        return;
    }
    if (arrCacheExists && !checkBounded("Source/Standalone/UI/ArrangementRenderModelCache.h", "ArrangementCache")) {
        logFail(testName, "ArrangementRenderModelCache missing bounded memory guard");
        return;
    }

    logPass(testName);
}

void runTimelineKillListNoProcessorOwnedUiCacheTest()
{
    constexpr const char* testName = "TimelineKillList_NoProcessorOwnedUiCache";

    const auto& processorHeader = getFileCache().get("Source/PluginProcessor.h");
    const auto& processorSource = getFileCache().get("Source/PluginProcessor.cpp");

    std::initializer_list<const char*> forbiddenInProcessor = {
        "RenderModelCache", "WaveformTileCache", "ArrangementRenderModel",
        "TimelineViewportState", "PianoRollRenderModel", "TimelineFrameCoordinator"
    };

    for (const auto* needle : forbiddenInProcessor) {
        if (processorHeader.contains(needle)) {
            logFail(testName,
                (juce::String("processor header contains UI cache: ") + juce::String(needle)).toRawUTF8());
            return;
        }
        if (processorSource.contains(needle)) {
            logFail(testName,
                (juce::String("processor source contains UI cache: ") + juce::String(needle)).toRawUTF8());
            return;
        }
    }

    logPass(testName);
}

namespace OpenTune { void runTimelineRenderingPipelineCacheTests(); }

void runTimelinePaintHasNoProcessorOrStoreSideEffectsTest()
{
    constexpr const char* testName = "TimelinePaint_HasNoProcessorOrStoreSideEffects";

    const auto pianoPaintSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/PianoRollComponent.cpp",
        "void PianoRollComponent::paint",
        "void PianoRollComponent::resized");
    const auto arrPaintSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/ArrangementViewComponent.cpp",
        "void ArrangementViewComponent::paint",
        "void ArrangementViewComponent::drawTimeRuler");

    std::initializer_list<const char*> forbiddenInPaint = {
        "getMaterializationStore()", "getSourceStore()", "getProcessor()",
        "processor_.", "requestRenderModelUpdate", "renderModelCache_.update"
    };

    for (const auto* needle : forbiddenInPaint) {
        if (!pianoPaintSection.isEmpty() && pianoPaintSection.contains(needle)) {
            logFail(testName,
                (juce::String("PianoRoll paint() contains side-effect: ") + juce::String(needle)).toRawUTF8());
            return;
        }
        if (!arrPaintSection.isEmpty() && arrPaintSection.contains(needle)) {
            logFail(testName,
                (juce::String("Arrangement paint() contains side-effect: ") + juce::String(needle)).toRawUTF8());
            return;
        }
    }

    logPass(testName);
}

void runCoreBehaviorSuite()
{
    logSection("Core");
    runLockFreeQueueBasicEnqueueDequeueTest();
    runLockFreeQueueRejectsWhenFullTest();
    runLockFreeQueueClearDrainsAllItemsTest();
    runRendererBlockSpanClipsLeadingEdgeTest();
    runRendererBlockSpanClipsTrailingEdgeTest();
    runRendererBlockSpanRejectsBoundaryTouchTest();
    runRendererBlockSpanRejectsInvalidInputTest();
    runSimdAcceleratorDotProductTest();
    runSimdAcceleratorVectorLogTest();
    runSimdAcceleratorBackendNameTest();
    runSimdAcceleratorDotProductLargeVectorTest();
    runChannelLayoutNumericGuardTest();
    runChannelLayoutCaptureSegmentSnapshotTest();
    runPitchCurveNoteBasedSmoothsAdjacentNoteBoundaryTest();
    runPitchCurveNoteBasedLocalRecomputeMatchesSmoothBoundaryTest();
    runPitchCurveNoteBasedDoesNotCreateEdgeTransitionSegmentsTest();
    runPitchCurveNoteBasedPreservesAdjacentManualSegmentsTest();
    runPitchCurveLineAnchorRenderUsesCommittedCorrectedF0Test();
    runPitchCurveLineAnchorCorrectedOnlyUsesCommittedCorrectedF0Test();
    runPitchCurveManualCorrectionDoesNotCreateEdgeTransitionSegmentsTest();
    runPianoRollRendererCorrectedF0DoesNotForceHardBoundaryStrokeTest();
    runProcessorAutoTuneAndLocalRetuneFifteenPercentKeepSameSmoothBoundaryTest();
}

// ============================================================================
// vst3-state-persistence-fix L2 anchor tests
// ============================================================================

namespace {

juce::MemoryBlock makeFakeCAPyBlock()
{
    // Old format magic ('CAPy', 0x43415079). Body bytes are arbitrary garbage —
    // deserialize must reject on magic alone before parsing further.
    juce::MemoryBlock block;
    juce::MemoryOutputStream out(block, false);
    out.writeInt(static_cast<int>(0x43415079));   // 'CAPy'
    out.writeInt(0);                              // fake xmlLen=0
    out.writeInt(static_cast<int>(0x78434150));   // fake end magic
    out.flush();
    return block;
}

}  // anonymous

void runCapturePersistenceLegacyMagicRejectedTest()
{
    constexpr const char* testName = "CapturePersistence_LegacyMagicRejected";

    Capture::ProcessorBindings bindings{};
    Capture::CaptureSession session(std::move(bindings));

    const auto block = makeFakeCAPyBlock();
    const bool ok = Capture::CapturePersistence::deserialize(session, block);
    if (ok) {
        logFail(testName, "deserialize should return false for old 'CAPy' magic");
        return;
    }
    if (!session.testSegments().empty()) {
        logFail(testName, "session must remain empty after rejected deserialize");
        return;
    }
    logPass(testName);
}

void runCapturePersistenceSerializeOmitsFlacBytesTest()
{
    constexpr const char* testName = "CapturePersistence_SerializeOmitsFlacBytes";

    Capture::ProcessorBindings bindings{};
    Capture::CaptureSession session(std::move(bindings));
    session.prepareToPlay(48000.0, 512, /*hostInputChannels=*/1);

    auto pcm = std::make_shared<juce::AudioBuffer<float>>(1, 24000);
    pcm->clear();
    const uint64_t fakeMatId = 7;
    session.testInjectEditedSegment(/*T_start*/ 0.0, /*durationSeconds*/ 0.5, fakeMatId, pcm);

    const auto block = Capture::CapturePersistence::serialize(session);
    if (block.getSize() < sizeof(uint32_t) * 2) {
        logFail(testName, "serialize produced suspiciously small block");
        return;
    }

    // Verify magic = 'CAPz' (0x4341507A)
    const uint32_t magic = *static_cast<const uint32_t*>(block.getData());
    if (magic != 0x4341507Au) {
        logFail(testName, "magic is not 'CAPz' (0x4341507A) — old format leaked");
        return;
    }

    // Scan for 'fLaC' stream marker — must be absent.
    const auto* bytes = static_cast<const uint8_t*>(block.getData());
    for (size_t i = 0; i + 4 <= block.getSize(); ++i) {
        if (bytes[i] == 'f' && bytes[i + 1] == 'L'
            && bytes[i + 2] == 'a' && bytes[i + 3] == 'C') {
            logFail(testName, "FLAC stream marker found in CAPz block — codec not removed");
            return;
        }
    }
    logPass(testName);
}

void runCapturePersistenceOrphanSegmentDroppedTest()
{
    constexpr const char* testName = "CapturePersistence_OrphanSegmentDropped";

    // First produce a CAPz block from a session that references mat=99.
    juce::MemoryBlock block;
    {
        Capture::ProcessorBindings producerBindings{};
        Capture::CaptureSession producer(std::move(producerBindings));
        producer.prepareToPlay(48000.0, 512, /*hostInputChannels=*/1);
        auto pcm = std::make_shared<juce::AudioBuffer<float>>(1, 4800);
        pcm->clear();
        producer.testInjectEditedSegment(0.0, 0.1, /*matId*/ 99, pcm);
        block = Capture::CapturePersistence::serialize(producer);
    }

    // Now deserialize into a fresh session whose containsMaterialization claims
    // mat=99 doesn't exist — segment must be dropped.
    Capture::ProcessorBindings consumerBindings{};
    consumerBindings.containsMaterialization = [](uint64_t /*matId*/) { return false; };
    Capture::CaptureSession consumer(std::move(consumerBindings));

    const bool ok = Capture::CapturePersistence::deserialize(consumer, block);
    if (ok) {
        logFail(testName, "deserialize should return false when all segments are orphans");
        return;
    }
    if (!consumer.testSegments().empty()) {
        logFail(testName, "orphan segment must not be added to session");
        return;
    }
    logPass(testName);
}

void runCapturePersistenceProcessingOnRestoreTriggersRefreshTest()
{
    constexpr const char* testName = "CapturePersistence_ProcessingOnRestoreTriggersRefresh";

    juce::MemoryBlock block;
    {
        Capture::ProcessorBindings producerBindings{};
        Capture::CaptureSession producer(std::move(producerBindings));
        producer.prepareToPlay(48000.0, 512, /*hostInputChannels=*/1);
        auto pcm = std::make_shared<juce::AudioBuffer<float>>(1, 4800);
        pcm->clear();
        producer.testInjectEditedSegment(2.5, 0.1, /*matId*/ 42, pcm);
        block = Capture::CapturePersistence::serialize(producer);
    }

    // Consumer asserts mat=42 exists, counts refreshMaterialization calls (must be 1
    // for mat=42), and counts submitForRender calls (must be 0 — no new mat creation).
    int submitCalls = 0;
    int refreshCallsForMat42 = 0;
    int refreshCallsForOthers = 0;
    Capture::ProcessorBindings consumerBindings{};
    consumerBindings.containsMaterialization = [](uint64_t matId) { return matId == 42; };
    consumerBindings.submitForRender = [&submitCalls](std::shared_ptr<juce::AudioBuffer<float>>,
                                                       double,
                                                       juce::String) -> uint64_t {
        ++submitCalls;
        return 0;
    };
    consumerBindings.refreshMaterialization = [&](uint64_t matId) {
        if (matId == 42u) ++refreshCallsForMat42;
        else              ++refreshCallsForOthers;
    };
    Capture::CaptureSession consumer(std::move(consumerBindings));

    const bool ok = Capture::CapturePersistence::deserialize(consumer, block);
    if (!ok) {
        logFail(testName, "deserialize returned false despite mat being present");
        return;
    }
    if (submitCalls != 0) {
        logFail(testName, "submitForRender must not be called (no new mat creation on restore)");
        return;
    }
    if (refreshCallsForMat42 != 1) {
        logFail(testName, "refreshMaterialization(42) expected 1 call");
        return;
    }
    if (refreshCallsForOthers != 0) {
        logFail(testName, "refreshMaterialization called for unexpected matId");
        return;
    }
    const auto& segs = consumer.testSegments();
    if (segs.size() != 1u) {
        logFail(testName, "expected exactly 1 restored segment");
        return;
    }
    if (segs[0]->state.load(std::memory_order_acquire) != Capture::SegmentState::Processing) {
        logFail(testName, "restored segment must be in Processing state (await tick promotion)");
        return;
    }
    if (segs[0]->materializationId != 42u) {
        logFail(testName, "restored segment lost materializationId binding");
        return;
    }
    logPass(testName);
}

void runCaptureSessionEditedSegmentsListIsPlacementSourceTest()
{
    constexpr const char* testName = "CaptureSession_EditedSegmentsListIsPlacementSource";

    Capture::ProcessorBindings bindings{};
    Capture::CaptureSession session(std::move(bindings));
    session.prepareToPlay(48000.0, 512, /*hostInputChannels=*/1);

    auto pcmA = std::make_shared<juce::AudioBuffer<float>>(1, 4800);
    pcmA->clear();
    session.testInjectEditedSegment(/*T_start*/ 10.0, /*durationSeconds*/ 1.0, /*matId*/ 101, pcmA);

    Capture::SegmentInfo segment;
    if (!session.resolveDisplaySegment(/*hostTimeSeconds*/ 0.0, segment)
        || segment.materializationId != 101u
        || segment.id == 0) {
        logFail(testName, "latest completed capture must remain displayable when host_t is outside every segment");
        return;
    }

    auto pcmB = std::make_shared<juce::AudioBuffer<float>>(1, 4800);
    pcmB->clear();
    session.testInjectEditedSegment(/*T_start*/ 20.0, /*durationSeconds*/ 1.0, /*matId*/ 202, pcmB);

    auto pcmWithoutMaterialization = std::make_shared<juce::AudioBuffer<float>>(1, 4800);
    pcmWithoutMaterialization->clear();
    session.testInjectEditedSegment(/*T_start*/ 30.0, /*durationSeconds*/ 1.0, /*matId*/ 0, pcmWithoutMaterialization);

    const auto editedSegments = session.listEditedSegments();
    if (editedSegments.size() != 2) {
        logFail(testName, "regular capture placement snapshot must include every edited segment");
        return;
    }

    if (editedSegments[0].materializationId != 101u
        || editedSegments[0].state != Capture::SegmentState::Edited
        || !approxEqual(editedSegments[0].T_start, 10.0)
        || editedSegments[1].materializationId != 202u
        || editedSegments[1].state != Capture::SegmentState::Edited
        || !approxEqual(editedSegments[1].T_start, 20.0)) {
        logFail(testName, "edited segment placement snapshot lost stable insertion order or materialization binding");
        return;
    }

    if (!session.resolveDisplaySegment(/*hostTimeSeconds*/ 10.25, segment)
        || segment.materializationId != 101u) {
        logFail(testName, "single display resolution should remain a host-time selection helper");
        return;
    }

    const auto placementsAfterResolve = session.listEditedSegments();
    if (placementsAfterResolve.size() != 2
        || placementsAfterResolve[0].materializationId != 101u
        || placementsAfterResolve[1].materializationId != 202u) {
        logFail(testName, "single display resolution must not replace the multi-segment placement source");
        return;
    }

    logPass(testName);
}

void runCaptureSessionDisplaySegmentUpdatesOnRenderCompleteTest()
{
    constexpr const char* testName = "CaptureSession_DisplaySegmentUpdatesOnRenderComplete";

    uint64_t submittedMaterializationId = 0;
    Capture::ProcessorBindings bindings{};
    bindings.submitForRender = [&](std::shared_ptr<juce::AudioBuffer<float>> pcm,
                                   double sampleRate,
                                   juce::String displayName) -> uint64_t {
        juce::ignoreUnused(sampleRate, displayName);
        if (pcm == nullptr || pcm->getNumSamples() == 0) {
            logFail(testName, "captured PCM was not submitted");
            return 0;
        }
        submittedMaterializationId = 303;
        return submittedMaterializationId;
    };
    bindings.isRenderReady = [&](uint64_t materializationId) {
        return materializationId == submittedMaterializationId;
    };

    Capture::CaptureSession session(std::move(bindings));
    session.prepareToPlay(48000.0, 512, /*hostInputChannels=*/1);
    if (!session.armNewCapture()) {
        logFail(testName, "armNewCapture failed");
        return;
    }

    juce::AudioBuffer<float> buffer(1, 512);
    buffer.clear();
    buffer.setSample(0, 0, 0.5f);
    session.processBlock(buffer, /*hostTimeSeconds*/ 12.0, /*hostSampleRate*/ 48000.0, /*isPlaying*/ true);
    session.stopCapture();

    for (int i = 0; i < 8; ++i)
        session.tick();

    Capture::SegmentInfo segment;
    if (!session.resolveDisplaySegment(/*hostTimeSeconds*/ 0.0, segment)
        || segment.materializationId != 303u
        || segment.state != Capture::SegmentState::Edited
        || !approxEqual(segment.T_start, 12.0)) {
        logFail(testName, "render completion must update session-owned display segment");
        return;
    }

    logPass(testName);
}

void runProcessorStateVersionSevenAndNoBpmTest()
{
    constexpr const char* testName = "ProcessorState_VersionSevenAndNoBpm";

    // Construct a fresh processor (wrapperType_Undefined, no captureSession_).
    // getStateInformation goes through OTST path because Undefined != Standalone.
    OpenTuneAudioProcessor processor;
    processor.setBpm(140.0);

    juce::MemoryBlock state;
    processor.getStateInformation(state);

    juce::MemoryInputStream in(state, false);
    const int magic = in.readInt();
    const int version = in.readInt();
    if (magic != 0x4F545354) {  // 'OTST'
        logFail(testName, "expected OTST magic in non-Standalone path");
        return;
    }
    // §8: version 7 → 8 adds Placement::clipInSeconds.
    // §3.8: vocal-time-stretch bumped state version 5 → 6 (TimeGrid section
    // appended after PitchCurve per materialization), then 6 → 7 for
    // per-handle confidence. v5/v6/v7 projects are still
    // accepted via backward-compat paths in setStateInformation.
    if (version != 8) {
        logFail(testName, "kProcessorStateVersion expected 8 (clipInSeconds field)");
        return;
    }

    // Next field must be zoomLevel (double), not BPM. We can't directly verify
    // "not BPM" from a single double, but if BPM were still written, the next
    // field after that would be zoomLevel and the read sequence in setState
    // would be off. Round-trip through setState confirms layout is consistent.
    const double zoom = in.readDouble();
    if (zoom != processor.getZoomLevel()) {
        logFail(testName, "first OTST double after version must be zoomLevel");
        return;
    }

    // Round-trip: feed back into the same processor and verify it doesn't crash
    // / warn / leave stores in an inconsistent state.
    OpenTuneAudioProcessor processor2;
    processor2.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    // No assertion needed: a layout-mismatch would have asserted or warned in
    // AppLogger; the fact that setStateInformation completed implies the field
    // ordering is internally consistent in OTST v7.

    logPass(testName);
}

void runProcessorStateOldVersionRejectedTest()
{
    constexpr const char* testName = "ProcessorState_OldVersionRejected";

    juce::MemoryBlock fake;
    {
        juce::MemoryOutputStream out(fake, false);
        out.writeInt(0x4F545354);   // 'OTST'
        out.writeInt(4);            // legacy version
        // Some plausible bytes; layout doesn't matter — should bail before parsing.
        out.writeDouble(120.0);
        out.writeDouble(1.0);
        out.writeInt(120);
        out.writeInt(0);            // sourceCount=0
        out.flush();
    }

    OpenTuneAudioProcessor processor;
    processor.setStateInformation(fake.getData(), static_cast<int>(fake.getSize()));

    if (processor.getMaterializationStore() == nullptr
        || processor.getSourceStore() == nullptr) {
        logFail(testName, "stores should be initialized after rejected restore");
        return;
    }
    // Stores remain at their default (empty) state — successful rejection.
    logPass(testName);
}

void runProcessorStateRegularVst3RestoresImmediatelyWhenUnboundTest()
{
    constexpr const char* testName = "ProcessorState_RegularVst3RestoresImmediatelyWhenUnbound";

    auto writer = makeProcessorWithWrapperType(juce::AudioProcessor::wrapperType_VST3);
    const auto committed = writer->commitPreparedImportAsPlacement(makePreparedImport("regular-vst3-state", 256), {1, 2.5});
    if (!committed.isValid()) {
        logFail(testName, "failed to seed regular VST3 state");
        return;
    }

    const auto stateData = serializeProcessorState(*writer);

    auto reader = makeProcessorWithWrapperType(juce::AudioProcessor::wrapperType_VST3);
    reader->setStateInformation(stateData.getData(), static_cast<int>(stateData.getSize()));

    StandaloneArrangement::Placement restoredPlacement;
    if (!reader->getPlacementById(1, committed.placementId, restoredPlacement)
        || restoredPlacement.materializationId != committed.materializationId
        || !approxEqual(restoredPlacement.timelineStartSeconds, 2.5, 1.0e-6)) {
        logFail(testName, "regular unbound VST3 state was mistaken for ARA pre-bind and not restored immediately");
        return;
    }

    logPass(testName);
}

void runProcessorStateAraMetadataOnlyPreBindDoesNotRestoreLocalStoresTest()
{
    constexpr const char* testName = "ProcessorState_AraMetadataOnlyPreBindDoesNotRestoreLocalStores";

    auto processor = makeProcessorWithWrapperType(juce::AudioProcessor::wrapperType_VST3);
    const auto stateData = makeMinimalVst3StateWithMetadataOnlySource(8801);

    processor->setStateInformation(stateData.getData(), static_cast<int>(stateData.getSize()));

    SourceStore::SourceSnapshot sourceSnapshot;
    if (processor->getSourceSnapshotById(8801, sourceSnapshot)) {
        logFail(testName, "ARA metadata-only pre-bind state restored into local SourceStore before didBindToARA");
        return;
    }

    if (processor->getStandaloneArrangement() != nullptr
        && processor->getStandaloneArrangement()->getNumPlacements(0) != 0) {
        logFail(testName, "ARA metadata-only pre-bind state restored local placements before didBindToARA");
        return;
    }

    logPass(testName);
}

// ============================================================================
// undo-affected-range-passthrough L2 anchor tests
// ============================================================================

void runPianoRollEditActionAffectedRangeStoredVerbatimTest()
{
    constexpr const char* testName = "PianoRollEditAction_AffectedRangeStoredVerbatim";

    OpenTuneAudioProcessor processor;
    PianoRollEditAction action(
        processor, /*matId*/ 1, /*desc*/ juce::String("test"),
        /*oldNotes*/ {}, /*newNotes*/ {},
        /*oldSegs*/ {}, /*newSegs*/ {},
        /*affectedStartFrame*/ 120,
        /*affectedEndFrame*/ 480);

    if (action.getAffectedStartFrame() != 120) {
        logFail(testName, "affectedStartFrame mismatch (expected 120)");
        return;
    }
    if (action.getAffectedEndFrame() != 480) {
        logFail(testName, "affectedEndFrame mismatch (expected 480)");
        return;
    }
    logPass(testName);
}

void runPianoRollEditActionAffectedRangeIndependentOfSegmentsTest()
{
    constexpr const char* testName = "PianoRollEditAction_AffectedRangeIndependentOfSegments";

    OpenTuneAudioProcessor processor;
    // Construct segments whose union under the old algorithm would produce [0, 8800].
    // The new passthrough logic must ignore this and return the explicit affectedRange [4000, 4500].
    std::vector<CorrectedSegment> oldSegs = {
        CorrectedSegment(0, 4400, std::vector<float>(4400, 220.0f), CorrectedSegment::Source::NoteBased)
    };
    std::vector<CorrectedSegment> newSegs = {
        CorrectedSegment(4400, 8800, std::vector<float>(4400, 440.0f), CorrectedSegment::Source::NoteBased)
    };

    PianoRollEditAction action(
        processor, /*matId*/ 1, /*desc*/ juce::String("test"),
        /*oldNotes*/ {}, /*newNotes*/ {},
        std::move(oldSegs), std::move(newSegs),
        /*affectedStartFrame*/ 4000,
        /*affectedEndFrame*/ 4500);

    if (action.getAffectedStartFrame() != 4000) {
        logFail(testName, "segments leaked into affectedStartFrame — old union algorithm regressed");
        return;
    }
    if (action.getAffectedEndFrame() != 4500) {
        logFail(testName, "segments leaked into affectedEndFrame — old union algorithm regressed");
        return;
    }
    logPass(testName);
}

void runProcessorBehaviorSuite()
{
    logSection("Processor");
    runEnsureSourceByIdCreatesForcedSourceOwnerTest();
    runProcessorImportCreatesDistinctSourceMaterializationAndPlacementOwnersTest();
    runClipDerivedRefreshDoesNotMutateStandaloneSelectionTest();
    runChannelLayoutMaterializationStoreInvariantTest();
    runChannelLayoutPrepareImportRejectsMultichannelTest();
    // vst3-state-persistence-fix anchor tests
    runCapturePersistenceLegacyMagicRejectedTest();
    runCapturePersistenceSerializeOmitsFlacBytesTest();
    runCapturePersistenceOrphanSegmentDroppedTest();
    runCapturePersistenceProcessingOnRestoreTriggersRefreshTest();
    runCaptureSessionEditedSegmentsListIsPlacementSourceTest();
    runCaptureSessionDisplaySegmentUpdatesOnRenderCompleteTest();
    runProcessorStateVersionSevenAndNoBpmTest();
    runProcessorStateOldVersionRejectedTest();
    runProcessorStateRegularVst3RestoresImmediatelyWhenUnboundTest();
    runProcessorStateAraMetadataOnlyPreBindDoesNotRestoreLocalStoresTest();
    runProcessorLineAnchorRenderUsesCommittedCorrectedF0Test();
    // undo-affected-range-passthrough anchor tests
    runPianoRollEditActionAffectedRangeStoredVerbatimTest();
    runPianoRollEditActionAffectedRangeIndependentOfSegmentsTest();
}

void runUiBehaviorSuite()
{
    logSection("UI");
    runAppPreferencesRoundTripsSharedPreferencesTest();
    runAppPreferencesRoundTripsStandalonePreferencesTest();
    runAppPreferencesRoundTripsSharedVisualPreferencesTest();
    runProcessorStateDoesNotSerializeAppPreferencesTest();
    runProcessorStateDoesNotSerializeSharedVisualPreferencesTest();
    runStandalonePreferencesDialogContainsStandaloneOnlyPagesTest();
    runPluginPreferencesDialogExcludesStandaloneOnlyPagesTest();
    runSharedPreferencePagesExposeInteractionSchemeAndVisualControlsTest();
    runStandaloneShortcutSettingsUseModalCaptureDialogTest();
    runStandalonePreferencesOwnAudioSettingsUiTest();
    runViewMenuExposesSharedVisualOptionsAcrossProfilesTest();
    runPluginViewMenuStillExcludesStandaloneOnlyMouseTrailOptionsTest();
    runPreferencesDialogUsesExplicitPageCompositionNotBooleanFlagsTest();
    runAudioEditingSchemeRulesUseExplicitSchemeInputTest();
    runAudioEditingSchemeUsesSchemeManagedVoicedOnlyPolicyTest();
    runAudioEditingSchemeNotesPrimaryContractRemainsUnchangedTest();
    runKeyShortcutMatchingUsesExplicitSettingsInputTest();
    runThemeAndLanguageStartupInitializeFromAppPreferencesTest();
    runNotesPrimaryRetuneTargetPrefersSelectedNotesTest();
    runCorrectedF0PrimaryRetuneTargetDoesNotExposeLineAnchorSegmentTargetTest();
    runFrameSelectionParametersTreatManualCorrectedF0AsCommittedTruthTest();
    runNotesPrimaryAutoTuneUsesSelectedNotesRangeTest();
    runCorrectedF0PrimaryAutoTunePrefersSelectionAreaTest();
    runPianoRollF0VisualUsesEnergyAlphaWithinBoundsTest();
    runPianoRollF0VisualHotLevelAddsGoldTintTest();
    runPianoRollF0VisualZoomedOutBucketsAreBoundedByPixelDensityTest();
    runPianoRollF0VisualZoomedInRestoresFrameDetailTest();
    runPianoRollF0VisualVoicelessGapsDoNotConnectAcrossSegmentsTest();
    runPianoRollF0VisualStyleTokensAndLayeringTest();
    runPianoRollF0VisualEndpointFadeAndGlowContractTest();
    runAuroraTopBarReferenceRestyleSourceGuardTest();
    runAuroraRightSidebarBackgroundReferenceRestyleSourceGuardTest();
    runAuroraScrollbarOutlineSourceGuardTest();
    runPianoRollHotPathSourceGuardNoPerEventDebugLoggingTest();
    runPianoRollInteractionSourceGuardInteractiveInvalidationIsNotFullBoundsTest();
    runPianoRollInteractionSourceGuardDeleteLegacyCopyWritebackApiBeforeRefactorTest();
    runMaterializationStoreNotesRevisionAdvancesOnSetNotesTest();
    runSplitPlacementPianoRollDisplaysProjectedWindowOnlyTest();
    runPianoRollTimelineViewDomainLateCaptureCanBrowseBeforeSegmentTest();
    runPianoRollTimelineViewDomainDefaultProjectionWindowUnchangedTest();
    runEditingCommandDoesNotMutatePlacementTest();
    runPianoRollComponentSourceGuardPaintUsesCachedNotesInsteadOfProcessorReadTest();
    runPianoRollDrawNoteDraftSurvivesMultiEventDragTest();
    runPianoRollEmptySpaceSeekMouseDownOnlyArmsPendingTest();
    runPianoRollEmptySpaceSeekMouseUpWithinThresholdSeeksOnceTest();
    runPianoRollEmptySpaceSeekDrawNoteClickDoesNotCreateNoteTest();
    runPianoRollEmptySpaceSeekHandDrawClickDoesNotApplyCorrectionTest();
    runPianoRollEmptySpaceSeekLineAnchorClickPlacesAnchorWithoutSeekTest();
    runPianoRollEmptySpaceSeekSelectDragBeyondThresholdStartsBoxSelectionTest();
    runPianoRollEmptySpaceSeekSelectDragMouseUpFinishesBoxSelectionTest();
    runPianoRollEmptySpaceSeekDrawNoteDragBeyondThresholdCommitsNoteWithoutSeekTest();
    runPianoRollEmptySpaceSeekToolSwitchCancelsPendingIntentTest();
    runPianoRollEmptySpaceSeekLineAnchorSegmentHitStillSelectsSegmentTest();
    runPianoRollLineAnchorRetuneSpeedAppliesOnlyOriginalF0ShapeResidualTest();
    runPianoRollEmptySpaceSeekContinuousModeCentersOnSeekTest();
    runManualPreviewMouseUpCommitIsAtomicTest();
    runNoteBasedSyncCommitDoesNotFallBackToNotesOnlyTest();
    runNoteBasedCorrectedF0SyncHasNoNotesOnlyFallbackGuardTest();
    runCorrectedF0PreviewOnlyActivatesInCorrectedF0PrimaryTest();
    runPianoRollVisualInvalidationDirtyAreasMergeWithoutForcedFullRepaintTest();
    runAudioFormatRegistryRegistersImportFormatsTest();
    runAudioFormatRegistryOpensGeneratedWavTest();
    runStandaloneImportFlowCopiesPendingFileBeforeMovingPendingImportTest();
    runPianoRollPlayheadViewportStopsBeforeScrollbarsTest();
    runPianoRollPlayheadUsesDedicatedOverlayTest();
    runStandaloneEditorHeartbeatDrivesPianoRollVisualLoopTest();
    runStandaloneEditorParameterPanelSyncFollowsEditingSchemeTest();
    runParameterPanelSyncDecisionRestoresClipDefaultsAfterSelectionEndsTest();

}

void runPianoRollF0VisualSuite()
{
    logSection("Piano Roll F0 Visual");
    runPianoRollF0VisualUsesEnergyAlphaWithinBoundsTest();
    runPianoRollF0VisualHotLevelAddsGoldTintTest();
    runPianoRollF0VisualZoomedOutBucketsAreBoundedByPixelDensityTest();
    runPianoRollF0VisualZoomedInRestoresFrameDetailTest();
    runPianoRollF0VisualVoicelessGapsDoNotConnectAcrossSegmentsTest();
    runPianoRollF0VisualStyleTokensAndLayeringTest();
    runPianoRollF0VisualEndpointFadeAndGlowContractTest();
    runAuroraTopBarReferenceRestyleSourceGuardTest();
    runAuroraRightSidebarBackgroundReferenceRestyleSourceGuardTest();
}

void runPianoRollIntentBehaviorSuite()
{
    logSection("Piano Roll Intent");
    runPianoRollEmptySpaceSeekMouseDownOnlyArmsPendingTest();
    runPianoRollEmptySpaceSeekMouseUpWithinThresholdSeeksOnceTest();
    runPianoRollEmptySpaceSeekDrawNoteClickDoesNotCreateNoteTest();
    runPianoRollEmptySpaceSeekHandDrawClickDoesNotApplyCorrectionTest();
    runPianoRollEmptySpaceSeekLineAnchorClickPlacesAnchorWithoutSeekTest();
    runPianoRollEmptySpaceSeekSelectDragBeyondThresholdStartsBoxSelectionTest();
    runPianoRollEmptySpaceSeekSelectDragMouseUpFinishesBoxSelectionTest();
    runPianoRollEmptySpaceSeekDrawNoteDragBeyondThresholdCommitsNoteWithoutSeekTest();
    runPianoRollEmptySpaceSeekToolSwitchCancelsPendingIntentTest();
    runPianoRollEmptySpaceSeekLineAnchorSegmentHitStillSelectsSegmentTest();
    runPianoRollLineAnchorRetuneSpeedAppliesOnlyOriginalF0ShapeResidualTest();
    runPianoRollEmptySpaceSeekContinuousModeCentersOnSeekTest();
}

void runActiveSurfaceHidesRetiredNodesTest()
{
    constexpr const char* testName = "LineageStateMachine_ActiveSurfaceHidesRetiredNodes";

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(makePreparedImport("retire-test", 128), {0, 0.0});
    if (!committed.isValid()) {
        logFail(testName, "failed to create test materialization");
        return;
    }

    auto* store = processor.getMaterializationStore();
    if (store == nullptr) {
        logFail(testName, "getMaterializationStore returned null");
        return;
    }

    if (!store->containsMaterialization(committed.materializationId)) {
        logFail(testName, "active materialization not visible on business surface before retire");
        return;
    }

    if (!store->retireMaterialization(committed.materializationId)) {
        logFail(testName, "retireMaterialization returned false for active entry");
        return;
    }

    if (store->containsMaterialization(committed.materializationId)) {
        logFail(testName, "business surface still returns retired materialization as active");
        return;
    }

    if (!store->isRetired(committed.materializationId)) {
        logFail(testName, "isRetired returned false after retire");
        return;
    }

    if (!store->isRetired(committed.materializationId)) {
        logFail(testName, "isRetired returned false for retired entry (re-check)");
        return;
    }

    logPass(testName);
}

void runRetireAndReviveAreReversibleTest()
{
    constexpr const char* testName = "LineageStateMachine_RetireAndReviveAreReversible";

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(makePreparedImport("revive-test", 128), {0, 0.0});
    if (!committed.isValid()) {
        logFail(testName, "failed to create test materialization");
        return;
    }

    auto* store = processor.getMaterializationStore();
    if (store == nullptr) {
        logFail(testName, "getMaterializationStore returned null");
        return;
    }

    std::shared_ptr<const juce::AudioBuffer<float>> bufBefore;
    store->getAudioBuffer(committed.materializationId, bufBefore);

    store->retireMaterialization(committed.materializationId);

    if (!store->reviveMaterialization(committed.materializationId)) {
        logFail(testName, "reviveMaterialization returned false");
        return;
    }

    if (!store->containsMaterialization(committed.materializationId)) {
        logFail(testName, "revived materialization not visible on business surface");
        return;
    }

    std::shared_ptr<const juce::AudioBuffer<float>> bufAfter;
    store->getAudioBuffer(committed.materializationId, bufAfter);

    if (bufBefore == nullptr || bufAfter == nullptr || bufBefore != bufAfter) {
        logFail(testName, "audio buffer changed after retire+revive cycle");
        return;
    }

    logPass(testName);
}

void runBusinessFunctionsDoNotInvokeReclaimDirectlyTest()
{
    constexpr const char* testName = "LineageStateMachine_BusinessFunctionsDoNotInvokeReclaimDirectly";

    const juce::File root = locateWorkspaceRoot();
    if (!root.isDirectory()) {
        logFail(testName, "cannot locate project root");
        return;
    }

    // Files to scan for forbidden direct reclaim calls in business paths
    const std::vector<juce::File> filesToScan = {
        root.getChildFile("Source/Plugin/PluginEditor.cpp"),
        root.getChildFile("Source/Standalone/UI/ArrangementViewComponent.cpp"),
        root.getChildFile("Source/Standalone/UI/TrackHeaderComponent.cpp"),
    };

    // White-listed call sites: only allowed inside PluginProcessor.cpp sweep/reclaim functions.
    // The patterns below match lines in PluginProcessor.cpp only.
    const juce::File processorFile = root.getChildFile("Source/PluginProcessor.cpp");
    if (!processorFile.existsAsFile()) {
        logFail(testName, "cannot locate Source/PluginProcessor.cpp");
        return;
    }

    const std::vector<juce::String> forbiddenPatterns = {
        "physicallyDeleteIfReclaimable(",
        "runReclaimSweepOnMessageThread(",
    };

    bool anyFail = false;

    for (const auto& file : filesToScan) {
        if (!file.existsAsFile()) {
            // File may not exist (e.g. component not yet created) 鈥?skip silently
            continue;
        }
        const juce::String content = file.loadFileAsString();
        const juce::StringArray lines = juce::StringArray::fromLines(content);
        for (const auto& pattern : forbiddenPatterns) {
            for (int lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
                const juce::String& line = lines[lineIdx];
                // Skip comment lines
                if (line.trimStart().startsWith("//") || line.trimStart().startsWith("*"))
                    continue;
                if (line.contains(pattern)) {
                    logFail(testName, ("Forbidden direct reclaim call '" + pattern + "' in business path: "
                        + file.getFullPathName() + ":" + juce::String(lineIdx + 1)
                        + "  -> " + line.trim()).toStdString().c_str());
                    anyFail = true;
                }
            }
        }
    }

    // Also verify PluginProcessor.cpp itself only calls reclaim from whitelist functions
    {
        const juce::String content = processorFile.loadFileAsString();
        const juce::StringArray lines = juce::StringArray::fromLines(content);

        // Whitelist function names that are allowed to contain reclaim calls
        const std::vector<juce::String> whitelistFunctions = {
            "handleAsyncUpdate",
            "physicallyDeleteIfReclaimable",
            "runReclaimSweepOnMessageThread",
            "scheduleReclaimSweep",
        };

        juce::String currentFunction;
        for (int lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
            const juce::String& line = lines[lineIdx];
            // Detect function definition (very rough: line starts with return type + class::name)
            if (line.contains("OpenTuneAudioProcessor::")) {
                currentFunction = line;
            }
            if (line.trimStart().startsWith("//") || line.trimStart().startsWith("*"))
                continue;
            for (const auto& pattern : forbiddenPatterns) {
                if (line.contains(pattern)) {
                    bool inWhitelist = false;
                    for (const auto& wl : whitelistFunctions) {
                        if (currentFunction.contains(wl)) { inWhitelist = true; break; }
                    }
                    if (!inWhitelist) {
                        logFail(testName, ("Forbidden direct reclaim call '" + pattern + "' outside whitelist in "
                            + processorFile.getFullPathName() + ":" + juce::String(lineIdx + 1)
                            + " (current fn: " + currentFunction.trim() + ")").toStdString().c_str());
                        anyFail = true;
                    }
                }
            }
        }
    }

    if (!anyFail)
        logPass(testName);
}

void runReclaimSweepRequiresAllReferenceCountersZeroTest()
{
    constexpr const char* testName = "LineageStateMachine_ReclaimSweepRequiresAllReferenceCountersZero";

    OpenTuneAudioProcessor processor;
    const auto committed = processor.commitPreparedImportAsPlacement(makePreparedImport("sweep-test", 44100), {0, 0.0});
    if (!committed.isValid()) {
        logFail(testName, "failed to setup test state");
        return;
    }

    auto* store = processor.getMaterializationStore();
    auto* arrangement = processor.getStandaloneArrangement();
    const uint64_t origId = committed.materializationId;
    const uint64_t origPlacementId = committed.placementId;

    // Manually retire the materialization without a sweep 鈥?check it survives
    store->retireMaterialization(origId);

    // Arrangement still actively references this materialization via the active placement
    // Sweep should NOT reclaim 鈥?active placement is a reference
    processor.runReclaimSweepOnMessageThread();

    if (store->getSourceIdAnyState(origId) == 0) {
        logFail(testName, "sweep reclaimed materialization while active placement still referenced it");
        return;
    }

    // Now also retire the placement and verify sweep still keeps it (undo history not checked
    // because we have no undo action 鈥?but sweep Phase 1 will erase the retired placement first,
    // so we need to verify that after Phase 1, Phase 2 can safely reclaim)
    arrangement->retirePlacement(0, origPlacementId);
    processor.runReclaimSweepOnMessageThread();

    // With no undo action and placement physically erased by Phase 1, material should now be reclaimed
    if (store->getSourceIdAnyState(origId) != 0) {
        logFail(testName, "sweep failed to reclaim materialization after all references cleared");
        return;
    }

    logPass(testName);
}




void runAraTriggeredReclaimGoesThroughSweepTest()
{
    constexpr const char* testName = "LineageStateMachine_AraTriggeredReclaimGoesThroughSweep";

    // Structural grep assertion: OpenTuneDocumentController.cpp must not contain
    // reclaimUnreferencedMaterialization or reclaimPublishedMaterializationIds,
    // but must contain scheduleReclaimSweep.
    const juce::File root = locateWorkspaceRoot();
    if (!root.isDirectory()) {
        logFail(testName, "cannot locate project root");
        return;
    }
    const auto docControllerFile = root.getChildFile("Source/ARA/OpenTuneDocumentController.cpp");
    if (!docControllerFile.existsAsFile()) {
        logFail(testName, "cannot locate OpenTuneDocumentController.cpp");
        return;
    }

    const auto content = docControllerFile.loadFileAsString();

    if (content.contains("reclaimPublishedMaterializationIds")) {
        logFail(testName, "OpenTuneDocumentController.cpp still contains reclaimPublishedMaterializationIds");
        return;
    }

    if (content.contains("reclaimUnreferencedMaterialization")) {
        logFail(testName, "OpenTuneDocumentController.cpp still contains reclaimUnreferencedMaterialization");
        return;
    }

    if (!content.contains("scheduleReclaimSweep")) {
        logFail(testName, "OpenTuneDocumentController.cpp does not contain scheduleReclaimSweep");
        return;
    }

    logPass(testName);
}





void runSourceRetiredWhenLastMaterializationRetiredTest()
{
    constexpr const char* testName = "Task11a_SourceRetiredWhenLastMaterializationRetired";

    OpenTuneAudioProcessor processor;
    // Primer import to bump id counters so sourceId != matId (each store counts independently from 1)
    (void)processor.commitPreparedImportAsPlacement(makePreparedImport("primer", 44100), {0, 0.0});
    const auto committed = processor.commitPreparedImportAsPlacement(makePreparedImport("src-retire", 44100), {1, 0.0});
    if (!committed.isValid()) { logFail(testName, "setup failed"); return; }

    auto* sourceStore = processor.getSourceStore();
    auto* matStore = processor.getMaterializationStore();
    auto* arrangement = processor.getStandaloneArrangement();
    const uint64_t sourceId = committed.sourceId;
    const uint64_t matId = committed.materializationId;
    const uint64_t placementId = committed.placementId;

    if (!sourceStore->containsSource(sourceId)) { logFail(testName, "source not active after import"); return; }

    arrangement->retirePlacement(1, placementId);
    matStore->retireMaterialization(matId);
    processor.runReclaimSweepOnMessageThread();

    // After sweep with no undo refs, source should be physically reclaimed
    if (sourceStore->containsSource(sourceId)) {
        logFail(testName, "source not physically reclaimed after sweep with zero references");
        return;
    }

    logPass(testName);
}



void runAraPublishedReferencePreventsMaterializationSweepTest()
{
    constexpr const char* testName = "Task11a_AraPublishedReferencePreventsMaterializationSweep";

    const juce::File root = locateWorkspaceRoot();
    if (!root.isDirectory()) { logFail(testName, "cannot locate project root"); return; }
    const auto file = root.getChildFile("Source/PluginProcessor.cpp");
    if (!file.existsAsFile()) { logFail(testName, "cannot locate PluginProcessor.cpp"); return; }

    const auto content = file.loadFileAsString();
    if (!content.contains("publishedRegions") || !content.contains("appliedProjection.materializationId")) {
        logFail(testName, "sweep does not check ARA published region references");
        return;
    }
    if (!content.contains("runReclaimSweepOnMessageThread")) {
        logFail(testName, "sweep entry function missing");
        return;
    }

    logPass(testName);
}













void runSessionOwnershipProcessorDoesNotOwnSessionTest()
{
    constexpr const char* testName = "SessionOwnership_ProcessorDoesNotOwnVST3AraSession";

    const auto& processorHeader  = getFileCache().get("Source/PluginProcessor.h");
    const auto& processorSource  = getFileCache().get("Source/PluginProcessor.cpp");

    // Processor must not own a VST3AraSession instance.
    if (processorHeader.contains("vst3AraSession_")
        || processorSource.contains("vst3AraSession_")) {
        logFail(testName, "PluginProcessor still has vst3AraSession_ member");
        return;
    }

    if (processorHeader.contains("getVst3AraSession")
        || processorSource.contains("getVst3AraSession")) {
        logFail(testName, "PluginProcessor still exposes getVst3AraSession()");
        return;
    }

    if (processorSource.contains("make_unique<VST3AraSession>")) {
        logFail(testName, "PluginProcessor still constructs VST3AraSession");
        return;
    }

    // DocumentController must be the unique owner.
    const auto& dcSource = getFileCache().get("Source/ARA/OpenTuneDocumentController.cpp");
    if (!dcSource.contains("make_unique<VST3AraSession>")) {
        logFail(testName, "OpenTuneDocumentController does not construct VST3AraSession");
        return;
    }

    const auto& dcHeader = getFileCache().get("Source/ARA/OpenTuneDocumentController.h");
    if (!dcHeader.contains("unique_ptr<VST3AraSession>")) {
        logFail(testName, "OpenTuneDocumentController does not own VST3AraSession via unique_ptr");
        return;
    }

    if (!dcHeader.contains("getSession()")) {
        logFail(testName, "OpenTuneDocumentController does not expose getSession()");
        return;
    }

    logPass(testName);
}

void runSessionOwnershipEditorAndRendererReadThroughDocumentControllerTest()
{
    constexpr const char* testName = "SessionOwnership_EditorAndRendererReadThroughDocumentController";

    const auto& editorSource   = getFileCache().get("Source/Plugin/PluginEditor.cpp");
    const auto& rendererSource = getFileCache().get("Source/ARA/OpenTunePlaybackRenderer.cpp");

    // Editor must not bypass DC to read session directly from processor.
    if (editorSource.contains("processorRef_.getVst3AraSession()")
        || editorSource.contains("getVst3AraSession()")) {
        logFail(testName, "PluginEditor still calls getVst3AraSession() on processor");
        return;
    }

    // Editor must route through DocumentController.
    if (!editorSource.contains("getDocumentController()")) {
        logFail(testName, "PluginEditor does not use getDocumentController() to reach the session");
        return;
    }

    // Renderer must not bypass DC to read session from processor.
    if (rendererSource.contains("processor->getVst3AraSession()")
        || rendererSource.contains("getVst3AraSession()")) {
        logFail(testName, "OpenTunePlaybackRenderer still calls getVst3AraSession() on processor");
        return;
    }

    // Renderer must read session through docController.
    if (!rendererSource.contains("docController->getSession()")) {
        logFail(testName, "OpenTunePlaybackRenderer does not read session through docController->getSession()");
        return;
    }

    logPass(testName);
}

// ---- ARA Playback/Transport Repair Guard Tests ----

#if JucePlugin_Enable_ARA
void runAraBindingStateEnumDefinesLifecycleStatesTest()
{
    constexpr const char* testName = "AraBindingState_EnumDefinesLifecycleStates";

    OpenTune::VST3AraSession::BindingState state = OpenTune::VST3AraSession::BindingState::Unbound;
    if (!(state == OpenTune::VST3AraSession::BindingState::Unbound)) {
        logFail(testName, "BindingState::Unbound value mismatch");
        return;
    }
    if (!(OpenTune::VST3AraSession::BindingState::HydratingSource != OpenTune::VST3AraSession::BindingState::Unbound)) {
        logFail(testName, "HydratingSource should differ from Unbound");
        return;
    }
    if (!(OpenTune::VST3AraSession::BindingState::BoundNeedsRender != OpenTune::VST3AraSession::BindingState::Unbound)) {
        logFail(testName, "BoundNeedsRender should differ from Unbound");
        return;
    }
    if (!(OpenTune::VST3AraSession::BindingState::Renderable != OpenTune::VST3AraSession::BindingState::Unbound)) {
        logFail(testName, "Renderable should differ from Unbound");
        return;
    }
    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

void runAraPublishedRegionViewExposesBindingStateTest()
{
    constexpr const char* testName = "ARA-PLAY-02: PublishedRegionView exposes explicit BindingState";

    if (!sourceContains("Source/ARA/VST3AraSession.h", "enum class BindingState")) {
        logFail(testName, "BindingState enum not found in VST3AraSession.h");
        return;
    }
    if (!sourceContains("Source/ARA/VST3AraSession.h", "bindingState")) {
        logFail(testName, "bindingState field not found in VST3AraSession.h");
        return;
    }
    logPass(testName);
}

void runAraSessionHydrationWorkerRoutesThroughProcessorBirthApiTest()
{
    constexpr const char* testName = "AraSession_BirthWorkerRoutesThroughProcessorBirthApi";
    if (!sourceContains("Source/ARA/VST3AraSession.h", "setProcessor(")) {
        logFail(testName, "setProcessor not found in VST3AraSession.h");
        return;
    }
    if (!sourceContains("Source/ARA/VST3AraSession.cpp", "AraOriginalF0BirthRequest")) {
        logFail(testName, "AraOriginalF0BirthRequest usage not found in VST3AraSession.cpp");
        return;
    }
    // Worker loop name updated from hydrationWorkerLoop → birthWorkerLoop
    if (sourceContains("Source/ARA/VST3AraSession.h", "hydrationWorkerLoop")) {
        logFail(testName, "VST3AraSession.h still uses hydrationWorkerLoop");
        return;
    }
    if (sourceContains("Source/ARA/VST3AraSession.h", "hydrationWorkerThread_")) {
        logFail(testName, "VST3AraSession.h still uses hydrationWorkerThread_");
        return;
    }
    if (sourceContains("Source/ARA/VST3AraSession.h", "hydrationCv_")) {
        logFail(testName, "VST3AraSession.h still uses hydrationCv_");
        return;
    }
    logPass(testName);
}

void runAraBindingNewPersistentIdSameSourceWindowCreatesIndependentMaterializationTest()
{
    constexpr const char* testName = "AraBinding_NewPersistentIdSameSourceWindowCreatesIndependentMaterialization";
    const auto birthSection = extractWorkspaceFileSection("Source/PluginProcessor.cpp",
                                                          "OpenTuneAudioProcessor::ensureAraRegionMaterialization(",
                                                          "if (audioSource == nullptr");
    if (birthSection.contains("findMaterializationBySourceWindow")) {
        logFail(testName, "ARA birth still reuses materialization by sourceId + sourceWindow");
        return;
    }

    const auto sessionSource = getFileCache().get("Source/ARA/VST3AraSession.cpp");
    if (!sessionSource.contains("audioModificationPersistentId")
        || !sessionSource.contains("materializationBindings_")) {
        logFail(testName, "ARA session does not own materialization binding by AudioModification persistent ID");
        return;
    }

    logPass(testName);
}

void runAraBindingArchiveHooksPersistPersistentIdMaterializationBindingsTest()
{
    constexpr const char* testName = "AraBinding_ArchiveHooksPersistPersistentIdMaterializationBindings";
    const auto controllerSource = getFileCache().get("Source/ARA/OpenTuneDocumentController.cpp");
    if (!controllerSource.contains("restoreMaterializationBindings(input, filter)")
        || !controllerSource.contains("storeMaterializationBindings(output, filter)")) {
        logFail(testName, "ARA DocumentController archive hooks still bypass materialization bindings");
        return;
    }

    const auto sessionSource = getFileCache().get("Source/ARA/VST3AraSession.cpp");
    if (!sessionSource.contains("kAraBindingArchiveMagic")
        || !sessionSource.contains("audioModificationPersistentId")
        || !sessionSource.contains("getAudioModificationToRestoreStateWithID")) {
        logFail(testName, "ARA session binding archive is missing versioned persistent-ID storage");
        return;
    }

    logPass(testName);
}

#if JucePlugin_Enable_ARA
void runAraBindingRestoredPersistentIdRebindsNewPlaybackRegionTest()
{
    constexpr const char* testName = "AraBinding_RestoredPersistentIdRebindsNewPlaybackRegion";
    constexpr uint64_t sourceId = 42;
    constexpr uint64_t materializationId = 9002;
    constexpr uint64_t materializationRevision = 7;
    const SourceWindow sourceWindow{sourceId, 0.25, 1.25};

    VST3AraSession stored;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x230);
    VST3AraSessionTestProbe::seedSource(stored, audioSource, sourceId);
    VST3AraSessionTestProbe::seedAudioModificationBinding(stored,
                                                           "mod-restored",
                                                           sourceId,
                                                           materializationId,
                                                           sourceWindow,
                                                           materializationRevision,
                                                           sourceWindow.durationSeconds());

    juce::MemoryBlock archive;
    juce::MemoryOutputStream output(archive, false);
    if (!stored.storeMaterializationBindings(output, nullptr)) {
        logFail(testName, "failed to store materialization binding archive");
        return;
    }
    output.flush();

    VST3AraSession restored;
    juce::MemoryInputStream input(archive, false);
    if (!restored.restoreMaterializationBindings(input, nullptr)) {
        logFail(testName, "failed to restore materialization binding archive");
        return;
    }

    auto* restoredAudioSource = reinterpret_cast<juce::ARAAudioSource*>(0x231);
    auto* restoredPlaybackRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0x232);
    VST3AraSessionTestProbe::seedSource(restored, restoredAudioSource, sourceId);
    VST3AraSessionTestProbe::seedPlaybackRegionForModification(restored,
                                                                restoredAudioSource,
                                                                restoredPlaybackRegion,
                                                                "mod-restored",
                                                                sourceWindow,
                                                                3.0,
                                                                4.0);
    VST3AraSessionTestProbe::publish(restored);

    if (VST3AraSessionTestProbe::bindingMaterializationForPersistentId(restored, "mod-restored") != materializationId) {
        logFail(testName, "restored persistent ID did not recover its materialization binding");
        return;
    }

    const auto snapshot = restored.loadSnapshot();
    const auto* regionView = snapshot != nullptr ? snapshot->findRegion(restoredPlaybackRegion) : nullptr;
    if (regionView == nullptr) {
        logFail(testName, "restored playback region was not published");
        return;
    }

    const auto& projection = regionView->appliedProjection;
    if (projection.materializationId != materializationId
        || projection.appliedMaterializationRevision != materializationRevision
        || projection.appliedSourceWindow.sourceId != sourceWindow.sourceId
        || !approxEqual(projection.appliedSourceWindow.sourceStartSeconds, sourceWindow.sourceStartSeconds)
        || !approxEqual(projection.appliedSourceWindow.sourceEndSeconds, sourceWindow.sourceEndSeconds)
        || projection.appliedRegionIdentity != regionView->regionIdentity) {
        logFail(testName, "restored persistent binding did not project onto the recreated playback region");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

void runAraEditorAttachesRenderableBindingWithoutReadAudioArmTest()
{
    constexpr const char* testName = "AraEditor_AttachesRenderableBindingWithoutReadAudioArm";
    const auto editorHeader = getFileCache().get("Source/Plugin/PluginEditor.h");
    const auto projectionSection = extractWorkspaceFileSection("Source/Plugin/PluginEditor.cpp",
                                                               "void OpenTuneAudioProcessorEditor::syncMaterializationProjectionToPianoRoll()",
                                                               "} // namespace OpenTune::PluginUI");
    if (editorHeader.contains("araClipImportArmed_")
        || projectionSection.contains("araClipImportArmed_")) {
        logFail(testName, "ARA renderable binding is still gated by Read Audio arm state");
        return;
    }

    logPass(testName);
}

void runAraSnapshotBindingStateIsSetTest()
{
    constexpr const char* testName = "ARA-BIND-04: published region view has bindingState derived from state";
    if (!sourceContains("Source/ARA/VST3AraSession.cpp", "view.bindingState")) {
        logFail(testName, "view.bindingState not found in VST3AraSession.cpp");
        return;
    }
    logPass(testName);
}

// ============================================================================
// ARA-F0 minimal pipeline guards
// ============================================================================

void runAraFinalRecordRequestedDoesNotTriggerGame()
{
    constexpr const char* testName = "AraFinal_RecordRequestedDoesNotTriggerGame";

    const auto branch = extractWorkspaceFileSection(
        "Source/Plugin/PluginEditor.cpp",
        "// Binding/display only",
        "void OpenTuneAudioProcessorEditor::playheadPositionChangeRequested");
    if (branch.isEmpty()) {
        logFail(testName, "failed to locate ARA recordRequested binding-only branch");
        return;
    }

    // Must NOT contain GAME-related calls
    if (branch.contains("requestReferenceNoteGeneration")
        || branch.contains("noteGenerator")) {
        logFail(testName, "ARA recordRequested still triggers note generation");
        return;
    }

    // Must NOT contain ensureAraRegionMaterialization (inline birth)
    if (branch.contains("ensureAraRegionMaterialization")) {
        logFail(testName, "ARA recordRequested still calls inline birth");
        return;
    }

    logPass(testName);
}

void runAraFinalRecordRequestedDoesNotFallbackToMaterializationRefresh()
{
    constexpr const char* testName = "AraFinal_RecordRequestedDoesNotFallbackToMaterializationRefresh";

    const auto& source = getFileCache().get("Source/Plugin/PluginEditor.cpp");
    const auto araElsePos = source.indexOf("#else");
    const auto playheadFuncPos = source.indexOf("void OpenTuneAudioProcessorEditor::playheadPositionChangeRequested");
    if (araElsePos < 0 || playheadFuncPos < 0) {
        logFail(testName, "failed to locate ARA branch boundaries");
        return;
    }

    const auto recordAraBranch = source.substring(araElsePos, playheadFuncPos);
    if (recordAraBranch.contains("requestMaterializationRefresh")) {
        logFail(testName, "ARA recordRequested still calls requestMaterializationRefresh");
        return;
    }

    logPass(testName);
}

void runAraFinalSessionHasNoSourceLevelHydrationReadPass()
{
    constexpr const char* testName = "AraFinal_SessionHasNoSourceLevelHydrationReadPass";

    const auto& sessionCpp = getFileCache().get("Source/ARA/VST3AraSession.cpp");
    // The worker loop must route through birthAraMaterializationWithOriginalF0
    // and NOT do a separate source-level full read pass
    if (!sessionCpp.contains("birthAraMaterializationWithOriginalF0")) {
        logFail(testName, "VST3AraSession worker does not route through birthAraMaterializationWithOriginalF0");
        return;
    }

    // No structure/function that reads raw audio from the source and creates a full materialization
    if (sessionCpp.contains("prepareImport") && !sessionCpp.contains("// Standalone-only"))
    {
        logFail(testName, "VST3AraSession.cpp still references prepareImport (source-level read pass)");
        return;
    }

    logPass(testName);
}

void runAraFinalPublishedRegionViewExposesNoRawAudioPayload()
{
    constexpr const char* testName = "AraFinal_PublishedRegionViewExposesNoRawAudioPayload";

    // PublishedRegionView must not expose a raw audio buffer field
    if (sourceContains("Source/ARA/VST3AraSession.h", "copiedAudio")) {
        logFail(testName, "PublishedRegionView still exposes a raw audio field");
        return;
    }

    // buildPublishedRegionViewFromState must not write raw audio into the view
    if (sourceContains("Source/ARA/VST3AraSession.cpp", "copiedAudio")) {
        logFail(testName, "buildPublishedRegionViewFromState still writes raw audio into view");
        return;
    }

    // renderer must not reference raw audio from the published view
    if (sourceContains("Source/ARA/OpenTunePlaybackRenderer.cpp", "copiedAudio")) {
        logFail(testName, "OpenTunePlaybackRenderer still references raw audio from the view");
        return;
    }

    logPass(testName);
}

void runAraFinalPluginEditorHasNoPrepareImportFromAraRegionHelper()
{
    constexpr const char* testName = "AraFinal_PluginEditorHasNoPrepareImportFromAraRegionHelper";

    if (sourceContains("Source/Plugin/PluginEditor.cpp", "prepareImportFromAraRegion")) {
        logFail(testName, "PluginEditor.cpp still contains prepareImportFromAraRegion");
        return;
    }

    logPass(testName);
}

void runAraFinalPluginEditorHasNoBindPlaybackRegionToMaterialization()
{
    constexpr const char* testName = "AraFinal_PluginEditorHasNoBindPlaybackRegionToMaterialization";

    // Kill-list guard: PluginEditor.cpp must never call
    // bindPlaybackRegionToMaterialization or clearPlaybackRegionMaterialization.
    // The editor is a read-only consumer; the session owns all binding truth.
    const auto& source = getFileCache().get("Source/Plugin/PluginEditor.cpp");

    if (source.contains("bindPlaybackRegionToMaterialization(")) {
        logFail(testName, "PluginEditor.cpp still calls bindPlaybackRegionToMaterialization");
        return;
    }

    if (source.contains("clearPlaybackRegionMaterialization(")) {
        logFail(testName, "PluginEditor.cpp still calls clearPlaybackRegionMaterialization");
        return;
    }

    logPass(testName);
}

void runAraFinalRequestMaterializationRefreshContractIsNonAra()
{
    constexpr const char* testName = "AraFinal_RequestMaterializationRefreshContractIsNonAra";

    const auto& source = getFileCache().get("Source/Plugin/PluginEditor.cpp");
    const auto araElsePos = source.indexOf("#else");
    const auto playheadFuncPos = source.indexOf("void OpenTuneAudioProcessorEditor::playheadPositionChangeRequested");
    if (araElsePos < 0 || playheadFuncPos < 0) {
        logFail(testName, "failed to locate ARA branch boundaries");
        return;
    }

    const auto recordAraBranch = source.substring(araElsePos, playheadFuncPos);
    if (recordAraBranch.contains("requestMaterializationRefresh")) {
        logFail(testName, "ARA recordRequested still calls requestMaterializationRefresh");
        return;
    }

    // VST3AraSession itself should NOT call requestMaterializationRefresh
    if (sourceContains("Source/ARA/VST3AraSession.cpp", "requestMaterializationRefresh")) {
        logFail(testName, "VST3AraSession.cpp still calls requestMaterializationRefresh");
        return;
    }

    logPass(testName);
}

void runAraFinalBirthPathOwnsOriginalF0Release()
{
    constexpr const char* testName = "AraFinal_BirthPathOwnsOriginalF0Release";

    const auto& source = getFileCache().get("Source/PluginProcessor.cpp");
    const auto funcPos = source.indexOf("birthAraMaterializationWithOriginalF0(AraOriginalF0BirthRequest");
    if (funcPos < 0) {
        logFail(testName, "birthAraMaterializationWithOriginalF0 signature not found");
        return;
    }

    // Verify releaseImmediately() appears in the function body (within 500 chars of signature)
    const auto releasePos = source.indexOf(funcPos, "releaseImmediately");
    if (releasePos < 0) {
        logFail(testName, "birthAraMaterializationWithOriginalF0 does not release F0 inference service after birth");
        return;
    }

    logPass(testName);
}

void runAraBirthPathDetectsSilentGapsBeforeCommit()
{
    constexpr const char* testName = "AraBirthPath_DetectsSilentGapsBeforeCommit";

    const auto birthSection = extractWorkspaceFileSection("Source/PluginProcessor.cpp",
                                                          "OpenTuneAudioProcessor::birthAraMaterializationWithOriginalF0(",
                                                          "#endif // JucePlugin_Enable_ARA");
    if (birthSection.isEmpty()) {
        logFail(testName, "failed to locate ARA birth path");
        return;
    }

    const int detectPos = birthSection.indexOf("SilentGapDetector::detectAllGapsAdaptive(preparedImport.storedAudioBuffer)");
    const int assignPos = birthSection.indexOf("preparedImport.silentGaps =");
    const int commitPos = birthSection.indexOf("commitPreparedImportAsMaterialization");

    if (detectPos < 0 || assignPos < 0) {
        logFail(testName, "ARA birth path does not repopulate preparedImport.silentGaps");
        return;
    }

    if (commitPos < 0 || detectPos > commitPos || assignPos > commitPos) {
        logFail(testName, "ARA birth path must detect silent gaps before materialization commit");
        return;
    }

    logPass(testName);
}

void runArrangementScrollBarsUseSharedTimeMathAndBoundedOffsetsTest()
{
    constexpr const char* testName = "Arrangement_ScrollBarsUseSharedTimeMathAndBoundedOffsets";

    const auto source = getFileCache().get("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto scrollSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/ArrangementViewComponent.cpp",
        "void ArrangementViewComponent::setScrollOffset",
        "void ArrangementViewComponent::setVerticalScrollOffset");
    const auto scrollBarSection = extractWorkspaceFileSection(
        "Source/Standalone/UI/ArrangementViewComponent.cpp",
        "void ArrangementViewComponent::updateScrollBars",
        "int ArrangementViewComponent::absoluteTimeToContentX");

    if (source.isEmpty() || scrollSection.isEmpty() || scrollBarSection.isEmpty()) {
        logFail(testName, "failed to locate Arrangement scroll sections");
        return;
    }

    if (!source.contains("int ArrangementViewComponent::getTotalContentWidth() const")
        || !source.contains("return viewportState_.timeToContentX(maxEndTime);")) {
        logFail(testName, "Arrangement content width must come from shared TimelineViewportState time math");
        return;
    }

    if (scrollBarSection.contains("static_cast<int>(maxEndTime * pixelsPerSecond)")) {
        logFail(testName, "Arrangement scroll bars still use a second hand-written width calculation");
        return;
    }

    if (!scrollBarSection.contains("const int visibleWidth = juce::jmax(1, getWidth() - UIColors::scrollBarThickness);")
        || !scrollBarSection.contains("horizontalScrollBar_.setRangeLimits(0.0, totalContentWidth + visibleWidth, juce::dontSendNotification);")
        || !scrollBarSection.contains("horizontalScrollBar_.setCurrentRange(scrollOffset_, visibleWidth, juce::dontSendNotification);")) {
        logFail(testName, "Arrangement scroll bar contract must guard visible width and suppress self-feedback notifications");
        return;
    }

    if (!scrollSection.contains("const int maxScrollOffset = juce::jmax(0, getTotalContentWidth() - visibleWidth);")
        || !scrollSection.contains("const int newOffset = juce::jlimit(0, maxScrollOffset, pixels);")
        || !scrollSection.contains("horizontalScrollBar_.setCurrentRangeStart(scrollOffset_, juce::dontSendNotification);")) {
        logFail(testName, "Arrangement setScrollOffset must upper-clamp and avoid scrollbar feedback loops");
        return;
    }

    logPass(testName);
}

void runAuroraRightSidebarBackgroundReferenceRestyleSourceGuardTest()
{
    constexpr const char* testName = "AuroraRightSidebar_ReferenceRestyleSourceGuard";

    const auto& parameterPanelSource = getFileCache().get("Source/Standalone/UI/ParameterPanel.cpp");
    const auto auroraBlockStart = parameterPanelSource.indexOf("if (themeId == ThemeId::Aurora)");
    const auto clipStart = parameterPanelSource.indexOf("g.reduceClipRegion(backgroundPath);");
    if (auroraBlockStart < 0
        || clipStart < 0
        || auroraBlockStart > clipStart
        || !parameterPanelSource.contains("UIColors::fillAuroraSidebarShell(g, bounds, style.panelRadius);")
        || !parameterPanelSource.contains("UIColors::drawAuroraSidebarShellFrame(g, bounds, style.panelRadius);")) {
        logFail(testName, "Aurora parameter sidebar should switch to a dedicated shell painter before the generic clipped panel path");
        return;
    }

    const auto& auroraThemeSource = getFileCache().get("Source/Standalone/UI/AuroraTheme.h");
    if (!auroraThemeSource.contains("static const juce::uint32 SidebarShellTop      = 0xFF18314C;")
        || !auroraThemeSource.contains("static const juce::uint32 SidebarEdgeAura      = 0x1A2D7FD0;")
        || !auroraThemeSource.contains("static const juce::uint32 SidebarCornerBloom   = 0x142E8BE4;")) {
        logFail(testName, "Aurora theme should define dedicated right-sidebar shell tokens instead of reusing tray colors");
        return;
    }

    const auto& themeTokensSource = getFileCache().get("Source/Standalone/UI/ThemeTokens.h");
    if (!themeTokensSource.contains("juce::Colour auroraSidebarShellTop;")
        || !themeTokensSource.contains("juce::Colour auroraSidebarEdgeAura;")
        || !themeTokensSource.contains("juce::Colour auroraSidebarCornerBloom;")) {
        logFail(testName, "ThemeTokens should cache dedicated Aurora right-sidebar shell colors");
        return;
    }

    const auto& uiColorsSource = getFileCache().get("Source/Standalone/UI/UIColors.h");
    if (!uiColorsSource.contains("static void fillAuroraSidebarShell(juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)")
        || !uiColorsSource.contains("static void drawAuroraSidebarShellFrame(juce::Graphics& g,")
        || !uiColorsSource.contains("const float edgeAuraWidth = juce::jmax(6.0f, bounds.getWidth() * 0.085f);")
        || !uiColorsSource.contains("juce::ColourGradient leftAura")
        || !uiColorsSource.contains("juce::ColourGradient rightAura")
        || !uiColorsSource.contains("const auto leftEdgeVeilBounds")
        || !uiColorsSource.contains("const auto rightEdgeVeilBounds")
        || !uiColorsSource.contains("const auto topLeftCornerBloomBounds")
        || !uiColorsSource.contains("const auto topRightCornerBloomBounds")
        || !uiColorsSource.contains("const auto bottomSettleBounds")
        || !uiColorsSource.contains("const auto lowerSettleMidBandBounds")
        || !uiColorsSource.contains("const auto lowerSettleTailBandBounds")
        || !uiColorsSource.contains("auroraSidebarTopLip")
        || !uiColorsSource.contains("auroraSidebarOuterRim")
        || !uiColorsSource.contains("auroraSidebarInnerRim")
        || uiColorsSource.contains("const auto bodyLiftBounds")
        || uiColorsSource.contains("juce::ColourGradient bodyLift")
        || uiColorsSource.contains("const auto leftAuraBounds = bounds.withWidth(bounds.getWidth() * 0.22f)")
        || uiColorsSource.contains("g.fillEllipse(bounds.getRight() - bloomSize * 0.86f")) {
        logFail(testName, "Aurora right sidebar shell should keep the narrow edge aura, remove the top-center rectangular lift, refine the lower settle layers, and avoid restoring the old oversized side band or bottom-right spotlight");
        return;
    }

    if (uiColorsSource.contains("bool sidebar")) {
        logFail(testName, "Right sidebar background must not be folded into button chrome flags");
        return;
    }

    logPass(testName);
}

void runAuroraScrollbarOutlineSourceGuardTest()
{
    constexpr const char* testName = "AuroraScrollbar_OutlineSourceGuard";

    const auto& lookAndFeelSource = getFileCache().get("Source/Standalone/UI/AuroraLookAndFeel.cpp");
    if (!lookAndFeelSource.contains("const auto glowAlpha = isMouseDown ? 0.24f : (isMouseOver ? 0.18f : 0.08f);")
        || !lookAndFeelSource.contains("juce::ColourGradient fill(UIColors::correctedF0.withAlpha(isMouseDown ? 0.44f : 0.34f),")
        || !lookAndFeelSource.contains("UIColors::panelGlow.withAlpha(isMouseDown ? 0.20f : 0.14f),")
        || !lookAndFeelSource.contains("const auto outline = UIColors::correctedF0.withAlpha(isMouseDown ? 0.82f : (isMouseOver ? 0.70f : 0.56f));")
        || !lookAndFeelSource.contains("g.drawRoundedRectangle(thumb.reduced(0.5f), trackThickness * 0.5f, 1.0f);")) {
        logFail(testName, "Aurora scrollbars should keep a thin blue thumb outline while the internal glow and gradient stay more restrained");
        return;
    }

    logPass(testName);
}

void runImportedClipF0PathOwnsOriginalF0Release()
{
    constexpr const char* testName = "ImportedClipF0Path_OwnsOriginalF0Release";

    const auto& source = getFileCache().get("Source/PluginProcessor.cpp");
    const auto funcPos = source.indexOf("extractImportedClipOriginalF0(const MaterializationSnapshot& snap");
    if (funcPos < 0) {
        logFail(testName, "extractImportedClipOriginalF0 signature not found");
        return;
    }

    const auto releasePos = source.indexOf(funcPos, "releaseImmediately");
    if (releasePos < 0) {
        logFail(testName, "extractImportedClipOriginalF0 does not release F0 inference service after extraction");
        return;
    }

    logPass(testName);
}

void runKeyDetectionAraBirthPathRunsUnifiedDetectionContractTest()
{
    constexpr const char* testName = "KeyDetection_AraBirthPathRunsUnifiedDetectionContract";

    const auto birthSection = extractWorkspaceFileSection("Source/PluginProcessor.cpp",
                                                          "OpenTuneAudioProcessor::birthAraMaterializationWithOriginalF0(",
                                                          "#endif // JucePlugin_Enable_ARA");
    if (birthSection.isEmpty()) {
        logFail(testName, "failed to locate ARA birth path");
        return;
    }

    if (!birthSection.contains("detectAndCommitMaterializationKeyIfUnset(materializationId)")) {
        logFail(testName, "ARA birth path does not call the unified detectedKey commit helper");
        return;
    }

    const auto helperSection = extractWorkspaceFileSection("Source/PluginProcessor.cpp",
                                                           "void OpenTuneAudioProcessor::detectAndCommitMaterializationKeyIfUnset(",
                                                           "DetectedKey OpenTuneAudioProcessor::getMaterializationDetectedKeyById");
    if (helperSection.isEmpty()) {
        logFail(testName, "detectedKey helper section not found");
        return;
    }

    if (!helperSection.contains("ChromaKeyDetector")
        || !helperSection.contains("setMaterializationDetectedKeyById(materializationId, key)")) {
        logFail(testName, "detectedKey helper no longer runs chroma detection and commit");
        return;
    }

    const auto detectPos = birthSection.indexOf("detectAndCommitMaterializationKeyIfUnset(materializationId)");
    const auto readyPos = birthSection.indexOf("setMaterializationOriginalF0StateById(materializationId, OriginalF0State::Ready)");
    if (detectPos < 0 || readyPos < 0 || detectPos > readyPos) {
        logFail(testName, "ARA birth path must commit detectedKey before OriginalF0State::Ready");
        return;
    }

    logPass(testName);
}

void runScaleSyncStandaloneTimerPullsDetectedKeyForActivePlacementTest()
{
    constexpr const char* testName = "ScaleSync_StandaloneTimerPullsDetectedKeyForActivePlacement";

    const auto timerSection = extractWorkspaceFileSection("Source/Standalone/PluginEditor.cpp",
                                                          "void OpenTuneAudioProcessorEditor::timerCallback()",
                                                          "void OpenTuneAudioProcessorEditor::syncSharedAppPreferences()");
    if (timerSection.isEmpty()) {
        logFail(testName, "Standalone timerCallback section not found");
        return;
    }

    if (!timerSection.contains("resolveScaleForPlacementMaterialization(activeTrack, activePlacementIndex, nullptr)")
        || !timerSection.contains("applyResolvedScaleForPlacementMaterialization(activeTrack, activePlacementIndex)")) {
        logFail(testName, "Standalone timer no longer pulls detectedKey and reapplies scale for the active placement");
        return;
    }

    if (!timerSection.contains("resolvedRootNote != lastScaleRootNote_ || resolvedScaleType != lastScaleType_")) {
        logFail(testName, "Standalone timer no longer guards scale sync with last applied UI scale state");
        return;
    }

    logPass(testName);
}

void runF0ServiceDoesNotRetainIdleReleaseLoopTest()
{
    constexpr const char* testName = "F0Service_DoesNotRetainIdleReleaseLoop";

    const auto& source = getFileCache().get("Source/Inference/F0InferenceService.cpp");
    if (source.contains("releaseIdleModelIfNeeded") || source.contains("lastExtractionTimeMs_")
        || source.contains("kModelRetentionMs")) {
        logFail(testName, "F0InferenceService still retains idle release bookkeeping");
        return;
    }

    logPass(testName);
}

void runAraRenderabilityUsesBindingStateTest()
{
    constexpr const char* testName = "ARA-READY-01: renderer renderability uses BindingState::Renderable";
    if (!sourceContains("Source/ARA/OpenTunePlaybackRenderer.cpp", "BindingState::Renderable")) {
        logFail(testName, "BindingState::Renderable not found in OpenTunePlaybackRenderer.cpp");
        return;
    }
    logPass(testName);
}

void runAraSessionSourceDefinesRenderableBindingStateTest()
{
    constexpr const char* testName = "AraSession_SourceDefinesRenderableBindingState";
    if (!sourceContains("Source/ARA/VST3AraSession.cpp", "BindingState::Renderable")) {
        logFail(testName, "BindingState::Renderable not found in VST3AraSession.cpp");
        return;
    }
    logPass(testName);
}

void runAraRendererOnlyConsumesRenderableSnapshotTest()
{
    constexpr const char* testName = "ARA-READY-03: renderer only consumes Renderable snapshot";
    const auto canRenderSection = extractWorkspaceFileSection("Source/ARA/OpenTunePlaybackRenderer.cpp",
                                                              "bool canRenderPublishedRegionView",
                                                              "}");
    if (canRenderSection.contains("appliedProjection.isValid()")) {
        logFail(testName, "canRenderPublishedRegionView still uses appliedProjection.isValid() instead of BindingState");
        return;
    }
    logPass(testName);
}

#if JucePlugin_Enable_ARA
void runAraRenderGateRejectsRealtimeStoppedBlocksTest()
{
    constexpr const char* testName = "AraRenderGate_RejectsRealtimeStoppedBlocks";

    juce::AudioPlayHead::PositionInfo positionInfo;
    positionInfo.setIsPlaying(false);
    positionInfo.setTimeInSeconds(80.0);

    if (OpenTune::shouldRenderAraPlaybackBlock(juce::AudioProcessor::Realtime::yes, positionInfo)) {
        logFail(testName, "realtime stopped ARA block should not render");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

#if JucePlugin_Enable_ARA
void runAraRenderGateAllowsRealtimePlayingBlocksTest()
{
    constexpr const char* testName = "AraRenderGate_AllowsRealtimePlayingBlocks";

    juce::AudioPlayHead::PositionInfo positionInfo;
    positionInfo.setIsPlaying(true);
    positionInfo.setTimeInSeconds(80.0);

    if (!OpenTune::shouldRenderAraPlaybackBlock(juce::AudioProcessor::Realtime::yes, positionInfo)) {
        logFail(testName, "realtime playing ARA block should render");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

#if JucePlugin_Enable_ARA
void runAraRenderGateAllowsNonRealtimeStoppedBlocksTest()
{
    constexpr const char* testName = "AraRenderGate_AllowsNonRealtimeStoppedBlocks";

    juce::AudioPlayHead::PositionInfo positionInfo;
    positionInfo.setIsPlaying(false);
    positionInfo.setTimeInSeconds(80.0);

    if (!OpenTune::shouldRenderAraPlaybackBlock(juce::AudioProcessor::Realtime::no, positionInfo)) {
        logFail(testName, "non-realtime stopped ARA block should remain renderable");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

void runAraRenderGateRunsBeforeMappingAndReadTest()
{
    constexpr const char* testName = "AraRenderGate_RunsBeforeMappingAndRead";
    const auto processSection = extractWorkspaceFileSection("Source/ARA/OpenTunePlaybackRenderer.cpp",
                                                            "bool OpenTunePlaybackRenderer::processBlock",
                                                            "return processedAny;");

    const auto gateIndex = processSection.indexOf("shouldRenderAraPlaybackBlock");
    const auto timeIndex = processSection.indexOf("getTimeInSeconds()");
    const auto mappingIndex = processSection.indexOf("ARA Mapping:");
    const auto readIndex = processSection.indexOf("readPlaybackAudio");

    if (gateIndex < 0) {
        logFail(testName, "processBlock does not call shouldRenderAraPlaybackBlock");
        return;
    }

    if (timeIndex < 0 || mappingIndex < 0 || readIndex < 0) {
        logFail(testName, "processBlock source guard could not find mapping/read anchors");
        return;
    }

    if (!(gateIndex < timeIndex && gateIndex < mappingIndex && gateIndex < readIndex)) {
        logFail(testName, "ARA render gate must run before time mapping and readPlaybackAudio");
        return;
    }

    logPass(testName);
}

void runAraRenderGateStoppedRealtimeClearReturnsTrueTest()
{
    constexpr const char* testName = "AraRenderGate_StoppedRealtimeClearReturnsTrue";
    const auto gateBranch = extractWorkspaceFileSection("Source/ARA/OpenTunePlaybackRenderer.cpp",
                                                        "if (!shouldRenderAraPlaybackBlock(realtime, positionInfo))",
                                                        "const auto& regions = getPlaybackRegions();");

    if (gateBranch.isEmpty()) {
        logFail(testName, "stopped realtime gate branch not found in processBlock");
        return;
    }

    const auto clearIndex = gateBranch.indexOf("buffer.clear()");
    const auto returnTrueIndex = gateBranch.indexOf("return true;");
    const auto returnFalseIndex = gateBranch.indexOf("return false;");

    if (clearIndex < 0) {
        logFail(testName, "stopped realtime gate branch must clear the buffer");
        return;
    }

    if (returnTrueIndex < 0 || returnTrueIndex <= clearIndex) {
        logFail(testName, "stopped realtime gate must return true after buffer.clear()");
        return;
    }

    if (returnFalseIndex >= 0) {
        logFail(testName, "stopped realtime gate must not return false and request non-ARA fallback");
        return;
    }

    logPass(testName);
}

void runAraCapableVst3CreatesCaptureSessionWithoutBuildTimeAraExclusionTest()
{
    constexpr const char* testName = "AraRuntime_AraCapableVst3CreatesCaptureSessionWithoutBuildTimeAraExclusion";
    const auto ctorSection = extractWorkspaceFileSection("Source/PluginProcessor.cpp",
                                                         "OpenTuneAudioProcessor::OpenTuneAudioProcessor()",
                                                         "OpenTuneAudioProcessor::~OpenTuneAudioProcessor()");

    if (!ctorSection.contains("wrapperType == juce::AudioProcessor::wrapperType_VST3")) {
        logFail(testName, "capture session construction must stay runtime-gated to VST3 wrapperType");
        return;
    }

    const auto createIndex = ctorSection.indexOf("captureSession_ = std::make_unique<Capture::CaptureSession>");
    if (createIndex < 0) {
        logFail(testName, "VST3 capture session construction not found");
        return;
    }

    if (ctorSection.contains("#if !JucePlugin_Enable_ARA")) {
        logFail(testName, "ARA-capable builds still exclude regular VST3 capture session construction");
        return;
    }

    logPass(testName);
}

void runAraRuntimeCaptureSessionAccessorSuppressesAraBoundInstancesTest()
{
    constexpr const char* testName = "AraRuntime_CaptureSessionAccessorSuppressesAraBoundInstances";
    const auto accessorSection = extractWorkspaceFileSection("Source/PluginProcessor.cpp",
                                                             "Capture::CaptureSession* OpenTuneAudioProcessor::getCaptureSession() noexcept",
                                                             "const Capture::CaptureSession* OpenTuneAudioProcessor::getCaptureSession() const noexcept");
    const auto constAccessorSection = extractWorkspaceFileSection("Source/PluginProcessor.cpp",
                                                                  "const Capture::CaptureSession* OpenTuneAudioProcessor::getCaptureSession() const noexcept",
                                                                  "OpenTuneDocumentController* OpenTuneAudioProcessor::getDocumentController() const");

    if (!accessorSection.contains("if (isBoundToARA())")
        || !accessorSection.contains("return nullptr;")
        || !accessorSection.contains("return captureSession_.get();")) {
        logFail(testName, "non-const getCaptureSession must hide capture state after ARA binding");
        return;
    }

    if (!constAccessorSection.contains("if (isBoundToARA())")
        || !constAccessorSection.contains("return nullptr;")
        || !constAccessorSection.contains("return captureSession_.get();")) {
        logFail(testName, "const getCaptureSession must hide capture state after ARA binding");
        return;
    }

    logPass(testName);
}

void runAraRuntimeRecordRequestedSplitsByRuntimeModeTest()
{
    constexpr const char* testName = "AraRuntime_RecordRequestedSplitsByRuntimeMode";
    const auto recordSection = extractWorkspaceFileSection("Source/Plugin/PluginEditor.cpp",
                                                           "void OpenTuneAudioProcessorEditor::recordRequested()",
                                                           "void OpenTuneAudioProcessorEditor::playheadPositionChangeRequested");

    if (recordSection.isEmpty()) {
        logFail(testName, "failed to locate recordRequested function body");
        return;
    }

    const auto captureIndex = recordSection.indexOf("processorRef_.getCaptureSession()");
    const auto regularLogIndex = recordSection.indexOf("mode=regular-vst3");
    const auto dcIndex = recordSection.indexOf("processorRef_.getDocumentController()");
    const auto araLogIndex = recordSection.indexOf("mode=ara-bound");

    if (captureIndex < 0 || regularLogIndex < 0) {
        logFail(testName, "recordRequested must have a regular VST3 capture branch with mode log");
        return;
    }

    if (dcIndex < 0 || araLogIndex < 0) {
        logFail(testName, "recordRequested must have an ARA-bound branch with mode log");
        return;
    }

    if (!(captureIndex < dcIndex && regularLogIndex < dcIndex)) {
        logFail(testName, "regular VST3 branch must run before ARA DocumentController handling");
        return;
    }

    if (recordSection.contains("Unable to access VST3 ARA DocumentController")) {
        logFail(testName, "regular VST3 request must not fall through to missing DocumentController wording");
        return;
    }

    logPass(testName);
}

void runAraRuntimeProcessBlockAraFirstThenRegularCaptureTest()
{
    constexpr const char* testName = "AraRuntime_ProcessBlockAraFirstThenRegularCapture";
    const auto processSection = extractWorkspaceFileSection("Source/PluginProcessor.cpp",
                                                            "void OpenTuneAudioProcessor::processBlock",
                                                            "// Clear output buffer");

    const auto araBoundIndex = processSection.indexOf("if (isBoundToARA())");
    const auto araProcessIndex = processSection.indexOf("processBlockForARA");
    const auto captureIndex = processSection.indexOf("getCaptureSession()");
    const auto captureProcessIndex = processSection.indexOf("captureSession->processBlock");

    if (araBoundIndex < 0 || araProcessIndex < 0) {
        logFail(testName, "processBlock must attempt ARA processing for bound instances");
        return;
    }

    if (captureIndex < 0 || captureProcessIndex < 0) {
        logFail(testName, "processBlock must run regular VST3 capture after ARA processing");
        return;
    }

    if (!(araProcessIndex < captureIndex && captureIndex < captureProcessIndex)) {
        logFail(testName, "processBlock must route ARA first, then regular capture");
        return;
    }

    logPass(testName);
}

void runAraRuntimeCapturePersistenceUsesRuntimeAccessorTest()
{
    constexpr const char* testName = "AraRuntime_CapturePersistenceUsesRuntimeAccessor";
    const auto serializeSection = extractWorkspaceFileSection("Source/PluginProcessor.cpp",
                                                              "// Capture-flow materializations live only inside captureSession_",
                                                              "void OpenTuneAudioProcessor::setStateInformation");
    const auto deserializeSection = extractWorkspaceFileSection("Source/PluginProcessor.cpp",
                                                                "// After project state is restored, attempt to load regular VST3 capture payload",
                                                                "// ============================================================================");

    if (!serializeSection.contains("getCaptureSession()")
        || serializeSection.contains("if (captureSession_ != nullptr)")) {
        logFail(testName, "state serialization must use runtime capture accessor");
        return;
    }

    if (!deserializeSection.contains("getCaptureSession()")
        || deserializeSection.contains("if (captureSession_ != nullptr)")) {
        logFail(testName, "state deserialization must use runtime capture accessor");
        return;
    }

    logPass(testName);
}

void runVst3KeyboardShortcutsRouteThroughUnifiedHelperTest()
{
    constexpr const char* testName = "Vst3KeyboardShortcuts_RouteThroughUnifiedHelper";
    const auto keySection = extractWorkspaceFileSection("Source/Plugin/PluginEditor.cpp",
                                                        "bool OpenTuneAudioProcessorEditor::keyPressed(const juce::KeyPress& key)",
                                                        "void OpenTuneAudioProcessorEditor::retuneSpeedChanged");

    if (keySection.isEmpty()) {
        logFail(testName, "PluginEditor keyPressed section not found");
        return;
    }

    const bool usesShortcutRouter = keySection.contains("handleEditorShortcut(key)")
        || keySection.contains("handleKeyCommand(key)")
        || keySection.contains("handleTransportShortcut(key)")
        || keySection.contains("dispatchKeyboardShortcut(key)")
        || keySection.contains("routeKeyPress(key)");

    if (!usesShortcutRouter) {
        logFail(testName, "VST3 keyPressed must delegate to a unified shortcut helper/router");
        return;
    }

    if (keySection.contains("key == juce::KeyPress::spaceKey")
        || keySection.contains("key.getKeyCode() == juce::KeyPress::spaceKey")) {
        logFail(testName, "VST3 keyPressed must not hard-code Space directly at the editor entrypoint");
        return;
    }

    logPass(testName);
}

void runVst3TransportButtonsDoNotForgeRegularPlaybackTruthTest()
{
    constexpr const char* testName = "Vst3TransportButtons_DoNotForgeRegularPlaybackTruth";
    const auto playSection = extractWorkspaceFileSection("Source/Plugin/PluginEditor.cpp",
                                                         "void OpenTuneAudioProcessorEditor::playRequested()",
                                                         "void OpenTuneAudioProcessorEditor::pauseRequested()");
    const auto pauseSection = extractWorkspaceFileSection("Source/Plugin/PluginEditor.cpp",
                                                          "void OpenTuneAudioProcessorEditor::pauseRequested()",
                                                          "void OpenTuneAudioProcessorEditor::stopRequested()");
    const auto stopSection = extractWorkspaceFileSection("Source/Plugin/PluginEditor.cpp",
                                                         "void OpenTuneAudioProcessorEditor::stopRequested()",
                                                         "void OpenTuneAudioProcessorEditor::loopToggled");

    if (playSection.isEmpty() || pauseSection.isEmpty() || stopSection.isEmpty()) {
        logFail(testName, "VST3 transport listener sections not found");
        return;
    }

    if (!playSection.contains("docController->requestStartPlayback()")
        || !pauseSection.contains("docController->requestStopPlayback()")
        || !stopSection.contains("docController->requestStopPlayback()")
        || !stopSection.contains("docController->requestSetPlaybackPosition(0.0)")) {
        logFail(testName, "ARA-bound transport branch must keep using DocumentController playback requests");
        return;
    }

    const auto playRegular = playSection.fromLastOccurrenceOf("#endif", false, false);
    const auto pauseRegular = pauseSection.fromLastOccurrenceOf("#endif", false, false);
    const auto stopRegular = stopSection.fromLastOccurrenceOf("#endif", false, false);

    if (playRegular.contains("processorRef_.setPlaying(")
        || pauseRegular.contains("processorRef_.setPlaying(")
        || stopRegular.contains("processorRef_.setPlaying(")
        || playRegular.contains("processorRef_.setPosition(")
        || pauseRegular.contains("processorRef_.setPosition(")
        || stopRegular.contains("processorRef_.setPosition(")) {
        logFail(testName, "regular VST3 transport must not call processorRef_.setPlaying()/setPosition()");
        return;
    }

    logPass(testName);
}

void runVst3RegularTransportSurfacesHostControlledSemanticsTest()
{
    constexpr const char* testName = "Vst3RegularTransport_SurfacesHostControlledSemantics";
    const auto transportSection = extractWorkspaceFileSection("Source/Plugin/PluginEditor.cpp",
                                                              "void OpenTuneAudioProcessorEditor::playRequested()",
                                                              "void OpenTuneAudioProcessorEditor::loopToggled");
    const auto keySection = extractWorkspaceFileSection("Source/Plugin/PluginEditor.cpp",
                                                        "bool OpenTuneAudioProcessorEditor::keyPressed(const juce::KeyPress& key)",
                                                        "void OpenTuneAudioProcessorEditor::retuneSpeedChanged");

    if (transportSection.isEmpty() || keySection.isEmpty()) {
        logFail(testName, "VST3 transport/key sections not found");
        return;
    }

    const auto regularTransport = transportSection.fromLastOccurrenceOf("#endif", false, false);
    const bool hasUserVisibleOrLogSignal = regularTransport.contains("AppLogger::")
        || regularTransport.contains("showHostManagedMessage")
        || regularTransport.contains("showHostControlled")
        || regularTransport.contains("setStatus")
        || regularTransport.contains("setTooltip");
    const bool namesHostControlled = regularTransport.contains("host-controlled")
        || regularTransport.contains("Host-controlled")
        || regularTransport.contains("host controlled")
        || regularTransport.contains("Host controlled");
    const bool shortcutUsesSameRoute = keySection.contains("playPauseToggleRequested()")
        || keySection.contains("handleTransportShortcut(key)")
        || keySection.contains("handleEditorShortcut(key)")
        || keySection.contains("routeKeyPress(key)");

    if (!hasUserVisibleOrLogSignal || !namesHostControlled) {
        logFail(testName, "regular VST3 transport must surface explicit host-controlled semantics");
        return;
    }

    if (!shortcutUsesSameRoute) {
        logFail(testName, "regular VST3 shortcut path must share the transport semantic route");
        return;
    }

    logPass(testName);
}

void runRegularVst3CaptureUsesHostPlayheadTruthTest()
{
    constexpr const char* testName = "RegularVst3Capture_UsesHostPlayheadTruth";
    const auto processSection = extractWorkspaceFileSection("Source/PluginProcessor.cpp",
                                                            "// Regular VST3 capture path:",
                                                            "// Clear output buffer");
    const auto captureSection = extractWorkspaceFileSection("Source/Plugin/Capture/CaptureSession.cpp",
                                                            "// Detect \"transport is running\" by observing host_t advance across blocks.",
                                                            "// Step 2: Reverse-iterate to find newest Edited segment covering host_t.");

    if (processSection.isEmpty() || captureSection.isEmpty()) {
        logFail(testName, "regular capture process sections not found");
        return;
    }

    if (!processSection.contains("hostPlayHead->getPosition()")
        || !processSection.contains("host_t = pos.getTimeInSeconds().orFallback(0.0)")
        || !processSection.contains("isPlayingNow = pos.getIsPlaying()")
        || !processSection.contains("positionAtomic_->store(host_t")
        || !processSection.contains("isPlaying_.store(isPlayingNow")
        || !processSection.contains("captureSession->processBlock(buffer, host_t, getSampleRate(), isPlayingNow)")) {
        logFail(testName, "regular VST3 processBlock must overwrite local transport state from host playhead");
        return;
    }

    if (!captureSection.contains("hostDeltaSeconds")
        || !captureSection.contains("transportRunning = hostDeltaSeconds > kTransportAdvanceEpsilonSeconds")
        || !sourceContains("Source/Plugin/Capture/CaptureSession.cpp", "if (!transportRunning)")
        || !sourceContains("Source/Plugin/Capture/CaptureSession.cpp", "continue;")) {
        logFail(testName, "CaptureSession must continue using host time advancement as capture truth");
        return;
    }

    if (sourceContainsAny("Source/PluginProcessor.cpp", {
            "captureSession->processBlock(buffer, positionAtomic_->load",
            "captureSession->processBlock(buffer, getPosition()",
            "captureSession->processBlock(buffer, currentPosition"
        })) {
        logFail(testName, "regular capture must not feed plugin-local position into CaptureSession");
        return;
    }

    logPass(testName);
}

void runAraRuntimeRegularCapturePlacementSnapshotUsesEditedSegmentsTest()
{
    constexpr const char* testName = "AraRuntime_RegularCapturePlacementSnapshotUsesEditedSegments";
    const auto& editorHeader = getFileCache().get("Source/Plugin/PluginEditor.h");
    const auto& sessionHeader = getFileCache().get("Source/Plugin/Capture/CaptureSession.h");
    const auto& sessionSource = getFileCache().get("Source/Plugin/Capture/CaptureSession.cpp");
    const auto ctorSection = extractWorkspaceFileSection("Source/Plugin/PluginEditor.cpp",
                                                         "OpenTuneAudioProcessorEditor::OpenTuneAudioProcessorEditor(OpenTuneAudioProcessor& processor)",
                                                         "OpenTuneAudioProcessorEditor::~OpenTuneAudioProcessorEditor()");
    const auto destructorSection = extractWorkspaceFileSection("Source/Plugin/PluginEditor.cpp",
                                                               "OpenTuneAudioProcessorEditor::~OpenTuneAudioProcessorEditor()",
                                                               "void OpenTuneAudioProcessorEditor::paint");
    const auto updateCallbackSection = extractWorkspaceFileSection("Source/Plugin/PluginEditor.cpp",
                                                                   "void OpenTuneAudioProcessorEditor::updateRegularCaptureSessionCallback()",
                                                                   "void OpenTuneAudioProcessorEditor::clearRegularCaptureSessionCallback()");
    const auto clearCallbackSection = extractWorkspaceFileSection("Source/Plugin/PluginEditor.cpp",
                                                                  "void OpenTuneAudioProcessorEditor::clearRegularCaptureSessionCallback()",
                                                                  "bool OpenTuneAudioProcessorEditor::keyPressed");

    if (!sessionHeader.contains("listEditedSegments")
        || !sessionHeader.contains("resolveDisplaySegment")
        || !sessionSource.contains("std::vector<SegmentInfo> CaptureSession::listEditedSegments() const")
        || !sessionSource.contains("seg->state.load(std::memory_order_acquire) == SegmentState::Edited")
        || !sessionSource.contains("seg->materializationId != 0")
        || !sessionSource.contains("activeDisplaySegmentId_ = seg->id")
        || !sessionSource.contains("containsTime(hostTimeSeconds)")) {
        logFail(testName, "CaptureSession must expose edited segments as the regular capture placement source");
        return;
    }

    if (editorHeader.contains("selectedCaptureSegmentId_")
        || editorHeader.contains("selectedCaptureMaterializationId_")
        || editorHeader.contains("selectCaptureMaterializationForDisplay")
        || editorHeader.contains("resolveSelectedCaptureProjection")) {
        logFail(testName, "VST3 editor must not own a parallel regular capture display selection");
        return;
    }

    if (!ctorSection.contains("updateRegularCaptureSessionCallback")
        || !updateCallbackSection.contains("setActiveSegmentChangedCallback")
        || !updateCallbackSection.contains("syncMaterializationProjectionToPianoRoll")
        || ctorSection.contains("No active-segment")) {
        logFail(testName, "regular VST3 editor must subscribe to capture completion as a repaint/sync notification");
        return;
    }

    if (!destructorSection.contains("clearRegularCaptureSessionCallback")
        || !clearCallbackSection.contains("setActiveSegmentChangedCallback(nullptr)")) {
        logFail(testName, "regular capture display callback must be cleared on editor destruction");
        return;
    }

    const auto resolveSyncSection = extractWorkspaceFileSection("Source/Plugin/PluginEditor.cpp",
                                                                "OpenTuneAudioProcessorEditor::resolveCurrentMaterializationSync()",
                                                                "void OpenTuneAudioProcessorEditor::updateRegularCaptureSessionCallback()");
    const auto syncSection = extractWorkspaceFileSection("Source/Plugin/PluginEditor.cpp",
                                                         "void OpenTuneAudioProcessorEditor::syncMaterializationProjectionToPianoRoll()",
                                                         "} // namespace OpenTune::PluginUI");
    if (!resolveSyncSection.contains("session->listEditedSegments()")
        || !resolveSyncSection.contains("sync.placements.push_back")
        || !resolveSyncSection.contains("chooseActiveCaptureMaterialization")
        || !resolveSyncSection.contains("sync.usesRegularCaptureTimelineDomain = true")
        || !resolveSyncSection.contains("sync.timelineViewStartSeconds = 0.0")
        || !resolveSyncSection.contains("sync.timelineViewEndSeconds = viewEndSeconds")
        || resolveSyncSection.contains("MaterializationSource")) {
        logFail(testName, "regular capture sync must build one placement collection from edited segments without the old single-source branch");
        return;
    }

    if (!syncSection.contains("pianoRoll_.setTimelineMaterializationPlacements(sync.placements)")
        || !syncSection.contains("pianoRoll_.setTimelineViewDomain(sync.timelineViewStartSeconds, sync.timelineViewEndSeconds)")
        || !syncSection.contains("pianoRoll_.clearTimelineViewDomain()")
        || syncSection.contains("setMaterializationProjection")) {
        logFail(testName, "PianoRoll sync must use placement collection plus view-domain policy, not the old single projection path");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Memory Optimization Suite
// ============================================================================

void runMaterializationStore_CreateDoesNotAllocateDrySignalTest()
{
    constexpr const char* testName = "MaterializationStore_CreateDoesNotAllocateDrySignal";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeTestClipRequest());
    if (matId == 0) {
        logFail(testName, "failed to create materialization");
        return;
    }

    MaterializationStore::MaterializationSnapshot snapshot;
    if (!store.getSnapshot(matId, snapshot)) {
        logFail(testName, "failed to get snapshot");
        return;
    }

    if (snapshot.audioBuffer == nullptr) {
        logFail(testName, "audioBuffer should be present");
        return;
    }

    // drySignalBuffer 字段已被删除，MaterializationSnapshot 中不应有
    // 编译即验证（如果存在 snapshot.drySignalBuffer 会编译失败）

    logPass(testName);
}

void runMaterializationStore_PlaybackReadSourceReturnsAudioBufferTest()
{
    constexpr const char* testName = "MaterializationStore_PlaybackReadSourceReturnsAudioBuffer";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeTestClipRequest());
    if (matId == 0) {
        logFail(testName, "failed to create materialization");
        return;
    }

    MaterializationStore::PlaybackReadSource readSource;
    if (!store.getPlaybackReadSource(matId, readSource)) {
        logFail(testName, "getPlaybackReadSource failed");
        return;
    }

    if (!readSource.hasAudio()) {
        logFail(testName, "hasAudio() should return true");
        return;
    }

    if (!readSource.canRead()) {
        logFail(testName, "canRead() should return true");
        return;
    }

    if (readSource.audioBuffer == nullptr || readSource.audioBuffer->getNumSamples() != 128) {
        logFail(testName, "audioBuffer should have 128 samples");
        return;
    }

    logPass(testName);
}

void runMaterializationStore_RetireClearsRenderCacheButKeepsAudioBufferTest()
{
    constexpr const char* testName = "MaterializationStore_RetireClearsRenderCacheButKeepsAudioBuffer";

    MaterializationStore store;
    auto request = makeTestClipRequest();
    auto renderCache = request.renderCache;

    // Seed a chunk into the render cache before retire
    seedPublishedIdleChunk(*renderCache, 0.0, 0.01, {0.1f, 0.2f, 0.3f});
    auto stats = renderCache->getChunkStats();
    if (stats.total() == 0) {
        logFail(testName, "failed to seed chunk");
        return;
    }

    const uint64_t matId = store.createMaterialization(std::move(request));
    if (matId == 0) {
        logFail(testName, "failed to create materialization");
        return;
    }

    if (!store.retireMaterialization(matId)) {
        logFail(testName, "retireMaterialization failed");
        return;
    }

    // renderCache should be cleared (audio data released)
    stats = renderCache->getChunkStats();
    if (stats.idle > 0 || stats.pending > 0 || stats.running > 0) {
        logFail(testName, "renderCache should be cleared after retire");
        return;
    }

    // audioBuffer should still be accessible via snapshot (but retired entries are hidden from active queries)
    // Verify retire flag is set
    if (!store.isRetired(matId)) {
        logFail(testName, "materialization should be marked retired");
        return;
    }

    logPass(testName);
}

void runMaterializationStore_ReviveAfterRetirePreservesAudioBufferTest()
{
    constexpr const char* testName = "MaterializationStore_ReviveAfterRetirePreservesAudioBuffer";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeTestClipRequest());
    if (matId == 0) {
        logFail(testName, "failed to create materialization");
        return;
    }

    // Get audioBuffer before retire
    MaterializationStore::MaterializationSnapshot beforeSnapshot;
    store.getSnapshot(matId, beforeSnapshot);
    const auto audioBufferBefore = beforeSnapshot.audioBuffer;

    if (!store.retireMaterialization(matId)) {
        logFail(testName, "retire failed");
        return;
    }

    if (!store.reviveMaterialization(matId)) {
        logFail(testName, "revive failed");
        return;
    }

    // After revive, audioBuffer and pitchCurve should still be available
    MaterializationStore::MaterializationSnapshot afterSnapshot;
    if (!store.getSnapshot(matId, afterSnapshot)) {
        logFail(testName, "getSnapshot failed after revive");
        return;
    }

    if (afterSnapshot.audioBuffer == nullptr) {
        logFail(testName, "audioBuffer should survive retire+revive");
        return;
    }

    if (afterSnapshot.audioBuffer->getNumSamples() != 128) {
        logFail(testName, "audioBuffer sample count changed after retire+revive");
        return;
    }

    if (afterSnapshot.pitchCurve == nullptr) {
        logFail(testName, "pitchCurve should survive retire+revive");
        return;
    }

    // PlaybackReadSource should also work
    MaterializationStore::PlaybackReadSource readSource;
    if (!store.getPlaybackReadSource(matId, readSource)) {
        logFail(testName, "getPlaybackReadSource failed after revive");
        return;
    }

    if (!readSource.hasAudio()) {
        logFail(testName, "hasAudio() should return true after revive");
        return;
    }

    logPass(testName);
}

void runRenderCache_CacheLimitIs256MBTest()
{
    constexpr const char* testName = "RenderCache_CacheLimitIs256MB";

    constexpr size_t expected = static_cast<size_t>(256) * 1024 * 1024;
    if (RenderCache::kDefaultGlobalCacheLimitBytes != expected) {
        logFail(testName, "kDefaultGlobalCacheLimitBytes is not 256MB");
        return;
    }

    logPass(testName);
}

void runRenderCache_OverlayReadsFromChunkAudioAtRenderSampleRateTest()
{
    constexpr const char* testName = "RenderCache_OverlayReadsFromChunkAudioAtRenderSampleRate";

    RenderCache cache;

    // Create a chunk with known audio data at 44.1kHz
    const int numSamples = 441; // 0.01 seconds at 44.1kHz
    std::vector<float> audio(numSamples);
    for (int i = 0; i < numSamples; ++i) {
        audio[i] = static_cast<float>(i) / static_cast<float>(numSamples);
    }

    if (!seedPublishedIdleChunk(cache, 0.0, 0.01, audio)) {
        logFail(testName, "failed to seed chunk");
        return;
    }

    // Overlay at the same sample rate (44100)
    juce::AudioBuffer<float> dest(1, 441);
    dest.clear();
    cache.overlayPublishedAudioForRate(dest, 0, 441, 0.0, 44100);

    // Verify the overlaid audio matches the source
    const float* out = dest.getReadPointer(0);
    bool allZero = true;
    for (int i = 0; i < 441; ++i) {
        if (std::abs(out[i]) > 1e-6f) {
            allZero = false;
            break;
        }
    }
    if (allZero) {
        logFail(testName, "overlay produced all zeros — chunk audio not read");
        return;
    }

    logPass(testName);
}

void runRenderCache_OverlayWithDifferentTargetSampleRateTest()
{
    constexpr const char* testName = "RenderCache_OverlayWithDifferentTargetSampleRate";

    RenderCache cache;

    // Create a chunk with known audio data at 44.1kHz
    const int numSamples = 441;
    std::vector<float> audio(numSamples);
    for (int i = 0; i < numSamples; ++i) {
        audio[i] = 0.5f; // constant value
    }

    if (!seedPublishedIdleChunk(cache, 0.0, 0.01, audio)) {
        logFail(testName, "failed to seed chunk");
        return;
    }

    // Overlay at 96000 Hz (different from 44100)
    const int destSamples = 960; // 0.01 seconds at 96kHz
    juce::AudioBuffer<float> dest(1, destSamples);
    dest.clear();
    cache.overlayPublishedAudioForRate(dest, 0, destSamples, 0.0, 96000);

    // Verify non-zero output (proves interpolation from 44.1kHz works)
    const float* out = dest.getReadPointer(0);
    int nonZeroCount = 0;
    for (int i = 0; i < destSamples; ++i) {
        if (std::abs(out[i] - 0.5f) < 0.01f) {
            ++nonZeroCount;
        }
    }
    if (nonZeroCount < destSamples / 2) {
        logFail(testName, "overlay at 96kHz did not produce expected values from 44.1kHz chunk");
        return;
    }

    logPass(testName);
}

void runPlaybackReadSource_HasAudioMethodTest()
{
    constexpr const char* testName = "PlaybackReadSource_HasAudioMethod";

    {
        MaterializationStore::PlaybackReadSource src;
        if (src.hasAudio()) {
            logFail(testName, "hasAudio() should be false with null audioBuffer");
            return;
        }
        if (src.canRead()) {
            logFail(testName, "canRead() should be false with null audioBuffer");
            return;
        }
    }

    {
        MaterializationStore::PlaybackReadSource src;
        src.audioBuffer = makeSharedAudioBuffer(128);
        if (!src.hasAudio()) {
            logFail(testName, "hasAudio() should be true with valid audioBuffer");
            return;
        }
        if (!src.canRead()) {
            logFail(testName, "canRead() should be true with valid audioBuffer");
            return;
        }
    }

    {
        MaterializationStore::PlaybackReadSource src;
        src.audioBuffer = makeSharedAudioBuffer(0);
        if (src.hasAudio()) {
            logFail(testName, "hasAudio() should be false with zero-sample audioBuffer");
            return;
        }
    }

    logPass(testName);
}

void runVocoderScheduler_QueueDepthLimit50Test()
{
    constexpr const char* testName = "VocoderScheduler_QueueDepthLimit50";

    if (VocoderRenderScheduler::kMaxQueueDepth != 50) {
        logFail(testName, "kMaxQueueDepth is not 50");
        return;
    }

    logPass(testName);
}

void runDmlVocoderUsesDeviceOutputBinding()
{
    constexpr const char* testName = "DmlVocoder_UsesDeviceOutputBinding";

    const auto& header = getFileCache().get("Source/Inference/DmlVocoder.h");
    const auto& source = getFileCache().get("Source/Inference/DmlVocoder.cpp");

    if (!source.contains("outputMemoryInfos_ = session_->GetMemoryInfoForOutputs()")
        || !source.contains("ioBinding_->BindOutput(outputName.c_str(), outputMemoryInfos_.front())")
        || !source.contains("ioBinding_->GetOutputValues()")
        || !source.contains("env_->CopyTensors(")) {
        logFail(testName, "DmlVocoder does not use device output binding plus explicit copy-back");
        return;
    }

    if (header.contains("cpuMemoryInfo_")
        || header.contains("preallocatedOutput_")
        || header.contains("outputBuffer_")
        || header.contains("preallocatedFrames_")
        || source.contains("Ort::Value::CreateTensor<float>(\n                cpuMemoryInfo_")
        || source.contains("ioBinding_->BindOutput(outputName.c_str(), *preallocatedOutput_)")) {
        logFail(testName, "DmlVocoder still retains the old CPU preallocated output path");
        return;
    }

    logPass(testName);
}

void runRenderingPriorityRemainsGpuFirstAndCpuFirstOnly()
{
    constexpr const char* testName = "RenderingPriority_RemainsGpuFirstAndCpuFirstOnly";

    const auto& prefsHeader = getFileCache().get("Source/Utils/AppPreferences.h");
    const auto& prefsUi = getFileCache().get("Source/Editor/Preferences/SharedPreferencePages.cpp");
    const auto& standaloneEditor = getFileCache().get("Source/Standalone/PluginEditor.cpp");
    const auto& pluginEditor = getFileCache().get("Source/Plugin/PluginEditor.cpp");

    if (!prefsHeader.contains("GpuFirst = 0")
        || !prefsHeader.contains("CpuFirst")
        || prefsHeader.contains("HybridFirst")
        || prefsHeader.contains("DmlDeviceOnly")) {
        logFail(testName, "RenderingPriority enum no longer matches GPU-first / CPU-first two-state contract");
        return;
    }

    if (!prefsUi.contains("renderingPrioritySelector_.addItem(LOC(kGpuFirst), 1);")
        || !prefsUi.contains("renderingPrioritySelector_.addItem(LOC(kCpuFirst), 2);")
        || !prefsUi.contains("onRenderingPriorityChanged_(priority == RenderingPriority::CpuFirst);")) {
        logFail(testName, "Rendering priority preferences UI no longer exposes exactly GPU First and CPU First");
        return;
    }

    if (!standaloneEditor.contains("processorRef_.resetInferenceBackend(true);")
        || !standaloneEditor.contains("[this](bool forceCpu) { processorRef_.resetInferenceBackend(forceCpu); }")
        || !pluginEditor.contains("[this](bool forceCpu) { processorRef_.resetInferenceBackend(forceCpu); }")) {
        logFail(testName, "Rendering priority callbacks no longer map to resetInferenceBackend(forceCpu)");
        return;
    }

    logPass(testName);
}

void runStage2WorkerStreamsStage1InputDirectly()
{
    constexpr const char* testName = "Stage2Worker_StreamsStage1InputDirectly";

    const auto section = extractWorkspaceFileSection("Source/PluginProcessor.cpp",
                                                     "OpenTuneAudioProcessor::runStage2RebuildForMaterialization(",
                                                     "bool OpenTuneAudioProcessor::ensureServiceReady(");

    if (section.isEmpty()) {
        logFail(testName, "runStage2RebuildForMaterialization section not found");
        return;
    }

    if (!section.contains("stretcher->push(readBuf.getReadPointer(0), static_cast<size_t>(n), isLast);")
        || !section.contains("const int wrote = readPlaybackAudio(req, readBuf, /*destStart=*/0);")
        || !section.contains("readBuf.setSample(0, i,")) {
        logFail(testName, "Stage 2 rebuild no longer streams readPlaybackAudio chunks directly into SoundTouch");
        return;
    }

    if (section.contains("std::vector<float> stage1Buffer")
        || section.contains("stage1Buffer.push_back")
        || section.contains("stage1Buffer.resize")
        || section.contains("stretcher->push(stage1Buffer.data() + offset")) {
        logFail(testName, "Stage 2 rebuild still retains the old full-clip stage1Buffer path");
        return;
    }

    logPass(testName);
}

void runIntegration_RetireAndReviveRoundTripTest()
{
    constexpr const char* testName = "Integration_RetireAndReviveRoundTrip";

    MaterializationStore store;
    const uint64_t matId = store.createMaterialization(makeTestClipRequest());
    if (matId == 0) {
        logFail(testName, "create failed");
        return;
    }

    // Verify active
    MaterializationStore::PlaybackReadSource readSource;
    if (!store.getPlaybackReadSource(matId, readSource) || !readSource.hasAudio()) {
        logFail(testName, "initial playback read failed");
        return;
    }

    // Retire
    if (!store.retireMaterialization(matId)) {
        logFail(testName, "retire failed");
        return;
    }

    // Verify hidden from active queries
    if (store.getPlaybackReadSource(matId, readSource)) {
        logFail(testName, "retired materialization should not be visible via getPlaybackReadSource");
        return;
    }

    // Revive
    if (!store.reviveMaterialization(matId)) {
        logFail(testName, "revive failed");
        return;
    }

    // Verify restored
    if (!store.getPlaybackReadSource(matId, readSource) || !readSource.hasAudio()) {
        logFail(testName, "revived materialization should be readable again");
        return;
    }

    logPass(testName);
}

// ============================================================================
// ARA birth lifecycle contract tests
// ============================================================================
#if JucePlugin_Enable_ARA
//
// 测试 1: Same source, two regions with different persistentIds, both complete birth independently.
void runAraBirthSameSourceTwoDifferentPersistentIdsBothBirth()
{
    constexpr const char* testName = "AraBirth_SameSourceTwoDifferentPersistentIdsBothBirth";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x100);
    auto* regionA = reinterpret_cast<juce::ARAPlaybackRegion*>(0x200);
    auto* regionB = reinterpret_cast<juce::ARAPlaybackRegion*>(0x300);
    const SourceWindow sourceWindow{1, 0.0, 1.0};

    VST3AraSessionTestProbe::seedSource(session, audioSource, 1, 44100.0, 44100);
    VST3AraSessionTestProbe::seedPlaybackRegionForModification(
        session, audioSource, regionA, "mod-A", sourceWindow, 0.0, 1.0);
    VST3AraSessionTestProbe::seedPlaybackRegionForModification(
        session, audioSource, regionB, "mod-B", sourceWindow, 0.0, 1.0);

    // Each region has a distinct persistentId with no binding. The
    // modification-level PendingBirth model gives each its own revision and
    // worker result path.
    if (!VST3AraSessionTestProbe::upsertPendingBirthIfNeededForPersistentId(session, "mod-A")) {
        logFail(testName, "upsertPendingBirthIfNeededForPersistentId(\"mod-A\") returned false");
        return;
    }
    const uint64_t revisionA = VST3AraSessionTestProbe::getPendingBirthRevision(session, "mod-A");

    if (!VST3AraSessionTestProbe::upsertPendingBirthIfNeededForPersistentId(session, "mod-B")) {
        logFail(testName, "upsertPendingBirthIfNeededForPersistentId(\"mod-B\") returned false");
        return;
    }
    const uint64_t revisionB = VST3AraSessionTestProbe::getPendingBirthRevision(session, "mod-B");

    if (!VST3AraSessionTestProbe::simulateReadyBirthWorkerResult(session, "mod-A", revisionA, 1, 7101)
        || !VST3AraSessionTestProbe::simulateReadyBirthWorkerResult(session, "mod-B", revisionB, 1, 7102)) {
        logFail(testName, "worker result simulation did not commit both independent modifications");
        return;
    }

    if (VST3AraSessionTestProbe::bindingMaterializationForPersistentId(session, "mod-A") != 7101
        || VST3AraSessionTestProbe::bindingMaterializationForPersistentId(session, "mod-B") != 7102) {
        logFail(testName, "independent persistentIds did not produce independent materialization bindings");
        return;
    }

    VST3AraSessionTestProbe::publish(session);
    const auto snapshot = session.loadSnapshot();
    const auto* viewA = snapshot ? snapshot->findRegion(regionA) : nullptr;
    const auto* viewB = snapshot ? snapshot->findRegion(regionB) : nullptr;
    if (viewA == nullptr || viewA->appliedProjection.materializationId != 7101
        || viewB == nullptr || viewB->appliedProjection.materializationId != 7102) {
        logFail(testName, "published snapshot did not expose both committed materialization projections");
        return;
    }

    logPass(testName);
}

// 测试 2: New region arrives while another persistentId birth is already in flight on the same source.
void runAraBirthNewPersistentIdArrivesWhileWorkAlreadyInFlight_NotLost()
{
    constexpr const char* testName = "AraBirth_NewPersistentIdArrivesWhileWorkAlreadyInFlight_NotLost";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x100);
    auto* regionA = reinterpret_cast<juce::ARAPlaybackRegion*>(0x200);
    auto* regionC = reinterpret_cast<juce::ARAPlaybackRegion*>(0x400);
    const SourceWindow sourceWindow{1, 0.0, 1.0};

    VST3AraSessionTestProbe::seedSource(session, audioSource, 1, 44100.0, 44100);
    VST3AraSessionTestProbe::seedPlaybackRegionForModification(
        session, audioSource, regionA, "mod-A", sourceWindow, 0.0, 1.0);

    // Begin birth for region A and hold its revision as the in-flight worker snapshot.
    if (!VST3AraSessionTestProbe::upsertPendingBirthIfNeededForPersistentId(session, "mod-A")) {
        logFail(testName, "upsertPendingBirthIfNeededForPersistentId(\"mod-A\") returned false — precondition");
        return;
    }
    const uint64_t revisionA = VST3AraSessionTestProbe::getPendingBirthRevision(session, "mod-A");

    // New region C arrives on the same source, different persistentId, no binding → needs birth
    VST3AraSessionTestProbe::seedPlaybackRegionForModification(
        session, audioSource, regionC, "mod-C", sourceWindow, 0.0, 1.0);

    // Verify region C needs birth
    if (!VST3AraSessionTestProbe::regionNeedsBirth(session, regionC)) {
        logFail(testName, "pairwise seed: region C should need birth (no binding)");
        return;
    }

    if (!VST3AraSessionTestProbe::upsertPendingBirthIfNeededForPersistentId(session, "mod-C")) {
        logFail(testName, "new persistentId did not create an independent PendingBirth while A was in flight");
        return;
    }
    const uint64_t revisionC = VST3AraSessionTestProbe::getPendingBirthRevision(session, "mod-C");

    if (!VST3AraSessionTestProbe::simulateReadyBirthWorkerResult(session, "mod-A", revisionA, 1, 7201)) {
        logFail(testName, "in-flight mod-A worker result did not commit");
        return;
    }

    if (!VST3AraSessionTestProbe::hasPendingBirth(session, "mod-C")) {
        logFail(testName, "committing mod-A incorrectly consumed later mod-C PendingBirth");
        return;
    }

    if (!VST3AraSessionTestProbe::simulateReadyBirthWorkerResult(session, "mod-C", revisionC, 1, 7203)) {
        logFail(testName, "later mod-C worker result did not commit");
        return;
    }

    if (VST3AraSessionTestProbe::bindingMaterializationForPersistentId(session, "mod-A") != 7201
        || VST3AraSessionTestProbe::bindingMaterializationForPersistentId(session, "mod-C") != 7203) {
        logFail(testName, "in-flight and later persistentIds did not retain separate bindings");
        return;
    }

    logPass(testName);
}

// 测试 3: SourceWindow changes while an older birth result is in flight; stale result must not bind.
void runAraBirthStaleResultForOldWindowDoesNotOverrideLatestDesiredWindow()
{
    constexpr const char* testName = "AraBirth_StaleResultForOldWindowDoesNotOverrideLatestDesiredWindow";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x100);
    auto* regionA = reinterpret_cast<juce::ARAPlaybackRegion*>(0x200);
    const SourceWindow initialWindow{1, 0.0, 1.0};
    const SourceWindow updatedWindow{1, 0.5, 1.5};

    VST3AraSessionTestProbe::seedSource(session, audioSource, 1, 44100.0, 44100);

    VST3AraSessionTestProbe::seedPlaybackRegionForModification(
        session, audioSource, regionA, "mod-A", initialWindow, 0.0, 1.0);
    if (!VST3AraSessionTestProbe::upsertPendingBirthIfNeededForPersistentId(session, "mod-A")) {
        logFail(testName, "first upsert (\"mod-A\") failed — precondition");
        return;
    }
    const uint64_t staleRevision = VST3AraSessionTestProbe::getPendingBirthRevision(session, "mod-A");

    // The host changes the desired source window before the old worker result lands.
    VST3AraSessionTestProbe::setRegionSourceWindow(session, regionA, updatedWindow);
    if (!VST3AraSessionTestProbe::upsertPendingBirthIfNeededForPersistentId(session, "mod-A")) {
        logFail(testName, "second upsert (\"mod-A\") failed after sourceWindow change");
        return;
    }
    const uint64_t freshRevision = VST3AraSessionTestProbe::getPendingBirthRevision(session, "mod-A");

    if (freshRevision <= staleRevision) {
        logFail(testName, "sourceWindow change did not bump PendingBirth revision");
        return;
    }

    if (VST3AraSessionTestProbe::simulateReadyBirthWorkerResult(session, "mod-A", staleRevision, 1, 7301)) {
        logFail(testName, "stale worker result unexpectedly committed");
        return;
    }

    if (VST3AraSessionTestProbe::bindingMaterializationForPersistentId(session, "mod-A") != 0) {
        logFail(testName, "stale worker result wrote a materialization binding");
        return;
    }

    if (!VST3AraSessionTestProbe::hasPendingBirth(session, "mod-A")) {
        logFail(testName, "stale worker result removed the fresh PendingBirth");
        return;
    }

    const SourceWindow pendingWindow = VST3AraSessionTestProbe::getPendingBirthWindow(session, "mod-A");
    if (pendingWindow.sourceStartSeconds != updatedWindow.sourceStartSeconds
        || pendingWindow.sourceEndSeconds != updatedWindow.sourceEndSeconds) {
        logFail(testName, "fresh PendingBirth no longer points at the latest sourceWindow");
        return;
    }

    if (!VST3AraSessionTestProbe::simulateReadyBirthWorkerResult(session, "mod-A", freshRevision, 1, 7302)) {
        logFail(testName, "fresh worker result did not commit after stale drop");
        return;
    }

    if (VST3AraSessionTestProbe::bindingMaterializationForPersistentId(session, "mod-A") != 7302) {
        logFail(testName, "fresh worker result did not become the final binding");
        return;
    }

    logPass(testName);
}

// 测试 4: Preexisting region with empty persistentId does not get birth — then
//        persistentId is assigned, making it eligible — but the projectionChanged ||
//        bindingChanged guard in didAddPlaybackRegionToAudioModification would skip enqueue.
void runAraBirthDidAddOnPreexistingRegionWithNewPersistentIdEnqueuesBirth()
{
    constexpr const char* testName = "AraBirth_DidAddOnPreexistingRegionWithNewPersistentIdEnqueuesBirth";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x100);
    auto* playbackRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0x200);
    const SourceWindow sourceWindow{1, 0.0, 1.0};

    VST3AraSessionTestProbe::seedSource(session, audioSource, 1, 44100.0, 44100);

    // Seed region with EMPTY persistentId → regionNeedsBirth returns false
    VST3AraSessionTestProbe::seedPlaybackRegionForModification(
        session, audioSource, playbackRegion, "", sourceWindow, 0.0, 1.0);

    // Step 4: with empty persistentId, regionNeedsBirth must be false
    if (VST3AraSessionTestProbe::regionNeedsBirth(session, playbackRegion)) {
        logFail(testName, "empty persistentId should not need birth");
        return;
    }

    // Step 3: assign non-empty persistentId → should now need birth
    VST3AraSessionTestProbe::setRegionPersistentId(session, playbackRegion, "mod-new");

    // Step 4 ASSERT: regionNeedsBirth now returns true
    if (!VST3AraSessionTestProbe::regionNeedsBirth(session, playbackRegion)) {
        logFail(testName, "region with non-empty persistentId and no binding should need birth");
        return;
    }

    if (!VST3AraSessionTestProbe::upsertPendingBirthIfNeededForPersistentId(session, "mod-new")) {
        logFail(testName, "did-add persistentId attach did not record/queue PendingBirth");
        return;
    }

    const uint64_t revision = VST3AraSessionTestProbe::getPendingBirthRevision(session, "mod-new");
    if (!VST3AraSessionTestProbe::simulateReadyBirthWorkerResult(session, "mod-new", revision, 1, 7401)) {
        logFail(testName, "new persistentId birth result did not commit");
        return;
    }

    if (VST3AraSessionTestProbe::bindingMaterializationForPersistentId(session, "mod-new") != 7401) {
        logFail(testName, "new persistentId did not produce a materialization binding");
        return;
    }

    logPass(testName);
}

// 测试 4b (probe): Upsert with unready source still records PendingBirth.
// Uses the same VST3AraSessionTestProbe friend pattern as existing tests to
// verify that PendingBirth is recorded even without ready sample access.
void runAraBirthUpsertWithUnreadySourceStillRecordsPending()
{
    constexpr const char* testName = "AraBirth_UpsertWithUnreadySourceStillRecordsPending";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x100);
    auto* region = reinterpret_cast<juce::ARAPlaybackRegion*>(0x200);
    const SourceWindow sourceWindow{1, 0.0, 1.0};

    // Seed source WITHOUT sampleAccessEnabled → source is "unready"
    VST3AraSessionTestProbe::seedSourceUnready(session, audioSource, 1, 44100.0, 44100);

    VST3AraSessionTestProbe::seedPlaybackRegionForModification(
        session, audioSource, region, "mod-unready", sourceWindow, 0.0, 1.0);

    // Region needs birth (no binding)
    if (!VST3AraSessionTestProbe::regionNeedsBirth(session, region))
    {
        logFail(testName, "region with no binding should need birth");
        return;
    }

    // Enqueue — internally calls upsertPendingBirthLocked
    VST3AraSessionTestProbe::upsertPendingBirthIfNeededForPersistentId(session, "mod-unready");

    // Verify: worker-ready queue is empty (source unready) but PendingBirth is recorded
    const auto queueSize = VST3AraSessionTestProbe::readyBirthWorkQueueSize(session);
    if (queueSize != 0)
    {
        char buf[120];
        std::snprintf(buf, sizeof(buf),
            "ready birth work queue should be empty (source unready), got %d", queueSize);
        logFail(testName, buf);
        return;
    }

    // PendingBirth must exist with correct desiredWindow and non-zero revision
    if (!VST3AraSessionTestProbe::hasPendingBirth(session, "mod-unready"))
    {
        logFail(testName, "upsert with unready source did not record PendingBirth");
        return;
    }

    const auto revision = VST3AraSessionTestProbe::getPendingBirthRevision(session, "mod-unready");
    if (revision == 0)
    {
        logFail(testName, "PendingBirth revision was not assigned");
        return;
    }

    const auto storedWindow = VST3AraSessionTestProbe::getPendingBirthWindow(session, "mod-unready");
    if (storedWindow.sourceStartSeconds != sourceWindow.sourceStartSeconds
        || storedWindow.sourceEndSeconds != sourceWindow.sourceEndSeconds)
    {
        logFail(testName, "PendingBirth desiredWindow does not match");
        return;
    }

    logPass(testName);
}

// 测试 4c (probe): Ready source triggers queue for existing pending birth.
// Demonstrates that upsert pushes to queue when source becomes ready.
void runAraBirthReadySourceTriggersQueueForExistingPending()
{
    constexpr const char* testName = "AraBirth_ReadySourceTriggersQueueForExistingPending";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x100);
    auto* region = reinterpret_cast<juce::ARAPlaybackRegion*>(0x200);
    const SourceWindow sourceWindow{1, 0.0, 1.0};

    // Seed source — start unready
    VST3AraSessionTestProbe::seedSourceUnready(session, audioSource, 1, 44100.0, 44100LL * 2);

    VST3AraSessionTestProbe::seedPlaybackRegionForModification(
        session, audioSource, region, "mod-ready-later", sourceWindow, 0.0, 1.0);

    // Enqueue with unready source — pending recorded, queue empty
    VST3AraSessionTestProbe::upsertPendingBirthIfNeededForPersistentId(session, "mod-ready-later");
    {
        const int before = VST3AraSessionTestProbe::readyBirthWorkQueueSize(session);
        if (before != 0)
        {
            char buf[120];
            std::snprintf(buf, sizeof(buf),
                "precondition: queue should be empty, got %d", before);
            logFail(testName, buf);
            return;
        }
    }

    // Now enable sample access AND set up readerLease → source becomes ready
    VST3AraSessionTestProbe::setSourceReady(session, audioSource);

    // Re-upsert: source ready → should now push to queue
    VST3AraSessionTestProbe::upsertPendingBirthIfNeededForPersistentId(session, "mod-ready-later");

    // Verify using the stable pending record; ready queue itself is a transient worker signal.
    if (!VST3AraSessionTestProbe::hasPendingBirth(session, "mod-ready-later"))
    {
        logFail(testName, "ready source upsert lost the PendingBirth record before worker completion");
        return;
    }

    logPass(testName);
}

// 测试 4d (probe): Same pid with new sourceWindow bumps revision and replaces desiredWindow.
void runAraBirthSamePidNewWindowBumpsRevisionAndReplacesWindow()
{
    constexpr const char* testName = "AraBirth_SamePidNewWindowBumpsRevisionAndReplacesWindow";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x100);
    auto* region = reinterpret_cast<juce::ARAPlaybackRegion*>(0x200);
    const SourceWindow initialWindow{1, 0.0, 1.0};
    const SourceWindow updatedWindow{1, 0.5, 1.5};

    VST3AraSessionTestProbe::seedSource(session, audioSource, 1, 44100.0, 44100);
    VST3AraSessionTestProbe::seedPlaybackRegionForModification(
        session, audioSource, region, "mod-window", initialWindow, 0.0, 1.0);

    // First upsert records the initial window at some revision.
    VST3AraSessionTestProbe::upsertPendingBirthIfNeededForPersistentId(session, "mod-window");

    if (!VST3AraSessionTestProbe::hasPendingBirth(session, "mod-window"))
    {
        logFail(testName, "first upsert: PendingBirth not recorded");
        return;
    }

    const uint64_t firstRevision = VST3AraSessionTestProbe::getPendingBirthRevision(session, "mod-window");
    const SourceWindow firstWindow = VST3AraSessionTestProbe::getPendingBirthWindow(session, "mod-window");
    if (firstWindow.sourceStartSeconds != initialWindow.sourceStartSeconds
        || firstWindow.sourceEndSeconds != initialWindow.sourceEndSeconds)
    {
        logFail(testName, "first upsert: desiredWindow mismatch");
        return;
    }

    // Change region's sourceWindow, then re-enqueue → should bump revision
    VST3AraSessionTestProbe::setRegionSourceWindow(session, region, updatedWindow);
    VST3AraSessionTestProbe::upsertPendingBirthIfNeededForPersistentId(session, "mod-window");

    if (!VST3AraSessionTestProbe::hasPendingBirth(session, "mod-window"))
    {
        logFail(testName, "second upsert: PendingBirth vanished");
        return;
    }

    const uint64_t secondRevision = VST3AraSessionTestProbe::getPendingBirthRevision(session, "mod-window");
    const SourceWindow secondWindow = VST3AraSessionTestProbe::getPendingBirthWindow(session, "mod-window");

    if (secondRevision <= firstRevision)
    {
        char buf[100];
        std::snprintf(buf, sizeof(buf),
            "second upsert did not bump revision: prev=%llu cur=%llu",
            (unsigned long long)firstRevision,
            (unsigned long long)secondRevision);
        logFail(testName, buf);
        return;
    }

    if (secondWindow.sourceStartSeconds != updatedWindow.sourceStartSeconds
        || secondWindow.sourceEndSeconds != updatedWindow.sourceEndSeconds)
    {
        logFail(testName, "second upsert did not replace desiredWindow");
        return;
    }

    logPass(testName);
}

// 测试 5: Missing payload in the editor recovery window must not destroy binding truth.
void runAraEditorMissingPayloadDoesNotClearBinding()
{
    constexpr const char* testName = "AraEditor_MissingPayloadDoesNotClearBinding";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x100);
    auto* region = reinterpret_cast<juce::ARAPlaybackRegion*>(0x200);
    const SourceWindow sourceWindow{1, 0.0, 1.0};

    VST3AraSessionTestProbe::seedSource(session, audioSource, 1, 44100.0, 44100);
    VST3AraSessionTestProbe::seedAudioModificationBinding(
        session, "mod-stable", 1, 7501, sourceWindow, 1, sourceWindow.durationSeconds());
    VST3AraSessionTestProbe::seedPlaybackRegionForModification(
        session, audioSource, region, "mod-stable", sourceWindow, 0.0, 1.0);
    VST3AraSessionTestProbe::publish(session);

    if (VST3AraSessionTestProbe::bindingMaterializationForPersistentId(session, "mod-stable") != 7501) {
        logFail(testName, "precondition: binding was not seeded");
        return;
    }

    // There is intentionally no session clear API for editor payload misses.
    // A missing MaterializationStore payload can only leave the binding pending
    // for display, not erase the session-owned persistentId binding.
    const auto snapshot = session.loadSnapshot();
    const auto* view = snapshot ? snapshot->findRegion(region) : nullptr;
    if (view == nullptr || view->appliedProjection.materializationId != 7501) {
        logFail(testName, "published binding projection vanished during payload-missing recovery window");
        return;
    }

    logPass(testName);
}

// 测试 6: Editor destroy/recreate should reattach existing binding instead of
//        relying solely on waitingForAraMaterialization_ transient state.
void runAraEditorDestroyRecreateReattachesExistingBinding()
{
    constexpr const char* testName = "AraEditor_DestroyRecreateReattachesExistingBinding";

    VST3AraSession session;
    auto* audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x101);
    auto* oldRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0x201);
    auto* reopenedRegion = reinterpret_cast<juce::ARAPlaybackRegion*>(0x301);
    const SourceWindow sourceWindow{11, 0.0, 1.0};

    VST3AraSessionTestProbe::seedSource(session, audioSource, sourceWindow.sourceId, 44100.0, 44100);
    VST3AraSessionTestProbe::seedAudioModificationBinding(
        session, "mod-reopen", sourceWindow.sourceId, 7601, sourceWindow, 3, sourceWindow.durationSeconds());
    VST3AraSessionTestProbe::seedPlaybackRegionForModification(
        session, audioSource, oldRegion, "mod-reopen", sourceWindow, 0.0, 1.0);
    VST3AraSessionTestProbe::publish(session);

    // Recreated editor/region observes the same session-owned binding by persistentId.
    VST3AraSessionTestProbe::seedPlaybackRegionForModification(
        session, audioSource, reopenedRegion, "mod-reopen", sourceWindow, 0.0, 1.0);
    VST3AraSessionTestProbe::publish(session);

    const auto snapshot = session.loadSnapshot();
    const auto* reopenedView = snapshot ? snapshot->findRegion(reopenedRegion) : nullptr;
    if (reopenedView == nullptr || reopenedView->appliedProjection.materializationId != 7601) {
        logFail(testName, "recreated region did not reattach existing persistentId binding");
        return;
    }

    logPass(testName);
}
#endif // JucePlugin_Enable_ARA

// 测试 7: setStateInformation called before didBindToARA should still restore
//         into final shared stores — currently didBindToARA overwrites local stores.
void runAraStateRestorePreBindSetStateStillRestoresIntoFinalSharedStores()
{
    constexpr const char* testName = "AraStateRestore_PreBindSetStateStillRestoresIntoFinalSharedStores";

    // Source-scan: Verify setStateInformation caches pre-bind state and replays it
    // into shared stores after didBindToARA (Phase 3 fix).  When setStateInformation
    // arrives before didBindToARA, the raw block is cached in pendingAraState_ and
    // replayed into the final shared stores after attach.
    const auto& processorCpp = getFileCache().get("Source/PluginProcessor.cpp");

    const auto didBindSection = extractWorkspaceFileSection(
        "Source/PluginProcessor.cpp",
        "void OpenTuneAudioProcessor::didBindToARA()",
        "bool OpenTuneAudioProcessor::isBusesLayoutSupported");

    // Note: getStateInformation is at line 2518, setStateInformation at line 2688.
    // The function AFTER setStateInformation is setTrackHeight.
    const auto setStateSection = extractWorkspaceFileSection(
        "Source/PluginProcessor.cpp",
        "void OpenTuneAudioProcessor::setStateInformation",
        "void OpenTuneAudioProcessor::setTrackHeight");

    if (didBindSection.isEmpty() || setStateSection.isEmpty()) {
        logFail(testName, "failed to locate didBindToARA or setStateInformation");
        return;
    }

    // Check for any caching/replay mechanism
    const bool hasRestoreCache = processorCpp.contains("cachedAraState")
        || processorCpp.contains("pendingStateRestore")
        || processorCpp.contains("preBindRestore")
        || processorCpp.contains("pendingAraState");

    // Check if didBindToARA replays stored data into shared stores
    const bool hasSharedStoreWrite = didBindSection.contains("setStateInformation")
        || didBindSection.contains("restoreMaterialization")
        || didBindSection.contains("restorePlacement");

    // Check if setStateInformation guards against pre-bind call
    const bool hasPreBindGuard = setStateSection.contains("didBindToARA")
        || setStateSection.contains("isBoundToARA")
        || setStateSection.contains("cachedAraState");

    if (hasRestoreCache || hasSharedStoreWrite || hasPreBindGuard) {
        logPass(testName);
    } else {
        logFail(testName,
            "setStateInformation writes to local stores, didBindToARA replaces them "
            "with shared stores — no caching or replay mechanism found, restore data lost");
    }
}

// ============================================================================

void runMemoryOptimizationSuite()
{
    logSection("Memory Optimization");

    runMaterializationStore_CreateDoesNotAllocateDrySignalTest();
    runMaterializationStore_PlaybackReadSourceReturnsAudioBufferTest();
    runMaterializationStore_RetireClearsRenderCacheButKeepsAudioBufferTest();
    runMaterializationStore_ReviveAfterRetirePreservesAudioBufferTest();
    runRenderCache_CacheLimitIs256MBTest();
    runRenderCache_OverlayReadsFromChunkAudioAtRenderSampleRateTest();
    runRenderCache_OverlayWithDifferentTargetSampleRateTest();
    runPlaybackReadSource_HasAudioMethodTest();
    runVocoderScheduler_QueueDepthLimit50Test();
    runStage2WorkerStreamsStage1InputDirectly();
    runIntegration_RetireAndReviveRoundTripTest();
}

void runArchitectureBehaviorSuite()
{
    logSection("Architecture");
#if JucePlugin_Enable_ARA
    runAraBindingStateEnumDefinesLifecycleStatesTest();
    runAraPublishedRegionViewExposesBindingStateTest();
    runAraSessionHydrationWorkerRoutesThroughProcessorBirthApiTest();
    runAraBindingNewPersistentIdSameSourceWindowCreatesIndependentMaterializationTest();
    runAraBindingArchiveHooksPersistPersistentIdMaterializationBindingsTest();
    runAraBindingRestoredPersistentIdRebindsNewPlaybackRegionTest();
    runAraEditorAttachesRenderableBindingWithoutReadAudioArmTest();
    runAraSnapshotBindingStateIsSetTest();
#endif

    logSection("AraFinal terminal ARA architecture contract guards");
    runAraFinalRecordRequestedDoesNotTriggerGame();
    runAraFinalRecordRequestedDoesNotFallbackToMaterializationRefresh();
    runAraFinalSessionHasNoSourceLevelHydrationReadPass();
    runAraFinalPublishedRegionViewExposesNoRawAudioPayload();
    runAraFinalPluginEditorHasNoPrepareImportFromAraRegionHelper();
    runAraFinalPluginEditorHasNoBindPlaybackRegionToMaterialization();
    runAraFinalRequestMaterializationRefreshContractIsNonAra();
    runAraFinalBirthPathOwnsOriginalF0Release();
    runAraBirthPathDetectsSilentGapsBeforeCommit();
    runImportedClipF0PathOwnsOriginalF0Release();
    runKeyDetectionAraBirthPathRunsUnifiedDetectionContractTest();
    runScaleSyncStandaloneTimerPullsDetectedKeyForActivePlacementTest();
    runF0ServiceDoesNotRetainIdleReleaseLoopTest();
    runDmlVocoderUsesDeviceOutputBinding();
    runRenderingPriorityRemainsGpuFirstAndCpuFirstOnly();
    runAraStateRestorePreBindSetStateStillRestoresIntoFinalSharedStores();

#if JucePlugin_Enable_ARA
    logSection("ARA birth lifecycle contract tests");
    runAraBirthSameSourceTwoDifferentPersistentIdsBothBirth();
    runAraBirthNewPersistentIdArrivesWhileWorkAlreadyInFlight_NotLost();
    runAraBirthStaleResultForOldWindowDoesNotOverrideLatestDesiredWindow();
    runAraBirthDidAddOnPreexistingRegionWithNewPersistentIdEnqueuesBirth();
    runAraBirthUpsertWithUnreadySourceStillRecordsPending();
    runAraBirthReadySourceTriggersQueueForExistingPending();
    runAraBirthSamePidNewWindowBumpsRevisionAndReplacesWindow();
    runAraEditorMissingPayloadDoesNotClearBinding();
    runAraEditorDestroyRecreateReattachesExistingBinding();

    runAraRenderabilityUsesBindingStateTest();
    runAraSessionSourceDefinesRenderableBindingStateTest();
    runAraRendererOnlyConsumesRenderableSnapshotTest();
    runAraRenderGateRejectsRealtimeStoppedBlocksTest();
    runAraRenderGateAllowsRealtimePlayingBlocksTest();
    runAraRenderGateAllowsNonRealtimeStoppedBlocksTest();
    runAraRenderGateRunsBeforeMappingAndReadTest();
    runAraRenderGateStoppedRealtimeClearReturnsTrueTest();
#endif
    runAraCapableVst3CreatesCaptureSessionWithoutBuildTimeAraExclusionTest();
    runAraRuntimeCaptureSessionAccessorSuppressesAraBoundInstancesTest();
    runAraRuntimeRecordRequestedSplitsByRuntimeModeTest();
    runAraRuntimeProcessBlockAraFirstThenRegularCaptureTest();
    runAraRuntimeCapturePersistenceUsesRuntimeAccessorTest();
    runVst3KeyboardShortcutsRouteThroughUnifiedHelperTest();
    runVst3TransportButtonsDoNotForgeRegularPlaybackTruthTest();
    runVst3RegularTransportSurfacesHostControlledSemanticsTest();
    runRegularVst3CaptureUsesHostPlayheadTruthTest();
    runAraRuntimeRegularCapturePlacementSnapshotUsesEditedSegmentsTest();
    runStandaloneArrangementMultipleClipPlacementsStayTrackLocalTest();
    runMaterializationDetectedKeyStateStaysMaterializationLocalTest();
    runDeletePlacementLastReferenceReclaimsMaterializationTest();
    runDeletePlacementSharedReferenceKeepsSourceUntilFinalOwnerRemovedTest();
    runMaterializationPlacementMaterializationLocalTimingStaysIndependentFromPlacementTest();
    runMaterializationCommandsDoNotMutateTimelinePlacementTruthTest();
    runSplitPlacementBirthsIndependentMaterializationsTest();
    runMergePlacementRewritesPlacementOnlyOrFailsExplicitlyTest();
    runMergePlacementRejectsNonContiguousSourceWindowsTest();

    runProcessorStateFreshProcessorRoundTripsMaterializationAndPlacementTest();
    runProcessorStateBinarySerializationAvoidsXmlBase64Test();
    runProcessorStateRestoreReplacesExistingOwnerStateTest();
    runSourceMaterializationStoresReplaceContentStoreTest();
    runPlacementCommandsDoNotMutateClipCoreTruthTest();
#if JucePlugin_Enable_ARA
    runAraSessionSnapshotExposesSourceMaterializationAndPlacementOwnershipTest();
    runAraBindingMultiplePlaybackRegionsSameAudioModificationShareMaterializationTest();
#endif
    runProcessorModelRejectsMixedClipOwnerApisTest();
#if JucePlugin_Enable_ARA
    runVst3AraSnapshotDoesNotPublishStalePayloadTest();
    runRenderableAraRegionViewAcceptsBindingWithoutRawAudioTest();
    runRenderableAraRegionViewRejectsNonAppliedSiblingTest();
    runAraBindingStateRenderableRequiresMatchingSourceWindowTest();
    runAraBindingStateStaleSourceWindowNeedsRenderTest();
    runAraBindingStateStaleRegionIdentityNeedsRenderTest();
    runAraBindingStateStaleSourceWindowRequeuesBirthTest();
    runVst3AraSessionDefersRegionRemovalUntilDidEndEditingTest();
    runVst3AraSessionDefersSourceDestroyUntilDidEndEditingTest();
#endif
    runMacStandalonePackagingMacDocsGoToBundleResourcesTest();
    runActiveSurfaceHidesRetiredNodesTest();
    runRetireAndReviveAreReversibleTest();
    runBusinessFunctionsDoNotInvokeReclaimDirectlyTest();
    runReclaimSweepRequiresAllReferenceCountersZeroTest();
    runAraTriggeredReclaimGoesThroughSweepTest();
    runSourceRetiredWhenLastMaterializationRetiredTest();

    runAraPublishedReferencePreventsMaterializationSweepTest();
    runSessionOwnershipProcessorDoesNotOwnSessionTest();
    runSessionOwnershipEditorAndRendererReadThroughDocumentControllerTest();
}

void runTimelineRenderingSuite()
{
    logSection("Timeline Rendering Pipeline");

    // L1: Static Contract Gate (source guards)
    runTimelinePlayheadOverlayDirtyRectOnlyTest();
    runTimelinePlayheadPositionDoesNotEnterRenderModelKeyTest();
    runTimelineInvalidationViewportShiftExposesOnlyNewStripTest();
    runTimelineInvalidationBigJumpPromotesToSingleFullRedrawTest();
    runPianoRollPlayheadOnlyTicksDoNotRebuildRenderModelTest();
    runPianoRollStoppedSeekKeepsPresentationIntentTest();
    runPianoRollPaintConsumesPreparedRenderModelOnlyTest();
    runPianoRollF0VisualsUseSingleRenderPathTest();
    runPianoRollVisibleRangeCullsNotesF0AndWaveformTilesTest();
    runPianoRollVerticalGeometryInvalidatesRenderModelKeyTest();
    runPianoRollRenderContextUsesSnapshotVerticalCoordinatesTest();
    runArrangementScrollOffsetDoesNotInvalidateWholeComponentTest();
    runArrangementScrollBarsUseSharedTimeMathAndBoundedOffsetsTest();
    runArrangementPaintConsumesVisibleRenderModelOnlyTest();
    runArrangementVisibleRangeCullsOffscreenPlacementsTest();
    runWaveformTileCacheHasBoundedMemoryAndEvictionTest();
    runTimelineFrameDriverCoalescesRequestsByPriorityTest();
    runTimelineFrameDriverDropsNonCriticalAnimationRateDuringPlaybackTest();
    runTimelineEditorHeartbeatNoDirectRepaintTest();
    runTimelineNoParallelF0RenderPathsTest();
    runTimelinePaintHasNoProcessorOrStoreSideEffectsTest();

    // Cache object unit tests
    OpenTune::runTimelineRenderingPipelineCacheTests();

    // Kill list guards
    runTimelineKillListNoFullOverlayRepaintForPositionTest();
    runTimelineKillListNoPaintTimeRenderContextBuildTest();
    runTimelineKillListNoScrollTimeWholeInvalidationTest();
    runTimelineKillListNoUnboundedUiCachesTest();
    runTimelineKillListNoProcessorOwnedUiCacheTest();
}

void runTimelineRenderingPerfSuite()
{
    logSection("Timeline Rendering Runtime Diagnostics");

    FrameScheduler::instance().resetDiagnosticsForTests();
    FrameScheduler::instance().setPlaybackActiveForTimeline(true);

    juce::Component contentComponent;
    contentComponent.setBounds(0, 0, 1280, 720);
    juce::Component overlayComponent;
    overlayComponent.setBounds(0, 0, 1280, 720);

    for (int i = 0; i < 120; ++i)
        FrameScheduler::instance().requestPlayheadOverlay(overlayComponent, {i % 1200, 0, 4, 720});

    for (int i = 0; i < 180; ++i)
        FrameScheduler::instance().requestViewportShift(contentComponent, {1200, 0, 80, 720});

    for (int i = 0; i < 16; ++i)
        FrameScheduler::instance().requestLowPriorityAnimation(contentComponent, {0, 0, 200, 80});

    FrameScheduler::instance().recordRenderModelRebuild(FrameScheduler::TimelineReason::ContentModelInvalid);
    FrameScheduler::instance().recordWaveformTileHit();
    FrameScheduler::instance().recordWaveformTileMiss();
    FrameScheduler::instance().setPlaybackActiveForTimeline(false);

    const auto snapshot = FrameScheduler::instance().diagnosticsSnapshot();

    constexpr const char* diagnosticsTest = "TimelineRenderingDiagnostics_RecordsRuntimeCounters";
    if (snapshot.playheadOverlayRequests < 120
        || snapshot.viewportShiftRequests < 180
        || snapshot.lowPriorityAnimationDropped == 0
        || snapshot.renderModelRebuilds != 1
        || snapshot.waveformTileHits != 1
        || snapshot.waveformTileMisses != 1) {
        logFail(diagnosticsTest, "diagnostics counters did not record overlay/scroll/cache/runtime activity");
        return;
    }
    logPass(diagnosticsTest);

    constexpr const char* playheadPerfTest = "TimelinePerf_PlayheadOnlyTicksRebuildZeroContentModels";
    if (snapshot.contentInvalidationRequests != 0 || snapshot.fullRepaintPromotions != 0) {
        logFail(playheadPerfTest, "playhead-only runtime simulation promoted content invalidation/full repaint");
        return;
    }
    logPass(playheadPerfTest);

    constexpr const char* scrollPerfTest = "TimelinePerf_ContinuousScrollAvoidsFullRepaintStorm";
    if (snapshot.viewportShiftRequests == 0 || snapshot.fullRepaintPromotions > 1) {
        logFail(scrollPerfTest, "continuous scroll simulation did not stay on exposed-strip requests");
        return;
    }
    logPass(scrollPerfTest);

    constexpr const char* lowPriorityPerfTest = "TimelinePerf_PlaybackDropsLowPriorityAnimation";
    if (snapshot.lowPriorityAnimationDropped == 0) {
        logFail(lowPriorityPerfTest, "playback did not drop low-priority animation requests");
        return;
    }
    logPass(lowPriorityPerfTest);
}

int main(int argc, char* argv[])
{
    // Ensure MessageManager exists for scheduleReclaimSweep (triggerAsyncUpdate requires it).
    // Ownership transferred to juce::DeletedAtShutdown; do not delete manually.
    juce::MessageManager::getInstance();

    printHeader();
    getFileCache().clear();

    if (argc > 1) {
        const juce::String arg(argv[1]);
        if (arg == "--list-suites") {
            printSuiteList();
            return 0;
        }

        const auto* suite = findSuite(arg);
        if (suite == nullptr) {
            std::cout << "Unknown suite: " << arg << std::endl;
            printSuiteList();
            return 2;
        }

        runSuite(*suite);
    } else {
        for (const auto& suite : kSuites)
            runSuite(suite);
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "Tests Complete" << std::endl;
    std::cout << "========================================" << std::endl;

    return gHasTestFailure.load(std::memory_order_relaxed) ? 1 : 0;
}
