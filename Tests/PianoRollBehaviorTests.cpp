#include "../Source/Utils/PianoRollEditAction.h"
#include "../Source/Utils/UndoManager.h"
#include "../Source/Standalone/UI/PianoRoll/InteractionState.h"
#include "../Source/Content/EditableContentSnapshot.h"
#include "../Source/Content/CaptureSegmentContent.h"
#include "../Source/Utils/TimeGrid.h"
#include "../Source/PluginProcessor.h"
#include "../Source/Standalone/UI/ViewMapper.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef OPENTUNE_SOURCE_DIR
#error "OPENTUNE_SOURCE_DIR must be defined by CMake"
#endif

#ifndef OPENTUNE_BINARY_DIR
#error "OPENTUNE_BINARY_DIR must be defined by CMake"
#endif

using namespace OpenTune;

namespace {

int failures = 0;

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        ++failures;
        std::cout << "[FAIL] " << message << "\n";
    }
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

std::filesystem::path binaryPath(std::string_view relative)
{
    return std::filesystem::path(OPENTUNE_BINARY_DIR) / std::filesystem::path(relative);
}

std::string readBinaryText(std::string_view relative)
{
    const auto path = binaryPath(relative);
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

std::string extractBracedBlock(std::string_view text, size_t markerPos)
{
    if (markerPos == std::string_view::npos)
        return {};

    const size_t bracePos = text.find('{', markerPos);
    if (bracePos == std::string_view::npos)
        return {};

    int depth = 0;
    for (size_t i = bracePos; i < text.size(); ++i) {
        if (text[i] == '{') {
            ++depth;
        } else if (text[i] == '}') {
            --depth;
            if (depth == 0)
                return std::string(text.substr(markerPos, i - markerPos + 1));
        }
    }
    return {};
}

std::string extractBlockByMarker(std::string_view text, std::string_view marker)
{
    return extractBracedBlock(text, text.find(marker));
}

void expectTokens(std::string_view blockName,
                  std::string_view text,
                  std::initializer_list<std::string_view> tokens)
{
    for (const auto token : tokens)
        expect(contains(text, token), std::string(blockName) + " missing token: " + std::string(token));
}

void expectNoTokens(std::string_view blockName,
                    std::string_view text,
                    std::initializer_list<std::string_view> tokens)
{
    for (const auto token : tokens)
        expect(!contains(text, token), std::string(blockName) + " contains forbidden token: " + std::string(token));
}

size_t countOccurrences(std::string_view text, std::string_view token)
{
    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(token, position)) != std::string_view::npos) {
        ++count;
        position += token.size();
    }
    return count;
}

void pianoRollPendingSeekPresentationSourceContract()
{
    const auto source = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto header = readText("Source/Standalone/UI/PianoRollComponent.h");
    const auto notifyBlock = extractBlockByMarker(
        source, "toolCtx.notifyPlayheadChange = [this](double time)");
    const auto heartbeatBlock = extractBlockByMarker(
        source, "void PianoRollComponent::onHeartbeatTick()");
    const auto vblankBlock = extractBlockByMarker(
        source, "void PianoRollComponent::onScrollVBlankCallback(double timestampSec)");
    const auto hiddenVBlankBlock = extractBlockByMarker(
        vblankBlock, "if (!isShowing())");

    expectTokens("PianoRoll notify playhead request",
                 notifyBlock,
                 {"const auto seekRevision = playHeadState_.hostPositionRevision.load",
                  "bool requestDispatched = false;",
                  "if (l.playheadPositionChangeRequested(time))",
                  "seekSentRevision_ = seekRevision;"});
    expectTokens("PianoRoll accepted seek presentation",
                 notifyBlock,
                 {"playheadTimeForPaint_ = time;", "pendingSeekTime_ = time;"});
    expectTokens("PianoRoll rejected seek presentation",
                 notifyBlock,
                 {"pendingSeekTime_ = -1.0;",
                  "seekSentRevision_ = 0;",
                  "playheadTimeForPaint_ = playHeadState_.timeInSeconds.load"});
    expectTokens("PianoRoll heartbeat revision confirmation",
                   heartbeatBlock,
                   {"playHeadState_.hostPositionRevision.load",
                    "hostRevision != seekSentRevision_",
                    "pendingSeekTime_ = -1.0;"});
    expectTokens("PianoRoll VBlank revision confirmation",
                 vblankBlock,
                 {"playHeadState_.hostPositionRevision.load",
                  "hostRevision != seekSentRevision_",
                  "pendingSeekTime_ = -1.0;"});
    expectTokens("PianoRoll state binding",
                 header,
                 {"PianoRollComponent(const PlayHeadState& playHeadState);",
                  "const PlayHeadState& playHeadState_;",
                  "uint64_t seekSentRevision_{0};"});
    expectNoTokens("PianoRoll revision ownership",
                   source,
                   {"hostPositionRevision.fetch_add", "hostPositionRevision.store"});
    expectNoTokens("PianoRoll pending seek tolerance",
                   source,
                   {"std::abs(currentPlayheadTime - pendingSeekTime_)", "setTimeout", "retry"});
    expectNoTokens("PianoRoll hidden VBlank path",
                   hiddenVBlankBlock,
                   {"pendingSeekTime_ = -1.0;"});
    expectNoTokens("PianoRoll retained playhead architecture",
                   source,
                   {"playheadOverlay_", "PlayheadOverlayComponent"});
}

void pianoRollPlayheadPaintsDirectly()
{
    const auto source = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto notifyBlock = extractBlockByMarker(
        source, "toolCtx.notifyPlayheadChange = [this](double time)");
    const auto drawPlayheadBlock = extractBlockByMarker(
        source, "void PianoRollComponent::drawPlayhead(juce::Graphics& g)");

    expectTokens("PianoRoll direct playhead draw",
                 drawPlayheadBlock,
                 {"g.reduceClipRegion(timeAxisRect())"});
    expectTokens("PianoRoll direct playhead notify repaint",
                 notifyBlock,
                 {"repaint();"});
}

void pluginEditorPlayheadRequestSourceContract()
{
    const auto source = readText("Source/Plugin/PluginEditor.cpp");
    const auto functionBlock = extractBlockByMarker(
        source, "bool OpenTuneAudioProcessorEditor::playheadPositionChangeRequested(double timeSeconds)");

    expectTokens("Plugin editor ARA playhead request",
                 functionBlock,
                 {"return docController->requestSetPlaybackPosition(timeSeconds);"});
    expectNoTokens("Plugin editor ARA playhead request",
                   functionBlock,
                   {"processorRef_.setPosition(timeSeconds);"});
    expectTokens("Plugin editor host-controlled playhead",
                 functionBlock,
                 {"Non-ARA VST3: playhead is host-controlled only.",
                  "juce::ignoreUnused(timeSeconds);",
                  "return false;"});
}

void standalonePlayheadRequestSourceContract()
{
    const auto source = readText("Source/Standalone/PluginEditor.cpp");
    const auto functionBlock = extractBlockByMarker(
        source, "bool OpenTuneAudioProcessorEditor::playheadPositionChangeRequested(double timeSeconds)");

    expectTokens("Standalone playhead request",
                  functionBlock,
                  {"processorRef_.setPosition(timeSeconds);", "return false;"});
}

