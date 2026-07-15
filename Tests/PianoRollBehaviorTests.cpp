#include "../Source/Utils/PianoRollEditAction.h"
#include "../Source/Utils/UndoManager.h"
#include "../Source/Standalone/UI/PianoRoll/InteractionState.h"
#include "../Source/Content/EditableContentSnapshot.h"
#include "../Source/Content/CaptureSegmentContent.h"
#include "../Source/Utils/TimeGrid.h"
#include "../Source/Utils/F0Timeline.h"
#include "../Source/Utils/ContentTimelineProjection.h"
#include "../Source/Standalone/UI/ViewMapper.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef OPENTUNE_SOURCE_DIR
#error "OPENTUNE_SOURCE_DIR must be defined by CMake"
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

void pianoRollInitialF0ViewSourceContract()
{
    const auto header = readText("Source/Standalone/UI/PianoRollComponent.h");
    const auto source = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto standaloneHeader = readText("Source/Standalone/PluginEditor.h");
    const auto standaloneSource = readText("Source/Standalone/PluginEditor.cpp");
    const auto pluginHeader = readText("Source/Plugin/PluginEditor.h");
    const auto pluginSource = readText("Source/Plugin/PluginEditor.cpp");

    expectTokens("PianoRoll initial F0 view declaration",
                 header,
                 {"void requestInitialF0View(ContentKey contentKey);",
                  "bool tryConsumeInitialF0View(ContentKey contentKey);",
                  "std::set<ContentKey> pendingInitialF0ViewRequests_;"});

    const auto helper = extractBlockByMarker(
        source, "bool PianoRollComponent::tryConsumeInitialF0View(ContentKey contentKey)");
    expectTokens("PianoRoll initial F0 view consume",
                 helper,
                 {"pendingInitialF0ViewRequests_.find(contentKey)",
                  "contentKey != editedContentKey_",
                  "snapshot->originalF0State != OriginalF0State::Ready",
                  "currentCurve_->getSnapshot()",
                  "snapshot->timeGrid",
                  "!projection.isValid()",
                  "std::isfinite(f0)",
                  "f0 >= 20.0f",
                  "f0 <= 2000.0f",
                  "F0Timeline f0Timeline",
                  "f0Timeline.timeAtFrame(firstFrame)",
                  "snapshot->timeGrid->tauForward(sourceSeconds)",
                  "projection.projectContentTimeToTimeline(contentSeconds)",
                  "TimelineViewportRequest::Kind::Manual",
                  "camera_.pixelsPerSecond",
                  "TimelineViewportPolicy::resolve(request)",
                  "verticalScrollOffset_ = (maxMidi_ - startMidi) * pixelsPerSemitone_",
                  "contentHeight * 0.5f",
                  "std::clamp",
                  "getTotalHeight() - contentHeight"});
    expectNoTokens("PianoRoll initial F0 view consume",
                   helper,
                   {"commitViewportRequest", "fitToScreen"});

    const size_t rebuildPosition = helper.find("rebuildTimelineCoverage();");
    const size_t scrollBarPosition = helper.find("updateScrollBars();", rebuildPosition);
    const size_t repaintPosition = helper.find("repaint();", scrollBarPosition);
    expect(rebuildPosition != std::string::npos
               && scrollBarPosition != std::string::npos
               && repaintPosition != std::string::npos
               && rebuildPosition < scrollBarPosition
               && scrollBarPosition < repaintPosition,
           "PianoRoll initial F0 view must rebuild coverage, update scrollbars, then repaint");

    expectTokens("PianoRoll initial F0 view visibility hooks",
                 extractBlockByMarker(source, "void PianoRollComponent::visibilityChanged()"),
                 {"tryConsumeInitialF0View(editedContentKey_);"});
    expectTokens("PianoRoll initial F0 view resize hook",
                 extractBlockByMarker(source, "void PianoRollComponent::resized()"),
                 {"tryConsumeInitialF0View(editedContentKey_);"});
    expectTokens("PianoRoll initial F0 view heartbeat hook",
                 extractBlockByMarker(source, "void PianoRollComponent::onHeartbeatTick()"),
                 {"tryConsumeInitialF0View(editedContentKey_);"});
    expectNoTokens("PianoRoll legacy region focus path",
                   header + source + standaloneSource,
                   {"focusActiveContentForRegionSwitch"});

    expectTokens("Standalone OriginalF0 edge baseline",
                 standaloneHeader + standaloneSource,
                 {"OriginalF0State lastPianoRollOriginalF0State_",
                  "const OriginalF0State currentOriginalF0State",
                  "lastPianoRollOriginalF0State_ == OriginalF0State::Extracting",
                  "currentOriginalF0State == OriginalF0State::Ready",
                  "pianoRoll_.requestInitialF0View(activeKey);"});
    expect(countOccurrences(standaloneSource, "pianoRoll_.requestInitialF0View(activeKey);") == 1,
           "Standalone must request initial F0 view only from its edge detector");
    expectNoTokens("Standalone selection sync initial F0 path",
                   extractBlockByMarker(
                       standaloneSource,
                       "void OpenTuneAudioProcessorEditor::syncPianoRollFromPlacementSelection(int trackId, int placementIndex)"),
                   {"requestInitialF0View", "focusActiveContentForRegionSwitch"});

    const auto pluginTimer = extractBlockByMarker(
        pluginSource, "void OpenTuneAudioProcessorEditor::timerCallback()");
    expectTokens("VST3 OriginalF0 observation table",
                 pluginHeader + pluginSource,
                 {"std::map<ContentKey, OriginalF0State> lastObservedOriginalF0States_;"});
    expectTokens("VST3 OriginalF0 observation timer",
                 pluginTimer,
                 {"syncContentProjectionToPianoRoll();",
                  "for (const auto& placement : sync.placements)",
                  "processorRef_.getCaptureSession()",
                  "session->listSegments()",
                  "lastObservedOriginalF0States_.find(contentKey)",
                  "previous->second == OriginalF0State::Extracting",
                  "previous->second == OriginalF0State::NotRequested",
                  "currentState == OriginalF0State::Ready",
                  "pianoRoll_.requestInitialF0View(contentKey);",
                  "lastObservedOriginalF0States_[contentKey] = currentState;"});
    expect(countOccurrences(pluginSource, "pianoRoll_.requestInitialF0View(contentKey);") == 1,
           "VST3 must request initial F0 view only from its edge detector");
    expectNoTokens("VST3 Editor capture callback path",
                   pluginHeader + pluginSource,
                   {"updateRegularCaptureSessionCallback",
                    "clearRegularCaptureSessionCallback",
                    "regularCaptureCallbackSession_",
                    "setActiveSegmentChangedCallback"});
}

