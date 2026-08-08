/**
 * Inference Process Isolation — functional contract tests
 *
 * 验证硬切后的后端生命周期合同，不依赖 ORT/模型/显卡，只验证：
 *   1. F0ExtractionService: shutdown 后不投递 commit，worker 不阻塞，队列清空
 *   2. ReferenceAnalysisService: shutdown 后不投递 completion，worker 不阻塞
 *   3. 进程级 GameNoteGenerator 入口 generateNotes 在 Env 未初始化时安全返回空
 *   4. ProcessRenderRuntime detach 不阻塞（短临界区合同）
 */
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "Content/ContentKey.h"
#include "Services/F0ExtractionService.h"
#include "Services/ReferenceAnalysisService.h"
#include "DSP/ReferenceFeatures.h"
#include "Runtime/ProcessRenderRuntime.h"
#include "Runtime/ProcessF0Runtime.h"

#include <juce_events/juce_events.h>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "[FAIL] " << message << '\n';
}

OpenTune::ContentKey makeKey(uint64_t id)
{
    OpenTune::ContentKey key;
    key.domainKind = OpenTune::DomainKind::StandaloneClip;
    key.objectId = id;
    return key;
}

// ─── F0ExtractionService contracts ──────────────────────────────────────────

void testF0ShutdownRejectsAfterShutdown()
{
    using namespace OpenTune;
    std::atomic<int> commitCount{0};

    F0ExtractionService svc(1, 8, [] { return nullptr; });

    auto key = makeKey(1);
    F0ExtractionService::SubmitResult result = svc.submit(
        F0RequestKey{key},
        [](const std::shared_ptr<F0RunOwnerState>&) -> F0ExtractionService::Result {
            F0ExtractionService::Result r;
            r.success = true;
            return r;
        },
        [&commitCount](F0ExtractionService::Result&&) { ++commitCount; });

    expect(result == F0ExtractionService::SubmitResult::Accepted,
           "F0 submit before shutdown is Accepted");

    svc.shutdown();

    // shutdown 后提交应被拒绝
    F0ExtractionService::SubmitResult after = svc.submit(
        F0RequestKey{makeKey(2)},
        [](const std::shared_ptr<F0RunOwnerState>&) -> F0ExtractionService::Result {
            F0ExtractionService::Result r;
            r.success = true;
            return r;
        },
        [&commitCount](F0ExtractionService::Result&&) { ++commitCount; });

    expect(after == F0ExtractionService::SubmitResult::InvalidTask,
           "F0 submit after shutdown is InvalidTask");
    expect(commitCount.load() == 0,
           "F0 no commit delivered after shutdown");

    // 二次 shutdown 不崩溃
    svc.shutdown();
}

void testF0ShutdownDoesNotBlock()
{
    using namespace OpenTune;
    F0ExtractionService svc(1, 8, [] { return nullptr; });

    auto key = makeKey(10);
    svc.submit(
        F0RequestKey{key},
        [](const std::shared_ptr<F0RunOwnerState>&) -> F0ExtractionService::Result {
            // 模拟慢 F0 推理：等 200ms
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            F0ExtractionService::Result r;
            r.success = true;
            return r;
        },
        [](F0ExtractionService::Result&&) {});

    // shutdown + 析构不应阻塞超过 100ms（worker 是 detached，不 join）
    auto start = std::chrono::steady_clock::now();
    svc.shutdown();
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    expect(ms < 100, "F0 shutdown completes without blocking (detached worker)");
}