void pluginEditorPlayheadPositionBindingSourceContract()
{
    const auto header = readText("Source/Plugin/PluginEditor.h");
    const auto source = readText("Source/Plugin/PluginEditor.cpp");
    expectTokens("Plugin editor PlayHeadState construction",
                 source,
                 {"pianoRoll_(processor.getPlayHeadState())"});
    expectNoTokens("Plugin editor ARA presentation mirror removed",
                   header + source,
                   {"araPresentationPlayHeadState_",
                    "getHostTransportObservation",
                    "setPlayheadPositionSource",
                    "getPositionAtomic",
                    "setIsPlaying"});
}

void standalonePlayheadPositionBindingSourceContract()
{
    const auto source = readText("Source/Standalone/PluginEditor.cpp");
    const auto pianoRollHeader = readText("Source/Standalone/UI/PianoRollComponent.h");
    const auto arrangementHeader = readText("Source/Standalone/UI/ArrangementViewComponent.h");
    expectTokens("Standalone editor PlayHeadState construction",
                 source,
                 {"arrangementView_(p)", "pianoRoll_(p.getPlayHeadState())"});
    expectTokens("Standalone Arrangement PlayHeadState construction",
                 arrangementHeader,
                 {"const PlayHeadState& playHeadState_"});
    expectNoTokens("Standalone editor legacy playhead binding",
                   source + pianoRollHeader + arrangementHeader,
                   {"setPlayheadPositionSource", "getPositionAtomic", "setIsPlaying", "positionSource_"});
}

void processorOwnedPlayHeadStateContract()
{
    const auto header = readText("Source/PluginProcessor.h");
    const auto source = readText("Source/PluginProcessor.cpp");
    const auto stateBlock = extractBlockByMarker(header, "struct PlayHeadState");

    expectTokens("PlayHeadState declaration",
                 stateBlock,
                 {"std::atomic<bool>    isPlaying { false };",
                  "std::atomic<bool>    isLooping { false };",
                  "std::atomic<double>  timeInSeconds { 0.0 };",
                  "std::atomic<double>  loopPpqStart { 0.0 };",
                  "std::atomic<double>  loopPpqEnd { 0.0 };",
                  "std::atomic<uint64_t> hostPositionRevision { 0 };",
                  "void update(const juce::Optional<juce::AudioPlayHead::PositionInfo>& info)",
                  "void reset()"});
    expectTokens("PlayHeadState nullopt contract",
                 stateBlock,
                 {"if (!info.hasValue())", "return;"});
    expectTokens("PlayHeadState valid observation contract",
                 stateBlock,
                 {"isPlaying.store(positionInfo.getIsPlaying()",
                  "isLooping.store(positionInfo.getIsLooping()",
                  "if (const auto timeSeconds = positionInfo.getTimeInSeconds())",
                  "hostPositionRevision.fetch_add(1"});
    expectNoTokens("PlayHeadState missing time fallback",
                   stateBlock,
                   {"getTimeInSeconds().orFallback(0.0)", "timeInSeconds.store(0.0"});
    expectTokens("PlayHeadState reset contract",
                 stateBlock,
                 {"isPlaying.store(false", "isLooping.store(false"});
    expectTokens("processor single state owner",
                 header,
                 {"PlayHeadState playHeadState_;",
                  "const PlayHeadState& getPlayHeadState() const noexcept"});

    const auto processBlock = extractBlockByMarker(
        source, "void OpenTuneAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer");
    expect(countOccurrences(processBlock, "hostPlayHead->getPosition()") == 1,
           "VST3/ARA processBlock must read host PositionInfo exactly once");
    expectTokens("processBlock canonical observation order",
                 processBlock,
                 {"juce::Optional<juce::AudioPlayHead::PositionInfo> hostPosOpt",
                  "playHeadState_.update(hostPosOpt)",
                  "updateHostTransportSnapshot(araPositionInfo)",
                  "processBlockForARA(buffer, isRealtime(), araPositionInfo)"});
    expectNoTokens("processBlock legacy position fallback",
                   processBlock,
                   {"getDocumentController()->updateTransport",
                    "publishHostTransportObservation",
                    "getHostTransportObservation",
                    "positionAtomic_",
                    "getPositionAtomic",
                    "orFallback(0.0)"});
    expectTokens("processor lifecycle reset",
                 source,
                 {"void OpenTuneAudioProcessor::prepareToPlay",
                  "void OpenTuneAudioProcessor::releaseResources",
                  "playHeadState_.reset();"});
}

void sharedCodeRuntimeWrapperTypeDispatchContract()
{
    // OpenTune_SharedCode is compiled with BOTH JucePlugin_Build_Standalone=1
    // and JucePlugin_Build_VST3=1 simultaneously, so shared source must never
    // gate host playhead / host transport reads on the compile-time
    // JucePlugin_Build_Standalone macro -- a compile-time branch would excise
    // the VST3 host-read path from the linked object. Dispatch must be runtime
    // via AudioProcessor::wrapperType.
    const auto header = readText("Source/PluginProcessor.h");
    const auto source = readText("Source/PluginProcessor.cpp");

    const auto processBlock = extractBlockByMarker(
        source, "void OpenTuneAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer");
    expectNoTokens("processBlock must not gate host read on JucePlugin_Build_Standalone macro",
                   processBlock,
                   {"#if !JucePlugin_Build_Standalone",
                    "#if JucePlugin_Build_Standalone"});
    expectTokens("processBlock runtime VST3 wrapperType host-read dispatch",
                 processBlock,
                 {"wrapperType == juce::AudioProcessor::wrapperType_VST3",
                  "hostPlayHead->getPosition()"});

    const auto getBpmBlock = extractBlockByMarker(header, "double getBpm() const");
    const auto getNumBlock = extractBlockByMarker(header, "int getTimeSigNumerator() const");
    const auto getDenBlock = extractBlockByMarker(header, "int getTimeSigDenominator() const");

    expectNoTokens("getBpm must not gate host read on JucePlugin_Build_Standalone macro",
                   getBpmBlock,
                   {"#if !JucePlugin_Build_Standalone", "#if JucePlugin_Build_Standalone"});
    expectNoTokens("getTimeSigNumerator must not gate host read on JucePlugin_Build_Standalone macro",
                   getNumBlock,
                   {"#if !JucePlugin_Build_Standalone", "#if JucePlugin_Build_Standalone"});
    expectNoTokens("getTimeSigDenominator must not gate host read on JucePlugin_Build_Standalone macro",
                   getDenBlock,
                   {"#if !JucePlugin_Build_Standalone", "#if JucePlugin_Build_Standalone"});

    expectTokens("getBpm runtime VST3 wrapperType dispatch",
                 getBpmBlock,
                 {"wrapperType == juce::AudioProcessor::wrapperType_VST3",
                  "getHostTransportSnapshot().bpm",
                  "return bpm_"});
    expectTokens("getTimeSigNumerator runtime VST3 wrapperType dispatch",
                 getNumBlock,
                 {"wrapperType == juce::AudioProcessor::wrapperType_VST3",
                  "getHostTransportSnapshot().timeSignatureNumerator",
                  "return 4"});
    expectTokens("getTimeSigDenominator runtime VST3 wrapperType dispatch",
                 getDenBlock,
                 {"wrapperType == juce::AudioProcessor::wrapperType_VST3",
                  "getHostTransportSnapshot().timeSignatureDenominator",
                  "return 4"});
}

