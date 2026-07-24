#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef OPENTUNE_SOURCE_DIR
#error "OPENTUNE_SOURCE_DIR must be defined by CMake"
#endif

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

std::string extractFunctionBlock(std::string_view text, std::string_view signature)
{
    return extractBracedBlock(text, text.find(signature));
}

std::string extractBlockByMarker(std::string_view text, std::string_view marker)
{
    return extractBracedBlock(text, text.find(marker));
}

std::string textBetween(std::string_view text, std::string_view startMarker, std::string_view endMarker)
{
    const size_t start = text.find(startMarker);
    if (start == std::string_view::npos)
        return {};

    const size_t end = text.find(endMarker, start + startMarker.size());
    if (end == std::string_view::npos)
        return {};

    return std::string(text.substr(start, end - start + endMarker.size()));
}

bool inOrder(std::string_view text, std::initializer_list<std::string_view> tokens)
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

void expectTokens(std::string_view blockName,
                  std::string_view text,
                  std::initializer_list<std::string_view> tokens)
{
    for (const auto token : tokens)
        expect(contains(text, token), std::string(blockName) + " missing token: " + std::string(token));
}

void expectNoTokens(std::string_view blockName,
                    std::string_view text,
                    std::initializer_list<std::string_view> tokens,
                    std::string_view detail = {})
{
    for (const auto token : tokens) {
        std::string msg = std::string(blockName) + " contains forbidden token: " + std::string(token);
        if (!detail.empty())
            msg += " — " + std::string(detail);
        expect(!contains(text, token), msg);
    }
}

void noteAndPitchRevisionPollingIsIndependent()
{
    const auto standalone = readText("Source/Standalone/PluginEditor.cpp");
    const auto plugin = readText("Source/Plugin/PluginEditor.cpp");

    const auto standalonePoll = textBetween(
        standalone,
        "const bool contentChanged =",
        "lastPianoRollPitchRevision_ = currentPitchRevision;");
    const auto pluginPoll = extractBlockByMarker(plugin, "if (activeKey.isValid() && !contentJustSwitched)");

    expectTokens("Standalone revision poll",
                 standalonePoll,
                 {"pianoRoll_.onNotesRevisionChanged();", "pianoRoll_.onTimeGridRevisionChanged();", "pianoRoll_.onPitchRevisionChanged();"});
    expectNoTokens("Standalone revision poll",
                   standalonePoll,
                   {"} else if (currentNotesRevision != lastPianoRollNotesRevision_)"});
    expect(inOrder(standalonePoll, {"pianoRoll_.onNotesRevisionChanged();", "if (currentTimeGridRevision", "if (currentPitchRevision"}),
           "Standalone polling must continue from notes refresh to time-grid and pitch refresh");

    expectTokens("Plugin revision poll",
                 pluginPoll,
                 {"pianoRoll_.onNotesRevisionChanged();", "pianoRoll_.onTimeGridRevisionChanged();", "pianoRoll_.onPitchRevisionChanged();"});
    expect(inOrder(pluginPoll, {"pianoRoll_.onNotesRevisionChanged();", "if (currentTimeGridRevision", "if (currentPitchRevision"}),
           "Plugin polling must continue from notes refresh to time-grid and pitch refresh");
}

void notePatchCommitReturnsAuthoritativeSnapshot()
{
    const auto commandsHeader = readText("Source/Content/ContentEditCommands.h");
    const auto processorHeader = readText("Source/PluginProcessor.h");
    const auto processor = readText("Source/PluginProcessor.cpp");
    const auto component = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    expectNoTokens("ContentEditCommands.h",
                   commandsHeader,
                   {"virtual bool commitNotePatch"});
    expectTokens("ContentEditCommands.h",
                 commandsHeader,
                 {"virtual ContentCommitSnapshot commitNotePatch"});

    expectNoTokens("PluginProcessor.h",
                   processorHeader,
                   {"bool commitContentNotePatch"});
    expectTokens("PluginProcessor.h",
                 processorHeader,
                 {"ContentCommitSnapshot commitContentNotePatch"});

    expectNoTokens("PluginProcessor.cpp",
                   processor,
                   {"bool OpenTuneAudioProcessor::commitContentNotePatch"});
    expectTokens("PluginProcessor.cpp",
                 processor,
                 {"ContentCommitSnapshot OpenTuneAudioProcessor::commitContentNotePatch",
                  "auto committedSnap = getContentSnapshot(key);",
                  "return committedSnap;"});

    const auto commitNoteDraft = extractFunctionBlock(component, "bool PianoRollComponent::commitNoteDraft");
    const auto noteOnlyFallback = textBetween(
        component,
        "ContentNoteRangePatch afterPatch;",
        "return true;");

    expectTokens("commitNoteDraft",
                 commitNoteDraft,
                 {"const auto committedSnap =", "cachedNotes_ = committedSnap->notes"});
    expectNoTokens("commitNoteDraft",
                   commitNoteDraft,
                   {"refreshEditedContentNotes();"});

    expectTokens("note-only parameter path",
                 noteOnlyFallback,
                 {"const auto committedSnap =", "cachedNotes_ = committedSnap->notes"});
    expectNoTokens("note-only parameter path",
                   noteOnlyFallback,
                   {"refreshEditedContentNotes();"});
}

void noLegacyNoteInteractionState()
{
    const auto componentHeader = readText("Source/Standalone/UI/PianoRollComponent.h");
    const auto component = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    expectNoTokens("PianoRollComponent.h", componentHeader, {"interactionRevision_", "notesEpoch_"});
    expectNoTokens("PianoRollComponent.cpp", component, {"interactionRevision_", "notesEpoch_"});
}

// ============================================================================
// Arrangement selection publication contract tests
// ============================================================================

void selectedFieldsOnlyWrittenInCommitHelpers()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto commitBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitPlacementSelection");
    const auto emptyCommitBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitEmptyPlacementSelection");

    expect(!commitBody.empty(), "commitPlacementSelection must be found");
    expect(!emptyCommitBody.empty(), "commitEmptyPlacementSelection must be found");

    // selectedTrack_
    {
        const auto total = countOf(src, "selectedTrack_ =");
        const auto inCommit = countOf(commitBody, "selectedTrack_ =");
        const auto inEmpty = countOf(emptyCommitBody, "selectedTrack_ =");
        expect(total == inCommit + inEmpty,
               "All selectedTrack_ writes must be in commit helpers (total=" + std::to_string(total)
               + " commit=" + std::to_string(inCommit) + " empty=" + std::to_string(inEmpty) + ")");
    }

    // selectedPlacementId_
    {
        const auto total = countOf(src, "selectedPlacementId_ =");
        const auto inCommit = countOf(commitBody, "selectedPlacementId_ =");
        const auto inEmpty = countOf(emptyCommitBody, "selectedPlacementId_ =");
        expect(total == inCommit + inEmpty,
               "All selectedPlacementId_ writes must be in commit helpers (total=" + std::to_string(total)
               + " commit=" + std::to_string(inCommit) + " empty=" + std::to_string(inEmpty) + ")");
    }

    // selectedPlacementIndex_
    {
        const auto total = countOf(src, "selectedPlacementIndex_ =");
        const auto inCommit = countOf(commitBody, "selectedPlacementIndex_ =");
        const auto inEmpty = countOf(emptyCommitBody, "selectedPlacementIndex_ =");
        expect(total == inCommit + inEmpty,
               "All selectedPlacementIndex_ writes must be in commit helpers (total=" + std::to_string(total)
               + " commit=" + std::to_string(inCommit) + " empty=" + std::to_string(inEmpty) + ")");
    }
}

void placementSelectionChangedOnlyInCommitHelpers()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto header = readText("Source/Standalone/UI/ArrangementViewComponent.h");

    const auto commitBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitPlacementSelection");
    const auto emptyCommitBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitEmptyPlacementSelection");

    expect(!commitBody.empty(), "commitPlacementSelection must be found");
    expect(!emptyCommitBody.empty(), "commitEmptyPlacementSelection must be found");

    const auto total = countOf(src, "placementSelectionChanged(");
    const auto inCommit = countOf(commitBody, "placementSelectionChanged(");
    const auto inEmpty = countOf(emptyCommitBody, "placementSelectionChanged(");

    expect(total == inCommit + inEmpty,
           "All placementSelectionChanged calls must be in commit helpers (total=" + std::to_string(total)
           + " commit=" + std::to_string(inCommit) + " empty=" + std::to_string(inEmpty) + ")");

    // Header: placementSelectionChanged should appear only as a virtual declaration in Listener
    const auto headerCount = countOf(header, "placementSelectionChanged");
    expect(headerCount == 1,
           "placementSelectionChanged should appear exactly once in header (as Listener virtual declaration, count=" + std::to_string(headerCount) + ")");
}

void setMutatingHelpersArePure()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");

    const auto toggleBody = extractFunctionBlock(src, "void ArrangementViewComponent::togglePlacementSelection");
    const auto clearBody = extractFunctionBlock(src, "void ArrangementViewComponent::clearPlacementSelection");
    const auto rangeBody = extractFunctionBlock(src, "void ArrangementViewComponent::selectPlacementsInRange");
    const auto trackBody = extractFunctionBlock(src, "void ArrangementViewComponent::selectAllPlacementsInTrack");

    expect(!toggleBody.empty(), "togglePlacementSelection must be found");
    expect(!clearBody.empty(), "clearPlacementSelection must be found");
    expect(!rangeBody.empty(), "selectPlacementsInRange must be found");
    expect(!trackBody.empty(), "selectAllPlacementsInTrack must be found");

    expectNoTokens("togglePlacementSelection", toggleBody,
                   {"listeners_.", "refreshVisualState", "repaint()", "requestInvalidate",
                    "contentCache_", "patternCache_", "prepareCoverageContentTiles",
                    "requestContentInvalidation"});
    expectNoTokens("clearPlacementSelection", clearBody,
                   {"listeners_.", "refreshVisualState", "repaint()", "requestInvalidate",
                    "contentCache_", "patternCache_", "prepareCoverageContentTiles",
                    "requestContentInvalidation"});
    expectNoTokens("selectPlacementsInRange", rangeBody,
                   {"listeners_.", "refreshVisualState", "repaint()", "FrameScheduler", "requestInvalidate",
                    "contentCache_", "patternCache_", "prepareCoverageContentTiles",
                    "requestContentInvalidation"});
    expectNoTokens("selectAllPlacementsInTrack", trackBody,
                   {"listeners_.", "refreshVisualState", "repaint()", "FrameScheduler", "requestInvalidate",
                    "contentCache_", "patternCache_", "prepareCoverageContentTiles",
                    "requestContentInvalidation"});
}