void initialF0ViewMathBehavior()
{
    const std::vector<float> originalF0 = {
        std::numeric_limits<float>::quiet_NaN(),
        10.0f,
        440.0f,
        2001.0f};
    const F0Timeline f0Timeline(100, 1000.0, static_cast<int>(originalF0.size()));

    int firstFrame = -1;
    float startFrequency = 0.0f;
    for (int frame = 0; frame < static_cast<int>(originalF0.size()); ++frame) {
        const float f0 = originalF0[static_cast<size_t>(frame)];
        if (std::isfinite(f0) && f0 >= 20.0f && f0 <= 2000.0f) {
            firstFrame = frame;
            startFrequency = f0;
            break;
        }
    }
    expect(firstFrame == 2, "initial F0 scan must select the first finite 20..2000 Hz frame");

    TimeHandle clipStart;
    clipStart.id = 1;
    clipStart.kind = HandleKind::ClipStart;
    clipStart.locked = true;

    TimeHandle internal;
    internal.id = 2;
    internal.source_seconds = 0.5;
    internal.output_seconds = 0.25;
    internal.kind = HandleKind::UserAdded;

    TimeHandle clipEnd;
    clipEnd.id = 3;
    clipEnd.source_seconds = 2.0;
    clipEnd.output_seconds = 2.0;
    clipEnd.kind = HandleKind::ClipEnd;
    clipEnd.locked = true;

    const auto timeGrid = TimeGridSnapshot::makeFromHandles(
        {clipStart, internal, clipEnd});
    expect(timeGrid != nullptr && !timeGrid->isIdentity(),
           "initial F0 math must use a valid non-identity TimeGrid");
    if (timeGrid == nullptr || firstFrame < 0)
        return;

    const double sourceSeconds = f0Timeline.timeAtFrame(firstFrame);
    const double contentSeconds = timeGrid->tauForward(sourceSeconds);
    const ContentTimelineProjection projection{10.0, 8.0, 2.0};
    const double timelineSeconds = projection.projectContentTimeToTimeline(contentSeconds);
    expect(std::abs(sourceSeconds - 0.2) < 1.0e-9,
           "F0Timeline timeAtFrame must produce the source frame time");
    expect(std::abs(contentSeconds - 0.1) < 1.0e-9,
           "TimeGrid tauForward must transform the source frame time");
    expect(std::abs(timelineSeconds - 10.4) < 1.0e-9,
           "ContentTimelineProjection must transform content time to timeline time");

    const double manualVisibleStartSeconds = timelineSeconds;
    const double currentPixelsPerSecond = 80.0;
    const float pixelsPerSemitone = 25.0f;
    const int contentHeight = 400;
    const ViewMapper xMapper{
        manualVisibleStartSeconds,
        currentPixelsPerSecond,
        60,
        640,
        contentHeight,
        pixelsPerSemitone,
        0.0f,
        108.0f};
    expect(xMapper.timeToX(timelineSeconds) == xMapper.contentStartX,
           "manual initial F0 view must place the first point at contentStartX");
    expect(std::abs(xMapper.pixelsPerSecond - currentPixelsPerSecond) < 1.0e-9,
           "initial F0 view must preserve the current pixelsPerSecond");

    const float startMidi = xMapper.freqToMidi(startFrequency);
    float verticalScrollOffset = (108.0f - startMidi) * pixelsPerSemitone
        - contentHeight * 0.5f;
    const float maxVerticalScroll = std::max(
        0.0f, (108.0f - 24.0f) * pixelsPerSemitone - contentHeight);
    verticalScrollOffset = std::clamp(verticalScrollOffset, 0.0f, maxVerticalScroll);
    const ViewMapper yMapper{
        manualVisibleStartSeconds,
        currentPixelsPerSecond,
        60,
        640,
        contentHeight,
        pixelsPerSemitone,
        verticalScrollOffset,
        108.0f};
    expect(std::abs(yMapper.freqToY(startFrequency) - contentHeight * 0.5f) < 1.0e-4f,
           "initial F0 view Y formula must center the first point in the content area");
    expect(std::abs(yMapper.pixelsPerSecond - 80.0) < 1.0e-9,
           "initial F0 math must not change pixelsPerSecond");
    expect(std::abs(yMapper.pixelsPerSemitone - pixelsPerSemitone) < 1.0e-6f,
           "initial F0 math must not change pixelsPerSemitone");
}