void documentControllerTransportBoundaryContract()
{
    const auto header = readText("Source/ARA/OpenTuneDocumentController.h");
    const auto source = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto processorHeader = readText("Source/PluginProcessor.h");

    expectNoTokens("DocumentController transport mirror removal",
                   header + source,
                   {"playbackPositionSource_", "playbackIsPlaying_",
                    "getPlaybackPositionSource", "getPlaybackPosition", "updateTransport",
                    "HostTransportObservation", "publishHostTransportObservation",
                    "getHostTransportObservation"});
    expectTokens("ARA one-way transport requests",
                 header + source,
                 {"requestSetPlaybackPosition", "requestStartPlayback", "requestStopPlayback",
                  "requestEnableCycle", "requestSetCycleRange",
                  "playbackController->requestSetPlaybackPosition",
                  "playbackController->requestStartPlayback",
                  "playbackController->requestStopPlayback",
                  "playbackController->requestEnableCycle",
                  "playbackController->requestSetCycleRange"});
    expectNoTokens("ARA document transport observation mirror removed",
                   header + source,
                   {"HostTransportObservation", "publishHostTransportObservation",
                    "getHostTransportObservation",
                    "SourceRole::EditorRenderer", "SourceRole::PlaybackRenderer",
                    "revision.fetch_add(1, std::memory_order_acq_rel)",
                    "editorRendererObserving_", "clearEditorRendererObservation",
                    "staleThreshold", "setTimeout", "retry"});

    const auto snapshotBlock = extractBlockByMarker(processorHeader, "struct HostTransportSnapshot");
    expectNoTokens("HostTransportSnapshot loop ownership removal",
                   snapshotBlock,
                   {"loopEnabled", "loopPpqStart", "loopPpqEnd"});
}

void transportProductionZeroResidueContract()
{
    const auto processorHeader = readText("Source/PluginProcessor.h");
    const auto processorSource = readText("Source/PluginProcessor.cpp");
    const auto pluginEditorHeader = readText("Source/Plugin/PluginEditor.h");
    const auto pluginEditorSource = readText("Source/Plugin/PluginEditor.cpp");
    const auto araRendererHeader = readText("Source/ARA/OpenTunePlaybackRenderer.h");
    const auto araRendererSource = readText("Source/ARA/OpenTunePlaybackRenderer.cpp");
    const auto araDocumentControllerHeader = readText("Source/ARA/OpenTuneDocumentController.h");
    const auto araDocumentControllerSource = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto standaloneEditorSource = readText("Source/Standalone/PluginEditor.cpp");
    const auto pianoRollHeader = readText("Source/Standalone/UI/PianoRollComponent.h");
    const auto pianoRollSource = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto arrangementHeader = readText("Source/Standalone/UI/ArrangementViewComponent.h");
    const auto arrangementSource = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");

    const auto production = processorHeader + processorSource
        + pluginEditorHeader + pluginEditorSource
        + araRendererHeader + araRendererSource
        + araDocumentControllerHeader + araDocumentControllerSource
        + standaloneEditorSource
        + pianoRollHeader + pianoRollSource
        + arrangementHeader + arrangementSource;

    expectNoTokens("production transport zero residue",
                   production,
                   {"playbackPositionSource_", "playbackIsPlaying_",
                    "getPlaybackPositionSource", "getPlaybackPosition", "updateTransport",
                    "positionAtomic_", "getPositionAtomic", "hostTransportLoopEnabled_",
                    "hostTransportLoopPpqStart_", "hostTransportLoopPpqEnd_",
                    "setPlayheadPositionSource", "PianoRollComponent::setIsPlaying",
                    "ArrangementViewComponent::setIsPlaying", "PianoRollComponent::isPlaying_",
                    "ArrangementViewComponent::isPlaying_", "positionSource_",
                    "std::abs(currentPlayheadTime - pendingSeekTime_)",
                    "HostTransportObservation", "publishHostTransportObservation",
                    "getHostTransportObservation",
                    "observationTimeSeconds", "observationValid", "observationIsPlaying",
                    "orFallback(0.0)"});
    expectNoTokens("VST3 editor local transport writes",
                   pluginEditorSource,
                   {"processorRef_.setLoopEnabled(", "processorRef_.setPlaying(",
                    "pianoRoll_.setIsPlaying(", "arrangementView_.setIsPlaying("});
    expectTokens("ARA loop request path",
                 pluginEditorSource,
                 {"docController->requestEnableCycle(enabled)"});
    expectTokens("Standalone loop owner path",
                 standaloneEditorSource,
                 {"processorRef_.setLoopEnabled(enabled);"});
    expectTokens("ARA renderer PositionInfo-driven readiness",
                 araRendererSource,
                 {"shouldRenderAraPlaybackBlock(juce::AudioProcessor::Realtime realtime,",
                  "positionInfo.getIsPlaying()",
                  "const auto positionTime = positionInfo.getTimeInSeconds();",
                  "if (!positionTime)",
                  "const double blockStartSeconds = *positionTime"});
    expectNoTokens("ARA renderer has no zero time fallback",
                   araRendererSource,
                   {"orFallback(0.0)", "positionInfo.getTimeInSeconds().orFallback",
                    "getBpm()"});
    const auto stopBlock = extractBlockByMarker(
        pluginEditorSource, "void OpenTuneAudioProcessorEditor::stopRequested()");
    expectTokens("ARA stop request",
                 stopBlock,
                  {"docController->requestStopPlayback()"});
    expectNoTokens("ARA stop request must not imply seek",
                    stopBlock,
                    {"requestSetPlaybackPosition(0.0)"});
}

void araContentProjectionContract()
{
    const auto dcHeader = readText("Source/ARA/OpenTuneDocumentController.h");
    const auto dcSource = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto editorSource = readText("Source/Plugin/PluginEditor.cpp");
    const auto rendererSource = readText("Source/ARA/OpenTunePlaybackRenderer.cpp");

    expectTokens("PlaybackRegionProjection readiness fields",
                 dcHeader,
                 {"bool playbackSourceReady{false};",
                  "bool isPlaybackRenderable() const noexcept;"});
    expectTokens("makeProjection content-state gate and source readiness",
                 dcSource,
                 {"!modification->hasContentState()",
                  "projection.contentKey",
                  "projection.playbackSourceReady = modification->isRenderable()"});
    expectNoTokens("makeProjection must not gate on isRenderable()",
                   dcSource,
                   {"!modification->isRenderable()"});
    expectTokens("processDocumentRenderJob preserves isRenderable() gate",
                 dcSource,
                 {"!mod->isRenderable()"});
    expectNoTokens("ARA content projection no legacy observation tokens",
                   dcHeader + dcSource + editorSource + rendererSource,
                   {"HostTransportObservation", "getHostTransportObservation",
                    "publishHostTransportObservation", "observationTimeSeconds",
                    "observationValid", "observationIsPlaying"});
}

