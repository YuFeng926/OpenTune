#include "TestSupport.h"
#include "DSP/TimeGridPatchBuilder.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

TimeHandle makeHandle(uint64_t id,
                      double sourceSeconds,
                      double outputSeconds,
                      HandleKind kind,
                      bool locked = false)
{
    TimeHandle handle;
    handle.id = id;
    handle.source_seconds = sourceSeconds;
    handle.output_seconds = outputSeconds;
    handle.kind = kind;
    handle.locked = locked;
    return handle;
}

std::shared_ptr<const TimeGridSnapshot> makeGridWithInteriorHandles()
{
    std::vector<TimeHandle> handles = {
        makeHandle(1, 0.0, 0.0, HandleKind::ClipStart, true),
        makeHandle(2, 1.0, 1.0, HandleKind::UserAdded),
        makeHandle(3, 2.0, 2.1, HandleKind::ReferenceAuto),
        makeHandle(4, 4.0, 4.0, HandleKind::OnsetVoiced),
        makeHandle(5, 8.0, 8.0, HandleKind::ClipEnd, true),
    };
    return TimeGridSnapshot::makeFromHandles(std::move(handles));
}

std::shared_ptr<const TimeGridSnapshot> makeGridWithIdentityInternalSeed()
{
    std::vector<TimeHandle> handles = {
        makeHandle(1, 0.0, 0.0, HandleKind::ClipStart, true),
        makeHandle(2, 1.0, 1.0, HandleKind::UserAdded),
        makeHandle(3, 2.0, 2.0, HandleKind::InternalOnset),
        makeHandle(4, 4.0, 4.0, HandleKind::OnsetVoiced),
        makeHandle(5, 8.0, 8.0, HandleKind::ClipEnd, true),
    };
    return TimeGridSnapshot::makeFromHandles(std::move(handles));
}

const TimeHandle* findHandleAtSource(const std::shared_ptr<const TimeGridSnapshot>& grid,
                                     double sourceSeconds)
{
    if (grid == nullptr) {
        return nullptr;
    }

    for (const auto& handle : grid->handles()) {
        if (std::abs(handle.source_seconds - sourceSeconds) < 1.0e-9) {
            return &handle;
        }
    }

    return nullptr;
}

bool hasKindAtSource(const std::shared_ptr<const TimeGridSnapshot>& grid,
                     HandleKind kind,
                     double sourceSeconds)
{
    const auto* handle = findHandleAtSource(grid, sourceSeconds);
    return handle != nullptr && handle->kind == kind;
}

bool localSpeedsWithinWindow(const std::shared_ptr<const TimeGridSnapshot>& grid,
                             double minSpeed = 0.8,
                             double maxSpeed = 1.3)
{
    if (grid == nullptr) {
        return false;
    }

    const auto& handles = grid->handles();
    for (size_t i = 1; i < handles.size(); ++i) {
        const double sourceSpan = handles[i].source_seconds - handles[i - 1].source_seconds;
        const double outputSpan = handles[i].output_seconds - handles[i - 1].output_seconds;
        if (!(sourceSpan > 0.0) || !(outputSpan > 0.0)) {
            return false;
        }

        const double localSpeed = sourceSpan / outputSpan;
        if (localSpeed < minSpeed - 1.0e-9 || localSpeed > maxSpeed + 1.0e-9) {
            return false;
        }
    }

    return true;
}

} // namespace

void runTimeGridPatchBuilderCompilesValidReferenceAutoIntentsTest()
{
    constexpr const char* testName = "TimeGridPatchBuilder_CompilesValidReferenceAutoIntents";

    auto before = TimeGridSnapshot::makeIdentity(8.0);
    TimeGridPatchRequest request;
    request.before = before;
    request.affectedSourceStartSeconds = 0.5;
    request.affectedSourceEndSeconds = 3.5;
    request.intents.push_back({ 1.0, 1.2, 0.9f });
    request.intents.push_back({ 2.0, 2.25, 0.6f });

    const auto result = TimeGridPatchBuilder::build(request);
    if (!result.success || result.after == nullptr) {
        logFail(testName, ("builder failed: " + result.diagnostic).toStdString().c_str());
        return;
    }
    if (!hasKindAtSource(result.after, HandleKind::ReferenceAuto, 1.0)
        || !hasKindAtSource(result.after, HandleKind::ReferenceAuto, 2.0)) {
        logFail(testName, "compiled grid is missing ReferenceAuto handles");
        return;
    }
    if (!localSpeedsWithinWindow(result.after)) {
        logFail(testName, "compiled grid violated the 0.8x~1.3x local speed window");
        return;
    }

    logPass(testName);
}