void pianoRollPendingSeekPresentationSourceContract()
{
    const auto source = readText("Source/Standalone/UI/PianoRollComponent.cpp");
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
                 {"bool seekRequestDispatched = false;",
                  "seekRequestDispatched = l.playheadPositionChangeRequested(time) || seekRequestDispatched;"});
    expectTokens("PianoRoll accepted seek presentation",
                 notifyBlock,
                 {"playheadTimeForPaint_ = time;", "pendingSeekTime_ = time;"});
    expectTokens("PianoRoll rejected seek presentation",
                 notifyBlock,
                 {"pendingSeekTime_ = -1.0;", "playheadTimeForPaint_ = readPlayheadTime();"});
    expectTokens("PianoRoll heartbeat pending confirmation",
                 heartbeatBlock,
                 {"std::abs(currentPlayheadTime - pendingSeekTime_) < 0.05"});
    expectTokens("PianoRoll VBlank pending confirmation",
                 vblankBlock,
                 {"std::abs(currentPlayheadTime - pendingSeekTime_) < 0.05"});
    expectNoTokens("PianoRoll hidden VBlank path",
                   hiddenVBlankBlock,
                   {"pendingSeekTime_ = -1.0;"});
    expectNoTokens("PianoRoll retained playhead architecture",
                   source,
                   {"playheadOverlay_", "PlayheadOverlayComponent"});
}

void pianoRollRetainedDirtyRectSourceContract()
{
    const auto source = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto notifyBlock = extractBlockByMarker(
        source, "toolCtx.notifyPlayheadChange = [this](double time)");
    const auto drawPlayheadBlock = extractBlockByMarker(
        source, "void PianoRollComponent::drawPlayhead(juce::Graphics& g)");
    const auto playheadDirtyRectBlock = extractBlockByMarker(
        source, "juce::Rectangle<int> PianoRollComponent::playheadDirtyRect() const");
    const auto rebuildTimelineCoverageBlock = extractBlockByMarker(
        source, "void PianoRollComponent::rebuildTimelineCoverage()");
    const auto rebuildSurface = rebuildTimelineCoverageBlock.find(
        "rebuildViewportSurfaceFromReadyTiles();");
    const auto rebuildPlayhead = rebuildTimelineCoverageBlock.find(
        "lastPlayheadDirtyRect_ = playheadDirtyRect();", rebuildSurface);

    expectTokens("PianoRoll retained playhead draw",
                 drawPlayheadBlock,
                 {"g.reduceClipRegion(timeAxisRect())"});
    expectTokens("PianoRoll retained playhead dirty rect",
                 playheadDirtyRectBlock,
                 {"const auto axis = timeAxisRect();", "getIntersection(axis)"});
    expectTokens("PianoRoll retained playhead dirty rect tracking",
                 notifyBlock,
                 {"lastPlayheadDirtyRect_ = playheadDirtyRect();"});
    expect(rebuildSurface != std::string::npos
               && rebuildPlayhead != std::string::npos
               && rebuildSurface < rebuildPlayhead,
           "PianoRoll coverage rebuild must refresh the retained playhead dirty rect after rebuilding the viewport surface");
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
                 {"processorRef_.setPosition(timeSeconds);", "return true;"});
}

