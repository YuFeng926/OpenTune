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

void commitEmptyClearsModelSelection()
{
    const auto src = readText("Source/Standalone/UI/ArrangementViewComponent.cpp");
    const auto emptyBody = extractFunctionBlock(src, "void ArrangementViewComponent::commitEmptyPlacementSelection");

    expect(!emptyBody.empty(), "commitEmptyPlacementSelection must be found");
    expectTokens("commitEmptyPlacementSelection", emptyBody,
                 {"clearAllSelections()"});
    expect(!contains(emptyBody, "selectPlacement(previousTrack"),
            "commitEmptyPlacementSelection must use clearAllSelections() not per-track selectPlacement");
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

void selectPlacementClearsOtherTracks()
{
    const auto src = readText("Source/StandaloneArrangement.cpp");
    const auto selectBody = extractFunctionBlock(src, "bool StandaloneArrangement::selectPlacement");
    const auto setIndexBody = extractFunctionBlock(src, "bool StandaloneArrangement::setSelectedPlacementIndex");

    expect(!selectBody.empty(), "StandaloneArrangement::selectPlacement must be found");
    expect(!setIndexBody.empty(), "StandaloneArrangement::setSelectedPlacementIndex must be found");

    // Both write paths must clear other tracks' selectedPlacementId before setting
    for (const auto* body : { &selectBody, &setIndexBody }) {
        expect(contains(*body, "kTrackCount"),
               "Must iterate all tracks to clear other selections");
        expect(contains(*body, "!= trackId"),
               "Must skip the target track when clearing");
    }
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

void pianoRollUsesDirectPaintingWithoutRetainedPixelCache()
{
    const auto componentHeader = readText("Source/Standalone/UI/PianoRollComponent.h");
    const auto componentImpl = readText("Source/Standalone/UI/PianoRollComponent.cpp");

    expectNoTokens("Piano Roll has no retained pixel cache",
                   componentHeader + componentImpl,
                   {"TimelineCompositeCache", "compositeCache_", "viewportSurface_",
                    "pianoKeySurface_", "moveImageSection", "scrollViewportSurfaceTo",
                    "rebuildViewportSurfaceFromReadyTiles"});
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

    expect(!verticalZoom.empty(), "vertical zoom handler must be found");
    expect(!horizontalZoom.empty(), "horizontal zoom handler must be found");

    // Zoom must set userHasManuallyZoomed_ but NOT pause CONT follow via userScrollHold_
    expectTokens("vertical zoom keeps manual zoom flag",
                 verticalZoom,
                 {"userHasManuallyZoomed_ = true"});
    expectTokens("horizontal zoom keeps manual zoom flag",
                 horizontalZoom,
                 {"userHasManuallyZoomed_ = true"});
    expectNoTokens("vertical zoom must not pause auto-follow",
                   verticalZoom,
                   {"userScrollHold_"});
    expectNoTokens("horizontal zoom must not pause auto-follow",
                   horizontalZoom,
                   {"userScrollHold_"});
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
        commitEmptyClearsModelSelection();
        noSelectPlacementOutsideCommitHelpers();
        selectPlacementClearsOtherTracks();

        // Capture audio buffer → identity TimeGrid contract (VST3 sync with Standalone fix eaf3bf7)
        captureApplyAudioBufferContractIsReferenceWithIdentityTimeGrid();
        captureCallSitesDereferenceNotNullSharedPtr();

        // PianoRoll direct-paint and ruler control contracts
        drawTimeRulerHasRulerPaintBounds();
        noArrangementClipExclusionLeakedToPianoRoll();
        f0CurvesStayFullyOpaque();
        pianoRollUsesDirectPaintingWithoutRetainedPixelCache();
        pianoRollZoomHandlersAvoidBusinessImageAllocation();
        pianoRollZoomHandlersDoNotPauseAutoFollow();

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