void runTimeGridPatchBuilderDenseCandidateNearEndpointIsIgnoredTest()
{
    constexpr const char* testName = "TimeGridPatchBuilder_DenseCandidateNearEndpointIsIgnored";

    auto before = TimeGridSnapshot::makeIdentity(8.0);
    TimeGridPatchRequest request;
    request.before = before;
    request.affectedSourceStartSeconds = 0.0;
    request.affectedSourceEndSeconds = 1.0;
    request.intents.push_back({ 0.10, 0.2, 0.8f });

    const auto result = TimeGridPatchBuilder::build(request);
    if (!result.success || result.after == nullptr) {
        logFail(testName, "dense endpoint-adjacent candidate should be ignored without failing the build");
        return;
    }
    if (result.after != before) {
        logFail(testName, "ignored candidate should preserve the original TimeGrid snapshot");
        return;
    }
    if (hasKindAtSource(result.after, HandleKind::ReferenceAuto, 0.10)) {
        logFail(testName, "dense endpoint-adjacent candidate should not materialize a ReferenceAuto handle");
        return;
    }

    logPass(testName);
}

void runTimeGridPatchBuilderPreservesClipStartAndClipEndTest()
{
    constexpr const char* testName = "TimeGridPatchBuilder_PreservesClipStartAndClipEnd";

    TimeGridPatchRequest request;
    request.before = TimeGridSnapshot::makeIdentity(8.0);
    request.affectedSourceStartSeconds = 1.0;
    request.affectedSourceEndSeconds = 3.0;
    request.intents.push_back({ 2.0, 2.2, 0.8f });

    const auto result = TimeGridPatchBuilder::build(request);
    if (!result.success || result.after == nullptr) {
        logFail(testName, ("builder failed: " + result.diagnostic).toStdString().c_str());
        return;
    }

    const auto& handles = result.after->handles();
    if (handles.front().kind != HandleKind::ClipStart || !handles.front().locked
        || handles.back().kind != HandleKind::ClipEnd || !handles.back().locked) {
        logFail(testName, "endpoints were not preserved as locked ClipStart/ClipEnd");
        return;
    }

    logPass(testName);
}

void runTimeGridPatchBuilderPreservesUserAddedHandlesTest()
{
    constexpr const char* testName = "TimeGridPatchBuilder_PreservesUserAddedHandles";

    TimeGridPatchRequest request;
    request.before = makeGridWithInteriorHandles();
    request.affectedSourceStartSeconds = 1.5;
    request.affectedSourceEndSeconds = 3.0;
    request.intents.push_back({ 2.5, 2.8, 0.8f });

    const auto result = TimeGridPatchBuilder::build(request);
    if (!result.success || result.after == nullptr) {
        logFail(testName, ("builder failed: " + result.diagnostic).toStdString().c_str());
        return;
    }
    if (!hasKindAtSource(result.after, HandleKind::UserAdded, 1.0)) {
        logFail(testName, "UserAdded handle outside affected range was not preserved");
        return;
    }

    logPass(testName);
}

void runTimeGridPatchBuilderReplacesPriorReferenceAutoInAffectedRangeTest()
{
    constexpr const char* testName = "TimeGridPatchBuilder_ReplacesPriorReferenceAutoHandlesInAffectedRange";

    TimeGridPatchRequest request;
    request.before = makeGridWithInteriorHandles();
    request.affectedSourceStartSeconds = 1.5;
    request.affectedSourceEndSeconds = 3.0;
    request.intents.push_back({ 2.5, 2.8, 0.8f });

    const auto result = TimeGridPatchBuilder::build(request);
    if (!result.success || result.after == nullptr) {
        logFail(testName, ("builder failed: " + result.diagnostic).toStdString().c_str());
        return;
    }
    if (hasKindAtSource(result.after, HandleKind::ReferenceAuto, 2.0)
        || !hasKindAtSource(result.after, HandleKind::ReferenceAuto, 2.5)) {
        logFail(testName, "prior ReferenceAuto handle was not replaced inside affected range");
        return;
    }

    logPass(testName);
}

void runTimeGridPatchBuilderReplacesMatchingIdentityInternalOnsetSeedTest()
{
    constexpr const char* testName = "TimeGridPatchBuilder_ReplacesMatchingIdentityInternalOnsetSeed";

    TimeGridPatchRequest request;
    request.before = makeGridWithIdentityInternalSeed();
    request.affectedSourceStartSeconds = 1.5;
    request.affectedSourceEndSeconds = 3.0;
    request.intents.push_back({ 2.0, 2.2, 0.9f });

    const auto result = TimeGridPatchBuilder::build(request);
    if (!result.success || result.after == nullptr) {
        logFail(testName, ("builder failed: " + result.diagnostic).toStdString().c_str());
        return;
    }

    const auto* handle = findHandleAtSource(result.after, 2.0);
    if (handle == nullptr || handle->kind != HandleKind::ReferenceAuto) {
        logFail(testName, "matching identity InternalOnset seed should be replaced by ReferenceAuto");
        return;
    }

    logPass(testName);
}