void moveBranchHasNoBareRepaint()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto mouseDownBody = extractFunctionBlock(src, "void ArrangementViewComponent::mouseDown");

    expect(!mouseDownBody.empty(), "mouseDown must be found");

    // Find the Move branch within mouseDown: the code block after commitPlacementSelection
    // The Move branch starts with: currentDragOp_ = hit.isTopEdge ? DragOperation::Gain : DragOperation::Move;
    // The repaint() we want to forbid is NOT in clearMoveDragOverlay (which is unrelated to selection)
    //
    // Strategy: extract the section inside "if (currentDragOp_ == DragOperation::Move)"
    // and verify it does NOT contain "repaint();"
    const auto moveBranch = extractBlockByMarker(mouseDownBody, "if (currentDragOp_ == DragOperation::Move)");

    if (!moveBranch.empty()) {
        expectNoTokens("Move branch in mouseDown", moveBranch, {"repaint()"});
    }

    // Also verify mouseDown doesn't call placementSelectionChanged directly
    const auto pscCount = countOf(mouseDownBody, "placementSelectionChanged(");
    expect(pscCount == 0,
           "mouseDown must not call placementSelectionChanged directly (count=" + std::to_string(pscCount) + ")");
}

void togglePlacementSelectionAllowsLastItemToggle()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto toggleBody = extractFunctionBlock(src, "void ArrangementViewComponent::togglePlacementSelection");

    expect(!toggleBody.empty(), "togglePlacementSelection must be found");
    expect(!contains(toggleBody, "size() > 1"),
           "togglePlacementSelection must allow toggling the last selected item (no size() > 1 guard)");
    expect(!contains(toggleBody, "juce::ignoreUnused"),
           "togglePlacementSelection must not have unused-param ceremony");
}

void commitEmptyDoesNotDestroyPerTrackMemory()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto emptyBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitEmptyPlacementSelection");

    expect(!emptyBody.empty(), "commitEmptyPlacementSelection must be found");
    expectTokens("commitEmptyPlacementSelection", emptyBody,
                 {"placementSelectionChanged"});
    expect(!contains(emptyBody, "clearAllSelections()"),
            "commitEmptyPlacementSelection must NOT use clearAllSelections() — preserves per-track selectedPlacementId");
    expect(!contains(emptyBody, "selectPlacement(previousTrack"),
            "commitEmptyPlacementSelection must not use per-track selectPlacement");
}

// ----------------------------------------------------------------------------
// ARA TimeGrid 计划的结构契约测试
//
// 本节为各组件的 TimeGrid 计划（TimeGridSnapshot 接口）提供结构级契约：
// - AudioModification：使用 TimeGridSnapshot::makeIdentity/makeFromHandles
//   和 std::optional 与 return nullptr 模式；旧 birth/lifecycle 路径不残留。
// - DocumentController：只存/恢复 hasContentState；pending 与存在项；无 ID 重绑；
//   仅 notify pending targets。若 TimeGrid 从 archive 读取失败则返回 nullopt。
// - PianoRollRenderer：ContentRenderItem 含 timeGrid；无 timeGridSnapshot/重名；
//   无 ctx.activeProjection。PianoRollComponent::buildContentRenderItem 返回
//   std::optional<PianoRollRenderer::ContentRenderItem>。
// - PianoRollToolHandler：Context 注入 getActiveContentTimeGrid 回调供 Time tool 读取。
// - 状态与接口无路径遗留；仅靠源码静态文本匹配，无 JUCE 运行时依赖。
// ----------------------------------------------------------------------------

// 测试 1: AudioModification.h/.cpp 的 TimeGrid 计划契约
void araAudioModificationStructureHasTimeGridPlan()
{
    const auto header = readText("Source/ARA/AudioModification.h");
    const auto impl = readText("Source/ARA/AudioModification.cpp");

    // 1. std::optional<AudioModificationContentState> 存在
    expectTokens("AudioModification.h optional content",
                 header,
                 {"std::optional<AudioModificationContentState> content"});

    // 2. 旧 birth 分支不残留
    expect(!contains(header, std::string("Pending") + "Birth"),
           "old pending birth branch removed — birth via makeBorn()");

    // 3. makeBorn 调用 TimeGridSnapshot::makeIdentity
    const auto bornMethod = extractFunctionBlock(impl, "AudioModificationContentState::makeBorn");
    expectTokens("makeBorn uses TimeGridSnapshot::makeIdentity",
                 bornMethod,
                 {"TimeGridSnapshot::makeIdentity"});

    // 4. attachSource 返回 bool
    const auto attachSourceMethod = extractFunctionBlock(impl, "bool AudioModification::attachSource");
    expectTokens("attachSource returns bool",
                 attachSourceMethod,
                 {"return true;", "return false;"});

    // 5. snapshotContent 无 content 返回 nullptr
    const auto snapshotMethod = extractFunctionBlock(impl, "AudioModification::snapshotContent");
    expectTokens("snapshotContent returns nullptr for no content",
                 snapshotMethod,
                 {"if (!content.has_value())", "return nullptr;"});

    // 6. ARA wrapper 不持有 shared lifecycle token
    const auto lifecycleToken = std::string("Content") + "Lifecycle";
    expect(!contains(header, lifecycleToken),
           "AudioModification.h has no shared lifecycle token");
    expect(!contains(impl, lifecycleToken),
           "AudioModification.cpp has no shared lifecycle token");
}

// 测试 2: OpenTuneDocumentController.cpp 的 TimeGrid 计划契约
void araDocumentControllerRestoresTimeGridPlanFromHandles()
{
    const auto impl = readText("Source/ARA/OpenTuneDocumentController.cpp");

    // 1. store 只收 hasContentState() modification
    const auto storeMethod = extractFunctionBlock(impl, "OpenTuneDocumentController::doStoreObjectsToStream");
    const auto serializer = extractFunctionBlock(impl, "serializeAudioModificationContent");
    expectTokens("doStoreObjectsToStream checks hasContentState",
                 storeMethod,
                 {"modification.hasContentState()"});
    expectTokens("serializer nests TimeGrid in EditableContent",
                 serializer,
                 {"new juce::XmlElement(\"EditableContent\")",
                  "new juce::XmlElement(\"TimeGrid\")",
                  "editable->addChildElement(tg)"});

    // 2. restore 解析 TimeGrid、makeFromHandles、缺失 TimeGrid 返回 nullopt
    const auto restoreMethod = extractFunctionBlock(impl, "restoreAudioModificationContent");
    expectTokens("restoreAudioModificationContent parses TimeGrid via makeFromHandles",
                 restoreMethod,
                 {"editable->getChildByName(\"TimeGrid\")", "makeFromHandles"});
    expectTokens("restoreAudioModificationContent returns nullopt for missing TimeGrid",
                 restoreMethod,
                 {"return std::nullopt;"});
    expectNoTokens("restoreAudioModificationContent no identity TimeGrid fallback",
                   restoreMethod,
                   {"makeIdentity"},
                   "missing TimeGrid → discard, not fallback to identity");

    // 3. pending 直接存 target/state；restore 清 CRS、设置 WaitingForSource、只通知 pending
    const auto restoreStream = extractFunctionBlock(impl, "OpenTuneDocumentController::doRestoreObjectsFromStream");
    expectTokens("pending stores target and state directly",
                 restoreStream,
                 {"pending.emplace_back(targetMod, std::move(*newContent))"});
    expectTokens("restore clears CRS artifacts",
                 restoreStream,
                 {"removeCRSArtifactsForModification"});
    expectTokens("restore sets WaitingForSource",
                 restoreStream,
                 {"birthState = AudioModificationBirthState::WaitingForSource"});
    expectTokens("restore only notifies pending targets",
                 restoreStream,
                 {"notifyContentChanged"});
    expectNoTokens("restore does not notify all audioModifications_",
                   restoreStream,
                   {"for (auto& modification : audioModifications_)"});

    // 4. willDestroy erase wrapper（按 Host 指针 erase，不触碰 persistent-id 映射）
    const auto destroyMethod = extractFunctionBlock(impl, "OpenTuneDocumentController::willDestroyAudioModification");
    expectTokens("willDestroyAudioModification erases wrapper by host pointer",
                 destroyMethod,
                 {"audioModifications_.erase"});
    expect(contains(destroyMethod, "removeCRSArtifactsForModification"),
           "willDestroyAudioModification calls removeCRSArtifactsForModification");

    // 5. ensure 无 persistent-ID rebind：新 Host modification 直接 push_back 新 wrapper，
    //    不通过 getPersistentID() 查找已销毁 wrapper 再重绑 host 指针
    const auto ensureMethod = extractFunctionBlock(impl, "OpenTuneDocumentController::ensureAudioModification");
    expectTokens("ensureAudioModification creates new wrapper",
                 ensureMethod,
                 {"audioModifications_.push_back"});
    expectNoTokens("ensureAudioModification no persistent-ID rebind",
                   ensureMethod,
                   {"getPersistentID"},
                   "new Host modification must create new wrapper, never rebind by persistent ID");

    // 6. ARA controller 不引用 shared lifecycle enum
    const auto lifecycleToken = std::string("Content") + "Lifecycle";
    expect(!contains(impl, lifecycleToken),
           "OpenTuneDocumentController.cpp has no shared lifecycle enum");
}

// 测试 3: PianoRollRenderer ContentRenderItem 结构
void pianoRollRendererContentRenderItemHasTimeGridNoActiveProjection()
{
    // PianoRollRenderer::ContentRenderItem 必须有 TimeGridSnapshot
    const auto rendererHeader = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");
    const auto rendererImpl = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");

    // 1. ContentRenderItem 有 timeGrid
    expect(contains(rendererHeader, "std::shared_ptr<const TimeGridSnapshot> timeGrid"),
           "PianoRollRenderer::ContentRenderItem has timeGrid field");

    // 2. 没有 timeGridSnapshot 这个旧字段（已被 timeGrid 替代）
    expectNoTokens("PianoRollRenderer::ContentRenderItem no timeGridSnapshot",
                   rendererHeader,
                   {"timeGridSnapshot", "std::shared_ptr<const TimeGridSnapshot> timeGridSnapshot"});
    expectNoTokens("PianoRollRenderer::ContentRenderItem no timeGridSnapshot",
                   rendererImpl,
                   {"timeGridSnapshot"});

    // 3. 没有 ctx.activeProjection（RenderContext 不持有 projection）
    // RenderContext 持有 contents 为 std::vector<ContentRenderItem>，而非 activeProjection
    expect(contains(rendererHeader, "std::vector<ContentRenderItem> contents"),
           "PianoRollRenderer::RenderContext has contents vector");

    expectNoTokens("PianoRollRenderer no ctx.activeProjection",
                   rendererHeader,
                   {"ctx.activeProjection", "activeProjection"});
    expectNoTokens("PianoRollRenderer no ctx.activeProjection",
                   rendererImpl,
                   {"ctx.activeProjection", "activeProjection"});
}