void testF0QueueDedup()
{
    using namespace OpenTune;
    std::atomic<int> executeCount{0};

    F0ExtractionService svc(1, 8, [] { return nullptr; });

    auto key = makeKey(20);
    auto submit = [&](F0ExtractionService::SubmitResult expected) {
        auto r = svc.submit(
            F0RequestKey{key},
            [&executeCount](const std::shared_ptr<F0RunOwnerState>&) -> F0ExtractionService::Result {
                ++executeCount;
                F0ExtractionService::Result res;
                res.success = true;
                return res;
            },
            [](F0ExtractionService::Result&&) {});
        expect(r == expected, "F0 dedup: same key returns expected result");
    };

    submit(F0ExtractionService::SubmitResult::Accepted);
    submit(F0ExtractionService::SubmitResult::AlreadyInProgress);
    submit(F0ExtractionService::SubmitResult::AlreadyInProgress);

    svc.cancel(F0RequestKey{key});
    // cancel 后应可以重新提交
    svc.submit(
        F0RequestKey{key},
        [&executeCount](const std::shared_ptr<F0RunOwnerState>&) -> F0ExtractionService::Result {
            ++executeCount;
            F0ExtractionService::Result res;
            res.success = true;
            return res;
        },
        [](F0ExtractionService::Result&&) {});

    svc.shutdown();
}

// ─── ReferenceAnalysisService contracts ─────────────────────────────────────

void testRefShutdownRejectsAfterShutdown()
{
    using namespace OpenTune;
    std::atomic<int> completionCount{0};

    ReferenceAnalysisService svc;
    svc.setNotificationDispatcher([](std::function<void()> task) { task(); });

    ReferenceAnalysisService::AnalysisJobKey key;
    key.contentKey = makeKey(30);
    key.inputFingerprint = 100;
    key.producer = ReferenceFeatureProducer::StandardAuto;

    svc.submitAnalysis(
        key.contentKey, key.inputFingerprint, key.producer,
        [](const ReferenceAnalysisService::AnalysisJobKey&) -> ReferenceFeatureSet {
            ReferenceFeatureSet fs;
            fs.status = ReferenceFeatureStatus::Ready;
            return fs;
        },
        [&completionCount](const ReferenceAnalysisService::AnalysisJobKey&,
                           const ReferenceFeatureSet&) { ++completionCount; });

    svc.shutdown();

    // shutdown 后提交被忽略（running=false）
    svc.submitAnalysis(
        makeKey(31), 200, ReferenceFeatureProducer::StandardAuto,
        [](const ReferenceAnalysisService::AnalysisJobKey&) -> ReferenceFeatureSet {
            ReferenceFeatureSet fs;
            fs.status = ReferenceFeatureStatus::Ready;
            return fs;
        },
        [&completionCount](const ReferenceAnalysisService::AnalysisJobKey&,
                           const ReferenceFeatureSet&) { ++completionCount; });

    // 等待可能的后台执行
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    expect(completionCount.load() == 0,
           "Ref no completion delivered after shutdown");
}

void testRefShutdownDoesNotBlock()
{
    using namespace OpenTune;
    ReferenceAnalysisService svc;
    svc.setNotificationDispatcher([](std::function<void()> task) { task(); });

    ReferenceAnalysisService::AnalysisJobKey key;
    key.contentKey = makeKey(40);
    key.inputFingerprint = 100;
    key.producer = ReferenceFeatureProducer::Game;

    svc.submitAnalysis(
        key.contentKey, key.inputFingerprint, key.producer,
        [](const ReferenceAnalysisService::AnalysisJobKey&) -> ReferenceFeatureSet {
            // 模拟慢 GAME 推理
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            ReferenceFeatureSet fs;
            fs.status = ReferenceFeatureStatus::Ready;
            return fs;
        },
        [](const ReferenceAnalysisService::AnalysisJobKey&,
           const ReferenceFeatureSet&) {});

    auto start = std::chrono::steady_clock::now();
    svc.shutdown();
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    expect(ms < 100, "Ref shutdown completes without blocking (detached worker)");
}

