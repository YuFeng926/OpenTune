#include "../Source/Utils/PianoRollEditAction.h"
#include "../Source/Utils/UndoManager.h"
#include "../Source/Standalone/UI/PianoRoll/InteractionState.h"
#include "../Source/Standalone/UI/PianoRoll/PianoRollRenderer.h"
#include "../Source/Content/EditableContentSnapshot.h"
#include "../Source/Content/CaptureSegmentContent.h"
#include "../Source/Utils/TimeGrid.h"
#include "../Source/Utils/PitchCurve.h"
#include "../Source/PluginProcessor.h"
#include "../Source/Standalone/UI/TimelineLayerComposer.h"
#include "../Source/Standalone/UI/UIColors.h"
#include "../Source/Standalone/UI/ViewMapper.h"
#include "../Source/Standalone/UI/TimelineViewportPolicy.h"

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
#include <utility>
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
    const auto vblankBlock = extractBlockByMarker(
        source, "void PianoRollComponent::onScrollVBlankCallback(double timestampSec)");

    expectTokens("PianoRoll notify playhead request",
                 notifyBlock,
                 {"const auto seekRevision = playHeadState_.hostPositionRevision.load",
                  "bool requestDispatched = false;",
                  "if (l.playheadPositionChangeRequested(time))",
                  "seekSentRevision_ = seekRevision;"});
    expectTokens("PianoRoll accepted seek pending only",
                 notifyBlock,
                 {"pendingSeekTime_ = time;"});
    expectTokens("PianoRoll rejected seek pending only",
                 notifyBlock,
                 {"pendingSeekTime_ = -1.0;",
                  "seekSentRevision_ = 0;"});

    // notifyPlayheadChange 不再写入 playheadTimeForPaint_ 或 overlay repaint
    expectNoTokens("PianoRoll notify no playheadTimeForPaint_ write",
                   notifyBlock,
                   {"playheadTimeForPaint_"});
    expectNoTokens("PianoRoll notify no overlay repaint",
                   notifyBlock,
                   {"overlay_->repaint();"});

    // VBlank 是唯一 playheadTimeForPaint_ 写入者
    expectTokens("PianoRoll VBlank reconciliation",
                 vblankBlock,
                 {"playHeadState_.hostPositionRevision.load",
                  "hostRevision != seekSentRevision_",
                  "pendingSeekTime_ = -1.0;"});
    expectTokens("PianoRoll VBlank unique presentation write",
                 vblankBlock,
                 {"playheadTimeForPaint_ = playheadTime"});

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
    // VBlank 不得 paused 早退（禁止 if (!playingNow) return; 形式）
    expectNoTokens("PianoRoll VBlank no paused early-return",
                   vblankBlock,
                   {"if (!playingNow)"});
    expectNoTokens("PianoRoll retained playhead architecture",
                    source,
                    {"playheadOverlay_", "PlayheadOverlayComponent"});
}