// 测试 4: PianoRollComponent buildContentRenderItem 返回 optional
void pianoRollComponentBuildContentRenderItemReturnsOptional()
{
    const auto componentImpl = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    // buildContentRenderItem 返回 optional
    const auto signature = extractFunctionBlock(componentImpl,
                                                "std::optional<PianoRollRenderer::ContentRenderItem> PianoRollComponent::buildContentRenderItem");
    expect(!signature.empty(),
           "PianoRollComponent::buildContentRenderItem returns optional");

    expectTokens("buildContentRenderItem returns std::optional",
                 signature,
                 {"std::optional<PianoRollRenderer::ContentRenderItem>"});
}

// 测试 5: PianoRollToolHandler getActiveContentTimeGrid
void pianoRollToolHandlerHasGetActiveContentTimeGrid()
{
    const auto handlerHeader = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h");

    // 工具处理器有 TimeGrid 读取回调：getActiveContentTimeGrid
    const auto contextMarker = "Context";
    const auto contextBlock = extractBlockByMarker(handlerHeader, contextMarker);

    expect(!contextBlock.empty(), "PianoRollToolHandler::Context has std::function callbacks");

    expectTokens("Context has getActiveContentTimeGrid",
                 contextBlock,
                 {"getActiveContentTimeGrid"});
    expectTokens("Context has getActiveContentTimeGrid type",
                 contextBlock,
                 {"std::function<std::shared_ptr<const TimeGridSnapshot>()> getActiveContentTimeGrid"});
}

// ============================================================================
// Phase 4: ARA TimeGrid 行为契约测试
// (docs/plans/2026-07-14-ara-timegrid-ownership-hard-cut.md §9.1/§9.2)
//
// 14 条行为契约逐条覆盖 §9.1 第 1-14 项，使用现有 helpers 做静态源码匹配，
// 不臆造 ARA Host runtime API。每条测试函数对应计划中一个编号项。
// ============================================================================

// §9.1 #1: attachSource 通过 makeBorn 创建 identity grid；makeBorn 用 window.durationSeconds
void araAttachSourceCreatesIdentityGridWithSourceWindowDuration()
{
    const auto impl = readText("Source/ARA/AudioModification.cpp");
    const auto attachMethod = extractFunctionBlock(impl, "bool AudioModification::attachSource");
    const auto bornMethod = extractFunctionBlock(impl, "std::optional<AudioModificationContentState> AudioModificationContentState::makeBorn");

    // makeBorn 用 window.durationSeconds 创建 identity grid
    expectTokens("makeBorn uses window.durationSeconds for identity grid",
                 bornMethod,
                 {"TimeGridSnapshot::makeIdentity(window.durationSeconds())"});

    // attachSource 通过 makeBorn 创建
    expectTokens("attachSource calls makeBorn",
                 attachMethod,
                 {"AudioModificationContentState::makeBorn"});

    // sourceWindow duration 作为 invariant：attachSource 用 source.getShape().durationSeconds()
    expect(contains(attachMethod, "source.getShape().durationSeconds()"),
           "attachSource uses source.durationSeconds for SourceWindow");
}

// §9.1 #2: snapshotContent 有 content 时复制 timeGrid；无 content 返回 nullptr
void araSnapshotContentCopiesTimeGridOrReturnsNull()
{
    const auto impl = readText("Source/ARA/AudioModification.cpp");
    const auto snapMethod = extractFunctionBlock(impl, "std::shared_ptr<const EditableContentSnapshot> AudioModification::snapshotContent");

    // 无 content 返回 nullptr
    expectTokens("snapshotContent returns nullptr without content",
                 snapMethod,
                 {"if (!content.has_value())", "return nullptr;"});

    // 有 content 时复制 timeGrid（不做 null-as-identity 替换）
    expect(contains(snapMethod, "snap->timeGrid = content->editable.timeGrid;"),
           "snapshotContent copies timeGrid from content");
}

// §9.1 #3: readContentSnapshot 无 modification/content 返回 nullptr
void araReadContentSnapshotReturnsNullWithoutContent()
{
    const auto impl = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto readMethod = extractFunctionBlock(impl, "OpenTuneDocumentController::readContentSnapshot");

    expect(!readMethod.empty(), "readContentSnapshot must exist");
    expect(contains(readMethod, "mod == nullptr"),
           "readContentSnapshot checks for null modification");
    expect(contains(readMethod, "hasContentState()"),
           "readContentSnapshot checks content state");
    expect(contains(readMethod, "return nullptr;"),
           "readContentSnapshot returns nullptr");
}

// §9.1 #4: birthContentForModification 不出现任何 timeGrid 创建/清空/替换表达式
void araBirthContentForModificationDoesNotTouchTimeGrid()
{
    const auto impl = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto birthMethod = extractFunctionBlock(impl, "bool OpenTuneDocumentController::birthContentForModification");

    expectNoTokens("birthContentForModification does not create TimeGrid",
                   birthMethod,
                   {"makeIdentity", "makeFromHandles"});
    expect(contains(birthMethod, "modification.content"),
           "birthContentForModification reads existing content state");
    expect(contains(birthMethod, "publishPlaybackReadSource"),
           "birthContentForModification publishes derived CRS");
}

// §9.1 #5: restore 合法 non-identity grid 通过 makeFromHandles；禁止 makeIdentity fallback
void araRestoreUsesMakeFromHandlesWithoutIdentityFallback()
{
    const auto impl = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto restoreMethod = extractFunctionBlock(impl, "std::optional<AudioModificationContentState> restoreAudioModificationContent");

    expectTokens("restore parses TimeGrid XML",
                 restoreMethod,
                 {"editable->getChildByName(\"TimeGrid\")"});
    expectTokens("restore uses makeFromHandles",
                 restoreMethod,
                 {"TimeGridSnapshot::makeFromHandles"});
    expectNoTokens("restore no identity fallback",
                   restoreMethod,
                   {"makeIdentity"},
                   "missing/invalid TimeGrid → nullopt, not identity");
}

// §9.1 #6: restore filter miss 走 continue，不改 target
void araRestoreFilterMissContinuesWithoutModifyingTarget()
{
    const auto impl = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto restoreStream = extractFunctionBlock(impl, "OpenTuneDocumentController::doRestoreObjectsFromStream");

    expect(contains(restoreStream, "restoredPersistentId.isEmpty()"),
           "doRestoreObjectsFromStream checks restoredPersistentId for filter miss");
    expect(contains(restoreStream, "continue;"),
           "doRestoreObjectsFromStream continues on filter miss");
}

// §9.1 #7: 任一 selected record 失败在 pending 提交前 return false；content 替换发生在 pending 全部解析后
void araRestoreFailReturnsFalseBeforePendingCommit()
{
    const auto impl = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto restoreStream = extractFunctionBlock(impl, "OpenTuneDocumentController::doRestoreObjectsFromStream");

    expect(contains(restoreStream, "return false;"),
           "doRestoreObjectsFromStream returns false on parse failure before commit");
    expect(contains(restoreStream, "pending.emplace_back(targetMod, std::move(*newContent))"),
           "pending stores target+state before commit");
    expect(contains(restoreStream, "for (auto& [targetMod, state] : pending)"),
           "content replacement happens after all records parsed");
    expect(contains(restoreStream, "targetMod->content = std::move(state);"),
           "atomic content replacement in pending loop");
}

// §9.1 #8: willDestroy 清理 CRS、PlaybackRegion 并按 Host pointer erase wrapper
void araWillDestroyRemovesCRSPlaybackRegionsByHostPointer()
{
    const auto impl = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto destroyMethod = extractFunctionBlock(impl, "void OpenTuneDocumentController::willDestroyAudioModification");

    expect(contains(destroyMethod, "removeCRSArtifactsForModification"),
           "willDestroy clears CRS artifacts");
    expect(contains(destroyMethod, "playbackRegions_.erase"),
           "willDestroy erases PlaybackRegion");
    expect(contains(destroyMethod, "[audioModification](const AudioModification& m)"),
           "willDestroy erases wrapper by Host pointer");
}

// §9.1 #9: ensureAudioModification 只按 pointer 复用活跃 wrapper，新 Host wrapper push_back，禁止 persistent-ID rebind
void araEnsureAudioModificationCreatesNewWrapperPerHostModification()
{
    const auto impl = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto ensureMethod = extractFunctionBlock(impl, "AudioModification& OpenTuneDocumentController::ensureAudioModification");

    expect(contains(ensureMethod, "findAudioModification(audioModification)"),
           "ensureAudioModification reuses active wrapper by pointer");
    expect(contains(ensureMethod, "audioModifications_.push_back"),
           "ensureAudioModification pushes new wrapper for new Host modification");
    expectNoTokens("ensureAudioModification no persistent-ID rebind",
                   ensureMethod,
                   {"getPersistentID"},
                   "new Host modification must create new wrapper");
}

// §9.1 #10: renderer ContentRenderItem 带自身 timeGrid，无 global timeGridSnapshot/ctx.activeProjection
void araRendererItemHasOwnTimeGridNoGlobalSnapshot()
{
    const auto rendererHeader = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");
    const auto rendererImpl = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");

    expect(contains(rendererHeader, "std::shared_ptr<const TimeGridSnapshot> timeGrid"),
           "ContentRenderItem has own timeGrid field");
    expectNoTokens("Renderer header no global snapshot",
                   rendererHeader,
                   {"timeGridSnapshot", "activeProjection"});
    expect(contains(rendererImpl, "item.timeGrid->tauForward"),
           "Renderer uses item.timeGrid->tauForward");
    expect(contains(rendererImpl, "item.timeGrid->tauInverse"),
           "Renderer uses item.timeGrid->tauInverse");
    expectNoTokens("Renderer impl no global snapshot",
                   rendererImpl,
                   {"timeGridSnapshot", "ctx.activeProjection"});
}

// §9.1 #11: buildContentRenderItem 无 snapshot 或无 timeGrid 返回 nullopt，并把 snap->timeGrid 赋给 item
void araBuildContentRenderItemReturnsNulloptWithoutSnapshotOrTimeGrid()
{
    const auto componentImpl = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto buildMethod = extractFunctionBlock(componentImpl,
                                                   "std::optional<PianoRollRenderer::ContentRenderItem> PianoRollComponent::buildContentRenderItem");

    expect(!buildMethod.empty(), "buildContentRenderItem must be found");
    expect(contains(buildMethod, "if (!snap || !snap->timeGrid)"),
           "buildContentRenderItem checks snapshot and timeGrid");
    expect(contains(buildMethod, "return std::nullopt;"),
           "buildContentRenderItem returns nullopt");
    expect(contains(buildMethod, "item.timeGrid = snap->timeGrid;"),
           "buildContentRenderItem assigns snap->timeGrid to item");
}

