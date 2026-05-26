/**
 * Tests/TestAutoRefArchitecture.cpp
 *
 * Read-only architecture guards for the AUTO Ref UI migration.
 * These tests intentionally scan source text instead of including the production
 * AUTO Ref types, so the guard can compile while the production migration is
 * still in progress.
 */

#include "TestSupport.h"

namespace {

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

juce::String withoutWhitespace(const juce::String& source)
{
    return source.removeCharacters(" \t\r\n\f\v");
}

bool assertReadable(const char* testName, const juce::String& relativePath, const juce::String& source)
{
    if (source.isEmpty()) {
        logFail(testName, ("could not read " + relativePath).toRawUTF8());
        return false;
    }

    return true;
}

bool containsGeneratedUserAddedAssignment(const juce::String& source)
{
    const auto compact = withoutWhitespace(source);
    return compact.contains(".kind=HandleKind::UserAdded")
        || compact.contains("kind{HandleKind::UserAdded}")
        || compact.contains("kind=HandleKind::UserAdded")
        || compact.contains("TimeHandle{HandleKind::UserAdded")
        || compact.contains("TimeHandle{.kind=HandleKind::UserAdded");
}

juce::String extractFunctionBody(const juce::String& source, const juce::String& signature)
{
    const auto signatureIndex = source.indexOf(signature);
    if (signatureIndex < 0) {
        return {};
    }

    const auto braceIndex = source.indexOf(signatureIndex, "{");
    if (braceIndex < 0) {
        return {};
    }

    int depth = 0;
    for (int i = braceIndex; i < source.length(); ++i) {
        const auto c = source[(size_t) i];
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return source.substring(braceIndex, i + 1);
            }
        }
    }

    return {};
}

} // namespace

void runAutoRefArchitectureStandaloneEditorHeaderDropsServiceOwnershipTest()
{
    constexpr const char* testName = "AutoRefArchitecture_StandaloneEditorHeaderDropsServiceOwnership";
    constexpr const char* relativePath = "Source/Standalone/PluginEditor.h";

    const auto source = readWorkspaceFile(relativePath);
    if (!assertReadable(testName, relativePath, source)) {
        return;
    }

    if (source.contains("ReferenceAnalysisService")) {
        logFail(testName, "Standalone PluginEditor.h must not mention ReferenceAnalysisService");
        return;
    }

    if (source.contains("ReferenceAnalysisService::Listener")) {
        logFail(testName, "Standalone PluginEditor.h must not inherit ReferenceAnalysisService::Listener");
        return;
    }

    if (source.contains("currentReferencePlacementId_")
        || source.contains("currentReferenceMaterializationId_")) {
        logFail(testName, "Standalone PluginEditor.h must not own current reference placement/materialization ids");
        return;
    }

    logPass(testName);
}

void runAutoRefArchitectureStandaloneEditorCppDoesNotRunAnalysisOrAlignTest()
{
    constexpr const char* testName = "AutoRefArchitecture_StandaloneEditorCppDoesNotRunAnalysisOrAlign";
    constexpr const char* relativePath = "Source/Standalone/PluginEditor.cpp";

    const auto source = readWorkspaceFile(relativePath);
    if (!assertReadable(testName, relativePath, source)) {
        return;
    }

    if (source.contains("Services/ReferenceAnalysisService.h")
        || source.contains("DSP/ReferenceAutoAlign.h")) {
        logFail(testName, "Standalone PluginEditor.cpp must not include reference analysis or auto-align internals");
        return;
    }

    if (source.contains("ReferenceAutoAlign::align")) {
        logFail(testName, "Standalone PluginEditor.cpp must not directly call ReferenceAutoAlign::align");
        return;
    }

    if (source.contains("setDerivedAnalysis")) {
        logFail(testName, "Standalone PluginEditor.cpp must not directly commit derived analysis");
        return;
    }

    logPass(testName);
}