void pianoRollPlayheadOverlayPaintsDirectly()
{
    const auto source = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto drawPlayheadBlock = extractBlockByMarker(
        source, "void PianoRollComponent::drawPlayheadOverlay(juce::Graphics& g)");

    expectTokens("PianoRoll direct playhead draw",
                 drawPlayheadBlock,
                 {"g.reduceClipRegion(timeAxisRect())"});

    // notifyPlayheadChange 不再调用 repaint；VBlank 统一处理 Overlay repaint
    expectTokens("PianoRoll VBlank handles overlay repaint",
                 extractBlockByMarker(source, "void PianoRollComponent::onScrollVBlankCallback"),
                 {"overlay_->repaint("});
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

void pitchCurveSnapshotForEachCorrectionF0SpanContract()
{
    struct SpanRecord {
        int startFrame = 0;
        bool isGap = false;
        int length = 0;
        std::vector<float> values;
    };

    auto collect = [](const PitchCurveSnapshot& snap, int start, int end) {
        std::vector<SpanRecord> records;
        snap.forEachCorrectionF0Span(start, end, [&](int sf, const float* data, int len) {
            SpanRecord r;
            r.startFrame = sf;
            r.isGap = (data == nullptr);
            r.length = len;
            if (data) r.values.assign(data, data + len);
            records.push_back(std::move(r));
        });
        return records;
    };

    // --- setup: original F0 length 8, adjacent correction [2,4)={220,221}, [4,6)={222,223} ---
    const std::vector<float> f0(8, 100.0f);
    const std::vector<float> energy(8, 0.5f);
    std::vector<PitchCorrectionSegment> segments;
    segments.emplace_back(2, 4, std::vector<float>{220.0f, 221.0f});
    segments.emplace_back(4, 6, std::vector<float>{222.0f, 223.0f});

    const PitchCurveSnapshot snap(f0, energy, segments, 512, 16000.0);

    // --- verify 1: query [0,8) → gap[0,2) data[2,4) data[4,6) gap[6,8), no nullptr between adjacent data ---
    {
        const auto r = collect(snap, 0, 8);
        expect(r.size() == 4, "[0,8) must produce 4 spans: gap, data, data, gap");

        expect(r[0].startFrame == 0 && r[0].isGap && r[0].length == 2 && r[0].values.empty(),
               "[0,8) span 0 must be gap [0,2)");
        expect(r[1].startFrame == 2 && !r[1].isGap && r[1].length == 2
                   && r[1].values == (std::vector<float>{220.0f, 221.0f}),
               "[0,8) span 1 must be data [2,4)={220,221}");
        expect(r[2].startFrame == 4 && !r[2].isGap && r[2].length == 2
                   && r[2].values == (std::vector<float>{222.0f, 223.0f}),
               "[0,8) span 2 must be data [4,6)={222,223}");
        expect(r[3].startFrame == 6 && r[3].isGap && r[3].length == 2 && r[3].values.empty(),
               "[0,8) span 3 must be gap [6,8)");
    }

    // --- verify 2: query [3,5) → clipped data [3,4)={221}, [4,5)={222}, no extra gap ---
    {
        const auto r = collect(snap, 3, 5);
        expect(r.size() == 2, "[3,5) must produce exactly 2 data spans");

        expect(r[0].startFrame == 3 && !r[0].isGap && r[0].length == 1
                   && r[0].values == (std::vector<float>{221.0f}),
               "[3,5) span 0 must be data [3,4)={221}");
        expect(r[1].startFrame == 4 && !r[1].isGap && r[1].length == 1
                   && r[1].values == (std::vector<float>{222.0f}),
               "[3,5) span 1 must be data [4,5)={222}");
    }

    // --- verify 3: empty / negative / inverted ranges must not callback ---
    {
        int callCount = 0;
        snap.forEachCorrectionF0Span(0, 0, [&](int, const float*, int) { ++callCount; });
        expect(callCount == 0, "empty range [0,0) must not callback");

        callCount = 0;
        snap.forEachCorrectionF0Span(-1, 3, [&](int, const float*, int) { ++callCount; });
        expect(callCount == 0, "negative start [-1,3) must not callback");

        callCount = 0;
        snap.forEachCorrectionF0Span(3, 0, [&](int, const float*, int) { ++callCount; });
        expect(callCount == 0, "inverted range [3,0) must not callback");
    }

    // --- verify 4: no-correction snapshot [0,>length) → one clipped nullptr gap [0,8) ---
    {
        const std::vector<float> rawF0(8, 100.0f);
        const std::vector<float> rawEnergy(8, 0.5f);
        const std::vector<PitchCorrectionSegment> noSegments;
        const PitchCurveSnapshot noCorrSnap(rawF0, rawEnergy, noSegments, 512, 16000.0);

        const auto r = collect(noCorrSnap, 0, 100);
        expect(r.size() == 1, "no-correction [0,100) must produce exactly 1 span");
        expect(r[0].startFrame == 0 && r[0].isGap && r[0].length == 8 && r[0].values.empty(),
               "no-correction span must be gap [0,8) clipped to originalF0 length");
    }
}

// ---------------------------------------------------------------------------
// F0 pixel contract helpers — DarkBlueGrey theme (no glow), identity timeline
// ---------------------------------------------------------------------------
namespace {
    const ThemeId kF0TestOrigTheme = UIColors::currentThemeId();
}

static void f0TestEnsureDarkBlueGrey()
{
    static bool applied = false;
    if (!applied) {
        UIColors::applyTheme(ThemeId::DarkBlueGrey);
        applied = true;
    }
}

static void f0TestRestoreTheme()
{
    UIColors::applyTheme(kF0TestOrigTheme);
}

// ---------------------------------------------------------------------------
// Contract 1 : Original F0 bucket extremes survive asymmetric energy swap
// ---------------------------------------------------------------------------
void f0CurveOriginalBucketExtremesWithAsymmetricEnergy()
{
    f0TestEnsureDarkBlueGrey();

    constexpr int numFrames = 40;
    constexpr int hopSize   = 512;
    constexpr double sr     = 16000.0;
    constexpr double pps    = 8.0;           // frame 0&1 → same int X via llround
    constexpr int startX    = 60;            // far from image boundary
    constexpr int imgW = 300, imgH = 250;

    // frame 0 (800 Hz) and frame 1 (200 Hz) share the bucket starting at X = startX
    std::vector<float> f0(numFrames, 440.0f);
    f0[0] = 800.0f;
    f0[1] = 200.0f;

    auto makeItem = [&](auto& snap) {
        F0Timeline tl(hopSize, sr, numFrames);
        const double dur = numFrames * static_cast<double>(hopSize) / sr;
        auto tg = TimeGridSnapshot::makeIdentity(dur);
        ContentTimelineProjection pr;
        pr.timelineStartSeconds = 0.0;
        pr.timelineDurationSeconds = dur;
        pr.contentDurationSeconds  = dur;

        PianoRollRenderer::ContentRenderItem it;
        it.contentKey.domainKind = DomainKind::StandaloneClip;
        it.contentKey.objectId   = 1;
        it.projection  = pr;
        it.timeGrid    = tg;
        it.f0Timeline  = tl;
        it.pitchSnapshot = snap;
        it.active = true;
        return it;
    };

    auto makeCtx = [&](bool showOrig) {
        PianoRollRenderer::RenderContext c;
        c.width            = imgW;
        c.height           = imgH;
        c.pianoKeyWidth    = startX;
        c.rulerHeight      = 0;
        c.pixelsPerSecond  = pps;
        c.pixelsPerSemitone = 3.0f;
        c.minMidi          = 24.0f;
        c.maxMidi          = 108.0f;
        c.showOriginalF0   = showOrig;
        c.showCorrectedF0  = false;

        c.coords.visibleStartSeconds = 0.0;
        c.coords.pixelsPerSecond    = pps;
        c.coords.contentStartX      = startX;
        c.coords.contentWidth       = imgW;
        c.coords.contentHeight      = imgH;
        c.coords.pixelsPerSemitone  = 3.0f;
        c.coords.maxMidi            = 108.0f;
        return c;
    };

    // --- target Y via freqToY (one reference context) ---
    auto refCtx   = makeCtx(true);
    const int hiY = static_cast<int>(refCtx.coords.freqToY(800.0f));   // ≈ 87
    const int loY = static_cast<int>(refCtx.coords.freqToY(200.0f));   // ≈ 159
    const int bucketX = startX;  // frame 0 & 1 both resolve to this int X

    auto renderAndCheck = [&](const std::vector<float>& energy,
                              std::string_view tag) -> std::pair<int,int>
    {
        auto snap = std::make_shared<const PitchCurveSnapshot>(
            f0, energy, std::vector<PitchCorrectionSegment>{}, hopSize, sr);
        juce::Image img(juce::Image::ARGB, imgW, imgH, true);
        {
            juce::Graphics g(img);
            PianoRollRenderer r;
            auto item = makeItem(snap);
            auto ctx  = makeCtx(true);
            r.drawF0Curve(g, ctx, item);
        }
        bool hi = false, lo = false;
        for (int dy = -1; dy <= 1; ++dy) {
            const int yH = hiY + dy;
            const int yL = loY + dy;
            if (yH >= 0 && yH < imgH)
                hi = hi || (img.getPixelAt(bucketX, yH).getAlpha() > 0);
            if (yL >= 0 && yL < imgH)
                lo = lo || (img.getPixelAt(bucketX, yL).getAlpha() > 0);
        }
        const auto label = std::string(tag);
        expect(hi, label + ": Original F0 bucket must cover high-freq Y extreme (freqToY(800))");
        expect(lo, label + ": Original F0 bucket must cover low-freq Y extreme (freqToY(200))");

        // Scan bucketX column for alpha range [minY, maxY]
        int colMin = imgH, colMax = -1;
        for (int y = 0; y < imgH; ++y) {
            if (img.getPixelAt(bucketX, y).getAlpha() > 0) {
                if (y < colMin) colMin = y;
                if (y > colMax) colMax = y;
            }
        }
        return {colMin, colMax};
    };

    std::pair<int,int> rangeA, rangeB;

    // Energy A — high energy on high-freq frame
    {
        std::vector<float> enA(numFrames, 0.3f);
        enA[0] = 0.9f;  enA[1] = 0.05f;
        rangeA = renderAndCheck(enA, "Energy-A (hi→800Hz)");
    }
    // Energy B — swapped: high energy on low-freq frame
    {
        std::vector<float> enB(numFrames, 0.3f);
        enB[0] = 0.05f; enB[1] = 0.9f;
        rangeB = renderAndCheck(enB, "Energy-B (hi→200Hz)");
    }

    expect(std::abs(rangeA.first  - rangeB.first)  <= 1,
           "Energy swap: bucket alpha minY must match within 1px");
    expect(std::abs(rangeA.second - rangeB.second) <= 1,
           "Energy swap: bucket alpha maxY must match within 1px");
}

// ---------------------------------------------------------------------------
// Contract 2 : Corrected F0 span extremes on same single bucket
// ---------------------------------------------------------------------------
void f0CurveCorrectedSpanExtremes()
{
    f0TestEnsureDarkBlueGrey();

    constexpr int numFrames = 40;
    constexpr int hopSize   = 512;
    constexpr double sr     = 16000.0;
    constexpr double pps    = 8.0;
    constexpr int startX    = 60;
    constexpr int imgW = 300, imgH = 250;

    std::vector<float> f0(numFrames, 440.0f);
    std::vector<float> energy(numFrames, 0.3f);

    // correction segment [0,8): frame 0=800, frame 1=200 → same bucket at startX
    std::vector<PitchCorrectionSegment> segments;
    segments.emplace_back(0, 8, std::vector<float>{
        800.0f, 200.0f, 440.0f, 440.0f, 440.0f, 440.0f, 440.0f, 440.0f });

    auto snap = std::make_shared<const PitchCurveSnapshot>(
        f0, energy, segments, hopSize, sr);

    F0Timeline tl(hopSize, sr, numFrames);
    const double dur = numFrames * static_cast<double>(hopSize) / sr;
    auto tg = TimeGridSnapshot::makeIdentity(dur);
    ContentTimelineProjection pr;
    pr.timelineStartSeconds = 0.0;
    pr.timelineDurationSeconds = dur;
    pr.contentDurationSeconds  = dur;

    PianoRollRenderer::RenderContext ctx;
    ctx.width            = imgW;
    ctx.height           = imgH;
    ctx.pianoKeyWidth    = startX;
    ctx.rulerHeight      = 0;
    ctx.pixelsPerSecond  = pps;
    ctx.pixelsPerSemitone = 3.0f;
    ctx.minMidi          = 24.0f;
    ctx.maxMidi          = 108.0f;
    ctx.showOriginalF0   = false;
    ctx.showCorrectedF0  = true;
    ctx.coords.visibleStartSeconds = 0.0;
    ctx.coords.pixelsPerSecond    = pps;
    ctx.coords.contentStartX      = startX;
    ctx.coords.contentWidth       = imgW;
    ctx.coords.contentHeight      = imgH;
    ctx.coords.pixelsPerSemitone  = 3.0f;
    ctx.coords.maxMidi            = 108.0f;

    PianoRollRenderer::ContentRenderItem item;
    item.contentKey.domainKind = DomainKind::StandaloneClip;
    item.contentKey.objectId   = 1;
    item.projection  = pr;
    item.timeGrid    = tg;
    item.f0Timeline  = tl;
    item.pitchSnapshot = snap;
    item.active = true;

    juce::Image img(juce::Image::ARGB, imgW, imgH, true);
    {
        juce::Graphics g(img);
        PianoRollRenderer r;
        r.drawF0Curve(g, ctx, item);
    }

    const int hiY = static_cast<int>(ctx.coords.freqToY(800.0f));
    const int loY = static_cast<int>(ctx.coords.freqToY(200.0f));
    const int bucketX = startX;

    bool hi = false, lo = false;
    for (int dy = -1; dy <= 1; ++dy) {
        const int yH = hiY + dy;
        const int yL = loY + dy;
        if (yH >= 0 && yH < imgH)
            hi = hi || (img.getPixelAt(bucketX, yH).getAlpha() > 0);
        if (yL >= 0 && yL < imgH)
            lo = lo || (img.getPixelAt(bucketX, yL).getAlpha() > 0);
    }
    expect(hi, "Corrected F0 span must cover high-freq Y extreme (freqToY(800))");
    expect(lo, "Corrected F0 span must cover low-freq Y extreme (freqToY(200))");
}

// ---------------------------------------------------------------------------
// Contract 3 : Original invalid-F0 gap & Corrected nullptr gap are isolated
//              (two separate renders — curves never drawn together)
// ---------------------------------------------------------------------------
void f0CurveGapIsolation()
{
    f0TestEnsureDarkBlueGrey();

    constexpr int numFrames = 120;
    constexpr int hopSize   = 512;
    constexpr double sr     = 16000.0;
    constexpr double pps    = 8.0;
    constexpr int startX    = 60;
    constexpr int imgW = 350, imgH = 250;

    auto makeItem = [&](int nf, auto& snap) {
        F0Timeline tl(hopSize, sr, nf);
        const double dur = nf * static_cast<double>(hopSize) / sr;
        auto tg = TimeGridSnapshot::makeIdentity(dur);
        ContentTimelineProjection pr;
        pr.timelineStartSeconds = 0.0;
        pr.timelineDurationSeconds = dur;
        pr.contentDurationSeconds  = dur;
        PianoRollRenderer::ContentRenderItem it;
        it.contentKey.domainKind = DomainKind::StandaloneClip;
        it.contentKey.objectId   = 1;
        it.projection  = pr;
        it.timeGrid    = tg;
        it.f0Timeline  = tl;
        it.pitchSnapshot = snap;
        it.active = true;
        return it;
    };

    auto makeCtx = [&](bool showOrig, bool showCorr) {
        PianoRollRenderer::RenderContext c;
        c.width            = imgW;
        c.height           = imgH;
        c.pianoKeyWidth    = startX;
        c.rulerHeight      = 0;
        c.pixelsPerSecond  = pps;
        c.pixelsPerSemitone = 3.0f;
        c.minMidi          = 24.0f;
        c.maxMidi          = 108.0f;
        c.showOriginalF0   = showOrig;
        c.showCorrectedF0  = showCorr;
        c.coords.visibleStartSeconds = 0.0;
        c.coords.pixelsPerSecond    = pps;
        c.coords.contentStartX      = startX;
        c.coords.contentWidth       = imgW;
        c.coords.contentHeight      = imgH;
        c.coords.pixelsPerSemitone  = 3.0f;
        c.coords.maxMidi            = 108.0f;
        return c;
    };

    // Col scan helper: true iff every pixel in column x has zero alpha
    auto columnClear = [&](const juce::Image& img, int x) {
        for (int y = 0; y < imgH; ++y)
            if (img.getPixelAt(x, y).getAlpha() > 0)
                return false;
        return true;
    };

    // ----- Original F0 gap (invalid frames [50, 80)) -----
    // Last valid before gap: frame 49  →  X = startX+llround(49*0.256)=73
    // First valid after  gap: frame 80  →  X = startX+llround(80*0.256)=80
    // Bucket may push last point to X=72; gap centre  ≈  (72+80)/2 = 76
    {
        std::vector<float> f0O(numFrames, 440.0f);
        for (int i = 50; i < 80; ++i) f0O[i] = 0.0f;
        std::vector<float> enO(numFrames, 0.3f);
        auto snapO = std::make_shared<const PitchCurveSnapshot>(
            f0O, enO, std::vector<PitchCorrectionSegment>{}, hopSize, sr);

        juce::Image img(juce::Image::ARGB, imgW, imgH, true);
        {
            juce::Graphics g(img);
            PianoRollRenderer r;
            auto item = makeItem(numFrames, snapO);
            auto ctx  = makeCtx(true, false);
            r.drawF0Curve(g, ctx, item);
        }
        const int gapCx = 76;
        expect(columnClear(img, gapCx),
               "Original-F0 gap centre column (X=76) must be fully transparent");
    }

    // ----- Corrected F0 nullptr gap (segments [5,15) & [50,60)) -----
    // Segment-1 last frame: 14 → X = startX+llround(14*0.256)=64
    // Segment-2 first frame: 50 → X = startX+llround(50*0.256)=73
    // Bucket may push last point to X=63; gap centre ≈ (63+73)/2 = 68
    {
        std::vector<float> f0C(numFrames, 440.0f);
        std::vector<float> enC(numFrames, 0.3f);
        std::vector<PitchCorrectionSegment> segs;
        segs.emplace_back( 5, 15, std::vector<float>(10, 440.0f));
        segs.emplace_back(50, 60, std::vector<float>(10, 440.0f));
        auto snapC = std::make_shared<const PitchCurveSnapshot>(
            f0C, enC, segs, hopSize, sr);

        juce::Image img(juce::Image::ARGB, imgW, imgH, true);
        {
            juce::Graphics g(img);
            PianoRollRenderer r;
            auto item = makeItem(numFrames, snapC);
            auto ctx  = makeCtx(false, true);
            r.drawF0Curve(g, ctx, item);
        }
        const int gapCx = 68;
        expect(columnClear(img, gapCx),
               "Corrected-F0 nullptr-gap centre column (X=68) must be fully transparent");
    }
}

// ---------------------------------------------------------------------------
// drawGridLines / drawTimeRuler narrow-clip pixel-equivalence tests
// Compares full-clip baseline against narrow-clip rendering; pixels inside the
// narrow clip must be identical. Covers Bars (timeUnit=1) and Seconds (timeUnit=0).
// Narrow clip boundaries are placed close to grid-line and ruler-label positions
// to exercise the 2px grid padding and 21px ruler padding.
// ---------------------------------------------------------------------------

namespace {
    const ThemeId kClipTestOrigTheme = UIColors::currentThemeId();
}

static void clipTestEnsureDarkBlueGrey()
{
    static bool applied = false;
    if (!applied) {
        UIColors::applyTheme(ThemeId::DarkBlueGrey);
        applied = true;
    }
}

static void clipTestRestoreTheme()
{
    UIColors::applyTheme(kClipTestOrigTheme);
}

static int countPixelDiffs(const juce::Image& a, const juce::Image& b, juce::Rectangle<int> region)
{
    int diffs = 0;
    for (int y = region.getY(); y < region.getBottom(); ++y)
        for (int x = region.getX(); x < region.getRight(); ++x)
            if (a.getPixelAt(x, y).getARGB() != b.getPixelAt(x, y).getARGB())
                ++diffs;
    return diffs;
}

// ---------------------------------------------------------------------------
// F0 curve strip pixel-equivalence — full-render baseline vs
// Graphics::reduceClipRegion(strip) + rasterBounds=strip render.
// Covers original/corrected separately, bucket/Bezier LOD, left/right strip,
// continuous varying curve, and invalid/nullptr gaps.
// ---------------------------------------------------------------------------
void f0CurveStripPixelEquivalenceContract()
{
    f0TestEnsureDarkBlueGrey();

    constexpr int w = 400, h = 300;
    constexpr int pianoKeyWidth = 60;
    constexpr int stripW = 80;
    const juce::Rectangle<int> leftStrip(pianoKeyWidth, 0, stripW, h);
    const juce::Rectangle<int> rightStrip(w - stripW, 0, stripW, h);
    constexpr int hopSize = 512;
    constexpr double sr = 16000.0;
    constexpr double frameSec = static_cast<double>(hopSize) / sr;

    // PPS / numFrames / LOD / gap frame ranges (hand-picked inside strips)
    struct LODConfig {
        double pps; int nf; const char* label;
        int gapLeftStart, gapLeftEnd, gapRightStart, gapRightEnd;
    };
    const LODConfig bucketLOD{8.0,  1563, "bucket",
                               100,  200,  1100, 1200};
    const LODConfig bezierLOD{40.0,  313, "Bezier",
                               20,   40,   220,  245};

    // Deterministic varying F0 20–2000 Hz and energy 0.2–0.8
    auto varyingF0 = [](int nf) {
        std::vector<float> f0(nf);
        for (int i = 0; i < nf; ++i) {
            const double phase = static_cast<double>(i) * 0.05;
            const double val = 500.0 + 400.0 * std::sin(phase)
                               + 0.3 * static_cast<double>(i);
            f0[i] = static_cast<float>(std::max(20.0, std::min(val, 2000.0)));
        }
        return f0;
    };
    auto varyingEn = [](int nf) {
        std::vector<float> en(nf);
        for (int i = 0; i < nf; ++i) {
            const double phase = static_cast<double>(i) * 0.07 + 1.0;
            en[i] = static_cast<float>(0.2 + 0.6 * (0.5 + 0.5 * std::sin(phase)));
        }
        return en;
    };

    // Full-vs-strip pixel comparison
    auto checkStripEquiv = [&](
        const std::vector<float>& f0,
        const std::vector<float>& energy,
        const std::vector<PitchCorrectionSegment>& segments,
        int numFrames, double pps,
        const juce::Rectangle<int>& strip,
        bool showOrig, bool showCorr,
        std::string_view tag)
    {
        auto snap = std::make_shared<const PitchCurveSnapshot>(
            f0, energy, segments, hopSize, sr);

        const double dur = numFrames * frameSec;
        F0Timeline tl(hopSize, sr, numFrames);
        auto tg = TimeGridSnapshot::makeIdentity(dur);
        ContentTimelineProjection pr;
        pr.timelineStartSeconds = 0.0;
        pr.timelineDurationSeconds = dur;
        pr.contentDurationSeconds  = dur;

        PianoRollRenderer::ContentRenderItem item;
        item.contentKey.domainKind = DomainKind::StandaloneClip;
        item.contentKey.objectId   = 1;
        item.projection  = pr;
        item.timeGrid    = tg;
        item.f0Timeline  = tl;
        item.pitchSnapshot = snap;
        item.active = true;

        auto makeCtx = [&](const juce::Rectangle<int>& rastBounds) {
            PianoRollRenderer::RenderContext c;
            c.width            = w;
            c.height           = h;
            c.pianoKeyWidth    = pianoKeyWidth;
            c.rulerHeight      = 0;
            c.pixelsPerSecond  = pps;
            c.pixelsPerSemitone = 3.0f;
            c.minMidi          = 24.0f;
            c.maxMidi          = 108.0f;
            c.showOriginalF0   = showOrig;
            c.showCorrectedF0  = showCorr;
            c.coords.visibleStartSeconds = 0.0;
            c.coords.pixelsPerSecond    = pps;
            c.coords.contentStartX      = pianoKeyWidth;
            c.coords.contentWidth       = w - pianoKeyWidth;
            c.coords.contentHeight      = h;
            c.coords.pixelsPerSemitone  = 3.0f;
            c.coords.maxMidi            = 108.0f;
            c.rasterBounds = rastBounds;
            return c;
        };

        // Full render — no clip
        juce::Image fullImg(juce::Image::ARGB, w, h, true);
        {
            juce::Graphics g(fullImg);
            PianoRollRenderer r;
            auto ctx = makeCtx(juce::Rectangle<int>(0, 0, w, h));
            r.drawF0Curve(g, ctx, item);
        }

        // Strip render — reduceClipRegion(strip) + rasterBounds=strip
        juce::Image stripImg(juce::Image::ARGB, w, h, true);
        {
            juce::Graphics g(stripImg);
            g.reduceClipRegion(strip);
            PianoRollRenderer r;
            auto ctx = makeCtx(strip);
            r.drawF0Curve(g, ctx, item);
        }

        const int diffs = countPixelDiffs(fullImg, stripImg, strip);
        expect(diffs == 0,
               std::string(tag) + ": strip pixels must match full render. diffs="
               + std::to_string(diffs));
    };

    // ── Continuous valid F0 — original & corrected, bucket & Bezier, L & R ──
    for (const auto& lod : {bucketLOD, bezierLOD}) {
        const auto f0Var = varyingF0(lod.nf);
        const auto enVar = varyingEn(lod.nf);
        std::vector<PitchCorrectionSegment> fullSegs;
        fullSegs.emplace_back(0, lod.nf, f0Var);

        for (int si = 0; si < 2; ++si) {
            const auto& strip = (si == 0) ? leftStrip : rightStrip;
            const auto side   = (si == 0) ? "L" : "R";

            checkStripEquiv(f0Var, enVar, {},
                            lod.nf, lod.pps, strip,
                            true, false,
                            std::string("origF0 cont ") + side + " " + lod.label);
            checkStripEquiv(f0Var, enVar, fullSegs,
                            lod.nf, lod.pps, strip,
                            false, true,
                            std::string("corrF0 cont ") + side + " " + lod.label);
        }
    }

    // ── Gap F0 — original invalid & corrected nullptr, bucket & Bezier, L & R ──
    for (const auto& lod : {bucketLOD, bezierLOD}) {
        const auto f0Var = varyingF0(lod.nf);
        const auto enVar = varyingEn(lod.nf);

        for (int si = 0; si < 2; ++si) {
            const auto& strip = (si == 0) ? leftStrip : rightStrip;
            const auto side   = (si == 0) ? "L" : "R";
            const int gs = (si == 0) ? lod.gapLeftStart  : lod.gapRightStart;
            const int ge = (si == 0) ? lod.gapLeftEnd    : lod.gapRightEnd;

            // Original invalid-F0 gap
            {
                auto f0Gap = f0Var;
                for (int i = gs; i < ge; ++i) f0Gap[i] = 0.0f;
                checkStripEquiv(f0Gap, enVar, {},
                                lod.nf, lod.pps, strip,
                                true, false,
                                std::string("origF0 gap ") + side + " " + lod.label);
            }

            // Corrected nullptr gap — segments copy the same varying F0
            {
                std::vector<PitchCorrectionSegment> segs;
                segs.emplace_back(0,  gs, std::vector<float>(f0Var.begin(),       f0Var.begin() + gs));
                segs.emplace_back(ge, lod.nf, std::vector<float>(f0Var.begin() + ge, f0Var.end()));
                checkStripEquiv(f0Var, enVar, segs,
                                lod.nf, lod.pps, strip,
                                false, true,
                                std::string("corrF0 gap ") + side + " " + lod.label);
            }
        }
    }
}

void timelineLayerDrawGridLinesBarsPixelEquivalence()
{
    clipTestEnsureDarkBlueGrey();

    RenderParams params;
    params.visibleStartSeconds = 0.0;
    params.visibleEndSeconds   = 3.0;
    params.pixelsPerSecond     = 100.0;
    params.timeUnit            = 1;
    params.tempo               = 120.0;
    params.themeId             = static_cast<int>(ThemeId::DarkBlueGrey);
    params.viewportWidth       = 300;
    params.viewportHeight      = 200;
    params.viewKind            = "pianoroll";

    const int w = params.viewportWidth;
    const int h = params.viewportHeight;

    juce::Image baseline(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(baseline);
        baseline.clear(juce::Rectangle<int>(w, h));
        TimelineLayerComposer::drawGridLines(g, params);
    }

    // Narrow clip sits between grid lines at x=50 and x=150; boundary
    // at x=51 is 1 px past the line at x=50, exercising the 2 px grid pad.
    const juce::Rectangle<int> narrowClip(51, 0, 100, h);
    juce::Image test(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(test);
        test.clear(juce::Rectangle<int>(w, h));
        g.reduceClipRegion(narrowClip);
        TimelineLayerComposer::drawGridLines(g, params);
    }

    const int diffs = countPixelDiffs(baseline, test, narrowClip);
    expect(diffs == 0,
        "drawGridLines Bars: narrow-clip pixels must match full render (2px pad). diffs="
        + std::to_string(diffs));
}

void timelineLayerDrawGridLinesSecondsPixelEquivalence()
{
    clipTestEnsureDarkBlueGrey();

    RenderParams params;
    params.visibleStartSeconds = 0.0;
    params.visibleEndSeconds   = 20.0;
    params.pixelsPerSecond     = 25.0;
    params.timeUnit            = 0;
    params.tempo               = 120.0;
    params.themeId             = static_cast<int>(ThemeId::DarkBlueGrey);
    params.viewportWidth       = 500;
    params.viewportHeight      = 200;
    params.viewKind            = "pianoroll";

    const int w = params.viewportWidth;
    const int h = params.viewportHeight;

    juce::Image baseline(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(baseline);
        baseline.clear(juce::Rectangle<int>(w, h));
        TimelineLayerComposer::drawGridLines(g, params);
    }

    // Narrow clip at x=124: line at x=125 (5 s marker) is 1 px inside,
    // exercising the 2 px grid pad on the left edge.
    const juce::Rectangle<int> narrowClip(124, 0, 127, h);
    juce::Image test(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(test);
        test.clear(juce::Rectangle<int>(w, h));
        g.reduceClipRegion(narrowClip);
        TimelineLayerComposer::drawGridLines(g, params);
    }

    const int diffs = countPixelDiffs(baseline, test, narrowClip);
    expect(diffs == 0,
        "drawGridLines Seconds: narrow-clip pixels must match full render (2px pad). diffs="
        + std::to_string(diffs));
}

void timelineLayerDrawTimeRulerBarsPixelEquivalence()
{
    clipTestEnsureDarkBlueGrey();

    // pps=30, tempo=120 → secondsPerBeat=0.5, pixelsPerBeat=15, beatInterval=4.
    // beat=4 centre at x=60, label rect [40,80).
    // narrowClip=[40,44): label centre x=60 is 16 px outside clip,
    // left 4 px of label [40,44) remain visible.
    //
    // Algebra: without 21 px pad → endBeat=int(min(44/30,3)/0.5)+1=3
    //   → beat 4 excluded, label at x=60 not drawn, test fails.
    // With pad → endBeat=int(min((44+21)/30,3)/0.5)+1=5
    //   → beat 4 included, pixels [40,44) match baseline.
    RenderParams params;
    params.visibleStartSeconds = 0.0;
    params.visibleEndSeconds   = 3.0;
    params.pixelsPerSecond     = 30.0;
    params.timeUnit            = 1;
    params.tempo               = 120.0;
    params.themeId             = static_cast<int>(ThemeId::DarkBlueGrey);
    params.rulerHeight         = 30;
    params.viewportWidth       = 200;
    params.viewportHeight      = 30;
    params.viewKind            = "pianoroll";

    const int w = params.viewportWidth;
    const int h = params.viewportHeight;

    juce::Image baseline(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(baseline);
        baseline.clear(juce::Rectangle<int>(w, h));
        TimelineLayerComposer::drawTimeRuler(g, params);
    }

    const juce::Rectangle<int> narrowClip(40, 0, 4, h);
    juce::Image test(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(test);
        test.clear(juce::Rectangle<int>(w, h));
        g.reduceClipRegion(narrowClip);
        TimelineLayerComposer::drawTimeRuler(g, params);
    }

    const int diffs = countPixelDiffs(baseline, test, narrowClip);
    expect(diffs == 0,
        "drawTimeRuler Bars: narrow-clip pixels must match full render (21px ruler pad). diffs="
        + std::to_string(diffs));
}

void timelineLayerDrawTimeRulerSecondsPixelEquivalence()
{
    clipTestEnsureDarkBlueGrey();

    RenderParams params;
    params.visibleStartSeconds = 0.0;
    params.visibleEndSeconds   = 20.0;
    params.pixelsPerSecond     = 25.0;
    params.timeUnit            = 0;
    params.tempo               = 120.0;
    params.themeId             = static_cast<int>(ThemeId::DarkBlueGrey);
    params.rulerHeight         = 30;
    params.viewportWidth       = 500;
    params.viewportHeight      = 30;
    params.viewKind            = "pianoroll";

    const int w = params.viewportWidth;
    const int h = params.viewportHeight;

    juce::Image baseline(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(baseline);
        baseline.clear(juce::Rectangle<int>(w, h));
        TimelineLayerComposer::drawTimeRuler(g, params);
    }

    // Label at pixelX=125 spans [105,145). Narrow clip right=124:
    // centre x=125 is 1 px outside; label portion [105,124) remains inside.
    // Without the 21 px pad endTime would be < 5.0 s, excluding x=125.
    // With pad x=125 is included so pixels [105,124) match full render.
    const juce::Rectangle<int> narrowClip(80, 0, 44, h);
    juce::Image test(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(test);
        test.clear(juce::Rectangle<int>(w, h));
        g.reduceClipRegion(narrowClip);
        TimelineLayerComposer::drawTimeRuler(g, params);
    }

    const int diffs = countPixelDiffs(baseline, test, narrowClip);
    expect(diffs == 0,
        "drawTimeRuler Seconds: narrow-clip pixels must match full render (21px ruler pad). diffs="
        + std::to_string(diffs));
}

// ---------------------------------------------------------------------------
// formatSecondsRulerLabel contract — MM:SS formatting for ruler display
// ---------------------------------------------------------------------------
void formatSecondsRulerLabelContract()
{
    auto label49  = TimelineLayerComposer::formatSecondsRulerLabel(49);
    auto label605 = TimelineLayerComposer::formatSecondsRulerLabel(605);

    expect(label49 == "00:49",
           "formatSecondsRulerLabel(49) must be \"00:49\", got: " + label49.toStdString());
    expect(label605 == "10:05",
           "formatSecondsRulerLabel(605) must be \"10:05\", got: " + label605.toStdString());
}

// ---------------------------------------------------------------------------
// selectMarkerInterval milestone contract — interval boundary crossings
// ---------------------------------------------------------------------------
void selectMarkerIntervalMilestoneContract()
{
    expect(std::abs(TimelineLayerComposer::selectMarkerInterval(40.0) - 5.0) < 1e-12,
           "selectMarkerInterval(40.0) must return 5.0");
    expect(std::abs(TimelineLayerComposer::selectMarkerInterval(59.999) - 5.0) < 1e-12,
           "selectMarkerInterval(59.999) must return 5.0 (just below 60)");
    expect(std::abs(TimelineLayerComposer::selectMarkerInterval(60.0) - 1.0) < 1e-12,
           "selectMarkerInterval(60.0) must return 1.0");
}

// ---------------------------------------------------------------------------
// makeRulerScrollDamage contract — entering / exiting bounds and overlap guard
// ---------------------------------------------------------------------------
void makeRulerScrollDamageContract()
{
    constexpr int timelineW = 200;
    constexpr int timelineH = 30;
    const juce::Rectangle<int> timelineBounds(timelineW, timelineH);

    // Forward scroll (delta=+10): content shifts left, new labels enter right.
    // entering expands left by kRulerLabelPaintOverflowX=21 to [169,200).
    // exiting is empty — left-edge stale content scrolled offscreen cleanly.
    {
        const int delta = +10;
        const juce::Rectangle<int> exposed(timelineW - 10, 0, 10, timelineH);
        const auto damage = TimelineLayerComposer::makeRulerScrollDamage(exposed, timelineBounds, delta);

        expect(damage.entering.getX() == 169,
               "delta=+10: entering.x must be 169, got: " + std::to_string(damage.entering.getX()));
        expect(damage.entering.getRight() == timelineW,
               "delta=+10: entering.right must be " + std::to_string(timelineW)
               + ", got: " + std::to_string(damage.entering.getRight()));
        expect(damage.entering.getWidth() == 31,
               "delta=+10: entering width must be 31, got: " + std::to_string(damage.entering.getWidth()));
        expect(timelineBounds.contains(damage.entering),
               "delta=+10: entering must be contained within timeline bounds");
        expect(damage.exiting.isEmpty(),
               "delta=+10: exiting must be empty, got: [" + std::to_string(damage.exiting.getX())
               + "," + std::to_string(damage.exiting.getRight()) + ")");
    }

    // Backward scroll (delta=-10): content shifts right, new labels enter
    // left, old right-edge labels exit. entering=[0,31), exiting=[179,200).
    {
        const int delta = -10;
        const juce::Rectangle<int> exposed(0, 0, 10, timelineH);
        const auto damage = TimelineLayerComposer::makeRulerScrollDamage(exposed, timelineBounds, delta);

        expect(damage.entering.getX() == 0,
               "delta=-10: entering.x must be 0, got: " + std::to_string(damage.entering.getX()));
        expect(damage.entering.getRight() == 31,
               "delta=-10: entering.right must be 31, got: " + std::to_string(damage.entering.getRight()));
        expect(damage.entering.getWidth() == 31,
               "delta=-10: entering width must be 31, got: " + std::to_string(damage.entering.getWidth()));
        expect(timelineBounds.contains(damage.entering),
               "delta=-10: entering must be contained within timeline bounds");

        expect(damage.exiting.getX() == 179,
               "delta=-10: exiting.x must be 179, got: " + std::to_string(damage.exiting.getX()));
        expect(damage.exiting.getRight() == timelineW,
               "delta=-10: exiting.right must be " + std::to_string(timelineW)
               + ", got: " + std::to_string(damage.exiting.getRight()));
        expect(damage.exiting.getWidth() == 21,
               "delta=-10: exiting width must be 21, got: " + std::to_string(damage.exiting.getWidth()));
        expect(timelineBounds.contains(damage.exiting),
               "delta=-10: exiting must be contained within timeline bounds");

        // entering and exiting must not overlap
        expect(!damage.entering.intersects(damage.exiting),
               "delta=-10: entering and exiting must not overlap");
    }

    // Narrow viewport: when entering and exiting would collide the API must
    // fuse them — never return two intersecting rectangles.
    {
        const int delta = -10;
        const juce::Rectangle<int> narrowBounds(25, timelineH);
        const juce::Rectangle<int> exposed(0, 0, 10, timelineH);
        const auto damage = TimelineLayerComposer::makeRulerScrollDamage(exposed, narrowBounds, delta);

        expect(!damage.entering.intersects(damage.exiting),
               "narrow viewport: entering and exiting must not intersect. "
               "entering=[" + std::to_string(damage.entering.getX()) + "," + std::to_string(damage.entering.getRight())
               + ") exiting=[" + std::to_string(damage.exiting.getX()) + "," + std::to_string(damage.exiting.getRight()) + ")");
        expect(narrowBounds.contains(damage.entering),
               "narrow viewport: entering must be contained within timeline bounds");
        expect(damage.exiting.isEmpty() || narrowBounds.contains(damage.exiting),
               "narrow viewport: exiting must be contained within timeline bounds");
    }
}

// ---------------------------------------------------------------------------
// Ruler forward scroll pixel equivalence — moveImageSection + incremental draw
// must match a full redraw. Covers 1-second ruler labels crossing the exposed
// strip boundary.
// ---------------------------------------------------------------------------
void rulerForwardScrollPixelEquivalence()
{
    clipTestEnsureDarkBlueGrey();

    constexpr double pps = 60.0;
    constexpr int w = 200;
    constexpr int h = 30;
    constexpr double oldVisibleStart = 48.8;
    constexpr double newVisibleStart = 49.05;
    constexpr double visibleDuration = static_cast<double>(w) / pps;
    constexpr int scrollDeltaPx = 15;

    auto makeParams = [&](double visibleStart) {
        RenderParams p;
        p.visibleStartSeconds = visibleStart;
        p.visibleEndSeconds   = visibleStart + visibleDuration;
        p.pixelsPerSecond     = pps;
        p.timeUnit            = 0;  // seconds
        p.tempo               = 120.0;
        p.themeId             = static_cast<int>(ThemeId::DarkBlueGrey);
        p.rulerHeight         = h;
        p.viewportWidth       = w;
        p.viewportHeight      = h;
        p.viewKind            = "pianoroll";
        return p;
    };

    const RenderParams oldParams = makeParams(oldVisibleStart);
    const RenderParams newParams = makeParams(newVisibleStart);
    const juce::Rectangle<int> timelineBounds(w, h);

    // 1. Full render of old surface into test surface
    juce::Image testSurface(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(testSurface);
        testSurface.clear(timelineBounds);
        TimelineLayerComposer::drawTimeRuler(g, oldParams);
    }

    // 2. Simulate forward scroll: moveImageSection shifts pixels left by 15px.
    //    src (15,0,185,30) → dst (0,0)
    testSurface.moveImageSection(0, 0, scrollDeltaPx, 0, w - scrollDeltaPx, h);

    // Exposed strip = the new rightmost 15 px that must be repainted.
    // Forward delta=+15 → entering=[164,200), exiting is empty.
    const juce::Rectangle<int> exposedStrip(w - scrollDeltaPx, 0, scrollDeltaPx, h);
    const auto damage = TimelineLayerComposer::makeRulerScrollDamage(
        exposedStrip, timelineBounds, scrollDeltaPx);
    expect(damage.exiting.isEmpty(),
           "forward scroll: exiting must be empty");

    // Clear entering zone to remove stale pixels from both the exposed
    // strip and the label-overflow zone where new labels may extend.
    testSurface.clear(damage.entering);

    // Redraw ruler only within entering damage clip
    {
        juce::Graphics g(testSurface);
        g.reduceClipRegion(damage.entering);
        TimelineLayerComposer::drawTimeRuler(g, newParams);
    }

    // 3. Full render of new surface (the golden baseline)
    juce::Image newBaseline(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(newBaseline);
        newBaseline.clear(timelineBounds);
        TimelineLayerComposer::drawTimeRuler(g, newParams);
    }

    // 4. Pixel comparison — every pixel must match
    const int diffs = countPixelDiffs(testSurface, newBaseline, timelineBounds);
    expect(diffs == 0,
           "ruler forward scroll: incremental render must match full baseline. diffs="
           + std::to_string(diffs));
}

// ---------------------------------------------------------------------------
// Ruler backward scroll pixel equivalence — moveImageSection (shift right) +
// incremental draw must match a full redraw.
// Backward delta=-15 → entering=[0,36), exiting=[179,200).
// exiting clears stale right-edge label text pushed right by moveImageSection.
// ---------------------------------------------------------------------------
void rulerBackwardScrollPixelEquivalence()
{
    clipTestEnsureDarkBlueGrey();

    constexpr double pps = 60.0;
    constexpr int w = 200;
    constexpr int h = 30;
    constexpr double oldVisibleStart = 49.05;
    constexpr double newVisibleStart = 48.8;
    constexpr double visibleDuration = static_cast<double>(w) / pps;
    constexpr int scrollDeltaPx = 15;

    auto makeParams = [&](double visibleStart) {
        RenderParams p;
        p.visibleStartSeconds = visibleStart;
        p.visibleEndSeconds   = visibleStart + visibleDuration;
        p.pixelsPerSecond     = pps;
        p.timeUnit            = 0;  // seconds
        p.tempo               = 120.0;
        p.themeId             = static_cast<int>(ThemeId::DarkBlueGrey);
        p.rulerHeight         = h;
        p.viewportWidth       = w;
        p.viewportHeight      = h;
        p.viewKind            = "pianoroll";
        return p;
    };

    const RenderParams oldParams = makeParams(oldVisibleStart);
    const RenderParams newParams = makeParams(newVisibleStart);
    const juce::Rectangle<int> timelineBounds(w, h);

    // 1. Full render of old surface into test surface
    juce::Image testSurface(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(testSurface);
        testSurface.clear(timelineBounds);
        TimelineLayerComposer::drawTimeRuler(g, oldParams);
    }

    // 2. Simulate backward scroll: moveImageSection shifts pixels right by 15px.
    //    src (0,0,185,30) → dst (15,0)
    testSurface.moveImageSection(scrollDeltaPx, 0, 0, 0, w - scrollDeltaPx, h);

    // Exposed strip = the new leftmost 15 px that must be repainted.
    // Backward delta=-15 → entering=[0,36), exiting=[179,200).
    // exiting clears stale right-edge label text pushed right by moveImageSection.
    const juce::Rectangle<int> exposedStrip(0, 0, scrollDeltaPx, h);
    const auto damage = TimelineLayerComposer::makeRulerScrollDamage(
        exposedStrip, timelineBounds, -scrollDeltaPx);

    // Clear entering zone (new labels from left) and exiting zone (stale
    // right-edge label text), then redraw the ruler in each clip.
    testSurface.clear(damage.entering);
    testSurface.clear(damage.exiting);

    {
        juce::Graphics g(testSurface);
        g.reduceClipRegion(damage.entering);
        TimelineLayerComposer::drawTimeRuler(g, newParams);
    }
    {
        juce::Graphics g(testSurface);
        g.reduceClipRegion(damage.exiting);
        TimelineLayerComposer::drawTimeRuler(g, newParams);
    }

    // 3. Full render of new surface (the golden baseline)
    juce::Image newBaseline(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(newBaseline);
        newBaseline.clear(timelineBounds);
        TimelineLayerComposer::drawTimeRuler(g, newParams);
    }

    // 4. Pixel comparison — every pixel must match
    const int diffs = countPixelDiffs(testSurface, newBaseline, timelineBounds);
    expect(diffs == 0,
           "ruler backward scroll: incremental render must match full baseline. diffs="
           + std::to_string(diffs));
}

// ============================================================================
// Real pixel rendering tests — linked against PianoRollRenderer,
// TimelineLayerComposer, ViewMapper.
// ============================================================================

// ── Composite vertical shrink pixel-equivalence ─────────────────────
// Old-pps render under new-pps render must match clean new-pps render
// pixel-by-pixel — no old background or piano-key leakage.

void compositedVerticalShrinkMatchesCleanRender()
{
    clipTestEnsureDarkBlueGrey();

    constexpr int w = 400, h = 350;
    constexpr float oldPPS = 25.0f, newPPS = 15.0f;
    constexpr int keyW = 60, rulerH = 30;

    auto makeGridParams = [&](float pps) {
        RenderParams p;
        p.visibleStartSeconds = 0.0;
        p.visibleEndSeconds   = 4.0;
        p.pixelsPerSecond     = 100.0;
        p.timeUnit            = 1;  // Bars
        p.tempo               = 120.0;
        p.themeId             = static_cast<int>(ThemeId::DarkBlueGrey);
        p.pixelsPerSemitone   = pps;
        p.worldTopY           = 0.0f;
        p.rulerHeight         = 0;
        p.laneStyle           = encodeLaneStyle(true, 0, 1);
        p.viewportWidth       = w - keyW;
        p.viewportHeight      = h - rulerH;
        p.viewKind            = "pianoroll";
        return p;
    };

    auto makeRenderCtx = [&](float pps, const juce::Rectangle<int>& bounds) {
        PianoRollRenderer::RenderContext rctx;
        rctx.width            = w;
        rctx.height           = bounds.getHeight();
        rctx.pianoKeyWidth    = keyW;
        rctx.rulerHeight      = 0;
        rctx.pixelsPerSecond  = 100.0;
        rctx.pixelsPerSemitone = pps;
        rctx.minMidi          = 24.0f;
        rctx.maxMidi          = 108.0f;
        rctx.scaleRootNote    = 0;
        rctx.scaleType        = 1;
        rctx.noteNameMode     = NoteNameMode::COnly;
        rctx.coords           = ViewMapper{0.0, 100.0, keyW, w - keyW, bounds.getHeight(), pps, 0.0f, 108.0f};
        rctx.rasterBounds     = bounds;
        return rctx;
    };

    // Shared piano-key render area (y starts below ruler)
    const juce::Rectangle<int> pianoArea(0, rulerH, w, h - rulerH);

    // Image A — composited: old view background + grid + lane + keys,
    //           then new view background + grid + lane + keys on top.
    juce::Image imgA(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(imgA);

        // Old view (larger pps)
        {
            const auto oldParams = makeGridParams(oldPPS);
            // full-area background — distinct from new to detect leaks
            g.setColour(juce::Colour(0xFF3A0000)); // dark red
            g.fillRect(pianoArea);

            {
                juce::Graphics::ScopedSaveState ss(g);
                g.reduceClipRegion(pianoArea);
                {
                    juce::Graphics::ScopedSaveState gs(g);
                    g.addTransform(juce::AffineTransform::translation(static_cast<float>(keyW), static_cast<float>(rulerH)));
                    TimelineLayerComposer::drawLaneStripRepeats(g, oldParams);
                    TimelineLayerComposer::drawGridLines(g, oldParams);
                }
            }

            // Old-pitch piano keys (must be fully covered by new-pitch keys)
            {
                PianoRollRenderer renderer;
                auto oldCtx = makeRenderCtx(oldPPS, pianoArea);
                juce::Graphics::ScopedSaveState ss(g);
                g.reduceClipRegion(pianoArea);
                g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerH)));
                oldCtx.rasterBounds = juce::Rectangle<int>(0, 0, w, h - rulerH);
                renderer.drawPianoKeys(g, oldCtx);
            }
        }

        // New view (smaller pps) on top — must fully cover old pixels
        {
            const auto newParams = makeGridParams(newPPS);
            g.setColour(UIColors::rollBackground);
            g.fillRect(pianoArea);

            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(pianoArea);
            {
                juce::Graphics::ScopedSaveState gs(g);
                g.addTransform(juce::AffineTransform::translation(static_cast<float>(keyW), static_cast<float>(rulerH)));
                TimelineLayerComposer::drawLaneStripRepeats(g, newParams);
                TimelineLayerComposer::drawGridLines(g, newParams);
            }

            PianoRollRenderer renderer;
            auto ctx = makeRenderCtx(newPPS, pianoArea);
            g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerH)));
            ctx.rasterBounds = juce::Rectangle<int>(0, 0, w, h - rulerH);
            renderer.drawPianoKeys(g, ctx);
        }
    }

    // Image B — clean render of new view only.
    juce::Image imgB(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(imgB);
        const auto newParams = makeGridParams(newPPS);

        g.setColour(UIColors::rollBackground);
        g.fillRect(pianoArea);

        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(pianoArea);
        {
            juce::Graphics::ScopedSaveState gs(g);
            g.addTransform(juce::AffineTransform::translation(static_cast<float>(keyW), static_cast<float>(rulerH)));
            TimelineLayerComposer::drawLaneStripRepeats(g, newParams);
            TimelineLayerComposer::drawGridLines(g, newParams);
        }

        PianoRollRenderer renderer;
        auto ctx = makeRenderCtx(newPPS, pianoArea);
        g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerH)));
        ctx.rasterBounds = juce::Rectangle<int>(0, 0, w, h - rulerH);
        renderer.drawPianoKeys(g, ctx);
    }

    // Full-area comparison
    const int diffs = countPixelDiffs(imgA, imgB, pianoArea);
    expect(diffs == 0,
           "composited vertical shrink must match clean render (old bg/key pixels, "
           "inward shrink, fractures). diffs=" + std::to_string(diffs));
}