// §9.1 #12: setReferenceOverlay 不在 projection 歧义时绑定其他 content grid，使用 matched snapshot 的 timeGrid
void araSetReferenceOverlayUsesMatchedSnapshotTimeGrid()
{
    const auto componentImpl = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto overlayMethod = extractFunctionBlock(componentImpl,
                                                    "void PianoRollComponent::setReferenceOverlay");

    expect(!overlayMethod.empty(), "setReferenceOverlay must be found");
    expect(contains(overlayMethod, "ambiguousMatch"),
           "setReferenceOverlay detects projection ambiguity");
    expect(contains(overlayMethod, "overlay.reset();"),
           "setReferenceOverlay rejects overlay on ambiguity or missing snapshot");
    expect(contains(overlayMethod, "overlay->timeGrid = snap->timeGrid;"),
           "setReferenceOverlay uses matched snapshot timeGrid");
}

// §9.1 #13: PianoRoll source/output 转换调用 item.timeGrid tauForward/tauInverse；ToolHandler 通过 getActiveContentTimeGrid
void araPianoRollSourceOutputUsesItemTimeGridAndToolHandlerUsesCallback()
{
    const auto rendererImpl = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");
    const auto toolImpl = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");

    expect(contains(rendererImpl, "item.timeGrid->tauForward(sourceTime)"),
           "PianoRollRenderer uses item.timeGrid->tauForward");
    expect(contains(rendererImpl, "item.timeGrid->tauInverse"),
           "PianoRollRenderer uses item.timeGrid->tauInverse");
    expect(contains(toolImpl, "ctx_.getActiveContentTimeGrid()"),
           "ToolHandler uses getActiveContentTimeGrid callback");
    expect(contains(toolImpl, "grid->tauInverse("),
           "ToolHandler calls grid->tauInverse");
    expect(contains(toolImpl, "grid->tauForward("),
           "ToolHandler calls grid->tauForward");
}

// §9.1 #14 / §9.2: 计划静态残留检查
void araStaticResidualChecksForTimeGridOwnershipPlan()
{
    const auto amHeader = readText("Source/ARA/AudioModification.h");
    const auto contentStateHeader = readText("Source/Content/AudioModificationContentState.h");
    const auto amImpl = readText("Source/ARA/AudioModification.cpp");
    const auto dcImpl = readText("Source/ARA/OpenTuneDocumentController.cpp");
    const auto rendererHeader = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");
    const auto componentImpl = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    // Source/ARA 无 ContentLifecycle（根因 #A）
    const auto lifecycleToken = std::string("Content") + "Lifecycle";
    expect(!contains(amImpl, lifecycleToken), "AudioModification.cpp has no ContentLifecycle");
    expect(!contains(dcImpl, lifecycleToken), "OpenTuneDocumentController.cpp has no ContentLifecycle");

    // Source/ARA 无 PendingBirth
    const auto pendingToken = std::string("Pending") + "Birth";
    expect(!contains(amImpl, pendingToken), "AudioModification.cpp has no PendingBirth");
    expect(!contains(dcImpl, pendingToken), "OpenTuneDocumentController.cpp has no PendingBirth");

    // TimeGrid edits and archive writes preserve the owner invariant.
    expect(contains(amHeader, "bool applyTimeGrid"),
           "AudioModification owns validated TimeGrid writes");
    expect(contains(amImpl, "std::abs(gridDuration - sourceDuration)"),
           "AudioModification validates TimeGrid duration");
    const auto serializer = extractFunctionBlock(dcImpl, "serializeAudioModificationContent");
    expect(contains(serializer, "mod.content->editable.timeGrid->handles()"),
           "serializer writes the complete owner TimeGrid");
    expectNoTokens("AudioModificationContentState has no Standalone lifecycle",
                   contentStateHeader,
                   {lifecycleToken});
    expectNoTokens("serializer has no legacy lifecycle field",
                   serializer,
                   {"setAttribute(\"lifecycle\""});

    const auto birthOwnerMethod = extractFunctionBlock(dcImpl, "bool OpenTuneDocumentController::birthContentForModification");
    expect(contains(birthOwnerMethod, "submitSilentGaps"),
           "birth submits derived analysis through the owner");
    expectNoTokens("birthContentForModification no direct analysis mutation",
                   birthOwnerMethod,
                   {"content.analysis.silentGaps"});

    const auto restoreStreamMethod = extractFunctionBlock(dcImpl, "OpenTuneDocumentController::doRestoreObjectsFromStream");
    expect(contains(restoreStreamMethod, "if (targetMod == nullptr)"),
           "restore skips records without a current target wrapper");

    // PianoRoll 源码无 global timeGridSnapshot/ctx.activeProjection
    expectNoTokens("PianoRollRenderer no global snapshot",
                   rendererHeader,
                   {"timeGridSnapshot", "activeProjection"});
    expectNoTokens("PianoRollComponent no global snapshot",
                   componentImpl,
                   {"timeGridSnapshot", "ctx.activeProjection"});

    // 有效内容消费无 makeIdentity fallback
    const auto birthMethod = extractFunctionBlock(dcImpl, "bool OpenTuneDocumentController::birthContentForModification");
    expectNoTokens("birthContentForModification no makeIdentity fallback",
                   birthMethod,
                   {"makeIdentity", "makeFromHandles"});

    const auto restoreMethod = extractFunctionBlock(dcImpl, "std::optional<AudioModificationContentState> restoreAudioModificationContent");
    expectNoTokens("restoreAudioModificationContent no makeIdentity fallback",
                   restoreMethod,
                   {"makeIdentity"});

     // ❌ 修复根因 #A & #B.2: removeCRSArtifactsForModification 在 CRS null 检查之前调用 F0 cancel
     const auto removeCRSMethod = extractFunctionBlock(dcImpl, "void OpenTuneDocumentController::removeCRSArtifactsForModification");
     const auto cancelPos = removeCRSMethod.find("contentF0ExtractionService_->cancel");
     const auto crsNullCheckPos = removeCRSMethod.find("contentRenderService_ == nullptr");
     expect(cancelPos != std::string::npos && crsNullCheckPos != std::string::npos
                && cancelPos < crsNullCheckPos,
            "F0 cancel must precede the CRS null check");

     // ❌ 修复根因 #B.3: scheduleAsyncF0Extraction 采用 birthRevision + Host pointer 检查 (#F0-part2)
     const auto f0BirthMethod = extractFunctionBlock(dcImpl, "bool OpenTuneDocumentController::birthContentForModification");
           // 1) birthContentForModification → scheduleAsyncF0Extraction 调用处捕获 hostModification
      expectTokens("birthContentForModification calls scheduleAsyncF0Extraction with hostModification",
                   f0BirthMethod,
                   {"auto* hostModification = modification.audioModification",
                    "scheduleAsyncF0Extraction(modification.contentKey()"});

      // 2) scheduleAsyncF0Extraction entry 检查 Host pointer 缺失不提交
      const auto scheduleMethod = extractFunctionBlock(dcImpl, "void OpenTuneDocumentController::scheduleAsyncF0Extraction");
      expect(contains(scheduleMethod, "birthRevision"),
             "scheduleAsyncF0Extraction captures birthRevision (#B.3)");
      expectTokens("scheduleAsyncF0Extraction checks hostModification absence before submission",
                   scheduleMethod,
                   {"if (hostModification == nullptr)", "return;"});

      // 3) scheduleAsyncF0Extraction 完成 completion Lambda 捕获 hostModification (#F0-part2)
      expectTokens("scheduleAsyncF0Extraction completion captures hostModification",
                   scheduleMethod,
                   {"[this, crs, key, birthRevision, hostModification, leaseToken"});

      // 4) scheduleAsyncF0Extraction completion stale guard mod->audioModification != hostModification (#F0-part2)
      expectTokens("scheduleAsyncF0Extraction completion stale guard host pointer",
                   scheduleMethod,
                   {"if (!mod->hasContentState())",
                     "if (mod->birthRevision != birthRevision)",
                     "if (mod->audioModification != hostModification)",
                     "return;"});
// 5) birthContentForModification 在方法开头取消旧 F0 request
     expect(contains(birthMethod, "contentF0ExtractionService_->cancel"),
            "birthContentForModification 在 read 开始前取消旧 F0 request (#B.4)");
     expectNoTokens("birthContentForModification no direct analysis mutation",
                    birthMethod,
                    {"content.analysis.silentGaps =", "content.analysis.originalF0State ="});
}

// Final static contract block.
void noSelectPlacementOutsideCommitHelpers()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto commitBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitPlacementSelection");
    const auto emptyBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitEmptyPlacementSelection");

    expect(!commitBody.empty(), "commitPlacementSelection must be found");
    expect(!emptyBody.empty(), "commitEmptyPlacementSelection must be found");

    // Count total selectPlacement calls in the file
    const auto total = countOf(src, "selectPlacement(");
    const auto inCommit = countOf(commitBody, "selectPlacement(");
    const auto inEmpty = countOf(emptyBody, "selectPlacement(");

    expect(total == inCommit + inEmpty,
           "All selectPlacement calls must be in commit helpers (total=" + std::to_string(total)
           + " commit=" + std::to_string(inCommit) + " empty=" + std::to_string(inEmpty) + ")");
}

void selectPlacementPreservesPerTrackMemory()
{
    const auto src = readText("Source/StandaloneArrangement.cpp");
    const auto selectBody = extractFunctionBlock(src, "bool StandaloneArrangement::selectPlacement");
    const auto setIndexBody = extractFunctionBlock(src, "bool StandaloneArrangement::setSelectedPlacementIndex");

    expect(!selectBody.empty(), "StandaloneArrangement::selectPlacement must be found");
    expect(!setIndexBody.empty(), "StandaloneArrangement::setSelectedPlacementIndex must be found");

    // Per-track selectedPlacementId must survive cross-track selection – no cross-track clearing
    for (const auto* body : { &selectBody, &setIndexBody }) {
        expect(!contains(*body, "kTrackCount"),
               "selectPlacement must not iterate all tracks – preserves per-track memory");
        expect(!contains(*body, "!= trackId"),
               "selectPlacement must not skip other tracks – no cross-track clearing");
    }
}