void runTimeGridPatchBuilderPreservesHandlesOutsideAffectedRangeTest()
{
    constexpr const char* testName = "TimeGridPatchBuilder_PreservesHandlesOutsideAffectedRange";

    TimeGridPatchRequest request;
    request.before = makeGridWithInteriorHandles();
    request.affectedSourceStartSeconds = 2.2;
    request.affectedSourceEndSeconds = 3.0;
    request.intents.push_back({ 2.5, 2.8, 0.8f });

    const auto result = TimeGridPatchBuilder::build(request);
    if (!result.success || result.after == nullptr) {
        logFail(testName, ("builder failed: " + result.diagnostic).toStdString().c_str());
        return;
    }
    if (!hasKindAtSource(result.after, HandleKind::ReferenceAuto, 2.0)
        || !hasKindAtSource(result.after, HandleKind::OnsetVoiced, 4.0)) {
        logFail(testName, "handles outside affected range were not preserved");
        return;
    }

    logPass(testName);
}

void runTimeGridPatchBuilderInfeasibleRunRestoresPriorReferenceAutoHandleTest()
{
    constexpr const char* testName = "TimeGridPatchBuilder_InfeasibleRunRestoresPriorReferenceAutoHandle";

    std::vector<TimeHandle> handles = {
        makeHandle(1, 0.0, 0.0, HandleKind::ClipStart, true),
        makeHandle(2, 1.0, 1.0, HandleKind::UserAdded),
        makeHandle(3, 2.0, 2.1, HandleKind::ReferenceAuto),
        makeHandle(4, 4.0, 3.0, HandleKind::OnsetVoiced),
        makeHandle(5, 8.0, 8.0, HandleKind::ClipEnd, true),
    };
    auto before = TimeGridSnapshot::makeFromHandles(std::move(handles));
    if (before == nullptr) {
        logFail(testName, "failed to build infeasible fixed-boundary grid");
        return;
    }

    TimeGridPatchRequest request;
    request.before = before;
    request.affectedSourceStartSeconds = 1.5;
    request.affectedSourceEndSeconds = 3.0;
    request.intents.push_back({ 2.0, 3.8, 0.9f });

    const auto result = TimeGridPatchBuilder::build(request);
    if (!result.success || result.after == nullptr) {
        logFail(testName, "infeasible run should roll back to the prior valid grid instead of failing");
        return;
    }
    if (result.after != before) {
        logFail(testName, "rollback should preserve the original TimeGrid snapshot when no feasible mutation exists");
        return;
    }

    const auto* restored = findHandleAtSource(result.after, 2.0);
    if (restored == nullptr
        || restored->kind != HandleKind::ReferenceAuto
        || !approxEqual(restored->output_seconds, 2.1, 1.0e-9)) {
        logFail(testName, "rollback should restore the prior ReferenceAuto handle inside the affected range");
        return;
    }

    logPass(testName);
}

void runTimeGridPatchBuilderInvalidCandidateSetReturnsFailureNoAfterGridTest()
{
    constexpr const char* testName = "TimeGridPatchBuilder_InvalidCandidateSetReturnsFailureNoAfterGrid";

    TimeGridPatchRequest request;
    request.before = TimeGridSnapshot::makeIdentity(8.0);
    request.affectedSourceStartSeconds = 1.0;
    request.affectedSourceEndSeconds = 3.0;
    request.intents.push_back({ 2.0, 0.0, 0.8f });

    const auto result = TimeGridPatchBuilder::build(request);
    if (result.success || result.after != nullptr || result.diagnostic.isEmpty()) {
        logFail(testName, "invalid candidate set should fail with no after grid and diagnostic");
        return;
    }

    logPass(testName);
}

void runTimeGridPatchBuilderSuite()
{
    logSection("TimeGridPatchBuilder");
    runTimeGridPatchBuilderCompilesValidReferenceAutoIntentsTest();
    runTimeGridPatchBuilderDenseCandidateNearEndpointIsIgnoredTest();
    runTimeGridPatchBuilderPreservesClipStartAndClipEndTest();
    runTimeGridPatchBuilderPreservesUserAddedHandlesTest();
    runTimeGridPatchBuilderReplacesPriorReferenceAutoInAffectedRangeTest();
    runTimeGridPatchBuilderReplacesMatchingIdentityInternalOnsetSeedTest();
    runTimeGridPatchBuilderPreservesHandlesOutsideAffectedRangeTest();
    runTimeGridPatchBuilderInfeasibleRunRestoresPriorReferenceAutoHandleTest();
    runTimeGridPatchBuilderInvalidCandidateSetReturnsFailureNoAfterGridTest();
}