void araUiReadinessContract()
{
    const auto editorSource = readText("Source/Plugin/PluginEditor.cpp");
    const auto syncBlock = extractBlockByMarker(
        editorSource, "void OpenTuneAudioProcessorEditor::syncContentProjectionToPianoRoll()");

    expectTokens("ARA UI sync block drives PianoRoll from projection",
                 syncBlock,
                 {"pianoRoll_.setEditedContent(sync.activeContentKey",
                  "syncBuffer",
                  "pianoRoll_.setTimelineContentPlacements(sync.placements)"});

    // A missing PCM buffer must not clear the active key. The only ContentKey{}
    // clears are the genuine no-placement and no-active-content states above
    // the PCM lookup.
    const auto noBufferBlock = extractBlockByMarker(syncBlock, "if (syncBuffer == nullptr)");
    expectTokens("syncBuffer-null preserves projection state", noBufferBlock, {"return;"});
    expectNoTokens("syncBuffer-null must not clear active content",
                   noBufferBlock,
                   {"setEditedContent(ContentKey{},"});

    // No artificial ARA wait gating forcing the render overlay.
    expectNoTokens("ARA UI no artificial wait state",
                   editorSource,
                   {"waitingForAraContent_",
                    "araWaitStartMs_",
                    "5000"});
    expectNoTokens("ARA UI recordRequested must not force overlay",
                   editorSource,
                   {"autoRenderOverlay_.setVisible(true)"});
}

void rendererReadinessPositionInfoContract()
{
    const auto rendererHeader = readText("Source/ARA/OpenTunePlaybackRenderer.h");
    const auto rendererSource = readText("Source/ARA/OpenTunePlaybackRenderer.cpp");

    expectTokens("ARA renderer builds plan from projection readiness",
                 rendererSource,
                 {"projection.isPlaybackRenderable()"});
    expectTokens("ARA renderer PositionInfo-driven block gate",
                 rendererSource,
                 {"shouldRenderAraPlaybackBlock(realtime, positionInfo.getIsPlaying())"});
    expectNoTokens("ARA renderer no legacy observation / revision / source role tokens",
                   rendererHeader + rendererSource,
                   {"getHostTransportObservation",
                    "HostTransportObservation",
                    "revision",
                    "sourceRole",
                    "SourceRole",
                    "observationTimeSeconds"});
}

void captureBoundaryContract()
{
    const auto pluginEditorSource = readText("Source/Plugin/PluginEditor.cpp");
    const auto standaloneEditorSource = readText("Source/Standalone/PluginEditor.cpp");

    expectTokens("regular VST3 Capture boundary: arm/stop via session",
                 pluginEditorSource,
                 {"session->armNewCapture()",
                  "session->stopCapture()",
                  "getCaptureSession()"});
    expectTokens("Standalone Capture boundary: transport writes via processorRef",
                 standaloneEditorSource,
                 {"processorRef_.setPosition(timeSeconds);",
                  "processorRef_.setLoopEnabled(enabled);"});
}

void transportBarFeedbackContract()
{
    const auto transportBar = readText("Source/Standalone/UI/TransportBarComponent.cpp");
    const auto standaloneEditor = readText("Source/Standalone/PluginEditor.cpp");
    const auto playBlock = extractBlockByMarker(
        transportBar, "void TransportBarComponent::onPlayClicked()");
    const auto pauseBlock = extractBlockByMarker(
        transportBar, "void TransportBarComponent::onPauseClicked()");
    const auto stopBlock = extractBlockByMarker(
        transportBar, "void TransportBarComponent::onStopClicked()");
    const auto loopBlock = extractBlockByMarker(
        transportBar, "void TransportBarComponent::onLoopToggled()");
    const auto standaloneLoopBlock = extractBlockByMarker(
        standaloneEditor, "void OpenTuneAudioProcessorEditor::loopToggled(bool enabled)");

    expectTokens("TransportBar loop waits for transport feedback",
                 transportBar,
                 {"loopButton_.setClickingTogglesState(false);"});
    expectNoTokens("TransportBar play waits for transport feedback",
                   playBlock,
                   {"setPlaying("});
    expectNoTokens("TransportBar pause waits for transport feedback",
                   pauseBlock,
                   {"setPlaying("});
    expectNoTokens("TransportBar stop waits for transport feedback",
                   stopBlock,
                   {"setPlaying("});
    expectTokens("TransportBar loop request uses current state",
                 loopBlock,
                 {"bool enabled = !loopButton_.getToggleState();",
                  "listeners_.call([enabled](Listener& l) { l.loopToggled(enabled); });"});
    expectNoTokens("TransportBar loop does not optimistically flip state",
                   loopBlock,
                   {"setLoopEnabled("});
    expectTokens("Standalone loop keeps immediate UI behavior",
                 standaloneLoopBlock,
                 {"processorRef_.setLoopEnabled(enabled);",
                  "transportBar_.setLoopEnabled(enabled);"});
}

void araReadAudioPlaybackSourceContract()
{
    const auto controller = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto renderer = readText("Source/ARA/OpenTunePlaybackRenderer.cpp");

    expectTokens("ARA ReadAudio publishes CRS source",
                 controller,
                 {"birthContentForModification(*modification)",
                  "publishPlaybackReadSourceForModification(modification, storedAudioBuffer)",
                  "readSource.contentKey = key",
                  "contentRenderService_->publishPlaybackSource(key, readSource)"});
    expectTokens("ARA renderer consumes CRS source by ContentKey",
                 renderer,
                 {"crs->getPlaybackReadSource(region.contentKey, readSource)",
                  "readPlaybackAudio(request, playbackScratch_, 0)"});
    expectNoTokens("ARA ReadAudio remains explicit",
                   controller,
                   {"setTimeout", "retry"});
}

void playHeadStateRuntimeContract()
{
    PlayHeadState state;
    state.timeInSeconds.store(12.5, std::memory_order_relaxed);
    state.isPlaying.store(true, std::memory_order_relaxed);
    state.isLooping.store(true, std::memory_order_relaxed);
    state.loopPpqStart.store(2.0, std::memory_order_relaxed);
    state.loopPpqEnd.store(8.0, std::memory_order_relaxed);

    const auto initialRevision = state.hostPositionRevision.load(std::memory_order_relaxed);
    state.update(juce::nullopt);
    expect(state.isPlaying.load(std::memory_order_relaxed),
           "PlayHeadState nullopt must preserve isPlaying");
    expect(state.isLooping.load(std::memory_order_relaxed),
           "PlayHeadState nullopt must preserve isLooping");
    expect(state.timeInSeconds.load(std::memory_order_relaxed) == 12.5,
           "PlayHeadState nullopt must preserve timeInSeconds");
    expect(state.loopPpqStart.load(std::memory_order_relaxed) == 2.0
               && state.loopPpqEnd.load(std::memory_order_relaxed) == 8.0,
           "PlayHeadState nullopt must preserve loop points");
    expect(state.hostPositionRevision.load(std::memory_order_relaxed) == initialRevision,
           "PlayHeadState nullopt must not bump hostPositionRevision");

    juce::AudioPlayHead::PositionInfo withoutTime;
    withoutTime.setIsPlaying(false);
    withoutTime.setIsLooping(false);
    withoutTime.setLoopPoints(juce::AudioPlayHead::LoopPoints{ 3.0, 9.0 });
    state.update(juce::Optional<juce::AudioPlayHead::PositionInfo>(withoutTime));
    expect(!state.isPlaying.load(std::memory_order_relaxed),
           "PositionInfo without time must still update isPlaying");
    expect(!state.isLooping.load(std::memory_order_relaxed),
           "PositionInfo without time must still update isLooping");
    expect(state.timeInSeconds.load(std::memory_order_relaxed) == 12.5,
           "PositionInfo without time must preserve last valid time");
    expect(state.hostPositionRevision.load(std::memory_order_relaxed) == initialRevision,
           "PositionInfo without time must not bump hostPositionRevision");
    expect(state.loopPpqStart.load(std::memory_order_relaxed) == 3.0
               && state.loopPpqEnd.load(std::memory_order_relaxed) == 9.0,
           "PositionInfo without time must update available loop points");

    juce::AudioPlayHead::PositionInfo withTime;
    withTime.setTimeInSeconds(24.75);
    withTime.setIsPlaying(true);
    withTime.setIsLooping(true);
    state.update(juce::Optional<juce::AudioPlayHead::PositionInfo>(withTime));
    expect(state.isPlaying.load(std::memory_order_relaxed)
               && state.isLooping.load(std::memory_order_relaxed),
           "valid PositionInfo time must update playing and looping");
    expect(state.timeInSeconds.load(std::memory_order_relaxed) == 24.75,
           "valid PositionInfo must update timeInSeconds");
    expect(state.hostPositionRevision.load(std::memory_order_relaxed) == initialRevision + 1,
           "valid PositionInfo time must bump hostPositionRevision once");

    state.reset();
    expect(!state.isPlaying.load(std::memory_order_relaxed)
               && !state.isLooping.load(std::memory_order_relaxed),
           "PlayHeadState reset must clear playing and looping");
    expect(state.timeInSeconds.load(std::memory_order_relaxed) == 24.75,
           "PlayHeadState reset must preserve timeInSeconds");
    expect(state.loopPpqStart.load(std::memory_order_relaxed) == 3.0
               && state.loopPpqEnd.load(std::memory_order_relaxed) == 9.0,
           "PlayHeadState reset must preserve loop points");
    expect(state.hostPositionRevision.load(std::memory_order_relaxed) == initialRevision + 1,
           "PlayHeadState reset must preserve hostPositionRevision");
}