void perTrackSelectionMemorySurvivesEmptyClick()
{
    // arrangement model must carry per-track selectedPlacementId
    const auto arrHeader = readText("Source/StandaloneArrangement.h");
    expect(contains(arrHeader, "selectedPlacementId"),
           "Track struct must have per-track selectedPlacementId for selection memory");

    // empty click must NOT zero out per-track memory via clearAllSelections
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto emptyBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitEmptyPlacementSelection");
    expect(!contains(emptyBody, "clearAllSelections"),
           "commitEmptyPlacementSelection must not call clearAllSelections");

    // trackSelected callback must still use per-track getSelectedPlacementIndex
    const auto editorSrc = readText("Source/Standalone/PluginEditor.cpp");
    const auto trackSelBody = extractFunctionBlock(editorSrc, "void OpenTuneAudioProcessorEditor::trackSelected");
    expect(contains(trackSelBody, "getStandaloneSelectedPlacementIndex"),
           "trackSelected must use per-track getSelectedPlacementIndex lookup");
}

void captureApplyAudioBufferContractIsReferenceWithIdentityTimeGrid()
{
    const auto header = readText("Source/Content/CaptureSegmentContent.h");
    const auto impl = readText("Source/Content/CaptureSegmentContent.cpp");
    const auto body = extractFunctionBlock(impl, "void CaptureSegmentContent::applyAudioBuffer");

    expect(!body.empty(), "applyAudioBuffer body must be found");

    // Signature: reference, not pointer
    expect(!contains(header, "applyAudioBuffer(const juce::AudioBuffer<float>*"),
           "applyAudioBuffer must take reference, not pointer");

    // No nullptr / empty early return
    expect(!contains(body, "nullptr"),
           "applyAudioBuffer must not have nullptr early return");
    expect(!contains(body, "getNumSamples() == 0"),
           "applyAudioBuffer must not have empty-buffer early return");

    // Identity TimeGrid birth
    expect(contains(body, "TimeGridSnapshot::makeIdentity"),
           "applyAudioBuffer must generate identity TimeGridSnapshot");
    expect(contains(body, "++editable_.timeGridRevision"),
           "applyAudioBuffer must bump timeGridRevision");
    expect(contains(body, "editable_.timeGrid = TimeGridSnapshot::makeIdentity"),
           "applyAudioBuffer must assign identity TimeGrid to editable_.timeGrid");
}

void captureCallSitesDereferenceNotNullSharedPtr()
{
    const auto session = readText("Source/Plugin/Capture/CaptureSession.cpp");
    const auto persistence = readText("Source/Plugin/Capture/CapturePersistence.cpp");

    // No .get() call in applyAudioBuffer call sites
    expect(!contains(session, "applyAudioBuffer(pcm.get()"),
           "CaptureSession must not pass pcm.get() to applyAudioBuffer");
    expect(!contains(persistence, "applyAudioBuffer(p.audio.get()"),
           "CapturePersistence must not pass p.audio.get() to applyAudioBuffer");

    // Dereference pattern present
    expect(contains(session, "applyAudioBuffer(*pcm"),
           "CaptureSession must dereference pcm with *pcm");
    expect(contains(persistence, "applyAudioBuffer(*p.audio"),
           "CapturePersistence must dereference p.audio with *p.audio");
}

// ============================================================================
// PianoRoll direct-paint and ruler control contracts.
// Time/Cont 按钮覆盖效果由 child component z-order 产生。
// ============================================================================

void drawTimeRulerHasRulerPaintBounds()
{
    const auto composer = readText("Source/Standalone/UI/TimelineLayerComposer.cpp");
    const auto drawBlock = extractFunctionBlock(
        composer, "void TimelineLayerComposer::drawTimeRuler");

    expect(!drawBlock.empty(), "drawTimeRuler must be found");
    expectTokens("drawTimeRuler uses rulerPaintBounds",
                 drawBlock,
                 {"rulerPaintBounds", "getWidth()"});
    expectNoTokens("drawTimeRuler no footprint",
                   drawBlock,
                   {"rulerControlFootprint"});
}

void noArrangementClipExclusionLeakedToPianoRoll()
{
    const auto component = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto header = readText("Source/Standalone/UI/PianoRollComponent.h");

    expectNoTokens("PianoRollComponent.cpp", component,
                   {"rulerClipExclusion", "buttonClipExclusion", "rulerButtonClip",
                    "exclusionRect", "clipExclusionRect"});
    expectNoTokens("PianoRollComponent.h", header,
                   {"rulerClipExclusion", "buttonClipExclusion", "rulerButtonClip",
                    "exclusionRect", "clipExclusionRect"});
}

void f0CurvesStayFullyOpaque()
{
    const auto rendererHeader = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");
    const auto rendererImpl = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");
    const auto drawF0Curve = extractFunctionBlock(
        rendererImpl,
        "void PianoRollRenderer::drawF0Curve");
    const auto originalCurve = extractBlockByMarker(drawF0Curve, "if (ctx.showOriginalF0)");
    const auto correctedCurve = extractBlockByMarker(
        drawF0Curve,
        "if (ctx.showCorrectedF0 && item.pitchSnapshot->hasCorrectionLayer())");

    expectNoTokens("F0 visual points have no segment taper", rendererHeader, {"segmentTaper"});
    expectNoTokens("F0 renderer has no segment taper", rendererImpl,
                   {"segmentTaper", "fadeSpanCount", "taperAlpha"});
    expect(countOf(drawF0Curve, "const float alpha = 1.0f;") == 2,
           "original and corrected F0 must use full base opacity");
    expectNoTokens("original F0 has no energy alpha", originalCurve,
                   {"p.energyAlpha", "pt.energyAlpha"});
    expectNoTokens("corrected F0 has no energy alpha", correctedCurve,
                   {"p.energyAlpha", "pt.energyAlpha"});
    expectTokens("original F0 full core opacity", originalCurve, {"colour.withAlpha(1.0f)"});
    expectTokens("corrected F0 full core opacity", correctedCurve,
                 {"withAlpha(1.0f)", "buildGradient(1.0f)"});
}

void pianoRollPaintRestoresOpaqueFillAfterShadow()
{
    const auto component = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto renderer = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");
    const auto rasterizeStatic = extractFunctionBlock(component, "void PianoRollComponent::rasterizeStatic");
    const auto drawNotes = extractFunctionBlock(renderer, "void PianoRollRenderer::drawNotes");

    expect(!rasterizeStatic.empty(), "PianoRollComponent::rasterizeStatic must be found");
    expect(!drawNotes.empty(), "PianoRollRenderer::drawNotes must be found");

    // 正常 paint 不再调用 drawShadow/fillAll；chrome 仅由 rasterizeStatic 和缩放预览分支绘制
    // rasterizeStatic 在阴影后恢复 opaque fill
    expectTokens("PianoRollComponent::rasterizeStatic restores opaque fill",
                 rasterizeStatic,
                 {"UIColors::drawShadow", "g.setColour(juce::Colours::white);", "TimelineLayerComposer::drawTimeRuler"});
    expect(inOrder(rasterizeStatic,
                   {"UIColors::drawShadow",
                    "g.setColour(juce::Colours::white);",
                    "TimelineLayerComposer::drawTimeRuler"}),
           "PianoRollComponent::rasterizeStatic must restore opaque fill after shadow before drawing");

    expectTokens("PianoRollRenderer::drawNotes uses opaque note fill",
                 drawNotes,
                 {"noteColor.withAlpha(0.90f)"});
    expectNoTokens("PianoRollRenderer::drawNotes has no translucent note fill",
                   drawNotes,
                   {"noteColor.withAlpha(0.50f)"},
                   "note fill must remain at 0.90f");
}