void documentControllerOwnsNoTransportContract()
{
    const auto documentControllerHeader = readText("Source/ARA/OpenTuneDocumentController.h");
    const auto documentControllerSource = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto processorHeader = readText("Source/PluginProcessor.h");
    const auto controllerSource = documentControllerHeader + documentControllerSource;

    expectNoTokens("ARA DocumentController transport ownership",
                   controllerSource,
                   {"playbackPositionSource_",
                    "playbackIsPlaying_",
                    "getPlaybackPositionSource",
                    "getPlaybackPosition",
                    "isPlaying() const noexcept",
                    "updateTransport",
                    "std::shared_ptr<std::atomic<double>>"});
    expectTokens("ARA DocumentController cycle request declaration",
                 documentControllerHeader,
                 {"bool requestEnableCycle(bool enabled);"});

    const auto cycleRequestBlock = extractBlockByMarker(
        documentControllerSource, "bool OpenTuneDocumentController::requestEnableCycle(bool enabled)");
    expectTokens("ARA DocumentController cycle request delegation",
                 cycleRequestBlock,
                 {"auto* playbackController = getDocumentController()->getHostPlaybackController();",
                  "playbackController->requestEnableCycle(enabled);",
                  "return true;"});

    const auto snapshotBlock = extractBlockByMarker(processorHeader, "struct HostTransportSnapshot");
    expectNoTokens("HostTransportSnapshot loop ownership",
                   snapshotBlock,
                   {"loopEnabled", "loopPpqStart", "loopPpqEnd"});

}

void processorOwnsSinglePlayHeadStateContract()
{
    const auto header = readText("Source/PluginProcessor.h");
    const auto source = readText("Source/PluginProcessor.cpp");
    const auto playHeadStateBlock = extractBlockByMarker(header, "struct PlayHeadState");

    expectTokens("processor-owned PlayHeadState declaration",
                 playHeadStateBlock,
                 {"std::atomic<bool> isPlaying { false };",
                  "std::atomic<bool> isLooping { false };",
                  "std::atomic<double> timeInSeconds { 0.0 };",
                  "std::atomic<double> loopPpqStart { 0.0 };",
                  "std::atomic<double> loopPpqEnd { 0.0 };",
                  "void update(const juce::Optional<juce::AudioPlayHead::PositionInfo>& info);",
                  "void reset();"});
    expect(countOccurrences(header, "PlayHeadState playHeadState_") == 1,
           "processor must own exactly one PlayHeadState");
    expectTokens("processor-owned PlayHeadState accessor",
                 header,
                 {"const PlayHeadState& getPlayHeadState() const noexcept { return playHeadState_; }"});

    const auto updateBlock = extractBlockByMarker(
        source, "void PlayHeadState::update(const juce::Optional<juce::AudioPlayHead::PositionInfo>& info)");
    expectTokens("PlayHeadState valid PositionInfo update",
                 updateBlock,
                 {"const auto& positionInfo = *info;",
                  "timeInSeconds.store(positionInfo.getTimeInSeconds().orFallback(0.0), std::memory_order_relaxed);",
                  "isPlaying.store(positionInfo.getIsPlaying(), std::memory_order_relaxed);",
                  "isLooping.store(positionInfo.getIsLooping(), std::memory_order_relaxed);",
                  "if (const auto loopPoints = positionInfo.getLoopPoints())",
                  "loopPpqStart.store(loopPoints->ppqStart, std::memory_order_relaxed);",
                  "loopPpqEnd.store(loopPoints->ppqEnd, std::memory_order_relaxed);"});

    const auto emptyUpdateBlock = extractBlockByMarker(updateBlock, "if (!info.hasValue())");
    expectTokens("PlayHeadState empty PositionInfo update",
                 emptyUpdateBlock,
                 {"isPlaying.store(false, std::memory_order_relaxed);",
                  "isLooping.store(false, std::memory_order_relaxed);",
                  "return;"});
    expectNoTokens("PlayHeadState empty PositionInfo update",
                   emptyUpdateBlock,
                   {"timeInSeconds.store(", "loopPpqStart.store(", "loopPpqEnd.store("});

    const auto resetBlock = extractBlockByMarker(source, "void PlayHeadState::reset()");
    expectTokens("PlayHeadState reset",
                 resetBlock,
                 {"update(juce::nullopt);"});
}

