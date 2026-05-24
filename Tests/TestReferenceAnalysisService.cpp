/**
 * Tests/TestReferenceAnalysisService.cpp — L3 tests for
 * ReferenceAnalysisService lifecycle, listener registration,
 * and cancel flow.
 *
 * Covers:
 *   - Construction/destruction (worker thread cleanup)
 *   - Listener add/remove during submit/cancel (no crash, no dangling pointer)
 *   - Cancel prevents stuck jobs
 *
 * Note: Listener callback delivery uses juce::callAsync, which requires
 * message-dispatch in the test process.  Verification of actual callback
 * content is deferred to L5 manual journeys where a real message loop runs.
 *
 * Suite aggregator: runReferenceAnalysisServiceSuite()
 */
#include "TestSupport.h"
#include "Services/ReferenceAnalysisService.h"

#include <atomic>
#include <thread>

namespace {

// A trivial analysis function that returns an empty Ready DerivedAnalysis
MaterializationStore::DerivedAnalysis trivialAnalysisFunc(
    const ReferenceAnalysisService::AnalysisJobKey& /*jobKey*/)
{
    MaterializationStore::DerivedAnalysis result;
    result.state = F0ExtractionState::Ready;
    result.analysisRevision = 1;
    result.backendMode = 0;
    result.inputFingerprint = 0;
    return result;
}

/** Mock listener that records callback invocations */
struct TestListener : public ReferenceAnalysisService::Listener {
    std::atomic<int> completedCount{0};
    std::atomic<int> failedCount{0};

    void analysisCompleted(uint64_t, const MaterializationStore::DerivedAnalysis&) override
    {
        ++completedCount;
    }

    void analysisFailed(uint64_t, const juce::String&) override
    {
        ++failedCount;
    }
};

} // namespace

// ============================================================================
// Test 1: Lifecycle
//
// Verify that ReferenceAnalysisService can be constructed and destroyed
// without hangs. The destructor must stop the internal worker thread.
// ============================================================================
void runReferenceAnalysisService_Lifecycle()
{
    constexpr const char* testName = "ReferenceAnalysisService_Lifecycle";

    // Construction + destruction: destructor must join worker without hang
    ReferenceAnalysisService service;
    // Service goes out of scope — no crash = pass

    logPass(testName);
}

// ============================================================================
// Test 2: ListenerRegistration
//
// Verify that listeners can be added and removed safely during the
// submit/cancel lifecycle without crashes or dangling references.
// The service holds no ownership of listener pointers, so the caller
// is responsible for lifetime management.
// ============================================================================
void runReferenceAnalysisService_ListenerRegistration()
{
    constexpr const char* testName = "ReferenceAnalysisService_ListenerRegistration";

    ReferenceAnalysisService service;
    service.setAnalysisFunc(trivialAnalysisFunc);

    TestListener listener;
    service.addListener(&listener);

    // Submit analysis — listener is registered during submit
    service.submitAnalysis(/*materializationId=*/42, /*renderRevision=*/1);

    // Remove listener while a job may be pending/running
    service.removeListener(&listener);

    // Submit another job without the listener — clean shutdown
    service.submitAnalysis(/*materializationId=*/43, /*renderRevision=*/1);

    // Let the worker finish: wait a bit for pending job to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Service destruction will call cancelAll and join the worker.
    // No crash = pass.

    logPass(testName);
}

// ============================================================================
// Test 3: CancelSafety
//
// Verify that cancelling a submitted analysis does not crash or leak,
// and that the cancel is safe whether the worker has picked up the
// job or not.
// ============================================================================
void runReferenceAnalysisService_CancelSafety()
{
    constexpr const char* testName = "ReferenceAnalysisService_CancelSafety";

    ReferenceAnalysisService service;
    service.setAnalysisFunc([](const ReferenceAnalysisService::AnalysisJobKey&) -> MaterializationStore::DerivedAnalysis {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        MaterializationStore::DerivedAnalysis result;
        result.state = F0ExtractionState::Ready;
        return result;
    });

    TestListener listener;
    service.addListener(&listener);

    // Submit and cancel immediately — race with worker
    service.submitAnalysis(/*materializationId=*/99, /*renderRevision=*/1);
    service.cancelAnalysis(99);

    // Submit another job and let it finish
    service.submitAnalysis(/*materializationId=*/100, /*renderRevision=*/1);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // No crash during mixed submit/cancel/teardown
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
        MaterializationStore::DerivedAnalysis result;
        result.state = F0ExtractionState::Failed;
        result.errorMessage = "synthetic failure";
        result.inputFingerprint = jobKey.renderRevision;
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
        logFail(testName, "Failed DerivedAnalysis should notify analysisFailed exactly once");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Suite entry point
// ============================================================================
void runReferenceAnalysisServiceSuite()
{
    logSection("ReferenceAnalysisService");
    runReferenceAnalysisService_Lifecycle();
    runReferenceAnalysisService_ListenerRegistration();
    runReferenceAnalysisService_CancelSafety();
    runReferenceAnalysisService_FailedStateRoutesToFailedCallback();
}