void pianoRollRetainedSurfaceArchitecture()
{
    const auto componentHeader = readText("Source/Standalone/UI/PianoRollComponent.h");
    const auto componentImpl = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto overlayImpl = readText("Source/Standalone/UI/PianoRoll/PianoRollOverlayComponent.cpp");
    const auto rendererImpl = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");
    const auto arrangementImpl = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");

    // 正面断言：两张 Image + 透明 Overlay
    expectTokens("Piano Roll has retained surfaces",
                 componentHeader,
                 {"staticSurface_", "contentSurface_", "overlay_"});

    // staticSurface_ 使用完整组件尺寸
    expectTokens("staticSurface_ uses full component dimensions",
                 componentImpl,
                 {"staticSurface_.getWidth() != fullW", "juce::Image(juce::Image::ARGB, fullW, fullH"});

    // 根 paint 不含 rasterizeDirtySurfaces（已迁出到各失效点）
    const auto paint = extractFunctionBlock(componentImpl, "void PianoRollComponent::paint");
    expectNoTokens("paint does not call rasterizeDirtySurfaces",
                   paint,
                   {"rasterizeDirtySurfaces"});
    expectNoTokens("paint does not call rasterizeStatic/recreateSurfaces",
                   paint,
                   {"rasterizeStatic()", "rasterizeContent()", "recreateSurfaces"});
    // 正常 else 分支仅 blit，无 fillAll/drawShadow；zoomPreviewActive_ 分支允许固定 chrome
    const auto normalBranch = extractBlockByMarker(paint, "else {");
    expect(!normalBranch.empty(), "normal paint else branch must be found");
    expectNoTokens("normal paint branch has no fillAll/drawShadow",
                   normalBranch,
                   {"fillAll", "UIColors::drawShadow"});
    expectTokens("paint blits staticSurface_",
                 paint,
                 {"drawImageAt(staticSurface_"});
    expectTokens("paint blits contentSurface_",
                 paint,
                  {"drawImageAt(contentSurface_"});

    // 缩放预览期间 invalidateLiveNotes 不栅格内容，仅标记 contentDirty_
    const auto invalLiveBlock = extractFunctionBlock(componentImpl, "void PianoRollComponent::invalidateLiveNotes");
    expectTokens("invalidateLiveNotes has zoomPreviewActive_ branch",
                 invalLiveBlock,
                 {"zoomPreviewActive_"});
    const auto zoomBranch = extractBlockByMarker(invalLiveBlock, "if (zoomPreviewActive_)");
    expect(!zoomBranch.empty(), "zoom preview branch in invalidateLiveNotes must be found");
    expectTokens("invalidateLiveNotes zoom branch sets contentDirty_",
                 zoomBranch,
                 {"contentDirty_ = true;"});
    expectNoTokens("invalidateLiveNotes zoom branch no rasterizeContent",
                   zoomBranch,
                   {"rasterizeContent"});
    expectNoTokens("invalidateLiveNotes zoom branch no Image alloc",
                   zoomBranch,
                   {"juce::Image"});

    // 缩放预览：从 paint 提取 zoomPreviewActive_ block，验证三块表面 clip + transform 结构
    const auto previewBlock = extractBlockByMarker(paint, "if (zoomPreviewActive_)");
    expect(!previewBlock.empty(), "zoom preview block must be found in paint");

    // 三块 staticSurface_.getClippedImage( 裁剪
    expect(countOf(previewBlock, "staticSurface_.getClippedImage(") == 3,
           "Preview block must have exactly three staticSurface_ clips");
    expectTokens("Preview clipper: ruler zone",
                  previewBlock,
                  {"pianoKeyWidth_, 0, tlW, rulerHeight_"});
    expectTokens("Preview clipper: piano key zone",
                  previewBlock,
                  {"0, rulerHeight_, pianoKeyWidth_, ch"});
    expectTokens("Preview clipper: timeline zone",
                  previewBlock,
                  {"pianoKeyWidth_, rulerHeight_, tlW, ch"});

    // 无 staticSurface_ 经 drawImageTransformed（即静态面不经 transform 绘制）
    expectNoTokens("Preview no drawImageTransformed(staticSurface_",
                   previewBlock,
                   {"drawImageTransformed(staticSurface_"});

    // contentSurface_ 经 drawImageTransformed(contentSurface_, fullXf, false)
    expectTokens("Preview contentSurface_ via drawImageTransformed with fullXf",
                  previewBlock,
                  {"drawImageTransformed(contentSurface_, fullXf, false)"});

    // 标尺 transform：原点补偿 + X-only scale
    expectTokens("Ruler transform origin compensation",
                  previewBlock,
                  {"tx + static_cast<float>(pianoKeyWidth_ * scaleX)"});
    expectTokens("Ruler transform X-only scale",
                  previewBlock,
                  {"static_cast<float>(scaleX), 1.0f"});
    expectTokens("Ruler transform translated(rulerTx, 0.0f)",
                  previewBlock,
                  {"translated(rulerTx, 0.0f)"});

    // 琴键 transform：原点补偿 + Y-only scale
    expectTokens("Piano key transform origin compensation",
                  previewBlock,
                  {"offsetY + static_cast<float>(rulerHeight_ * scaleY)"});
    expectTokens("Piano key transform Y-only scale",
                  previewBlock,
                  {"1.0f, static_cast<float>(scaleY)"});
    expectTokens("Piano key transform translated(0.0f, keyOffsetY)",
                  previewBlock,
                  {"translated(0.0f, keyOffsetY)"});

    // timeline transform：同时使用 rulerTx 与 keyOffsetY，XY static_cast<float> scale
    expectTokens("Timeline transform XY scale + dual origin",
                  previewBlock,
                  {"static_cast<float>(scaleX), static_cast<float>(scaleY)",
                   "translated(rulerTx, keyOffsetY)"});

    // strip 滚动更新保留
    expectTokens("Piano Roll uses moveImageSection for edge-scroll",
                 componentImpl,
                 {"moveImageSection"});

    // 探针入口：static-raster/content-raster/root-paint/vblank-to-root-paint 在 componentImpl，overlay-present 在 overlayImpl
    expectTokens("Static and Content probes in componentImpl",
                  componentImpl,
                  {"recordRenderProbe(RenderProbePoint::StaticRaster",
                   "recordRenderProbe(RenderProbePoint::ContentRaster"});
    expectTokens("Root paint probes in componentImpl",
                 componentHeader + componentImpl,
                 {"rootPaintProbe_",
                  "vblankToRootPaintProbe_",
                  "lastVBlankMs_",
                  "RenderProbePoint::RootPaint",
                  "RenderProbePoint::VBlankToRootPaint",
                  "recordRenderProbe(RenderProbePoint::RootPaint",
                  "recordRenderProbe(RenderProbePoint::VBlankToRootPaint"});
    expectTokens("Root paint probe log fields",
                 componentImpl,
                 {"root-paint:", "vblank-to-root-paint:"});
    expectTokens("Overlay probe in overlayImpl",
                 overlayImpl,
                 {"recordRenderProbe(PianoRollComponent::RenderProbePoint::OverlayPresent"});

    // playheadTimeForPaint_ 唯一 VBlank 写入
    const auto vblank = extractFunctionBlock(componentImpl, "void PianoRollComponent::onScrollVBlankCallback");
    const auto heartbeat = extractFunctionBlock(componentImpl, "void PianoRollComponent::onHeartbeatTick");
    expect(countOf(heartbeat, "waveformMipmapCache_.buildIncremental(") == 2,
           "Heartbeat must have exactly two waveform build branches (throttle + non-throttle)");
    const auto progressBlock = extractBlockByMarker(heartbeat, "if (progressed && waveformMipmapCache_.isComplete())");
    expect(!progressBlock.empty(), "Heartbeat progress block must be found");
    expectTokens("Heartbeat progress block triggers redraw chain",
                  progressBlock,
                  {"contentDirty_ = true;",
                   "rasterizeDirtySurfaces();",
                   "repaint();"});
    expectTokens("VBlank records absolute timestamp",
                 vblank,
                 {"lastVBlankMs_ = juce::Time::getMillisecondCounterHiRes();", "juce::ignoreUnused(timestampSec);"});
    expectTokens("playheadTimeForPaint_ only written in VBlank",
                 vblank,
                 {"playheadTimeForPaint_ = playheadTime"});
    expectNoTokens("playheadTimeForPaint_ not written in Heartbeat",
                   heartbeat,
                   {"playheadTimeForPaint_"});

    // mouseDrag 仅纵向变化时设 dirty，commit 后无冗余 updateScrollBars/repaint
    const auto mouseDragBlock = extractFunctionBlock(componentImpl, "if (interactionState_.isPanning)");
    expectTokens("mouseDrag vertical change sets dirty before commit",
                 mouseDragBlock,
                 {"verticalChanged", "staticDirty_ = true", "contentDirty_ = true"});
    expectNoTokens("mouseDrag no redundant updateScrollBars/repaint after commit",
                   mouseDragBlock,
                   {"updateScrollBars();", "repaint();"});

    // 所有完整失效点经 rasterizeDirtySurfaces 触发
    expectTokens("requestContentRedraw calls rasterizeDirtySurfaces",
                 extractFunctionBlock(componentImpl, "void PianoRollComponent::requestContentRedraw()"),
                 {"rasterizeDirtySurfaces();"});
    expectTokens("requestThemeRedraw calls rasterizeDirtySurfaces",
                 extractFunctionBlock(componentImpl, "void PianoRollComponent::requestThemeRedraw()"),
                 {"rasterizeDirtySurfaces();"});
    expectTokens("setShowWaveform calls rasterizeDirtySurfaces",
                 extractFunctionBlock(componentImpl, "void PianoRollComponent::setShowWaveform"),
                 {"rasterizeDirtySurfaces();"});
    expectTokens("setBpm calls rasterizeDirtySurfaces",
                 extractFunctionBlock(componentImpl, "void PianoRollComponent::setBpm"),
                 {"rasterizeDirtySurfaces();"});
    const auto vertScroll = extractFunctionBlock(componentImpl, "void PianoRollComponent::handleVerticalScrollWheel");
    expectTokens("handleVerticalScrollWheel calls rasterizeDirtySurfaces",
                 vertScroll,
                 {"rasterizeDirtySurfaces();"});

    // tryConsumeInitialF0View：纵向偏移脏标记置于 activateTimelineCamera 之前
    const auto initialViewBlock = extractFunctionBlock(componentImpl, "bool PianoRollComponent::tryConsumeInitialF0View");
    expect(!initialViewBlock.empty(), "tryConsumeInitialF0View must be found");
    {
        const auto verticalPos = initialViewBlock.find("verticalScrollOffset_ = std::clamp");
        const auto dirtyPos = initialViewBlock.find("staticDirty_ = true;", verticalPos);
        const auto activatePos = initialViewBlock.find("activateTimelineCamera(", dirtyPos);
        expect(dirtyPos != std::string::npos && activatePos != std::string::npos
               && dirtyPos < activatePos,
               "tryConsumeInitialF0View must set dirty before activateTimelineCamera");
    }

    // fitToScreen：纵向 fit 分支不提前栅格/repaint，仅标记 dirty，由最终 commitViewportRequest 触发一次重建
    const auto fitBlock = extractFunctionBlock(componentImpl, "void PianoRollComponent::fitToScreen()");
    const auto vertFitBlock = extractBlockByMarker(fitBlock, "// Reset scroll to show top");
    expect(!vertFitBlock.empty(), "fitToScreen vertical fit branch must be found");
    expectNoTokens("fitToScreen vertical branch no premature rasterize",
                   vertFitBlock,
                   {"rasterizeDirtySurfaces", "updateScrollBars", "repaint"});
    expectTokens("fitToScreen vertical branch sets dirty",
                 vertFitBlock,
                 {"staticDirty_ = true;", "contentDirty_ = true;"});
    // 确保无论何时都到 commitViewportRequest
    const auto afterFitBlock = fitBlock.substr(fitBlock.find("// 2. Horizontal Fit:"));
    expect(contains(afterFitBlock, "commitViewportRequest"),
           "fitToScreen horizontal branch must commit via commitViewportRequest");

    // 无旧根探针残留
    expectNoTokens("Piano Roll has no old root-paint probes",
                   componentImpl,
                   {"gFrameCount", "gRepaintCount", "gPaintTimerStart", "diagnosticReportFrameMs"});

    // RasterView 单一快照，rasterCamera_/surfacePixelsPerSemitone_/surfaceVerticalScrollOffset_ 已删除
    expectTokens("Piano Roll has struct RasterView",
                 componentHeader,
                 {"struct RasterView"});
    expectTokens("Piano Roll has rasterView_ member",
                 componentHeader,
                 {"RasterView rasterView_"});
    expectTokens("Piano Roll has makeViewMapperForRasterView",
                 componentHeader,
                 {"makeViewMapperForRasterView"});
    expectNoTokens("Piano Roll has no rasterCamera_ / surfacePixelsPerSemitone_ / surfaceVerticalScrollOffset_ / surfaceOriginPx_ etc.",
                   componentHeader + componentImpl,
                   {"rasterCamera_", "surfacePixelsPerSemitone_", "surfaceVerticalScrollOffset_",
                    "surfaceOriginPx_", "surfacePps_", "tlPhasePx", "tlSrcX"});
    expectTokens("PianoRollComponent.cpp uses rasterView_ for rasterization",
                  componentImpl,
                  {"rasterView_.camera", "rasterView_.pixelsPerSemitone", "rasterView_.verticalScrollOffset"});

    // 完整双表面 dirty 时一次性同步 rasterView_ 为 live 状态
    {
        const auto dirtySync = extractFunctionBlock(componentImpl, "void PianoRollComponent::rasterizeDirtySurfaces");
        expect(!dirtySync.empty(), "rasterizeDirtySurfaces must be found");
        expect(inOrder(dirtySync, {"staticDirty_ && contentDirty_",
                                   "rasterView_.camera = camera_",
                                   "rasterView_.pixelsPerSemitone = pixelsPerSemitone_",
                                   "rasterView_.verticalScrollOffset = verticalScrollOffset_"}),
               "full dual-surface dirty must sync all three rasterView_ fields in order");
    }

    // rasterizeStatic 从 rasterView_ 读取 source（相机 + 纵向）
    {
        const auto rasterStatic = extractFunctionBlock(componentImpl, "void PianoRollComponent::rasterizeStatic");
        expect(!rasterStatic.empty(), "rasterizeStatic must be found");
        expectTokens("rasterizeStatic reads rasterView_ camera source",
                      rasterStatic,
                      {"rasterView_.camera.pixelsPerSecond", "rasterView_.camera.visibleStartSeconds"});
        expectTokens("rasterizeStatic reads rasterView_ vertical source",
                      rasterStatic,
                      {"rasterView_.verticalScrollOffset", "rasterView_.pixelsPerSemitone"});
    }

    // rasterizeContent 从 rasterView_ 读取 source，通过 makeViewMapperForRasterView 构建坐标
    {
        const auto rasterContent = extractFunctionBlock(componentImpl, "void PianoRollComponent::rasterizeContent");
        expect(!rasterContent.empty(), "rasterizeContent must be found");
        expectTokens("rasterizeContent reads rasterView_ camera + vertical source",
                      rasterContent,
                      {"rasterView_.camera.pixelsPerSecond", "rasterView_.pixelsPerSemitone"});
        expectTokens("rasterizeContent uses makeViewMapperForRasterView",
                      rasterContent,
                      {"makeViewMapperForRasterView(rasterView_)"});
        expectTokens("rasterizeContent waveform draw uses isComplete gate",
                      rasterContent,
                      {"showWaveform_ && waveformMipmapCache_.isComplete()"});
    }

    // applyRasterCamera 条带路径只写 rasterView_.camera，不写纵向 source
    {
        const auto applyCam = extractFunctionBlock(componentImpl, "void PianoRollComponent::applyRasterCamera");
        expect(!applyCam.empty(), "applyRasterCamera must be found");
        expectTokens("applyRasterCamera strip writes rasterView_.camera",
                      applyCam,
                      {"rasterView_.camera = newCamera"});
        expectNoTokens("applyRasterCamera strip no vertical source write",
                        applyCam,
                        {"rasterView_.pixelsPerSemitone", "rasterView_.verticalScrollOffset"});

        const auto zeroPixelBlock = extractBlockByMarker(applyCam, "if (dPixels == 0)");
        expect(!zeroPixelBlock.empty(), "applyRasterCamera dPixels==0 block must be found");
        expectTokens("applyRasterCamera dPixels==0 returns directly",
                     zeroPixelBlock,
                     {"return;"});
        expectNoTokens("applyRasterCamera dPixels==0 has no repaint or raster",
                       zeroPixelBlock,
                       {"repaint", "rasterize", "rasterView_.camera ="});
    }

    // rasterizeStatic 仅统一 clip chrome 链，无 stripVisibleStart/fillRect(*dirtyRect)
    const auto rasterStatic2 = extractFunctionBlock(componentImpl, "void PianoRollComponent::rasterizeStatic");
    expectNoTokens("rasterizeStatic no strip-specific path",
                   rasterStatic2,
                   {"stripVisibleStart", "fillRect(*dirtyRect)", "inTimelineZone"});

    // drawWaveform / paintHistoricalClipWaveform：完成态 cache 为唯一 source，不含 buildProgress 中间状态
    {
        const auto drawWaveform = extractFunctionBlock(rendererImpl, "void PianoRollRenderer::drawWaveform");
        expect(!drawWaveform.empty(), "drawWaveform must be found");
        expectNoTokens("drawWaveform has no buildProgress",
                       drawWaveform,
                       {"buildProgress"});
    }
    {
        const auto paintHistWf = extractFunctionBlock(arrangementImpl, "static void paintHistoricalClipWaveform");
        expect(!paintHistWf.empty(), "paintHistoricalClipWaveform must be found");
        expectNoTokens("paintHistoricalClipWaveform has no buildProgress",
                       paintHistWf,
                       {"buildProgress"});
        expectTokens("paintHistoricalClipWaveform uses isComplete gate",
                      paintHistWf,
                      {"waveformMipmapCache.isComplete()"});
    }

    // Arrangement heartbeat 可视刷新条件含 progressed && isComplete
    {
        const auto arrHeartbeat = extractFunctionBlock(arrangementImpl, "void ArrangementViewComponent::onHeartbeatTick");
        expect(!arrHeartbeat.empty(), "Arrangement heartbeat must be found");
        expectTokens("Arrangement heartbeat visible refresh uses progressed && isComplete",
                      arrHeartbeat,
                      {"progressed && waveformMipmapCache_.isComplete()"});
    }

    // VBlank：稳定 CONT 有 fixedCentre 跳过 + playState/time/camera 变化 gate
    const auto vblankBlock = extractFunctionBlock(componentImpl, "void PianoRollComponent::onScrollVBlankCallback");
    expectNoTokens("VBlank no bare overlay repaint",
                   vblankBlock,
                   {"overlay_->repaint();"});
    expectTokens("VBlank has fixedCentre in playhead lambda",
                 vblankBlock,
                 {"pres.fixedCentre"});
    expectTokens("VBlank has stableCont skip gate",
                 vblankBlock,
                 {"stableCont"});
    expectTokens("VBlank uses playStateChanged || timeChanged || cameraChanged",
                 vblankBlock,
                 {"playStateChanged || timeChanged || cameraChanged"});

    // invalidateInteractionPreview 无条件转发 bounds，无 empty guard
    const auto invalBlock = extractFunctionBlock(componentImpl, "void PianoRollComponent::invalidateInteractionPreview");
    expectTokens("invalidateInteractionPreview calls overlay_->repaint(bounds)",
                 invalBlock,
                 {"overlay_->repaint(bounds)"});
    expectNoTokens("invalidateInteractionPreview has no empty guard",
                   invalBlock,
                   {"isEmpty"});

    // DrawNote 使用 sourceTimeToScreenX、contentOriginY、freqToMidi 真实几何，非 -12/16 硬编码
    const auto toolHandler = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto drawNoteHandler = extractFunctionBlock(toolHandler, "void PianoRollToolHandler::handleDrawNoteTool");
    expectTokens("DrawNote uses sourceTimeToScreenX",
                 drawNoteHandler,
                 {"sourceTimeToScreenX"});
    expectTokens("DrawNote uses contentOriginY",
                 drawNoteHandler,
                 {"contentOriginY"});
    expectTokens("DrawNote uses freqToMidi for pixelsPerSemitone",
                 drawNoteHandler,
                 {"freqToMidi"});
    expectTokens("DrawNote uses beforeBounds.getUnion",
                 drawNoteHandler,
                 {"beforeBounds.getUnion"});
    expectNoTokens("DrawNote no hardcoded -12 Y offset",
                   drawNoteHandler + extractFunctionBlock(toolHandler, "void PianoRollToolHandler::handleDrawNoteUp"),
                   {" - 12", " + 16"});

    // 缩放冻结不写 rasterView_（已有保留表面的冻结源，非缩放捕获目标）；
    // 两函数仍设 zoomPreviewActive_
    const auto beginZoom = extractFunctionBlock(componentImpl, "void PianoRollComponent::beginZoomPreview");
    expectNoTokens("beginZoomPreview does not write rasterView_",
                   beginZoom,
                   {"rasterView_"});
    expectTokens("beginZoomPreview sets zoomPreviewActive_",
                 beginZoom,
                 {"zoomPreviewActive_"});
    const auto vertZoom = extractFunctionBlock(componentImpl, "void PianoRollComponent::handleVerticalZoomWheel");
    expectNoTokens("handleVerticalZoomWheel does not write rasterView_",
                   vertZoom,
                   {"rasterView_"});
    expectTokens("handleVerticalZoomWheel sets zoomPreviewActive_",
                 vertZoom,
                 {"zoomPreviewActive_"});

    // resized：updateScrollBars 位于 tryConsume 失败 fallback 后
    const auto resizedBlock = extractFunctionBlock(componentImpl, "void PianoRollComponent::resized()");
    expect(inOrder(resizedBlock, {"tryConsumeInitialF0View", "updateScrollBars"}),
           "resized updateScrollBars must be after tryConsume");

    // 反面断言：旧架构残留不存在
    expectNoTokens("Piano Roll has no legacy tile cache or old surfaces",
                   componentHeader + componentImpl,
                   {"TimelineCompositeCache", "compositeCache_", "viewportSurface_",
                    "pianoKeySurface_", "scrollViewportSurfaceTo",
                    "rebuildViewportSurfaceFromReadyTiles", "tiles_"});
}