void processBlockUsesSinglePositionInfoContract()
{
    const auto source = readText("Source/PluginProcessor.cpp");
    const auto processBlock = extractBlockByMarker(
        source, "void OpenTuneAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer");
    const auto araBlock = extractBlockByMarker(processBlock, "if (isBoundToARA())");
    const auto araHostBlock = extractBlockByMarker(
        araBlock, "if (auto* hostPlayHead = getPlayHead())");
    const auto captureBlock = extractBlockByMarker(
        processBlock, "if (auto* captureSession = getCaptureSession())");

    expect(countOccurrences(araBlock, "hostPlayHead->getPosition()") == 1,
           "ARA processBlock must read host PositionInfo exactly once");
    expectTokens("ARA processBlock PositionInfo capture",
                 araBlock,
                 {"juce::Optional<juce::AudioPlayHead::PositionInfo> positionInfo;",
                  "if (auto* hostPlayHead = getPlayHead())",
                  "positionInfo = hostPlayHead->getPosition();",
                  "playHeadState_.update(positionInfo);",
                  "updateHostTransportSnapshot(*positionInfo);",
                  "processBlockForARA(buffer, isRealtime(), *positionInfo)",
                  "const juce::AudioPlayHead::PositionInfo emptyPositionInfo;",
                  "processBlockForARA(buffer, isRealtime(), emptyPositionInfo)"});

    const size_t positionRead = araBlock.find("positionInfo = hostPlayHead->getPosition();");
    const size_t stateUpdate = araBlock.find("playHeadState_.update(positionInfo);");
    const size_t metadataUpdate = araBlock.find("updateHostTransportSnapshot(*positionInfo);");
    const size_t araForward = araBlock.find("processBlockForARA(buffer, isRealtime(), *positionInfo)");
    expect(positionRead != std::string::npos
               && stateUpdate != std::string::npos
               && metadataUpdate != std::string::npos
               && araForward != std::string::npos
               && positionRead < stateUpdate
               && stateUpdate < metadataUpdate
               && metadataUpdate < araForward,
           "ARA processBlock must update processor state before metadata and ARA forwarding");
    expectNoTokens("ARA processBlock host read scope",
                   araHostBlock,
                   {"processBlockForARA(buffer, isRealtime(), *positionInfo)"});
    expectNoTokens("ARA processBlock legacy transport path",
                   processBlock,
                   {"getDocumentController()->updateTransport", "getPositionAtomic"});

    expect(countOccurrences(captureBlock, "hostPlayHead->getPosition()") == 1,
           "non-ARA capture must read one Optional PositionInfo");
    expectTokens("non-ARA capture PositionInfo state update",
                 captureBlock,
                 {"juce::Optional<juce::AudioPlayHead::PositionInfo> positionInfo;",
                  "if (auto* hostPlayHead = getPlayHead())",
                  "positionInfo = hostPlayHead->getPosition();",
                  "playHeadState_.update(positionInfo);",
                  "const auto& hostPositionInfo = *positionInfo;",
                  "updateHostTransportSnapshot(hostPositionInfo);"});
}

void uiReadsProcessorPlayHeadStateContract()
{
    const auto pianoRollHeader = readText("Source/Standalone/UI/PianoRollComponent.h");
    const auto pianoRollSource = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto arrangementHeader = readText("Source/Standalone/UI/ArrangementViewComponent.h");
    const auto arrangementSource = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto pluginEditorSource = readText("Source/Plugin/PluginEditor.cpp");
    const auto standaloneEditorSource = readText("Source/Standalone/PluginEditor.cpp");

    expectTokens("PianoRoll PlayHeadState declaration",
                 pianoRollHeader,
                 {"struct PlayHeadState;",
                  "void setPlayHeadState(const PlayHeadState& state);",
                  "const PlayHeadState* playHeadState_ = nullptr;"});
    expectTokens("PianoRoll PlayHeadState binding and reads",
                 extractBlockByMarker(
                     pianoRollSource, "void PianoRollComponent::setPlayHeadState(const PlayHeadState& state)"),
                 {"playHeadState_ = &state;", "playheadTimeForPaint_ = readPlayheadTime();"});
    expectTokens("PianoRoll direct PlayHeadState reads",
                 pianoRollSource,
                 {"playHeadState_->timeInSeconds.load(std::memory_order_relaxed)",
                  "playHeadState_->isPlaying.load(std::memory_order_relaxed)"});

    expectTokens("Arrangement PlayHeadState declaration",
                 arrangementHeader,
                 {"void setPlayHeadState(const PlayHeadState& state);",
                  "const PlayHeadState* playHeadState_ = nullptr;"});
    expectTokens("Arrangement PlayHeadState binding and reads",
                 extractBlockByMarker(
                     arrangementSource, "void ArrangementViewComponent::setPlayHeadState(const PlayHeadState& state)"),
                 {"playHeadState_ = &state;", "playheadTimeForPaint_ = readPlayheadSeconds();"});
    expectTokens("Arrangement direct PlayHeadState reads",
                 arrangementSource,
                 {"playHeadState_->timeInSeconds.load(std::memory_order_relaxed)",
                  "playHeadState_->isPlaying.load(std::memory_order_relaxed)"});

    expectNoTokens("UI legacy playhead source",
                   pianoRollHeader + pianoRollSource + arrangementHeader + arrangementSource,
                   {"weak_ptr", "positionSource_", "positionAtomic_", "getPositionAtomic",
                    "setIsPlaying", "setPlayheadPositionSource"});

    expectTokens("VST3 editor PlayHeadState binding",
                 pluginEditorSource,
                 {"pianoRoll_.setPlayHeadState(processorRef_.getPlayHeadState());"});
    expectTokens("Standalone editor PlayHeadState binding",
                 standaloneEditorSource,
                 {"arrangementView_.setPlayHeadState(processorRef_.getPlayHeadState());",
                  "pianoRoll_.setPlayHeadState(processorRef_.getPlayHeadState());"});
    expectNoTokens("editor legacy playhead binding",
                   pluginEditorSource + standaloneEditorSource,
                   {"setPlayheadPositionSource", "getPositionAtomic",
                    "pianoRoll_.setIsPlaying(", "arrangementView_.setIsPlaying("});
}