// ---------------------------------------------------------------------------
// PAGE 进入边缘稳定性：正向/反向越界只翻一次页，不连续跟随逐帧翻页。
// 不含时间量化、epsilon 生产逻辑、source-token 假测试。
// ---------------------------------------------------------------------------
void pageEdgeStability()
{
    // Page, viewportWidth=1000, pps=100, currentStart=20, target=25
    // target 在当前页内 → visibleStart 保持 20
    {
        TimelineViewportRequest req;
        req.kind = TimelineViewportRequest::Kind::Page;
        req.viewportWidth = 1000;
        req.pixelsPerSecond = 100.0;
        req.currentVisibleStartSeconds = 20.0;
        req.targetTime = 25.0;
        auto cam = TimelineViewportPolicy::resolve(req);
        expect(std::abs(cam.visibleStartSeconds - 20.0) < 1e-12,
               "Page: target in page must keep visibleStart at 20");
    }

    // 正向越界 target=30.1 → visibleStart 变为 30.1
    // 随后 currentStart=30.1、target=30.2 → visibleStart 仍为 30.1
    {
        TimelineViewportRequest req;
        req.kind = TimelineViewportRequest::Kind::Page;
        req.viewportWidth = 1000;
        req.pixelsPerSecond = 100.0;
        req.currentVisibleStartSeconds = 20.0;
        req.targetTime = 30.1;
        auto cam = TimelineViewportPolicy::resolve(req);
        expect(std::abs(cam.visibleStartSeconds - 30.1) < 1e-12,
               "Page: forward overshoot must flip visibleStart to 30.1");

        req.currentVisibleStartSeconds = 30.1;
        req.targetTime = 30.2;
        cam = TimelineViewportPolicy::resolve(req);
        expect(std::abs(cam.visibleStartSeconds - 30.1) < 1e-12,
               "Page: second forward target must not flip again");
    }

    // 反向越界 currentStart=20、target=19.9 → visibleStart 变为 9.9
    // 随后 currentStart=9.9、target=19.8 → visibleStart 仍为 9.9
    {
        TimelineViewportRequest req;
        req.kind = TimelineViewportRequest::Kind::Page;
        req.viewportWidth = 1000;
        req.pixelsPerSecond = 100.0;
        req.currentVisibleStartSeconds = 20.0;
        req.targetTime = 19.9;
        auto cam = TimelineViewportPolicy::resolve(req);
        expect(std::abs(cam.visibleStartSeconds - 9.9) < 1e-12,
               "Page: backward overshoot must flip visibleStart to 9.9");

        req.currentVisibleStartSeconds = 9.9;
        req.targetTime = 19.8;
        cam = TimelineViewportPolicy::resolve(req);
        expect(std::abs(cam.visibleStartSeconds - 9.9) < 1e-12,
               "Page: second backward target must not flip again");
    }

    // Cont 对照：target=30.1 → visibleStart=25.1；target=30.2 → visibleStart=25.2
    {
        TimelineViewportRequest req;
        req.kind = TimelineViewportRequest::Kind::Cont;
        req.viewportWidth = 1000;
        req.pixelsPerSecond = 100.0;
        req.targetTime = 30.1;
        auto cam = TimelineViewportPolicy::resolve(req);
        expect(std::abs(cam.visibleStartSeconds - 25.1) < 1e-12,
               "Cont: target 30.1 must centre at visibleStart 25.1");

        req.targetTime = 30.2;
        cam = TimelineViewportPolicy::resolve(req);
        expect(std::abs(cam.visibleStartSeconds - 25.2) < 1e-12,
               "Cont: target 30.2 must centre at visibleStart 25.2");
    }
}


} // namespace