void viewMapperContinuousConversionRoundTrips()
{
    const ViewMapper mapper{
        1.25,
        37.5,
        19,
        640,
        480,
        13.75f,
        6.25f,
        118.5f};

    for (const float midi : {24.25f, 48.75f, 73.125f, 101.5f}) {
        const float roundTripMidi = mapper.freqToMidi(mapper.midiToFreq(midi));
        expect(std::abs(roundTripMidi - midi) < 1.0e-3f,
               "ViewMapper MIDI/frequency conversion must round-trip fractional MIDI values");
    }

    for (const float y : {-4.75f, 18.125f, 67.625f, 143.5f}) {
        const float roundTripY = mapper.freqToY(mapper.yToFreq(y));
        expect(std::abs(roundTripY - y) < 1.0e-3f,
               "ViewMapper Y/frequency conversion must round-trip fractional Y values");
    }
}

void unifiedViewMappingAndReadableRenderSourceContracts()
{
    const auto viewMapperSource = readText("Source/Standalone/UI/ViewMapper.h");
    expectNoTokens("ViewMapper continuous conversion source",
                   viewMapperSource,
                   {"juce::MidiMessage::getMidiNoteInHertz", "juce::roundToInt(midi"});
    expectTokens("ViewMapper continuous conversion source",
                 viewMapperSource,
                 {"std::pow(2.0f, (midi + 0.5f - 69.0f) / 12.0f)"});

    const auto pianoRollHeader = readText("Source/Standalone/UI/PianoRollComponent.h");
    expectNoTokens("PianoRoll legacy mapping declarations",
                   pianoRollHeader,
                   {"midiToY(", "yToMidi(", "freqToMidi(",
                    "midiToFreq(", "yToFreq(", "freqToY("});

    const auto pianoRollSource = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto transientOverlayBlock = extractBlockByMarker(
        pianoRollSource, "void PianoRollComponent::drawTransientOverlay(");
    expectTokens("DrawNote transient preview ViewMapper mapping",
                 transientOverlayBlock,
                 {"mapper.freqToY(pitch)"});
    expectNoTokens("DrawNote transient preview legacy mapping",
                   transientOverlayBlock,
                   {"std::log2(pitch / 440.0f)"});
    expectNoTokens("PianoRoll legacy mapping definitions",
                   pianoRollSource,
                   {"PianoRollComponent::midiToY(", "PianoRollComponent::yToMidi(",
                    "PianoRollComponent::freqToMidi(", "PianoRollComponent::midiToFreq(",
                    "PianoRollComponent::yToFreq(", "PianoRollComponent::freqToY("});
    expectTokens("PianoRoll ViewMapper call sites",
                 pianoRollSource,
                 {"makeViewMapper().yToMidi", "makeViewMapper().freqToY"});

    const auto pluginProcessorHeader = readText("Source/PluginProcessor.h");
    const auto pluginProcessorSource = readText("Source/PluginProcessor.cpp");
    expectTokens("PluginProcessor readable chunk stats API",
                 pluginProcessorHeader,
                 {"getReadableContentChunkStats"});
    expectTokens("PluginProcessor readable chunk stats API",
                  pluginProcessorSource,
                  {"getReadableContentChunkStats", "resolveReadableContentRenderService"});
    const auto readableChunkStatsBlock = extractBlockByMarker(
        pluginProcessorSource,
        "RenderCache::ChunkStats OpenTuneAudioProcessor::getReadableContentChunkStats(ContentKey key) const noexcept");
    expectTokens("PluginProcessor readable chunk stats implementation",
                 readableChunkStatsBlock,
                 {"resolveReadableContentRenderService(key)"});

    const auto araControllerHeader = readText("Source/ARA/OpenTuneDocumentController.h");
    const auto araControllerSource = readText("Source/ARA/OpenTuneDocumentController.cpp");
    expectNoTokens("ARA controller chunk stats API",
                   araControllerHeader,
                   {"readChunkStats"});
    expectNoTokens("ARA controller chunk stats API",
                   araControllerSource,
                   {"readChunkStats"});

    const auto overlayHeader = readText("Source/Editor/AutoRenderOverlayComponent.h");
    expectTokens("shared render overlay title helper",
                 overlayHeader,
                 {"buildRenderingOverlayTitle"});

    const auto standaloneEditorSource = readText("Source/Standalone/PluginEditor.cpp");
    const auto pluginEditorSource = readText("Source/Plugin/PluginEditor.cpp");
    expectNoTokens("shared render overlay title definition",
                   standaloneEditorSource,
                   {"juce::String buildRenderingOverlayTitle(int completedTasks",
                    "buildRenderingOverlayTitle(int completedTasks"});
    expectNoTokens("shared render overlay title definition",
                   pluginEditorSource,
                   {"juce::String buildRenderingOverlayTitle(int completedTasks",
                    "buildRenderingOverlayTitle(int completedTasks"});

    const auto timerBlock = extractBlockByMarker(
        pluginEditorSource, "void OpenTuneAudioProcessorEditor::timerCallback()");
    expectTokens("Plugin editor render-state timer",
                 timerBlock,
                  {"bool shouldShowOverlay = false;",
                   "isAutoProcessing",
                   "else if (chunkStats.hasActiveWork())",
                   "shouldShowBadge",
                   "getReadableContentChunkStats"});
    expectTokens("Plugin editor render-state visibility bindings",
                 timerBlock,
                 {"renderBadge_.setVisible(shouldShowBadge)",
                  "autoRenderOverlay_.setVisible(shouldShowOverlay)"});
}

