/**
 * Tests/TestAutoRefArchitecture.cpp
 *
 * Read-only architecture guards for the AUTO(REF) reference-feature migration.
 * These tests intentionally scan source text so they can protect the contract
 * without depending on production implementation details at compile time.
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

bool assertReadable(const char* testName, const juce::String& relativePath, const juce::String& source)
{
    if (source.isEmpty()) {
        logFail(testName, ("could not read " + relativePath).toRawUTF8());
        return false;
    }

    return true;
}

bool requireContains(const char* testName,
                     const juce::String& source,
                     const juce::String& needle,
                     const char* failure)
{
    if (!source.contains(needle)) {
        logFail(testName, failure);
        return false;
    }

    return true;
}

} // namespace

void runAutoRefArchitectureReferenceAutoAlignRequestUsesFeatureSetAndTimeMapsTest()
{
    constexpr const char* testName =
        "AutoRefArchitecture_ReferenceAutoAlignRequestUsesFeatureSetAndTimeMaps";
    constexpr const char* relativePath = "Source/DSP/ReferenceAutoAlign.h";

    const auto source = readWorkspaceFile(relativePath);
    if (!assertReadable(testName, relativePath, source)) {
        return;
    }

    if (source.contains("MaterializationStore::DerivedAnalysis")) {
        logFail(testName, "ReferenceAutoAlign.h must not expose the removed DerivedAnalysis contract");
        return;
    }
    if (!requireContains(testName,
                         source,
                         "EffectiveTimeMap targetTimeMap;",
                         "ReferenceAlignmentRequest must carry target EffectiveTimeMap")) {
        return;
    }
    if (!requireContains(testName,
                         source,
                         "EffectiveTimeMap referenceTimeMap;",
                         "ReferenceAlignmentRequest must carry reference EffectiveTimeMap")) {
        return;
    }
    if (!requireContains(testName,
                         source,
                         "ReferenceFeatureSet targetFeatures;",
                         "ReferenceAlignmentRequest must carry target ReferenceFeatureSet")) {
        return;
    }
    if (!requireContains(testName,
                         source,
                         "ReferenceFeatureSet referenceFeatures;",
                         "ReferenceAlignmentRequest must carry reference ReferenceFeatureSet")) {
        return;
    }
    if (!requireContains(testName,
                         source,
                         "std::shared_ptr<const TimeGridSnapshot> targetTimeGridBefore;",
                         "ReferenceAlignmentRequest must retain targetTimeGridBefore for TimeGrid patching")) {
        return;
    }

    logPass(testName);
}

void runAutoRefArchitectureProcessorThreadsFeatureBuildersAndTimeMapsTest()
{
    constexpr const char* testName = "AutoRefArchitecture_ProcessorThreadsFeatureBuildersAndTimeMaps";

    const auto header = readWorkspaceFile("Source/PluginProcessor.h");
    const auto impl = readWorkspaceFile("Source/PluginProcessor.cpp");

    if (!assertReadable(testName, "Source/PluginProcessor.h", header)
        || !assertReadable(testName, "Source/PluginProcessor.cpp", impl)) {
        return;
    }

    if (!requireContains(testName,
                         header,
                         "ReferenceFeatureSet buildReferenceFeatureSet(",
                         "PluginProcessor.h must expose buildReferenceFeatureSet")) {
        return;
    }
    if (!requireContains(testName,
                         header,
                         "ReferenceFeatureSet buildBasicReferenceFeatureSet(",
                         "PluginProcessor.h must retain buildBasicReferenceFeatureSet fallback")) {
        return;
    }
    if (!requireContains(testName,
                         header,
                         "ReferenceFeatureSet buildGameReferenceFeatureSet(",
                         "PluginProcessor.h must retain buildGameReferenceFeatureSet producer")) {
        return;
    }
    if (!requireContains(testName,
                         impl,
                         "ReferenceFeatureProducer::Game",
                         "PluginProcessor.cpp must tag AUTO(REF) formal path as GAME")) {
        return;
    }
    if (!requireContains(testName,
                         impl,
                         "EffectiveTimeMap::fromTimeGrid",
                         "PluginProcessor.cpp must derive EffectiveTimeMap from TimeGrid snapshots")) {
        return;
    }
    if (!requireContains(testName,
                         impl,
                         "request.targetTimeGridBefore = oldTimeGrid;",
                         "executeReferenceAlignmentForPlacement must thread targetTimeGridBefore into the request")) {
        return;
    }

    logPass(testName);
}

void runAutoRefArchitectureEnsureTimeToolSeedReadsFeatureCacheAndFallbacksBasicTest()
{
    constexpr const char* testName =
        "AutoRefArchitecture_EnsureTimeToolSeedReadsFeatureCacheAndFallbacksBasic";
    constexpr const char* relativePath = "Source/PluginProcessor.cpp";

    const auto source = readWorkspaceFile(relativePath);
    if (!assertReadable(testName, relativePath, source)) {
        return;
    }

    if (!requireContains(testName,
                         source,
                         "getReferenceFeatures(materializationId, features)",
                         "ensureTimeToolAnchorSeed must read cached ReferenceFeatureSet first")) {
        return;
    }
    if (!requireContains(testName,
                         source,
                         "buildBasicReferenceFeatureSet(snapshot)",
                         "ensureTimeToolAnchorSeed must fallback to Basic reference features when cache is missing")) {
        return;
    }
    if (!requireContains(testName,
                         source,
                         "features.hasTimingAnchors()",
                         "ensureTimeToolAnchorSeed must require timing anchors before seeding handles")) {
        return;
    }

    logPass(testName);
}

void runAutoRefArchitectureLegacyProjectPersistenceAnalysisFieldsRemovedTest()
{
    constexpr const char* testName =
        "AutoRefArchitecture_LegacyProjectPersistenceAnalysisFieldsRemoved";

    const juce::StringArray files{
        "Source/Utils/ProjectModel.h",
        "Source/Utils/ProjectPersistence.h",
        "Source/Utils/ProjectPersistence.cpp",
        "Source/Utils/ProjectSession.cpp"
    };

    for (const auto& relativePath : files) {
        const auto source = readWorkspaceFile(relativePath);
        if (!assertReadable(testName, relativePath, source)) {
            return;
        }

        if (source.contains("basicAnalysis")
            || source.contains("enhancedAnalysis")
            || source.contains("analysisMode")) {
            logFail(testName, ("legacy analysis persistence field still present in " + relativePath).toRawUTF8());
            return;
        }
    }

    logPass(testName);
}

void runAutoRefArchitectureTimeGridPatchBuilderCarriesSpeedWindowAndRollbackTest()
{
    constexpr const char* testName =
        "AutoRefArchitecture_TimeGridPatchBuilderCarriesSpeedWindowAndRollback";
    constexpr const char* relativePath = "Source/DSP/TimeGridPatchBuilder.cpp";

    const auto source = readWorkspaceFile(relativePath);
    if (!assertReadable(testName, relativePath, source)) {
        return;
    }

    if (!requireContains(testName,
                         source,
                         "constexpr double kMinAutoSpeed = 0.8;",
                         "TimeGridPatchBuilder.cpp must define the 0.8x lower speed guard")) {
        return;
    }
    if (!requireContains(testName,
                         source,
                         "constexpr double kMaxAutoSpeed = 1.3;",
                         "TimeGridPatchBuilder.cpp must define the 1.3x upper speed guard")) {
        return;
    }
    if (!requireContains(testName,
                         source,
                         "HandleKind::ReferenceAuto",
                         "TimeGridPatchBuilder.cpp must emit ReferenceAuto handles")) {
        return;
    }
    if (!requireContains(testName,
                         source,
                         "restoreRemovedHandlesInRange",
                         "TimeGridPatchBuilder.cpp must restore prior auto handles when projection becomes infeasible")) {
        return;
    }
    if (!requireContains(testName,
                         source,
                         "0.8x~1.3x speed window",
                         "TimeGridPatchBuilder.cpp must diagnose local speed-window violations")) {
        return;
    }

    logPass(testName);
}

void runAutoRefArchitectureSuite()
{
    logSection("AutoRefArchitecture");
    runAutoRefArchitectureReferenceAutoAlignRequestUsesFeatureSetAndTimeMapsTest();
    runAutoRefArchitectureProcessorThreadsFeatureBuildersAndTimeMapsTest();
    runAutoRefArchitectureEnsureTimeToolSeedReadsFeatureCacheAndFallbacksBasicTest();
    runAutoRefArchitectureLegacyProjectPersistenceAnalysisFieldsRemovedTest();
    runAutoRefArchitectureTimeGridPatchBuilderCarriesSpeedWindowAndRollbackTest();
}
