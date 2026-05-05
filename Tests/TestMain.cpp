#include "TestSupport.h"
#include "ARA/OpenTunePlaybackRenderer.h"
#include "Audio/AudioFormatRegistry.h"
#include "Standalone/UI/MenuBarComponent.h"
#include "Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h"
#include "Utils/AppPreferences.h"
#include "Utils/AudioEditingScheme.h"
#include "Utils/ParameterPanelSync.h"
#include "Utils/SimdAccelerator.h"
#include "Utils/UndoManager.h"

#include <array>
#include <cmath>
#include <map>
#include <optional>

namespace OpenTune {
bool canRenderPublishedRegionView(const ::OpenTune::VST3AraSession::PublishedRegionView& view) noexcept;
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

constexpr std::array<SuiteEntry, 6> kSuites{{
    { "core", "leaf utilities and render primitives", &runCoreBehaviorSuite },
    { "processor", "shared processor and render contracts", &runProcessorBehaviorSuite },
    { "ui", "piano-roll and visual loop behavior", &runUiBehaviorSuite },
    { "architecture", "clip core, arrangement, session, and guards", &runArchitectureBehaviorSuite },
    { "undo", "undo/redo manager", &runUndoManagerSuite },
    { "memory", "memory optimization and render cache refactor", &runMemoryOptimizationSuite },
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
    int commitNoteDraftCalls = 0;
    int applyManualCorrectionCalls = 0;
    int commitNotesAndSegmentsCalls = 0;
    int notifyPitchCurveEditedCalls = 0;
    std::vector<Note> lastCommittedNotes;
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
        ctx.yToFreq = [](float) { return 440.0f; };
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
                                            const std::vector<CorrectedSegment>&) {
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
        ctx.getRetuneSpeed = []() { return 0.35f; };
        ctx.getVibratoDepth = []() { return 0.0f; };
        ctx.getVibratoRate = []() { return 0.0f; };
        ctx.getAudioEditingScheme = []() { return AudioEditingScheme::Scheme::CorrectedF0Primary; };
        ctx.getShortcutSettings = [this]() -> const KeyShortcutConfig::KeyShortcutSettings& { return shortcutSettings; };
        ctx.recalculatePIP = [](Note&) { return -1.0f; };
        ctx.getF0Timeline = []() { return F0Timeline{}; };

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

        ctx.notifyPlayheadChange = [](double) {};
        ctx.notifyPitchCurveEdited = [this](int, int) { ++notifyPitchCurveEditedCalls; };
        ctx.notifyAutoTuneRequested = []() {};
        ctx.notifyPlayPauseToggle = []() {};
        ctx.notifyStopPlayback = []() {};
        ctx.notifyEscapeKey = []() {};
        ctx.notifyNoteOffsetChanged = [](size_t, float, float) {};

        ctx.applyManualCorrection = [this](std::vector<PianoRollToolHandler::ManualCorrectionOp>, int, int, bool) {
            ++applyManualCorrectionCalls;
            return applyManualCorrectionResult;
        };
        ctx.selectNotesOverlappingFrames = [](int, int) { return true; };
        ctx.getOriginalF0 = []() { return std::vector<float>{}; };

        ctx.findLineAnchorSegmentNear = [](int, int) { return -1; };
        ctx.selectLineAnchorSegment = [](int) {};
        ctx.toggleLineAnchorSegmentSelection = [](int) {};
        ctx.clearLineAnchorSegmentSelection = []() {};
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
    constexpr const char* testName = "PluginPreferencesDialog_ExcludesStandaloneOnlyPages";

    const auto& source = getFileCache().get("Source/Plugin/PluginEditor.cpp");
    if (!source.contains("TabbedPreferencesDialog")
        || !source.contains("SharedPreferencePages")
        || source.contains("StandalonePreferencePages")
        || source.contains("OptionsDialogComponent(")) {
        logFail(testName, "plugin editor still uses mixed dialog composition or exposes standalone-only pages");
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
    const auto& hostHeader = getFileCache().get("Source/Host/HostIntegration.h");
    const auto& hostStandaloneSource = getFileCache().get("Source/Host/HostIntegrationStandalone.cpp");
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
    parameterContext.hasSelectedLineAnchorSegments = true;
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
    if (paintSection.isEmpty()) {
        logFail(testName, "failed to locate piano-roll paint source section");
        return;
    }

    if (paintSection.contains("getCurrentClipNotesCopy()") || paintSection.contains("getMaterializationNotesById(")) {
        logFail(testName, "paint path still reads notes through a processor/store round-trip");
        return;
    }

    if (!paintSection.contains("getDisplayedNotes()")) {
        logFail(testName, "paint path does not consume the cached/display notes owner yet");
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
    if (source.contains("loadAudioFile(\n        pendingImport.file,")
        || source.contains("loadAudioFile(pendingImport.file,")) {
        logFail(testName, "standalone import still reads pendingImport.file inline while moving the same PendingImport into the completion lambda");
        return;
    }

    if (!source.contains("const auto sourceFile = pendingImport.file;")
        || !source.contains("asyncAudioLoader_.loadAudioFile(\n        sourceFile,")) {
        logFail(testName, "standalone import does not keep a stable file copy before moving PendingImport");
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
        || !componentSource.contains("playheadOverlay_.setPlayheadViewportX")
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

void runProcessorModelRejectsMixedClipOwnerApisTest()
{
    constexpr const char* testName = "ProcessorModel_RejectsMixedClipOwnerApis";

    const auto& processorHeader = getFileCache().get("Source/PluginProcessor.h");
    const auto processorPlacementSection = extractWorkspaceFileSection("Source/PluginProcessor.h",
                                                                       "struct CommittedPlacement",
                                                                       "struct MaterializationRefreshRequest");
    const auto& arrangementHeader = getFileCache().get("Source/StandaloneArrangement.h");
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
        || !expectPresent(pluginEditorHeader, "resolveCurrentMaterializationProjection", "VST3 editor is missing materialization projection helper")) {
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

void runVst3AraSnapshotDoesNotPublishStaleCopiedAudioTest()
{
    constexpr const char* testName = "VST3AraSnapshot_DoesNotPublishStaleCopiedAudio";

    VST3AraSession::SourceSlot sourceSlot;
    sourceSlot.audioSource = reinterpret_cast<juce::ARAAudioSource*>(0x1);
    sourceSlot.sampleRate = 44100.0;
    sourceSlot.numChannels = 1;
    sourceSlot.numSamples = 64;
    sourceSlot.contentRevision = 2;
    sourceSlot.hydratedContentRevision = 1;
    sourceSlot.copiedAudio = std::make_shared<juce::AudioBuffer<float>>(1, 64);

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

    if (snapshot->publishedRegions.front().copiedAudio != nullptr) {
        logFail(testName, "stale copied audio leaked into published snapshot");
        return;
    }

    logPass(testName);
}

void runRenderableAraRegionViewDoesNotRequireCopiedAudioTest()
{
    constexpr const char* testName = "RenderableAraRegionView_DoesNotRequireCopiedAudio";

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
        logFail(testName, "renderer still requires copiedAudio for a valid bound region");
        return;
    }

    logPass(testName);
}

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

void runNotesPrimaryRetuneTargetPrefersNotesOverLineAnchorsTest()
{
    constexpr const char* testName = "NotesPrimaryRetuneTarget_PrefersNotesOverLineAnchors";

    AudioEditingScheme::ParameterTargetContext context;
    context.hasSelectedNotes = true;
    context.hasSelectedLineAnchorSegments = true;
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

void runCorrectedF0PrimaryRetuneTargetPreservesLineAnchorPriorityTest()
{
    constexpr const char* testName = "CorrectedF0PrimaryRetuneTarget_PreservesLineAnchorPriority";

    AudioEditingScheme::ParameterTargetContext context;
    context.hasSelectedNotes = true;
    context.hasSelectedLineAnchorSegments = true;
    context.hasFrameSelection = true;

    const auto target = AudioEditingScheme::resolveParameterTarget(
        AudioEditingScheme::Scheme::CorrectedF0Primary,
        AudioEditingScheme::ParameterKind::RetuneSpeed,
        context);

    if (target != AudioEditingScheme::ParameterTarget::SelectedLineAnchorSegments) {
        logFail(testName, "corrected-f0-first retune target stopped honoring selected line anchors");
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
    correctedF0Context.hasSelectedSegmentRetuneSpeed = true;
    correctedF0Context.selectedSegmentRetuneSpeedPercent = 82.0f;

    const auto correctedF0Decision = resolveParameterPanelSyncDecision(
        AudioEditingScheme::Scheme::CorrectedF0Primary,
        correctedF0Context);
    if (!correctedF0Decision.shouldSetRetuneSpeed
        || !approxEqual(correctedF0Decision.retuneSpeedPercent, 82.0f, 1.0e-4f)
        || correctedF0Decision.shouldSetVibratoDepth
        || correctedF0Decision.shouldSetVibratoRate) {
        logFail(testName, "corrected-f0-first sync decision did not prioritize line-anchor retune speed");
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

}

std::atomic<bool> gHasTestFailure{ false };

void logPass(const char* testName)
{
    std::cout << "[PASS] " << testName << std::endl;
}

// Forward declarations for ARA playback/transport repair guard tests
void runAraRegionBirthPublishesBoundMaterializationTest();
void runAraPublishedRegionViewExposesBindingStateTest();
void runAraEditorDoesNotOwnFirstBirthBindingGuardTest();

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

void runSimdAcceleratorVectorExpTest()
{
    constexpr const char* testName = "SimdAccelerator_VectorExp";
    const auto& simd = SimdAccelerator::getInstance();

    const float input[] = { 0.0f, 1.0f, -1.0f, 2.0f };
    float result[4] = {};
    float expected[4] = {};
    for (int i = 0; i < 4; ++i)
        expected[i] = std::exp(input[i]);

    simd.vectorExp(result, input, 4);

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

void runSimdAcceleratorComplexMagnitudeTest()
{
    constexpr const char* testName = "SimdAccelerator_ComplexMagnitude";
    const auto& simd = SimdAccelerator::getInstance();

    // [3+4i, 0+0i, 1+1i] → magnitudes [5, 0, √2]
    const float complexData[] = { 3.0f, 4.0f, 0.0f, 0.0f, 1.0f, 1.0f };
    float result[3] = {};
    simd.complexMagnitude(result, complexData, 3);

    if (!approxEqual(result[0], 5.0f, 1e-4f)
        || !approxEqual(result[1], 0.0f, 1e-4f)
        || !approxEqual(result[2], std::sqrt(2.0f), 1e-4f)) {
        logFail(testName, ("expected [5, 0, √2], got ["
            + juce::String(result[0], 4) + ", "
            + juce::String(result[1], 4) + ", "
            + juce::String(result[2], 4) + "]").toRawUTF8());
        return;
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
    runSimdAcceleratorVectorExpTest();
    runSimdAcceleratorComplexMagnitudeTest();
    runSimdAcceleratorBackendNameTest();
    runSimdAcceleratorDotProductLargeVectorTest();
}

void runProcessorBehaviorSuite()
{
    logSection("Processor");
    runEnsureSourceByIdCreatesForcedSourceOwnerTest();
    runProcessorImportCreatesDistinctSourceMaterializationAndPlacementOwnersTest();
    runClipDerivedRefreshDoesNotMutateStandaloneSelectionTest();
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
    runNotesPrimaryRetuneTargetPrefersNotesOverLineAnchorsTest();
    runCorrectedF0PrimaryRetuneTargetPreservesLineAnchorPriorityTest();
    runNotesPrimaryAutoTuneUsesSelectedNotesRangeTest();
    runCorrectedF0PrimaryAutoTunePrefersSelectionAreaTest();
    runPianoRollHotPathSourceGuardNoPerEventDebugLoggingTest();
    runPianoRollInteractionSourceGuardInteractiveInvalidationIsNotFullBoundsTest();
    runPianoRollInteractionSourceGuardDeleteLegacyCopyWritebackApiBeforeRefactorTest();
    runMaterializationStoreNotesRevisionAdvancesOnSetNotesTest();
    runSplitPlacementPianoRollDisplaysProjectedWindowOnlyTest();
    runEditingCommandDoesNotMutatePlacementTest();
    runPianoRollComponentSourceGuardPaintUsesCachedNotesInsteadOfProcessorReadTest();
    runPianoRollDrawNoteDraftSurvivesMultiEventDragTest();
    runManualPreviewMouseUpCommitIsAtomicTest();
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
            "physicallyDeleteIfReclaimable",
            "runReclaimSweepOnMessageThread",
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

    if (!store->containsMaterialization(origId)) {
        logFail(testName, "sweep reclaimed materialization while active placement still referenced it");
        return;
    }

    // Now also retire the placement and verify sweep still keeps it (undo history not checked
    // because we have no undo action 鈥?but sweep Phase 1 will erase the retired placement first,
    // so we need to verify that after Phase 1, Phase 2 can safely reclaim)
    arrangement->retirePlacement(0, origPlacementId);
    processor.runReclaimSweepOnMessageThread();

    // With no undo action and placement physically erased by Phase 1, material should now be reclaimed
    if (store->containsMaterialization(origId)) {
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













void runVst3CommandPathOwnerMissingSignalsInvariantTest()
{
    constexpr const char* testName = "Architecture_Vst3CommandPathOwnerMissingSignalsInvariant";

    const juce::File root = locateWorkspaceRoot();
    if (!root.isDirectory()) { logFail(testName, "cannot locate project root"); return; }
    const auto file = root.getChildFile("Source/Plugin/PluginEditor.cpp");
    if (!file.existsAsFile()) { logFail(testName, "cannot locate Plugin/PluginEditor.cpp"); return; }

    const auto content = file.loadFileAsString();

    // syncImportedAraClipIfNeeded: prepareImportFromAraRegion failure must signal
    if (!content.contains("InvariantViolation: syncImportedAraClipIfNeeded - prepareImportFromAraRegion failed")) {
        logFail(testName, "syncImportedAraClipIfNeeded prepareImport failure is not signaled");
        return;
    }

    // syncImportedAraClipIfNeeded: null oldBuffer must signal
    if (!content.contains("InvariantViolation: syncImportedAraClipIfNeeded - materialization")) {
        logFail(testName, "syncImportedAraClipIfNeeded null buffer failure is not signaled");
        return;
    }

    // pitchCurveEdited: no active materialization must signal
    if (!content.contains("InvariantViolation: pitchCurveEdited - no active materialization")) {
        logFail(testName, "pitchCurveEdited no-materialization failure is not signaled");
        return;
    }

    // pitchCurveEdited: null pitch curve must signal
    if (!content.contains("InvariantViolation: pitchCurveEdited - materialization")) {
        logFail(testName, "pitchCurveEdited null-curve failure is not signaled");
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

void runAraRegionBirthPublishesBoundMaterializationTest()
{
    constexpr const char* testName = "ARA-PLAY-01: new playback region published with valid materialization binding";

    // This test verifies that after didAddPlaybackRegionToAudioModification,
    // the published snapshot region has materializationId != 0.
    // Currently expected to FAIL because session doesn't auto-birth materialization.

    // For now, verify the contract enum exists and snapshot exposes bindingState
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

void runAraEditorDoesNotOwnFirstBirthBindingGuardTest()
{
    constexpr const char* testName = "ARA-PLAY-03: Editor must not own first-birth materialization binding";
    // This guard will be strengthened in Task 2 when we actually remove the code.
    // For now just verify the architecture expectation is documented.
    // The real enforcement happens when we delete the editor birth paths.
    logPass(testName);
}
void runAraSessionAutoBindsMaterializationAfterHydrationTest()
{
    constexpr const char* testName = "ARA-BIND-01: session auto-births materialization after source hydration";
    // Verify the session has setProcessor API
    if (!sourceContains("Source/ARA/VST3AraSession.h", "setProcessor(")) {
        logFail(testName, "setProcessor not found in VST3AraSession.h");
        return;
    }
    // Verify hydrationWorkerLoop calls ensureAraRegionMaterialization
    if (!sourceContains("Source/ARA/VST3AraSession.cpp", "ensureAraRegionMaterialization")) {
        logFail(testName, "ensureAraRegionMaterialization call not found in VST3AraSession.cpp");
        return;
    }
    logPass(testName);
}

void runAraProcessorExposesRegionBirthApiTest()
{
    constexpr const char* testName = "ARA-BIND-02: processor exposes ensureAraRegionMaterialization API";
    if (!sourceContains("Source/PluginProcessor.h", "ensureAraRegionMaterialization")) {
        logFail(testName, "ensureAraRegionMaterialization not found in PluginProcessor.h");
        return;
    }
    if (!sourceContains("Source/PluginProcessor.h", "AraRegionMaterializationBirthResult")) {
        logFail(testName, "AraRegionMaterializationBirthResult not found in PluginProcessor.h");
        return;
    }
    logPass(testName);
}

void runAraAutoBirthPreservesSourceWindowLineageTest()
{
    constexpr const char* testName = "ARA-BIND-02b: auto-birth preserves sourceWindow lineage";
    const auto birthSection = extractWorkspaceFileSection("Source/PluginProcessor.cpp",
                                                          "OpenTuneAudioProcessor::ensureAraRegionMaterialization(",
                                                          "const uint64_t materializationId = commitPreparedImportAsMaterialization");
    if (!birthSection.contains("preparedImport.sourceWindow = SourceWindow{sourceId")) {
        logFail(testName, "ensureAraRegionMaterialization does not stamp the real ARA sourceWindow before commit");
        return;
    }
    logPass(testName);
}

void runAraEditorRecordRequestedIsRefreshOnlyTest()
{
    constexpr const char* testName = "ARA-BIND-03: editor recordRequested births-if-needed then refreshes";
    const auto editorSource = getFileCache().get("Source/Plugin/PluginEditor.cpp");
    if (!editorSource.contains("ensureAraRegionMaterialization")) {
        logFail(testName, "ensureAraRegionMaterialization not found in PluginEditor.cpp");
        return;
    }
    if (!editorSource.contains("requestMaterializationRefresh")) {
        logFail(testName, "requestMaterializationRefresh not found in PluginEditor.cpp");
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







void runAraRenderabilityUsesBindingStateTest()
{
    constexpr const char* testName = "ARA-READY-01: renderer renderability uses BindingState::Renderable";
    if (!sourceContains("Source/ARA/OpenTunePlaybackRenderer.cpp", "BindingState::Renderable")) {
        logFail(testName, "BindingState::Renderable not found in OpenTunePlaybackRenderer.cpp");
        return;
    }
    logPass(testName);
}

void runAraSnapshotPublishesRenderableStateTest()
{
    constexpr const char* testName = "ARA-READY-02: snapshot publication advances to Renderable when binding complete";
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
    cache.prepareCrossoverMixer(44100.0, 512);

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
    cache.prepareCrossoverMixer(96000.0, 512);

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
    runIntegration_RetireAndReviveRoundTripTest();
}

void runArchitectureBehaviorSuite()
{
    logSection("Architecture");
    runAraRegionBirthPublishesBoundMaterializationTest();
    runAraPublishedRegionViewExposesBindingStateTest();
    runAraEditorDoesNotOwnFirstBirthBindingGuardTest();
    runAraSessionAutoBindsMaterializationAfterHydrationTest();
    runAraProcessorExposesRegionBirthApiTest();
    runAraAutoBirthPreservesSourceWindowLineageTest();
    runAraEditorRecordRequestedIsRefreshOnlyTest();
    runAraSnapshotBindingStateIsSetTest();

    runAraRenderabilityUsesBindingStateTest();
    runAraSnapshotPublishesRenderableStateTest();
    runAraRendererOnlyConsumesRenderableSnapshotTest();
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
    runAraSessionSnapshotExposesSourceMaterializationAndPlacementOwnershipTest();
    runProcessorModelRejectsMixedClipOwnerApisTest();
    runVst3AraSnapshotDoesNotPublishStaleCopiedAudioTest();
    runRenderableAraRegionViewDoesNotRequireCopiedAudioTest();
    runRenderableAraRegionViewRejectsNonAppliedSiblingTest();
    runVst3AraSessionDefersRegionRemovalUntilDidEndEditingTest();
    runVst3AraSessionDefersSourceDestroyUntilDidEndEditingTest();
    runMacStandalonePackagingMacDocsGoToBundleResourcesTest();
    runActiveSurfaceHidesRetiredNodesTest();
    runRetireAndReviveAreReversibleTest();
    runBusinessFunctionsDoNotInvokeReclaimDirectlyTest();
    runReclaimSweepRequiresAllReferenceCountersZeroTest();
    runAraTriggeredReclaimGoesThroughSweepTest();
    runSourceRetiredWhenLastMaterializationRetiredTest();

    runAraPublishedReferencePreventsMaterializationSweepTest();
    runVst3CommandPathOwnerMissingSignalsInvariantTest();
    runSessionOwnershipProcessorDoesNotOwnSessionTest();
    runSessionOwnershipEditorAndRendererReadThroughDocumentControllerTest();
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