Note makeNote(double startSeconds, double endSeconds, float pitchHz, float pitchOffset = 0.0f)
{
    Note note;
    note.startTime = startSeconds;
    note.endTime = endSeconds;
    note.pitch = pitchHz;
    note.originalPitch = pitchHz;
    note.pitchOffset = pitchOffset;
    return note;
}

bool sameNotes(const std::vector<Note>& a, const std::vector<Note>& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].startTime != b[i].startTime
            || a[i].endTime != b[i].endTime
            || a[i].pitch != b[i].pitch
            || a[i].originalPitch != b[i].originalPitch
            || a[i].pitchOffset != b[i].pitchOffset) {
            return false;
        }
    }

    return true;
}

struct CommitCall
{
    ContentKey key;
    std::vector<Note> notes;
    std::vector<PitchCorrectionSegment> segments;
    ContentEditRangeFrames range;
};

class RecordingContentEditCommands final : public ContentEditCommands
{
public:
    std::vector<CommitCall> commits;
    int unexpectedCalls = 0;

    bool replaceContentNotesForFullMutation(ContentKey, std::vector<Note>) override
    {
        ++unexpectedCalls;
        return false;
    }

    ContentCommitSnapshot commitNotePatch(ContentKey, ContentNoteRangePatch) override
    {
        ++unexpectedCalls;
        return {};
    }

    ContentCommitSnapshot commitNotesAndSegments(ContentKey key,
                                                 std::vector<Note> notes,
                                                 std::vector<PitchCorrectionSegment> segments,
                                                 ContentEditRangeFrames affectedRange) override
    {
        commits.push_back({key, notes, segments, affectedRange});

        auto snapshot = std::make_shared<EditableContentSnapshot>();
        snapshot->notes = std::move(notes);
        snapshot->correctionSegments = std::move(segments);
        snapshot->notesRevision = static_cast<uint64_t>(commits.size());
        snapshot->contentRevision = snapshot->notesRevision;
        return snapshot;
    }

    bool setPitchCurve(ContentKey, std::shared_ptr<PitchCurve>, ContentEditRangeFrames) override
    {
        ++unexpectedCalls;
        return false;
    }

    bool setTimeGrid(ContentKey, std::shared_ptr<const TimeGridSnapshot>) override
    {
        ++unexpectedCalls;
        return false;
    }

    bool setDetectedKey(ContentKey, const DetectedKey&) override
    {
        ++unexpectedCalls;
        return false;
    }

    bool setPitchShiftSettings(ContentKey, const PitchShiftSettings&) override
    {
        ++unexpectedCalls;
        return false;
    }

    bool commitAutoTuneGeneratedNotes(ContentKey,
                                      std::vector<Note>,
                                      int,
                                      int,
                                      float,
                                      float,
                                      float) override
    {
        ++unexpectedCalls;
        return false;
    }
};

void selectAllFeedbackPathCoversEveryNote()
{
    NoteSelectionState selection;
    selection.selectAll(3);

    expect((selection.selectedIndices == std::vector<int>{0, 1, 2}),
           "selectAll must select every note index");
    expect(selection.anchorIndex == 2,
           "selectAll must set anchor to the last selected note");

    selection.trimToNoteCount(2);
    expect((selection.selectedIndices == std::vector<int>{0, 1}),
           "selection trimming must keep selected indices inside the note range");
    expect(selection.anchorIndex == 1,
           "selection trimming must keep anchor inside the note range");

    const auto handler = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto selectAllBlock = extractBlockByMarker(
        handler, "if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::SelectAll");

    expectTokens("SelectAll shortcut path",
                 selectAllBlock,
                 {"committedNotes(ctx_)", "selectAllNotes(notes);", "updateF0SelectionFromNotes(committed)", "invalidateSelectionFeedback"});
    expectNoTokens("SelectAll shortcut path",
                   selectAllBlock,
                   {"invalidateLiveNotes", "invalidateInteractionPreview", "prepareVisibleContentTiles", "beginNoteDraft", "commitNoteDraft"});
}

void dragPreviewUsesWorkingNotesAndLiveInvalidation()
{
    NoteInteractionDraft draft;
    draft.active = true;
    draft.baselineNotes = {makeNote(0.0, 1.0, 220.0f)};
    draft.workingNotes = draft.baselineNotes;
    draft.workingNotes[0].pitchOffset = 2.0f;
    draft.workingNotes[0].dirty = true;

    expect(draft.baselineNotes[0].pitchOffset == 0.0f,
           "draft baseline must preserve the committed note pitch offset");
    expect(draft.workingNotes[0].pitchOffset == 2.0f,
           "draft working notes must carry the live dragged pitch offset");

    const auto handler = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto dragBlock = extractBlockByMarker(
        handler, "if (ctx_.getState().noteDrag.draggedNoteIndex >= 0)");

    expectTokens("note drag path",
                 dragBlock,
                 {"ctx_.beginNoteDraft()", "auto& notes = workingDraftNotes(ctx_)", "resetDraftNotesToBaseline(ctx_)", "pitchOffset = snappedOffset", "invalidateLiveNotes"});
    expectNoTokens("note drag path",
                   dragBlock,
                   {"invalidateSelectionFeedback", "prepareVisibleContentTiles", "renderer_->drawNotes", "contentCache_"});
}

void pianoRollEditActionUndoRedoCommitsRangeSnapshots()
{
    auto commands = std::make_shared<RecordingContentEditCommands>();

    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = 42;

    const std::vector<Note> beforeNotes = {
        makeNote(0.25, 0.75, 220.0f)
    };
    const std::vector<Note> afterNotes = {
        makeNote(0.25, 1.00, 246.94165f, 1.0f)
    };
    const std::vector<PitchCorrectionSegment> beforeSegments = {
        PitchCorrectionSegment(10, 20, {220.0f, 221.0f}, PitchCorrectionSegment::Source::NoteBased)
    };
    const std::vector<PitchCorrectionSegment> afterSegments = {
        PitchCorrectionSegment(10, 24, {246.0f, 247.0f}, PitchCorrectionSegment::Source::NoteBased)
    };
    const ContentEditRangeFrames range {10, 24};

    UndoManager undoManager;
    undoManager.addAction(std::make_unique<PianoRollEditAction>(
        commands,
        key,
        "edit note",
        beforeNotes,
        afterNotes,
        beforeSegments,
        afterSegments,
        range));

    expect(undoManager.canUndo(), "UndoManager must own the PianoRoll edit action");
    expect(!undoManager.canRedo(), "UndoManager must not expose redo before undo");

    undoManager.undo();
    expect(commands->commits.size() == 1, "undo must commit once");
    if (commands->commits.size() >= 1) {
        const auto& undoCommit = commands->commits[0];
        expect(undoCommit.key == key, "undo must use the original ContentKey");
        expect(sameNotes(undoCommit.notes, beforeNotes), "undo must commit before notes");
        expect(undoCommit.segments.size() == beforeSegments.size(), "undo must commit before segments");
        expect(undoCommit.range.startFrame == range.startFrame, "undo must preserve range start");
        expect(undoCommit.range.endFrameExclusive == range.endFrameExclusive, "undo must preserve range end");
    }

    expect(undoManager.canRedo(), "UndoManager must expose redo after undo");
    undoManager.redo();
    expect(commands->commits.size() == 2, "redo must commit once");
    if (commands->commits.size() >= 2) {
        const auto& redoCommit = commands->commits[1];
        expect(redoCommit.key == key, "redo must use the original ContentKey");
        expect(sameNotes(redoCommit.notes, afterNotes), "redo must commit after notes");
        expect(redoCommit.segments.size() == afterSegments.size(), "redo must commit after segments");
        expect(redoCommit.range.startFrame == range.startFrame, "redo must preserve range start");
        expect(redoCommit.range.endFrameExclusive == range.endFrameExclusive, "redo must preserve range end");
    }

    expect(commands->unexpectedCalls == 0, "PianoRollEditAction must only call commitNotesAndSegments");
}