void loopRequestAndStandaloneWriteContract()
{
    const auto pluginEditorSource = readText("Source/Plugin/PluginEditor.cpp");
    const auto standaloneEditorSource = readText("Source/Standalone/PluginEditor.cpp");
    const auto processorSource = readText("Source/PluginProcessor.cpp");
    const auto arrangementSource = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");

    const auto pluginLoopBlock = extractBlockByMarker(
        pluginEditorSource, "void OpenTuneAudioProcessorEditor::loopToggled(bool enabled)");
    expectTokens("VST3 ARA loop request",
                 pluginLoopBlock,
                 {"docController->requestEnableCycle(enabled)",
                  "surfaceRegularVst3HostControlledTransport(enabled ? \"loop-on\" : \"loop-off\");"});
    expectNoTokens("VST3 local loop write",
                   pluginLoopBlock,
                   {"processorRef_.setLoopEnabled("});
    expectNoTokens("VST3 local loop write",
                   pluginEditorSource,
                   {"processorRef_.setLoopEnabled("});

    expectTokens("Standalone loop write",
                 extractBlockByMarker(
                     standaloneEditorSource, "void OpenTuneAudioProcessorEditor::loopToggled(bool enabled)"),
                 {"processorRef_.setLoopEnabled(enabled);"});

    expectTokens("processor setPlaying PlayHeadState write",
                 extractBlockByMarker(
                     processorSource, "void OpenTuneAudioProcessor::setPlaying(bool playing)"),
                 {"playHeadState_.isPlaying.store(true, std::memory_order_relaxed);",
                  "playHeadState_.isPlaying.store(false, std::memory_order_relaxed);"});
    expectTokens("processor setPosition PlayHeadState write",
                 extractBlockByMarker(
                     processorSource, "void OpenTuneAudioProcessor::setPosition(double seconds)"),
                 {"playHeadState_.timeInSeconds.store(seconds, std::memory_order_relaxed);"});
    expectTokens("processor setLoopEnabled PlayHeadState write",
                 extractBlockByMarker(
                     processorSource, "void OpenTuneAudioProcessor::setLoopEnabled(bool enabled)"),
                 {"playHeadState_.isLooping.store(enabled, std::memory_order_relaxed);"});

    expectNoTokens("Arrangement direct processor seek",
                   arrangementSource,
                   {"processor_.setPosition("});
    expectTokens("Arrangement listener seek request",
                 arrangementSource,
                 {"listeners_.call([&](Listener& listener) {",
                  "listener.playheadPositionChangeRequested(newPosSeconds)"});
    expect(countOccurrences(arrangementSource, "listener.playheadPositionChangeRequested(newPosSeconds)") > 0,
           "Arrangement playhead seeks must go through Listener requests");
}