void pianoRollZoomHandlersAvoidBusinessImageAllocation()
{
    const auto component = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto verticalZoom = extractFunctionBlock(
        component, "void PianoRollComponent::handleVerticalZoomWheel");
    const auto horizontalZoom = extractFunctionBlock(
        component, "void PianoRollComponent::handleHorizontalZoomWheel");

    expect(!verticalZoom.empty(), "vertical Piano Roll zoom handler must be found");
    expect(!horizontalZoom.empty(), "horizontal Piano Roll zoom handler must be found");
    expectNoTokens("vertical Piano Roll zoom handler",
                   verticalZoom,
                   {"juce::Image", "tiles_.clear()"});
    expectNoTokens("horizontal Piano Roll zoom handler",
                   horizontalZoom,
                   {"juce::Image", "tiles_.clear()"});
}

void pianoRollZoomHandlersDoNotPauseAutoFollow()
{
    const auto component = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    const auto verticalZoom = extractFunctionBlock(
        component, "void PianoRollComponent::handleVerticalZoomWheel");
    const auto horizontalZoom = extractFunctionBlock(
        component, "void PianoRollComponent::handleHorizontalZoomWheel");
    const auto beginZoom = extractFunctionBlock(
        component, "void PianoRollComponent::beginZoomPreview");

    expect(!verticalZoom.empty(), "vertical zoom handler must be found");
    expect(!horizontalZoom.empty(), "horizontal zoom handler must be found");
    expect(!beginZoom.empty(), "beginZoomPreview must be found");

    // Zoom must set userHasManuallyZoomed_ but NOT pause CONT follow via userScrollHold_
    expectTokens("vertical zoom keeps manual zoom flag",
                 verticalZoom,
                 {"userHasManuallyZoomed_ = true"});
    expectTokens("horizontal zoom delegates to beginZoomPreview",
                 horizontalZoom,
                 {"beginZoomPreview"});
    expectTokens("beginZoomPreview keeps manual zoom flag",
                 beginZoom,
                 {"userHasManuallyZoomed_ = true"});
    expectNoTokens("vertical zoom must not pause auto-follow",
                   verticalZoom,
                   {"userScrollHold_"});
    expectNoTokens("horizontal zoom must not pause auto-follow via handler nor beginZoom",
                   horizontalZoom + beginZoom,
                   {"userScrollHold_"});
}