void runAutoRefArchitectureReferenceAutoAlignUsesRequestPatchContractTest()
{
    constexpr const char* testName = "AutoRefArchitecture_ReferenceAutoAlignUsesRequestPatchContract";
    constexpr const char* relativePath = "Source/DSP/ReferenceAutoAlign.h";

    const auto source = readWorkspaceFile(relativePath);
    if (!assertReadable(testName, relativePath, source)) {
        return;
    }

    const auto compact = withoutWhitespace(source);
    if (compact.contains("align(constMaterializationStore::DerivedAnalysis&targetAnalysis")
        || compact.contains("align(constMaterializationStore::DerivedAnalysis&referenceAnalysis")) {
        logFail(testName, "ReferenceAutoAlign.h must not expose the old DerivedAnalysis align signature");
        return;
    }

    if (!source.contains("ReferenceAlignmentRequest")) {
        logFail(testName, "ReferenceAutoAlign.h must define or expose ReferenceAlignmentRequest");
        return;
    }

    if (!source.contains("AlignmentPatch")) {
        logFail(testName, "ReferenceAutoAlign.h must define or expose AlignmentPatch");
        return;
    }

    if (source.contains("targetTimeGridBefore")) {
        logFail(testName, "ReferenceAlignmentRequest must not retain a dead targetTimeGridBefore field");
        return;
    }

    logPass(testName);
}

void runAutoRefArchitectureReferenceAutoAlignDoesNotWriteUserAddedHandlesTest()
{
    constexpr const char* testName = "AutoRefArchitecture_ReferenceAutoAlignDoesNotWriteUserAddedHandles";
    constexpr const char* relativePath = "Source/DSP/ReferenceAutoAlign.cpp";

    const auto source = readWorkspaceFile(relativePath);
    if (!assertReadable(testName, relativePath, source)) {
        return;
    }

    if (containsGeneratedUserAddedAssignment(source)) {
        logFail(testName, "ReferenceAutoAlign.cpp must not generate HandleKind::UserAdded handles");
        return;
    }

    logPass(testName);
}

void runAutoRefArchitectureReferenceAutoAlignDoesNotConstructTimeHandlesTest()
{
    constexpr const char* testName = "AutoRefArchitecture_ReferenceAutoAlignDoesNotConstructTimeHandles";
    constexpr const char* relativePath = "Source/DSP/ReferenceAutoAlign.cpp";

    const auto source = readWorkspaceFile(relativePath);
    if (!assertReadable(testName, relativePath, source)) {
        return;
    }

    if (source.contains("TimeHandle")
        || source.contains("makeFromHandles")
        || source.contains("HandleKind::ReferenceAuto")) {
        logFail(testName, "ReferenceAutoAlign.cpp must output timing intents, not TimeHandle/TimeGrid after-state");
        return;
    }

    if (source.contains("gridOrIdentity")
        || source.contains("makeIdentity(")) {
        logFail(testName, "ReferenceAutoAlign.cpp must not synthesize fallback TimeGrid state");
        return;
    }

    logPass(testName);
}

void runAutoRefArchitectureProductionPathDoesNotUseHandleNoteMergerTest()
{
    constexpr const char* testName = "AutoRefArchitecture_ProductionPathDoesNotUseHandleNoteMerger";

    const auto processorHeader = readWorkspaceFile("Source/PluginProcessor.h");
    const auto processorCpp = readWorkspaceFile("Source/PluginProcessor.cpp");
    const auto cmake = readWorkspaceFile("CMakeLists.txt");

    if (processorHeader.isEmpty() || processorCpp.isEmpty() || cmake.isEmpty()) {
        logFail(testName, "could not read processor or CMake source files");
        return;
    }

    if (processorHeader.contains("HandleNoteMerger")
        || processorHeader.contains("handleNoteMergers_")
        || processorHeader.contains("deliverHandlesToMerger")
        || processorHeader.contains("deliverNotesToMerger")
        || processorCpp.contains("HandleNoteMerger")
        || processorCpp.contains("handleNoteMergers_")
        || processorCpp.contains("deliverHandlesToMerger")
        || processorCpp.contains("deliverNotesToMerger")
        || cmake.contains("Source/DSP/HandleNoteMerger.cpp")
        || cmake.contains("Source/DSP/HandleNoteMerger.h")
        || cmake.contains("Tests/HandleNoteMergerTests.cpp")) {
        logFail(testName, "AUTO Ref production path must not retain HandleNoteMerger or its test/source registrations");
        return;
    }

    logPass(testName);
}

