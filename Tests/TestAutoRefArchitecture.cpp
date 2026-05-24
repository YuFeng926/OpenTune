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
}
