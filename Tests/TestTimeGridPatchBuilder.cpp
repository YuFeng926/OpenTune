#include "TestSupport.h"
#include "DSP/TimeGridPatchBuilder.h"

#include <algorithm>
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

bool hasKindAtSource(const std::shared_ptr<const TimeGridSnapshot>& grid,
                     HandleKind kind,
                     double sourceSeconds)
{
    if (grid == nullptr) {
        return false;
    }

    for (const auto& handle : grid->handles()) {
        if (handle.kind == kind && std::abs(handle.source_seconds - sourceSeconds) < 1.0e-9) {
            return true;
        }
    }
    return false;
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
    request.intents.push_back({1.0, 1.2, 0.9f});
    request.intents.push_back({2.0, 2.25, 0.6f});

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

    logPass(testName);
}

void runTimeGridPatchBuilderRejectsDenseCandidatesNearEndpointTest()
{
    constexpr const char* testName = "TimeGridPatchBuilder_RejectsDenseCandidatesNearEndpoint";

    TimeGridPatchRequest request;
    request.before = TimeGridSnapshot::makeIdentity(8.0);
    request.affectedSourceStartSeconds = 0.0;
    request.affectedSourceEndSeconds = 1.0;
    request.intents.push_back({0.10, 0.2, 0.8f});

    const auto result = TimeGridPatchBuilder::build(request);
    if (result.success || result.after != nullptr) {
        logFail(testName, "builder should reject a candidate closer than TimeGrid source spacing");
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
    request.intents.push_back({2.0, 2.2, 0.8f});

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
    request.intents.push_back({2.5, 2.8, 0.8f});

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
    request.intents.push_back({2.5, 2.8, 0.8f});

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

void runTimeGridPatchBuilderPreservesHandlesOutsideAffectedRangeTest()
{
    constexpr const char* testName = "TimeGridPatchBuilder_PreservesHandlesOutsideAffectedRange";

    TimeGridPatchRequest request;
    request.before = makeGridWithInteriorHandles();
    request.affectedSourceStartSeconds = 2.2;
    request.affectedSourceEndSeconds = 3.0;
    request.intents.push_back({2.5, 2.8, 0.8f});

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

void runTimeGridPatchBuilderInvalidCandidateSetReturnsFailureNoAfterGridTest()
{
    constexpr const char* testName = "TimeGridPatchBuilder_InvalidCandidateSetReturnsFailureNoAfterGrid";

    TimeGridPatchRequest request;
    request.before = TimeGridSnapshot::makeIdentity(8.0);
    request.affectedSourceStartSeconds = 1.0;
    request.affectedSourceEndSeconds = 3.0;
    request.intents.push_back({2.0, 0.0, 0.8f});

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
    runTimeGridPatchBuilderRejectsDenseCandidatesNearEndpointTest();
    runTimeGridPatchBuilderPreservesClipStartAndClipEndTest();
    runTimeGridPatchBuilderPreservesUserAddedHandlesTest();
    runTimeGridPatchBuilderReplacesPriorReferenceAutoInAffectedRangeTest();
    runTimeGridPatchBuilderPreservesHandlesOutsideAffectedRangeTest();
    runTimeGridPatchBuilderInvalidCandidateSetReturnsFailureNoAfterGridTest();
}