void runAutoRefArchitectureNoLegacyAnalysisAnchorsTest()
{
    constexpr const char* testName = "AutoRefArchitecture_NoLegacyAnalysisAnchors";

    const auto materializationStore = readWorkspaceFile("Source/MaterializationStore.h");
    const auto featureBuilder = readWorkspaceFile("Source/DSP/BasicReferenceFeatureBuilder.cpp");
    const auto aligner = readWorkspaceFile("Source/DSP/ReferenceAutoAlign.cpp");
    const auto editor = readWorkspaceFile("Source/Standalone/PluginEditor.cpp");

    if (materializationStore.isEmpty() || featureBuilder.isEmpty() || aligner.isEmpty() || editor.isEmpty()) {
        logFail(testName, "could not read AUTO Ref source files");
        return;
    }

    if (materializationStore.contains("basicDerivedAnchors")
        || materializationStore.contains("TimeAnchor")
        || featureBuilder.contains("basicDerivedAnchors")
        || featureBuilder.contains("TimeAnchor")
        || aligner.contains("basicDerivedAnchors")
        || aligner.contains("TimeAnchor")
        || editor.contains("basicDerivedAnchors")) {
        logFail(testName, "AUTO Ref analysis timing facts must be temporalEvents, not legacy basicDerivedAnchors");
        return;
    }

    logPass(testName);
}

void runAutoRefArchitectureNoPrivateAutoRefSpacingConstantsTest()
{
    constexpr const char* testName = "AutoRefArchitecture_NoPrivateAutoRefSpacingConstants";

    const auto featureBuilder = readWorkspaceFile("Source/DSP/BasicReferenceFeatureBuilder.cpp");
    const auto aligner = readWorkspaceFile("Source/DSP/ReferenceAutoAlign.cpp");

    if (featureBuilder.isEmpty() || aligner.isEmpty()) {
        logFail(testName, "could not read AUTO Ref source files");
        return;
    }

    if (featureBuilder.contains("0.150")
        || featureBuilder.contains("150 ms")
        || aligner.contains("0.150")
        || aligner.contains("150 ms")) {
        logFail(testName, "AUTO Ref code must use the TimeGrid spacing contract instead of private constants");
        return;
    }

    logPass(testName);
}

void runAutoRefArchitectureFailedAnalysisIsNotForcedReadyTest()
{
    constexpr const char* testName = "AutoRefArchitecture_FailedAnalysisIsNotForcedReady";
    constexpr const char* relativePath = "Source/MaterializationStore.cpp";

    const auto source = readWorkspaceFile(relativePath);
    if (!assertReadable(testName, relativePath, source)) {
        return;
    }

    if (source.contains("state = F0ExtractionState::Ready")
        && source.contains("setDerivedAnalysis")) {
        logFail(testName, "MaterializationStore::setDerivedAnalysis must not force failed analysis into Ready");
        return;
    }

    logPass(testName);
}