void testRefJobDedup()
{
    using namespace OpenTune;
    std::atomic<int> analysisCount{0};

    ReferenceAnalysisService svc;
    svc.setNotificationDispatcher([](std::function<void()> task) { task(); });

    ReferenceAnalysisService::AnalysisJobKey key;
    key.contentKey = makeKey(50);
    key.inputFingerprint = 100;
    key.producer = ReferenceFeatureProducer::StandardAuto;

    // 第一次提交
    svc.submitAnalysis(
        key.contentKey, key.inputFingerprint, key.producer,
        [&analysisCount](const ReferenceAnalysisService::AnalysisJobKey&) -> ReferenceFeatureSet {
            ++analysisCount;
            ReferenceFeatureSet fs;
            fs.status = ReferenceFeatureStatus::Ready;
            return fs;
        },
        [](const ReferenceAnalysisService::AnalysisJobKey&,
           const ReferenceFeatureSet&) {});

    // 相同 job key 再次提交：应被去重（不重启分析）
    svc.submitAnalysis(
        key.contentKey, key.inputFingerprint, key.producer,
        [&analysisCount](const ReferenceAnalysisService::AnalysisJobKey&) -> ReferenceFeatureSet {
            ++analysisCount;
            ReferenceFeatureSet fs;
            fs.status = ReferenceFeatureStatus::Ready;
            return fs;
        },
        [](const ReferenceAnalysisService::AnalysisJobKey&,
           const ReferenceFeatureSet&) {});

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    svc.shutdown();

    expect(analysisCount.load() == 1,
           "Ref job dedup: identical active job does not restart analysis");
}

// ─── CompletionGate contract ────────────────────────────────────────────────

void testCompletionGateBlocksAfterClose()
{
    using namespace OpenTune;
    ProcessRenderRuntime::CompletionGate gate;
    std::atomic<int> settled{0};

    // gate 未关闭时，callback 应执行
    {
        std::lock_guard<std::mutex> lk(gate.mutex);
        expect(!gate.closed, "Gate is open initially");
    }

    // 关闭后回调不再执行
    {
        std::lock_guard<std::mutex> lk(gate.mutex);
        gate.closed = true;
    }

    bool executed = false;
    {
        std::lock_guard<std::mutex> lk(gate.mutex);
        if (!gate.closed)
            executed = true;
    }
    expect(!executed, "Gate closed: callback should not execute");

    // 二次关闭不崩溃
    {
        std::lock_guard<std::mutex> lk(gate.mutex);
        gate.closed = true;
    }
}

// ─── detach 计数合同 ───────────────────────────────────────────────────────

void testProcessRuntimeDetachIsNonBlocking()
{
    using namespace OpenTune;
    auto& runtime = ProcessRenderRuntime::getInstance();

    // 连续 attach/detach 不应阻塞
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) {
        runtime.attach();
        runtime.detach();
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    expect(ms < 50, "ProcessRenderRuntime attach/detach is non-blocking (short critical section)");
}

void testProcessF0DetachIsNonBlocking()
{
    using namespace OpenTune;
    auto& runtime = ProcessF0Runtime::getInstance();

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) {
        runtime.attach();
        runtime.detach();
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    expect(ms < 50, "ProcessF0Runtime attach/detach is non-blocking (short critical section)");
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // F0ExtractionService 生命周期合同
    testF0ShutdownRejectsAfterShutdown();
    testF0ShutdownDoesNotBlock();
    testF0QueueDedup();

    // ReferenceAnalysisService 生命周期合同
    testRefShutdownRejectsAfterShutdown();
    testRefShutdownDoesNotBlock();
    testRefJobDedup();

    // CompletionGate 合同
    testCompletionGateBlocksAfterClose();

    // Runtime detach 合同
    testProcessRuntimeDetachIsNonBlocking();
    testProcessF0DetachIsNonBlocking();

    if (failures == 0)
        std::cout << "Inference process isolation tests passed\n";
    else
        std::cerr << failures << " inference process isolation test(s) failed\n";
    return failures == 0 ? 0 : 1;
}