void captureSegmentContentAudioBufferBirthsIdentityTimeGrid()
{
    CaptureSegmentContent content(1);

    juce::AudioBuffer<float> buffer(2, 480);
    buffer.clear();
    const double sampleRate = 48000.0;

    content.applyAudioBuffer(buffer, sampleRate);

    const auto snap = content.snapshotContent();

    expect(snap->audioBuffer != nullptr, "audioBuffer must not be null after applyAudioBuffer");
    expect(snap->timeGrid != nullptr, "timeGrid must not be null after applyAudioBuffer");
    expect(snap->timeGrid->isIdentity(), "timeGrid must be identity after applyAudioBuffer");

    const double expectedDuration = static_cast<double>(buffer.getNumSamples()) / sampleRate;
    expect(snap->timeGrid->totalDurationSeconds() == expectedDuration,
           "timeGrid totalDurationSeconds must match buffer duration");
    expect(snap->timeGridRevision == 1, "timeGridRevision must be 1 after first applyAudioBuffer");
}

void timeGridSnapshotIdentityRoundTrip()
{
    const double duration = 0.1;

    // makeIdentity(0.1) must succeed without throwing (returns shared_ptr<const TimeGridSnapshot>)
    const auto identity = TimeGridSnapshot::makeIdentity(duration);
    expect(identity != nullptr, "makeIdentity must return non-null snapshot");

    expect(identity->totalDurationSeconds() == duration,
           "makeIdentity duration must match argument");

    expect(identity->isIdentity(),
           "makeIdentity result must be identity time grid");

    // Extract handles and round-trip through makeFromHandles
    const std::vector<TimeHandle> handles = identity->handles();
    expect(!handles.empty(),
           "makeIdentity snapshot must have non-empty handles vector");

    // makeFromHandles(std::vector<TimeHandle> handles, uint64_t revision = 1) → returns shared_ptr<const TimeGridSnapshot>
    const auto restored = TimeGridSnapshot::makeFromHandles(handles);
    expect(restored != nullptr, "makeFromHandles must return non-null snapshot");

    expect(restored->totalDurationSeconds() == duration,
           "round-tripped snapshot duration must match original");

    expect(restored->isIdentity(),
           "round-tripped snapshot must still be identity");
}

void vst3ClientOverlayGenerationContract()
{
    // Template-level contract: verify the overlay cmake file carries the
    // generation logic. This is intentionally a weak contract -- the strong
    // structural contract is enforced against the actually generated source
    // by vst3ClientGeneratedSourceZeroDataTransportContract() to avoid
    // template-only false positives.
    const auto overlay = readText("cmake/OpenTuneJuceVST3ClientOverlay.cmake");

    // Overlay must not gate wrapper generation on ARA -- zero-data transport
    // blocks arrive even on regular VST3 inserts, so the patched VST3 client
    // must always be generated.
    expectNoTokens("VST3 overlay not ARA-only early-return gated",
                   overlay,
                   {"if(NOT OPENTUNE_ENABLE_ARA)"});

    // Overlay must keep the original audio guard and add a transport-only else
    // branch. Mutating the guard to also accept processContext routes
    // zero-data blocks into processAudio / ClientRemappedBuffer, which is the
    // bug being fixed.
    expectTokens("VST3 overlay preserves original audio guard",
                 overlay,
                 {"if (data.numSamples != 0 || data.numInputs != 0 || data.numOutputs != 0)"});
    expectNoTokens("VST3 overlay must not conflate guard with processContext",
                   overlay,
                   {"data.numSamples != 0 || data.numInputs != 0 || data.numOutputs != 0 || data.processContext != nullptr"});
    expectTokens("VST3 overlay transport-only else branch",
                 overlay,
                 {"else if (data.processContext != nullptr)"});

    // ARA legacy bind patch must remain ARA-gated.
    expectTokens("VST3 overlay ARA legacy bind gated",
                 overlay,
                 {"if(OPENTUNE_ENABLE_ARA)"});
}

void vst3ClientGeneratedSourceZeroDataTransportContract()
{
    // Strong contract: read the actually generated JUCE VST3 client source
    // from the build tree. This catches false positives where the overlay
    // template looks correct but the generated source is wrong.
    const auto generatedSource = readBinaryText(
        "Generated/OpenTune/JUCE/juce_audio_plugin_client_VST3.cpp");

    // Original audio guard must be preserved verbatim -- no processContext
    // conflation that would route zero-data blocks into processAudio and
    // ClientRemappedBuffer.
    expectTokens("VST3 generated source preserves original audio guard",
                 generatedSource,
                 {"if (data.numSamples != 0 || data.numInputs != 0 || data.numOutputs != 0)"});
    expectNoTokens("VST3 generated source must not conflate guard with processContext",
                   generatedSource,
                   {"data.numSamples != 0 || data.numInputs != 0 || data.numOutputs != 0 || data.processContext != nullptr"});

    // Transport-only else branch must exist in the generated source.
    const auto transportBlock = extractBlockByMarker(
        generatedSource, "else if (data.processContext != nullptr)");
    expect(!transportBlock.empty(),
           "VST3 generated source must contain transport-only else branch");

    // Direct branch must contain the callback lock, the non-realtime flag,
    // sample32/sample64 dispatch and the processBlock call.
    expectTokens("VST3 transport-only branch callback lock",
                 transportBlock,
                 {"const ScopedLock sl (pluginInstance->getCallbackLock())"});
    expectTokens("VST3 transport-only branch non-realtime flag",
                 transportBlock,
                 {"pluginInstance->setNonRealtime (data.processMode == Vst::kOffline)"});
    expectTokens("VST3 transport-only branch suspension boundary",
                 transportBlock,
                 {"if (!pluginInstance->isSuspended())"});
    expectTokens("VST3 transport-only branch sample32 dispatch",
                 transportBlock,
                 {"processSetup.symbolicSampleSize == Vst::kSample32"});
    expectTokens("VST3 transport-only branch sample64 dispatch",
                 transportBlock,
                 {"processSetup.symbolicSampleSize == Vst::kSample64"});

    // Strengthened double zero-sample contract: the float and double empty
    // buffers are declared with the exact typed names and passed to the
    // matching processBlock overload, so overload resolution selects the
    // float and double paths respectively.
    expectTokens("VST3 transport-only branch float empty buffer declaration",
                 transportBlock,
                 {"juce::AudioBuffer<float> emptyFloatBuffer;"});
    expectTokens("VST3 transport-only branch float overload call",
                 transportBlock,
                 {"pluginInstance->processBlock (emptyFloatBuffer, midiBuffer)"});
    expectTokens("VST3 transport-only branch double empty buffer declaration",
                 transportBlock,
                 {"juce::AudioBuffer<double> emptyDoubleBuffer;"});
    expectTokens("VST3 transport-only branch double overload call",
                 transportBlock,
                 {"pluginInstance->processBlock (emptyDoubleBuffer, midiBuffer)"});

    // Direct branch must NOT touch processAudio or ClientRemappedBuffer.
    expectNoTokens("VST3 transport-only branch must not call processAudio",
                   transportBlock,
                   {"processAudio"});
    expectNoTokens("VST3 transport-only branch must not touch ClientRemappedBuffer",
                   transportBlock,
                   {"ClientRemappedBuffer"});

    const auto processBlock = extractBlockByMarker(
        generatedSource, "tresult PLUGIN_API process (Vst::ProcessData& data)");
    const auto processContextCopyPos = processBlock.find("processContext = *data.processContext");
    const auto transportBranchPos = processBlock.find("else if (data.processContext != nullptr)");
    expect(processContextCopyPos != std::string::npos
               && transportBranchPos != std::string::npos
               && processContextCopyPos < transportBranchPos,
           "VST3 process() must copy ProcessContext before transport-only dispatch");

    // Downstream paths (outputParameterChanges / MIDI output / Wavelab guard)
    // must remain intact after the transport-only branch.
    expectTokens("VST3 process() outputParameterChanges path preserved",
                 processBlock,
                 {"data.outputParameterChanges"});
    expectTokens("VST3 process() Wavelab guard preserved",
                 processBlock,
                 {"detail::PluginUtilities::getHostType().isWavelab()"});
}