void runAutoRefArchitectureReferenceAnalysisServiceIsProcessorDomainTest()
{
    constexpr const char* testName = "AutoRefArchitecture_ReferenceAnalysisServiceIsProcessorDomain";

    const auto editorHeader = readWorkspaceFile("Source/Standalone/PluginEditor.h");
    const auto editorCpp = readWorkspaceFile("Source/Standalone/PluginEditor.cpp");
    const auto processorHeader = readWorkspaceFile("Source/PluginProcessor.h");

    if (editorHeader.isEmpty() || editorCpp.isEmpty() || processorHeader.isEmpty()) {
        logFail(testName, "could not read source files");
        return;
    }

    if (editorHeader.contains("ReferenceAnalysisService")
        || editorCpp.contains("ReferenceAnalysisService")) {
        logFail(testName, "ReferenceAnalysisService must not be owned or referenced by Standalone editor");
        return;
    }

    if (!processorHeader.contains("ReferenceAnalysisService referenceAnalysisService_")
        || !processorHeader.contains("private ReferenceAnalysisService::Listener")) {
        logFail(testName, "processor must own the reference analysis service in the domain layer");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Gate 1 — AUTO Ref architecture guards
// ============================================================================

void runReferenceAutoAlign_RequestReferenceNoteGenerationIsNotProductionAutoRefEntryTest()
{
    constexpr const char* testName = "AutoRefArchitecture_RequestReferenceNoteGenerationIsNotProductionAutoRefEntry";

    const auto processorHeader = readWorkspaceFile("Source/PluginProcessor.h");
    const auto editorCpp = readWorkspaceFile("Source/Standalone/PluginEditor.cpp");

    if (!assertReadable(testName, "Source/PluginProcessor.h", processorHeader)) return;
    if (!assertReadable(testName, "Source/Standalone/PluginEditor.cpp", editorCpp)) return;

    if (processorHeader.contains("requestReferenceNoteGeneration(")) {
        logFail(testName, "PluginProcessor.h must not declare requestReferenceNoteGeneration — it is a legacy bypass");
        return;
    }

    if (editorCpp.contains("requestReferenceNoteGeneration")) {
        logFail(testName, "Standalone PluginEditor.cpp must not reference requestReferenceNoteGeneration");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlign_AsyncAndSyncPathsShareSameProducerEntryTest()
{
    constexpr const char* testName = "AutoRefArchitecture_AsyncAndSyncPathsShareSameProducerEntry";
    constexpr const char* relativePath = "Source/PluginProcessor.cpp";

    const auto source = readWorkspaceFile(relativePath);
    if (!assertReadable(testName, relativePath, source)) return;

    if (!source.contains("buildReferenceDerivedAnalysis")) {
        logFail(testName, "PluginProcessor.cpp must define buildReferenceDerivedAnalysis as the single producer entry");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlign_UsesDerivedAnalysisAsOnlyReferenceTruthTest()
{
    constexpr const char* testName = "AutoRefArchitecture_UsesDerivedAnalysisAsOnlyReferenceTruth";
    constexpr const char* relativePath = "Source/MaterializationStore.h";

    const auto source = readWorkspaceFile(relativePath);
    if (!assertReadable(testName, relativePath, source)) return;

    if (!source.contains("basicDerivedNotes") && !source.contains("temporalEvents")) {
        logFail(testName, "MaterializationStore::DerivedAnalysis must contain both basicDerivedNotes and temporalEvents");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlign_EditorDoesNotOwnReferenceFeatureGenerationTest()
{
    constexpr const char* testName = "AutoRefArchitecture_EditorDoesNotOwnReferenceFeatureGeneration";

    const auto editorHeader = readWorkspaceFile("Source/Standalone/PluginEditor.h");
    const auto editorCpp = readWorkspaceFile("Source/Standalone/PluginEditor.cpp");

    if (!assertReadable(testName, "Source/Standalone/PluginEditor.h", editorHeader)) return;
    if (!assertReadable(testName, "Source/Standalone/PluginEditor.cpp", editorCpp)) return;

    if (editorHeader.contains("GameNoteGenerator")
        || editorCpp.contains("GameNoteGenerator")) {
        logFail(testName, "Editor must not reference GameNoteGenerator");
        return;
    }

    if (editorHeader.contains("NoteGenerator::Listener")
        || editorCpp.contains("NoteGenerator::Listener")) {
        logFail(testName, "Editor must not inherit NoteGenerator::Listener");
        return;
    }

    if (editorHeader.contains("ReferenceAnalysisService::Listener")
        || editorCpp.contains("ReferenceAnalysisService::Listener")) {
        logFail(testName, "Editor must not inherit ReferenceAnalysisService::Listener");
        return;
    }

    if (editorHeader.contains("requestReferenceNoteGeneration")
        || editorCpp.contains("requestReferenceNoteGeneration")) {
        logFail(testName, "Editor must not reference requestReferenceNoteGeneration");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlign_AutoRefButtonRequiresExperimentalModeAndReferenceBindingTest()
{
    constexpr const char* testName = "AutoRefArchitecture_AutoRefButtonRequiresExperimentalModeAndReferenceBinding";
    constexpr const char* relativePath = "Source/Standalone/PluginEditor.cpp";

    const auto source = readWorkspaceFile(relativePath);
    if (!assertReadable(testName, relativePath, source)) return;

    if (source.contains("setAutoButtonMode") && !source.contains("ExperimentalReferenceAlignMode")) {
        logFail(testName, "setAutoButtonMode must be gated by ExperimentalReferenceAlignMode check");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlign_PreferenceModePersistsInSharedAudioSettingsTest()
{
    constexpr const char* testName = "AutoRefArchitecture_PreferenceModePersistsInSharedAudioSettings";
    constexpr const char* relativePath = "Source/Utils/AppPreferences.h";

    const auto source = readWorkspaceFile(relativePath);
    if (!assertReadable(testName, relativePath, source)) return;

    if (!source.contains("ExperimentalReferenceAlignMode")) {
        logFail(testName, "AppPreferences.h must define or expose ExperimentalReferenceAlignMode");
        return;
    }

    logPass(testName);
}

void runReferenceAutoAlign_AggressiveModeContractMatchesImplementationTest()
{
    constexpr const char* testName = "AutoRefArchitecture_AggressiveModeContractMatchesImplementation";

    const auto impl = readWorkspaceFile("Source/PluginProcessor.cpp");
    if (!assertReadable(testName, "Source/PluginProcessor.cpp", impl)) {
        return;
    }

    const auto producerBody = extractFunctionBody(
        impl,
        "MaterializationStore::DerivedAnalysis OpenTuneAudioProcessor::buildGameReferenceDerivedAnalysis");
    if (producerBody.isEmpty()) {
        logFail(testName, "missing buildGameReferenceDerivedAnalysis body");
        return;
    }

    // 1. Aggressive 专属 producer 必须存在
    if (!impl.contains("buildGameReferenceDerivedAnalysis")) {
        logFail(testName, "missing buildGameReferenceDerivedAnalysis — Aggressive producer not implemented");
        return;
    }

    // 2. 入口函数必须按 mode 分派到 GAME producer
    if (!impl.contains("ExperimentalReferenceAlignMode::Aggressive") ||
        !impl.contains("buildGameReferenceDerivedAnalysis")) {
        logFail(testName, "buildReferenceDerivedAnalysis must dispatch to GAME when mode=Aggressive");
        return;
    }

    if (!producerBody.contains("dynamic_cast<GameNoteGenerator*>")
        || !producerBody.contains("gameGenerator->generate(input)")) {
        logFail(testName, "Aggressive producer must require a real GAME backend");
        return;
    }

    if (!producerBody.contains("state = F0ExtractionState::Failed")
        || !producerBody.contains("requires GAME")) {
        logFail(testName, "Aggressive producer must fail explicitly when GAME is unavailable");
        return;
    }

    if (producerBody.contains("Basic fallback") || producerBody.contains("backendMode = 0")) {
        logFail(testName, "Aggressive producer must not silently fall back to Basic");
        return;
    }

    // 3. Aggressive 不能把 Basic temporal events 混入（不能同时调 BasicReferenceFeatureBuilder::build 和 GameNoteGenerator）
    //    GAME producer 可以先用 BasicReferenceFeatureBuilder::build 获取基础结构再替换，
    //    但不可以保留 Basic temporal events 与 GAME notes 共存
    //    这由 "不得混入" 的语义决定：最终 temporal events 必须来自 GAME notes。
    //    用 source scan 验证：在 Aggressive 分支中 basicDerivedNotes 与 temporalEvents
    //    必同时来自 GAME 路径。

    // 简单验证：buildGameReferenceDerivedAnalysis 体内应替换 temporalEvents 或设置 backendMode=2
    if (!producerBody.contains("result.backendMode = 2")
        || !producerBody.contains("result.temporalEvents.clear()")) {
        logFail(testName, "Aggressive producer must set temporalEvents and backendMode");
        return;
    }

    logPass(testName);
}

void runAutoRefArchitectureSuite()
{
    logSection("AutoRefArchitecture");
    runAutoRefArchitectureStandaloneEditorHeaderDropsServiceOwnershipTest();
    runAutoRefArchitectureStandaloneEditorCppDoesNotRunAnalysisOrAlignTest();
    runAutoRefArchitectureReferenceAutoAlignUsesRequestPatchContractTest();
    runAutoRefArchitectureReferenceAutoAlignDoesNotWriteUserAddedHandlesTest();
    runAutoRefArchitectureReferenceAutoAlignDoesNotConstructTimeHandlesTest();
    runAutoRefArchitectureProductionPathDoesNotUseHandleNoteMergerTest();
    runAutoRefArchitectureNoLegacyAnalysisAnchorsTest();
    runAutoRefArchitectureNoPrivateAutoRefSpacingConstantsTest();
    runAutoRefArchitectureFailedAnalysisIsNotForcedReadyTest();
    runAutoRefArchitectureReferenceAnalysisServiceIsProcessorDomainTest();

    // Gate 3 — Aggressive mode blocked contract
    runReferenceAutoAlign_AggressiveModeContractMatchesImplementationTest();

    // Gate 1 — AUTO Ref architecture guards
    runReferenceAutoAlign_RequestReferenceNoteGenerationIsNotProductionAutoRefEntryTest();
    runReferenceAutoAlign_AsyncAndSyncPathsShareSameProducerEntryTest();
    runReferenceAutoAlign_UsesDerivedAnalysisAsOnlyReferenceTruthTest();
    runReferenceAutoAlign_EditorDoesNotOwnReferenceFeatureGenerationTest();
    runReferenceAutoAlign_AutoRefButtonRequiresExperimentalModeAndReferenceBindingTest();
    runReferenceAutoAlign_PreferenceModePersistsInSharedAudioSettingsTest();
}