int main()
{
    std::cout << "=== OpenTune PianoRoll Behavior Tests ===\n\n";

    try {
        pianoRollPendingSeekPresentationSourceContract();
        pianoRollPlayheadOverlayPaintsDirectly();
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
        pitchCurveSnapshotForEachCorrectionF0SpanContract();
        f0CurveOriginalBucketExtremesWithAsymmetricEnergy();
        f0CurveCorrectedSpanExtremes();
        f0CurveGapIsolation();
        f0CurveStripPixelEquivalenceContract();

        // Narrow-clip pixel-equivalence for grid/ruler optimizations
        timelineLayerDrawGridLinesBarsPixelEquivalence();
        timelineLayerDrawGridLinesSecondsPixelEquivalence();
        timelineLayerDrawTimeRulerBarsPixelEquivalence();
        timelineLayerDrawTimeRulerSecondsPixelEquivalence();

        // Ruler scroll damage and pixel-equivalence for incremental rendering
        formatSecondsRulerLabelContract();
        selectMarkerIntervalMilestoneContract();
        makeRulerScrollDamageContract();
        rulerForwardScrollPixelEquivalence();
        rulerBackwardScrollPixelEquivalence();

        // ── Page viewport policy edge stability ────────────────────────
        pageEdgeStability();

        // ── Real pixel rendering test ──────────────────────────────────
        compositedVerticalShrinkMatchesCleanRender();
    } catch (const std::exception& e) {
        ++failures;
        std::cout << "[FAIL] uncaught exception: " << e.what() << "\n";
    }

    f0TestRestoreTheme();
    clipTestRestoreTheme();

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "ALL PIANOROLL BEHAVIOR TESTS PASSED\n";
        return 0;
    }

    std::cout << failures << " PIANOROLL BEHAVIOR TEST(S) FAILED\n";
    return 1;
}