void productionTransportZeroResidueContract()
{
    const auto processorHeader = readText("Source/PluginProcessor.h");
    const auto processorSource = readText("Source/PluginProcessor.cpp");
    const auto documentControllerHeader = readText("Source/ARA/OpenTuneDocumentController.h");
    const auto documentControllerSource = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto pluginEditorHeader = readText("Source/Plugin/PluginEditor.h");
    const auto pluginEditorSource = readText("Source/Plugin/PluginEditor.cpp");
    const auto standaloneEditorHeader = readText("Source/Standalone/PluginEditor.h");
    const auto standaloneEditorSource = readText("Source/Standalone/PluginEditor.cpp");
    const auto pianoRollHeader = readText("Source/Standalone/UI/PianoRollComponent.h");
    const auto pianoRollSource = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto arrangementHeader = readText("Source/Standalone/UI/ArrangementViewComponent.h");
    const auto arrangementSource = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");

    const auto production = processorHeader + processorSource
        + documentControllerHeader + documentControllerSource
        + pluginEditorHeader + pluginEditorSource
        + standaloneEditorHeader + standaloneEditorSource
        + pianoRollHeader + pianoRollSource
        + arrangementHeader + arrangementSource;

    expectNoTokens("production transport zero residue",
                   production,
                   {"playbackPositionSource_",
                    "playbackIsPlaying_",
                    "getPlaybackPositionSource",
                    "updateTransport",
                    "positionAtomic_",
                    "getPositionAtomic",
                    "hostTransportTimeSeconds_",
                    "hostTransportIsPlaying_",
                    "hostTransportLoopEnabled_",
                    "hostTransportLoopPpqStart_",
                    "hostTransportLoopPpqEnd_",
                    "OpenTuneAudioProcessor::isPlaying_",
                    "OpenTuneAudioProcessor::loopEnabled_",
                    "PianoRollComponent::isPlaying_",
                    "ArrangementViewComponent::isPlaying_",
                    "positionSource_",
                    "PianoRollComponent::setIsPlaying",
                    "ArrangementViewComponent::setIsPlaying",
                    "PianoRollComponent::setPlayheadPositionSource",
                    "ArrangementViewComponent::setPlayheadPositionSource",
                    "getPlaybackPosition",
                    "HostTransportSnapshot::loopEnabled",
                    "HostTransportSnapshot::loopPpqStart",
                    "HostTransportSnapshot::loopPpqEnd"});
    expectNoTokens("VST3 editor transport write residue",
                   pluginEditorSource,
                   {"processorRef_.setLoopEnabled(", "pianoRoll_.setIsPlaying(",
                    "arrangementView_.setIsPlaying("});
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
        pianoRollSource, "void PianoRollComponent::drawTransientOverlay(juce::Graphics& g)");
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
                 {"bool shouldShowOverlay = waitingForAraContent_;",
                  "if (!waitingForAraContent_)",
                  "isAutoTuneProcessing",
                  "else if (chunkStats.hasActiveWork())",
                  "shouldShowBadge",
                  "getReadableContentChunkStats"});
    expectTokens("Plugin editor render-state visibility bindings",
                 timerBlock,
                 {"renderBadge_.setVisible(shouldShowBadge)",
                  "autoRenderOverlay_.setVisible(shouldShowOverlay)"});
}