void absoluteTimelineTimeRemainsUnquantized()
{
    const auto viewMapper = readText("Source/Standalone/UI/ViewMapper.h");
    const auto xToTime = extractFunctionBlock(viewMapper, "double xToTime(int x) const");
    expect(!xToTime.empty(), "ViewMapper::xToTime must be found");
    expectTokens("ViewMapper::xToTime uses continuous absolute seconds",
                 xToTime,
                 {"visibleStartSeconds +", "/ pixelsPerSecond"});
    expectNoTokens("ViewMapper has no quantized visible start",
                   viewMapper,
                   {"llround(visibleStartSeconds * pixelsPerSecond)"});

    const auto viewportPolicy = readText("Source/Standalone/UI/TimelineViewportPolicy.cpp");
    expectNoTokens("TimelineViewportPolicy has no time quantization",
                   viewportPolicy,
                   {"std::round(pps * 1000.0)",
                    "std::floor(request.targetTime / visibleDuration)"});

    const auto pianoRoll = readText("Source/Standalone/UI/PianoRollComponent.cpp");
    expectNoTokens("PianoRollComponent has no absolute-time quantization",
                   pianoRoll,
                   {"std::llround(camera_.visibleStartSeconds * camera_.pixelsPerSecond) / camera_.pixelsPerSecond",
                    "newRangeStart / pps"});

    const auto viewportPolicyHeader = readText("Source/Standalone/UI/TimelineViewportPolicy.h");
    expectNoTokens("TimelineViewportPolicy has no pixel-derived range API",
                   viewportPolicyHeader,
                   {"absoluteStartPx", "absoluteEndPx", "visibleStartPx", "visibleWidthPx"});

    const auto layerComposerHeader = readText("Source/Standalone/UI/TimelineLayerComposer.h");
    expectTokens("TimelineLayerComposer keeps tempo continuous",
                 layerComposerHeader,
                 {"double tempo = 120.0;"});
}

void pitchCurveF0SpanApiReplacesOldRenderPath()
{
    const auto pitchCurveHeader = readText("Source/Utils/PitchCurve.h");
    const auto pitchCurveImpl = readText("Source/Utils/PitchCurve.cpp");

    expectTokens("PitchCurve.h has forEachCorrectionF0Span",
                 pitchCurveHeader,
                 {"forEachCorrectionF0Span"});
    expectTokens("PitchCurve.h has template Sink",
                 pitchCurveHeader,
                 {"template <typename Sink>"});
    expectTokens("PitchCurve.h has nullptr gap semantic",
                 pitchCurveHeader,
                 {"nullptr"});
    expectNoTokens("PitchCurve.h no renderCorrectionLayerF0Range",
                   pitchCurveHeader,
                   {"renderCorrectionLayerF0Range"});

    expectNoTokens("PitchCurve.cpp no renderCorrectionLayerF0Range",
                   pitchCurveImpl,
                   {"renderCorrectionLayerF0Range"});
    expectNoTokens("PitchCurve.cpp no tempBuffer",
                   pitchCurveImpl,
                   {"tempBuffer"});
    expectNoTokens("PitchCurve.cpp no tempBuffer.assign",
                   pitchCurveImpl,
                   {"tempBuffer.assign"});

    const auto rendererHeader = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.h");
    expectNoTokens("PianoRollRenderer.h no F0FrameToX",
                   rendererHeader,
                   {"F0FrameToX"});
    expectNoTokens("PianoRollRenderer.h no F0FrameToY",
                   rendererHeader,
                   {"F0FrameToY"});
    expectNoTokens("PianoRollRenderer.h no sourceFrameBase",
                   rendererHeader,
                   {"sourceFrameBase"});
    expectNoTokens("PianoRollRenderer.h no buildF0VisualSegments",
                   rendererHeader,
                   {"buildF0VisualSegments"});
    expectNoTokens("PianoRollRenderer.h no F0VisualBuildOptions",
                   rendererHeader,
                   {"F0VisualBuildOptions"});

    const auto rendererImpl = readText("Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp");
    expectNoTokens("PianoRollRenderer.cpp no std::function",
                   rendererImpl,
                   {"std::function"});
    expectNoTokens("PianoRollRenderer.cpp no F0VisualLOD",
                   rendererImpl,
                   {"F0VisualLOD"});
    expectNoTokens("PianoRollRenderer.cpp no f0LODCache_",
                   rendererImpl,
                   {"f0LODCache_"});
    expectNoTokens("PianoRollRenderer.cpp no correctedF0Scratch_",
                   rendererImpl,
                   {"correctedF0Scratch_"});
    expectNoTokens("PianoRollRenderer.cpp no thread_local",
                   rendererImpl,
                   {"thread_local"});

    const auto drawF0Curve = extractFunctionBlock(
        rendererImpl,
        "void PianoRollRenderer::drawF0Curve");

    expect(!drawF0Curve.empty(), "drawF0Curve must be found");

    expectNoTokens("drawF0Curve no std::vector<float> correctedF0",
                   drawF0Curve,
                   {"std::vector<float> correctedF0"});
    expectTokens("drawF0Curve uses forEachCorrectionF0Span",
                 drawF0Curve,
                 {"forEachCorrectionF0Span"});

    const auto toolHandlerImpl = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    expectNoTokens("PianoRollToolHandler.cpp no correctedF0",
                   toolHandlerImpl,
                   {"correctedF0"});
    expectTokens("PianoRollToolHandler.cpp uses forEachCorrectionF0Span",
                 toolHandlerImpl,
                 {"forEachCorrectionF0Span"});

    const auto hitTestF0Curve = extractFunctionBlock(
        toolHandlerImpl,
        "bool PianoRollToolHandler::hitTestF0Curve");
    expect(!hitTestF0Curve.empty(), "hitTestF0Curve must be found");
    expectTokens("hitTestF0Curve uses forEachCorrectionF0Span",
                 hitTestF0Curve,
                 {"forEachCorrectionF0Span"});
    expect(inOrder(hitTestF0Curve,
                   {"testCandidate(frame, originalF0[", "testCandidate(frame, data[i])"}),
           "hitTestF0Curve must test originalF0 first, then correction data");
    expectNoTokens("PianoRollToolHandler.cpp no ctx_.getViewMapper().freqToY hot path",
                   toolHandlerImpl,
                   {"ctx_.getViewMapper().freqToY"});
}

} // namespace

int main()
{
    std::cout << "=== OpenTune Architecture Tests ===\n\n";

    try {
        noteAndPitchRevisionPollingIsIndependent();
        notePatchCommitReturnsAuthoritativeSnapshot();
        noLegacyNoteInteractionState();

        // Arrangement selection publication contract
        selectedFieldsOnlyWrittenInCommitHelpers();
        placementSelectionChangedOnlyInCommitHelpers();
        setMutatingHelpersArePure();
        moveBranchHasNoBareRepaint();
        togglePlacementSelectionAllowsLastItemToggle();
        commitEmptyDoesNotDestroyPerTrackMemory();
        perTrackSelectionMemorySurvivesEmptyClick();
        noSelectPlacementOutsideCommitHelpers();
        selectPlacementPreservesPerTrackMemory();

        // Capture audio buffer → identity TimeGrid contract (VST3 sync with Standalone fix eaf3bf7)
        captureApplyAudioBufferContractIsReferenceWithIdentityTimeGrid();
        captureCallSitesDereferenceNotNullSharedPtr();

        // PianoRoll direct-paint and ruler control contracts
        drawTimeRulerHasRulerPaintBounds();
        noArrangementClipExclusionLeakedToPianoRoll();
        f0CurvesStayFullyOpaque();
        pianoRollPaintRestoresOpaqueFillAfterShadow();
        pianoRollRetainedSurfaceArchitecture();
        pianoRollZoomHandlersAvoidBusinessImageAllocation();
        pianoRollZoomHandlersDoNotPauseAutoFollow();
        absoluteTimelineTimeRemainsUnquantized();

        // PitchCurve F0 span API replaces old render path
        pitchCurveF0SpanApiReplacesOldRenderPath();

        // ARA TimeGrid 计划的结构契约
        araAudioModificationStructureHasTimeGridPlan();
        araDocumentControllerRestoresTimeGridPlanFromHandles();
        pianoRollRendererContentRenderItemHasTimeGridNoActiveProjection();
        pianoRollComponentBuildContentRenderItemReturnsOptional();
        pianoRollToolHandlerHasGetActiveContentTimeGrid();

        // Phase 4: ARA TimeGrid 行为契约测试 (§9.1/§9.2)
        araAttachSourceCreatesIdentityGridWithSourceWindowDuration();
        araSnapshotContentCopiesTimeGridOrReturnsNull();
        araReadContentSnapshotReturnsNullWithoutContent();
        araBirthContentForModificationDoesNotTouchTimeGrid();
        araRestoreUsesMakeFromHandlesWithoutIdentityFallback();
        araRestoreFilterMissContinuesWithoutModifyingTarget();
        araRestoreFailReturnsFalseBeforePendingCommit();
        araWillDestroyRemovesCRSPlaybackRegionsByHostPointer();
        araEnsureAudioModificationCreatesNewWrapperPerHostModification();
        araRendererItemHasOwnTimeGridNoGlobalSnapshot();
        araBuildContentRenderItemReturnsNulloptWithoutSnapshotOrTimeGrid();
        araSetReferenceOverlayUsesMatchedSnapshotTimeGrid();
        araPianoRollSourceOutputUsesItemTimeGridAndToolHandlerUsesCallback();
        araStaticResidualChecksForTimeGridOwnershipPlan();
    } catch (const std::exception& e) {
        ++failures;
        std::cout << "[FAIL] uncaught exception: " << e.what() << "\n";
    }

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "ALL ARCHITECTURE TESTS PASSED\n";
        return 0;
    }

    std::cout << failures << " ARCHITECTURE TEST(S) FAILED\n";
    return 1;
}
