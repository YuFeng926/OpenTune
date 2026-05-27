/**
 * Tests/TestReferenceAnalysisService.cpp - L3 tests for
 * ReferenceAnalysisService lifecycle, listener registration,
 * and cancel flow.
 *
 * Covers:
 *   - Construction/destruction (worker thread cleanup)
 *   - Listener add/remove during submit/cancel (no crash, no dangling pointer)
 *   - Cancel prevents stuck jobs
 *   - Failed ReferenceFeatureSet routes to analysisFailed
 *
 * Note: default callback delivery uses juce::MessageManager::callAsync().
 * Tests that need deterministic callback observation inject an inline
 * dispatcher via setNotificationDispatcher().
 *
 * Suite aggregator: runReferenceAnalysisServiceSuite()
 */
#include "TestSupport.h"
#include "Services/ReferenceAnalysisService.h"

#include <atomic>
#include <thread>

namespace {

ReferenceFeatureSet makeReadyFeatures(int64_t fingerprint = 0)
{
    ReferenceFeatureSet result;
    result.status = ReferenceFeatureStatus::Ready;
    result.producer = ReferenceFeatureProducer::Basic;
    result.analysisRevision = 1;
    result.inputFingerprint = fingerprint;
    result.sourceDurationSeconds = 1.0;
    return result;
}

struct TestListener : public ReferenceAnalysisService::Listener {
    std::atomic<int> completedCount{0};
    std::atomic<int> failedCount{0};

    void analysisCompleted(uint64_t, const ReferenceFeatureSet&) override
    {
        ++completedCount;
    }

    void analysisFailed(uint64_t, const juce::String&) override
    {
        ++failedCount;
    }
};

} // namespace

void runReferenceAnalysisService_Lifecycle()
{
    constexpr const char* testName = "ReferenceAnalysisService_Lifecycle";

    ReferenceAnalysisService service;
    logPass(testName);
}

void runReferenceAnalysisService_ListenerRegistration()
{
    constexpr const char* testName = "ReferenceAnalysisService_ListenerRegistration";

    ReferenceAnalysisService service;
    service.setAnalysisFunc([](const ReferenceAnalysisService::AnalysisJobKey&) {
        return makeReadyFeatures();
    });

    TestListener listener;
    service.addListener(&listener);
    service.submitAnalysis(/*materializationId=*/42, /*renderRevision=*/1);
    service.removeListener(&listener);

    service.submitAnalysis(/*materializationId=*/43, /*renderRevision=*/1);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    logPass(testName);
}

void runReferenceAnalysisService_CancelSafety()
{
    constexpr const char* testName = "ReferenceAnalysisService_CancelSafety";

    ReferenceAnalysisService service;
    service.setAnalysisFunc([](const ReferenceAnalysisService::AnalysisJobKey& jobKey) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return makeReadyFeatures(jobKey.renderRevision);
    });

    TestListener listener;
    service.addListener(&listener);
    service.submitAnalysis(/*materializationId=*/99, /*renderRevision=*/1);
    service.cancelAll();

    service.submitAnalysis(/*materializationId=*/100, /*renderRevision=*/1);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    service.removeListener(&listener);
    logPass(testName);
}

void runReferenceAnalysisService_FailedStateRoutesToFailedCallback()
{
    constexpr const char* testName = "ReferenceAnalysisService_FailedStateRoutesToFailedCallback";

    ReferenceAnalysisService service;
    service.setNotificationDispatcher([](std::function<void()> task) {
        task();
    });
    service.setAnalysisFunc([](const ReferenceAnalysisService::AnalysisJobKey& jobKey) {
        ReferenceFeatureSet result;
        result.status = ReferenceFeatureStatus::Failed;
        result.inputFingerprint = jobKey.renderRevision;
        result.errorMessage = "synthetic failure";
        return result;
    });

    TestListener listener;
    service.addListener(&listener);
    service.submitAnalysis(/*materializationId=*/101, /*renderRevision=*/7);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < deadline
           && listener.failedCount.load() == 0
           && listener.completedCount.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    service.removeListener(&listener);
    if (listener.completedCount.load() != 0 || listener.failedCount.load() != 1) {
        logFail(testName, "Failed ReferenceFeatureSet should notify analysisFailed exactly once");
        return;
    }

    logPass(testName);
}

void runReferenceAnalysisServiceSuite()
{
    logSection("ReferenceAnalysisService");
    runReferenceAnalysisService_Lifecycle();
    runReferenceAnalysisService_ListenerRegistration();
    runReferenceAnalysisService_CancelSafety();
    runReferenceAnalysisService_FailedStateRoutesToFailedCallback();
}