void pianoRollContentOriginSnapContract()
{
    const auto toolHeader = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h");
    const auto toolSource = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto componentSource = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    expectTokens("PianoRoll content origin context",
                 toolHeader,
                 {"int contentOriginY;"});
    expectTokens("PianoRoll content origin binding",
                 componentSource,
                 {"toolCtx.contentOriginY = rulerHeight_;"});

    const auto paintOverChildrenBlock = extractBlockByMarker(
        componentSource, "void PianoRollComponent::paintOverChildren(juce::Graphics& g)");
    expectTokens("PianoRoll piano key ruler translation",
                 paintOverChildrenBlock,
                 {"auto keyContext = ctx;",
                  "keyContext.height = getTimelineContentViewportHeight();",
                  "juce::Graphics::ScopedSaveState pianoKeyOrigin(g);",
                  "g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));",
                  "g.reduceClipRegion(juce::Rectangle<int>(0, 0, pianoKeyWidth_, keyContext.height));",
                  "renderer_->drawPianoKeys(g, keyContext);"});

    const auto mouseDownBlock = extractBlockByMarker(
        componentSource, "void PianoRollComponent::mouseDown(const juce::MouseEvent& e)");
    const auto mouseDragBlock = extractBlockByMarker(
        componentSource, "void PianoRollComponent::mouseDrag(const juce::MouseEvent& e)");
    const auto verticalZoomBlock = extractBlockByMarker(
        componentSource, "void PianoRollComponent::handleVerticalZoomWheel(const juce::MouseEvent& e, float deltaY)");
    expectTokens("PianoRoll piano key mouse-down origin",
                 mouseDownBlock,
                 {"if (e.y < rulerHeight_ && e.x < pianoKeyWidth_)",
                  "e.y >= rulerHeight_",
                  "yToMidi(static_cast<float>(e.y - rulerHeight_))"});
    expectTokens("PianoRoll piano key mouse-drag origin",
                 mouseDragBlock,
                 {"yToMidi(static_cast<float>(e.y - rulerHeight_))",
                  "int deltaY = e.y - interactionState_.dragStartPos.y;"});
    expectTokens("PianoRoll vertical zoom origin",
                 verticalZoomBlock,
                 {"const float contentY = static_cast<float>(e.y - rulerHeight_);",
                  "yToMidi(contentY)",
                  "verticalScrollOffset_ = targetY - contentY;"});

    const auto toolMouseDownBlock = extractBlockByMarker(
        toolSource, "void PianoRollToolHandler::mouseDown(const juce::MouseEvent& e)");
    expectTokens("PianoRoll ToolHandler ruler origin",
                 toolMouseDownBlock,
                 {"const int rulerHeight = ctx_.contentOriginY;"});
    expectNoTokens("PianoRoll ToolHandler duplicated ruler origin",
                   toolSource,
                   {"constexpr int rulerHeight = 30;"});

    expectTokens("PianoRoll ToolHandler mouseMove content origin",
                 extractBlockByMarker(
                     toolSource, "void PianoRollToolHandler::mouseMove(const juce::MouseEvent& e)"),
                 {"contentOriginY"});
    expectTokens("PianoRoll ToolHandler note hit content origin",
                 extractBlockByMarker(
                     toolSource, "bool PianoRollToolHandler::hitsNoteBodyOrResizeEdge(const juce::MouseEvent& e)"),
                 {"contentOriginY"});
    expectTokens("PianoRoll ToolHandler F0 hit content origin",
                 extractBlockByMarker(
                     toolSource, "bool PianoRollToolHandler::hitTestF0Curve(const juce::MouseEvent& e, int& frameIndex) const"),
                 {"contentOriginY"});
    expectTokens("PianoRoll ToolHandler select content origin",
                 extractBlockByMarker(
                     toolSource, "void PianoRollToolHandler::handleSelectTool(const juce::MouseEvent& e)"),
                 {"contentOriginY"});
    expectTokens("PianoRoll ToolHandler curve draw content origin",
                 extractBlockByMarker(
                     toolSource, "void PianoRollToolHandler::handleDrawCurveTool(const juce::MouseEvent& e)"),
                 {"contentOriginY"});
    expectTokens("PianoRoll ToolHandler DrawNote down content origin",
                 extractBlockByMarker(
                     toolSource, "void PianoRollToolHandler::handleDrawNoteMouseDown(const juce::MouseEvent& e)"),
                 {"contentOriginY"});
    expectTokens("PianoRoll ToolHandler DrawNote content origin",
                 extractBlockByMarker(
                     toolSource, "void PianoRollToolHandler::handleDrawNoteTool(const juce::MouseEvent& e)"),
                 {"contentOriginY"});
    expectTokens("PianoRoll ToolHandler select drag content origin",
                 extractBlockByMarker(
                     toolSource, "void PianoRollToolHandler::handleSelectDrag(const juce::MouseEvent& e)"),
                 {"contentOriginY"});
    expectTokens("PianoRoll ToolHandler line anchor content origin",
                 extractBlockByMarker(
                     toolSource, "void PianoRollToolHandler::handleLineAnchorMouseDown(const juce::MouseEvent& e)"),
                 {"contentOriginY"});

    const auto drawNoteBlock = extractBlockByMarker(
        toolSource, "void PianoRollToolHandler::handleDrawNoteTool(const juce::MouseEvent& e)");
    expectTokens("DrawNote content-local mapping",
                 drawNoteBlock,
                 {"yToFreq(static_cast<float>(e.y - ctx_.contentOriginY))"});
    expectNoTokens("DrawNote standard MIDI quantization",
                   drawNoteBlock,
                   {"- 0.5f"});

    constexpr float rulerHeight = 30.0f;
    constexpr float pixelsPerSemitone = 25.0f;
    const ViewMapper mapper{
        0.0,
        1.0,
        0,
        640,
        400,
        pixelsPerSemitone,
        0.0f,
        72.0f};
    const float componentY = rulerHeight
        + mapper.midiToY(60.0f)
        + pixelsPerSemitone * 0.75f;
    const float contentY = componentY - rulerHeight;

    const auto standardMidi = [&mapper](float y) {
        const float targetF0 = mapper.yToFreq(y);
        return 69.0f + 12.0f * std::log2(targetF0 / 440.0f);
    };

    expect(static_cast<int>(std::round(standardMidi(contentY))) == 60,
           "content-local DrawNote Y must quantize the MIDI 60 row to 60");
    expect(static_cast<int>(std::round(standardMidi(componentY))) == 59,
           "component-local Y without ruler subtraction must quantize one key lower");
    expect(static_cast<int>(std::round(standardMidi(contentY) - 0.5f)) == 59,
           "an extra -0.5f must change lower-half content-local MIDI snapping");
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

} // namespace

int main()
{
    std::cout << "=== OpenTune PianoRoll Behavior Tests ===\n\n";

    try {
        pianoRollInitialF0ViewSourceContract();
        initialF0ViewMathBehavior();
        pianoRollPendingSeekPresentationSourceContract();
        pianoRollRetainedDirtyRectSourceContract();
        pluginEditorPlayheadRequestSourceContract();
        standalonePlayheadRequestSourceContract();
        processorOwnsSinglePlayHeadStateContract();
        processBlockUsesSinglePositionInfoContract();
        documentControllerOwnsNoTransportContract();
        uiReadsProcessorPlayHeadStateContract();
        loopRequestAndStandaloneWriteContract();
        productionTransportZeroResidueContract();
        viewMapperContinuousConversionRoundTrips();
        unifiedViewMappingAndReadableRenderSourceContracts();
        pianoRollContentOriginSnapContract();
        selectAllFeedbackPathCoversEveryNote();
        dragPreviewUsesWorkingNotesAndLiveInvalidation();
        pianoRollEditActionUndoRedoCommitsRangeSnapshots();
        captureSegmentContentAudioBufferBirthsIdentityTimeGrid();
        timeGridSnapshotIdentityRoundTrip();
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