void processBlockZeroSampleTransportObservationContract()
{
    const auto source = readText("Source/PluginProcessor.cpp");

    // Float processBlock: playHeadState_.update(hostPosOpt) must precede the
    // numSamples <= 0 early return so zero-sample blocks still observe host
    // transport (hosts send zero-sample blocks for parameter automation).
    const auto floatProcessBlock = extractBlockByMarker(
        source, "void OpenTuneAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer");

    const auto floatUpdatePos = floatProcessBlock.find("playHeadState_.update(hostPosOpt)");
    const auto floatZeroSamplePos = floatProcessBlock.find("numSamples <= 0");

    expect(floatUpdatePos != std::string::npos,
           "float processBlock must call playHeadState_.update(hostPosOpt)");
    expect(floatZeroSamplePos != std::string::npos,
           "float processBlock must gate zero-sample blocks via numSamples <= 0");
    expect(floatUpdatePos < floatZeroSamplePos,
           "float processBlock playHeadState_.update(hostPosOpt) must precede numSamples <= 0 early return");

    // Double processBlock: zero-sample path must still observe transport by
    // delegating to the float processBlock (which performs the update) or via
    // an equivalent host PositionInfo read + playHeadState_.update. Normal
    // double->float conversion path must be preserved.
    const auto doubleProcessBlock = extractBlockByMarker(
        source, "void OpenTuneAudioProcessor::processBlock(juce::AudioBuffer<double>& buffer");

    const auto doubleZeroSamplePos = doubleProcessBlock.find("if (numSamples <= 0)");
    const auto emptyFloatBufferPos = doubleProcessBlock.find(
        "juce::AudioBuffer<float> emptyFloatBuffer;", doubleZeroSamplePos);
    const auto emptyFloatProcessBlockPos = doubleProcessBlock.find(
        "processBlock(emptyFloatBuffer, midiMessages);", emptyFloatBufferPos);
    expect(doubleZeroSamplePos != std::string::npos
               && emptyFloatBufferPos != std::string::npos
               && emptyFloatProcessBlockPos != std::string::npos
               && doubleZeroSamplePos < emptyFloatBufferPos
               && emptyFloatBufferPos < emptyFloatProcessBlockPos,
           "double zero-sample path must call the float overload with emptyFloatBuffer");

    expectTokens("double processBlock normal path preserved",
                 doubleProcessBlock,
                 {"processBlock(floatBuffer, midiMessages)"});
}

void playHeadStatePausedExternalSeekRuntimeContract()
{
    PlayHeadState state;
    state.timeInSeconds.store(10.0, std::memory_order_relaxed);
    state.isPlaying.store(true, std::memory_order_relaxed);
    state.isLooping.store(false, std::memory_order_relaxed);

    const auto initialRevision = state.hostPositionRevision.load(std::memory_order_relaxed);

    // Paused external seek: host moves the playhead while not playing.
    // isPlaying transitions true->false, timeInSeconds jumps 10.0->42.0.
    juce::AudioPlayHead::PositionInfo pausedSeek;
    pausedSeek.setIsPlaying(false);
    pausedSeek.setTimeInSeconds(42.0);
    state.update(juce::Optional<juce::AudioPlayHead::PositionInfo>(pausedSeek));

    expect(!state.isPlaying.load(std::memory_order_relaxed),
           "paused external seek must reflect isPlaying=false");
    expect(state.timeInSeconds.load(std::memory_order_relaxed) == 42.0,
           "paused external seek must update timeInSeconds to the new position");
    expect(state.hostPositionRevision.load(std::memory_order_relaxed) == initialRevision + 1,
           "paused external seek must bump hostPositionRevision so observers notice the jump");
}

} // namespace

int main()
{
    std::cout << "=== OpenTune PianoRoll Behavior Tests ===\n\n";

    try {
        pianoRollPendingSeekPresentationSourceContract();
        pianoRollPlayheadPaintsDirectly();
        pluginEditorPlayheadRequestSourceContract();
        standalonePlayheadRequestSourceContract();
        pluginEditorPlayheadPositionBindingSourceContract();
        standalonePlayheadPositionBindingSourceContract();
        processorOwnedPlayHeadStateContract();
        sharedCodeRuntimeWrapperTypeDispatchContract();
        documentControllerTransportBoundaryContract();
        transportProductionZeroResidueContract();
        araContentProjectionContract();
        araUiReadinessContract();
        rendererReadinessPositionInfoContract();
        captureBoundaryContract();
        transportBarFeedbackContract();
        araReadAudioPlaybackSourceContract();
        playHeadStateRuntimeContract();
        viewMapperContinuousConversionRoundTrips();
        unifiedViewMappingAndReadableRenderSourceContracts();
        selectAllFeedbackPathCoversEveryNote();
        dragPreviewUsesWorkingNotesAndLiveInvalidation();
        pianoRollEditActionUndoRedoCommitsRangeSnapshots();
        captureSegmentContentAudioBufferBirthsIdentityTimeGrid();
        timeGridSnapshotIdentityRoundTrip();
        vst3ClientOverlayGenerationContract();
        vst3ClientGeneratedSourceZeroDataTransportContract();
        processBlockZeroSampleTransportObservationContract();
        playHeadStatePausedExternalSeekRuntimeContract();
    } catch (const std::exception& e) {
        ++failures;
        std::cout << "[FAIL] uncaught exception: " << e.what() << "\n";
    }

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "ALL PIANOROLL BEHAVIOR TESTS PASSED\n";
        return 0;
    }

    std::cout << failures << " PIANOROLL BEHAVIOR TEST(S) FAILED\n";
    return 1;
}
