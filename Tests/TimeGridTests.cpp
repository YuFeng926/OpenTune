/**
 * Tests/TimeGridTests.cpp — Unit tests for Source/Utils/TimeGrid.{h,cpp}.
 *
 * Covers spec: openspec/changes/vocal-time-stretch/specs/time-grid/spec.md
 *   - TimeHandle 数据结构与字段语义
 *   - TimeGridSnapshot 不变量
 *   - TimeWarpCurve piecewise-linear τ / τ⁻¹ 查询
 *   - (handle drag physical-bound test deferred to Phase B; UI layer enforces 30ms)
 *
 * Test functions follow the existing logPass/logFail pattern in TestMain.cpp.
 * Aggregator: runTimeGridSuite() — registered in TestMain.cpp's suite array.
 */
#include "TestSupport.h"
#include "Utils/TimeGrid.h"
#include "Utils/TimeGridEditAction.h"
#include "PluginProcessor.h"

#include <cmath>
#include <vector>

// Suite functions live at global scope (consistent with TestMain.cpp's existing
// suites). TestSupport.h provides `using namespace OpenTune;` so OpenTune::*
// types are visible without explicit qualification.
namespace {

TimeHandle makeHandle(uint64_t id, double src, double out, HandleKind kind, bool locked = false)
{
    TimeHandle h;
    h.id = id;
    h.source_seconds = src;
    h.output_seconds = out;
    h.kind = kind;
    h.locked = locked;
    return h;
}

constexpr double kEpsBitExact = 0.0;
constexpr double kEpsLinear   = 1e-9;

} // namespace

// ============================================================================
// makeIdentity / basic snapshot
// ============================================================================

void runTimeGridIdentityFactoryTest()
{
    constexpr const char* testName = "TimeGrid_IdentityFactory";

    auto snap = TimeGridSnapshot::makeIdentity(12.0);
    if (snap == nullptr) {
        logFail(testName, "makeIdentity(12.0) returned nullptr");
        return;
    }

    const auto& h = snap->handles();
    if (h.size() != 2) {
        logFail(testName, "identity snapshot must have exactly 2 handles");
        return;
    }
    if (h[0].kind != HandleKind::ClipStart || !h[0].locked
        || h[0].source_seconds != 0.0 || h[0].output_seconds != 0.0) {
        logFail(testName, "ClipStart handle invariants violated");
        return;
    }
    if (h[1].kind != HandleKind::ClipEnd || !h[1].locked
        || h[1].source_seconds != 12.0 || h[1].output_seconds != 12.0) {
        logFail(testName, "ClipEnd handle invariants violated");
        return;
    }
    if (!snap->isIdentity()) {
        logFail(testName, "snapshot.isIdentity() should be true for identity factory");
        return;
    }
    if (snap->totalDurationSeconds() != 12.0) {
        logFail(testName, "totalDurationSeconds() != 12.0");
        return;
    }

    logPass(testName);
}

void runTimeGridIdentityRejectsZeroDurationTest()
{
    constexpr const char* testName = "TimeGrid_IdentityRejectsZeroDuration";

    auto snap1 = TimeGridSnapshot::makeIdentity(0.0);
    auto snap2 = TimeGridSnapshot::makeIdentity(-1.0);

    if (snap1 != nullptr || snap2 != nullptr) {
        logFail(testName, "makeIdentity should reject non-positive duration");
        return;
    }
    logPass(testName);
}

// ============================================================================
// validate() — invariants
// ============================================================================

void runTimeGridValidateLockedEndpointsTest()
{
    constexpr const char* testName = "TimeGrid_Validate_LockedEndpointsTotalDurationPreserved";

    std::vector<TimeHandle> good = {
        makeHandle(1, 0.0,  0.0,  HandleKind::ClipStart, true),
        makeHandle(2, 5.0,  5.0,  HandleKind::OnsetVoiced, false),
        makeHandle(3, 12.0, 12.0, HandleKind::ClipEnd, true),
    };

    juce::String err;
    if (!TimeGridSnapshot::validate(good, err)) {
        logFail(testName, ("identity grid should validate: " + err).toStdString().c_str());
        return;
    }

    logPass(testName);
}

void runTimeGridValidateRejectsMonotonicityViolationTest()
{
    constexpr const char* testName = "TimeGrid_Validate_RejectMonotonicityViolation";

    // output_seconds not strictly increasing (h[2].out == h[1].out)
    std::vector<TimeHandle> bad = {
        makeHandle(1, 0.0,  0.0,  HandleKind::ClipStart, true),
        makeHandle(2, 3.0,  4.0,  HandleKind::OnsetVoiced, false),
        makeHandle(3, 6.0,  4.0,  HandleKind::OnsetVoiced, false),  // ← out same as prev
        makeHandle(4, 12.0, 12.0, HandleKind::ClipEnd, true),
    };

    juce::String err;
    if (TimeGridSnapshot::validate(bad, err)) {
        logFail(testName, "validate should reject non-strictly-increasing output_seconds");
        return;
    }
    if (err.isEmpty()) {
        logFail(testName, "validate should set error message on failure");
        return;
    }

    logPass(testName);
}

void runTimeGridValidateRejectsUnlockedClipStartTest()
{
    constexpr const char* testName = "TimeGrid_Validate_RejectUnlockedClipStart";

    std::vector<TimeHandle> bad = {
        makeHandle(1, 0.0,  0.0,  HandleKind::ClipStart, false),  // ← not locked
        makeHandle(2, 12.0, 12.0, HandleKind::ClipEnd, true),
    };

    juce::String err;
    if (TimeGridSnapshot::validate(bad, err)) {
        logFail(testName, "validate should reject unlocked ClipStart");
        return;
    }
    logPass(testName);
}

void runTimeGridValidateRejectsTotalDurationViolationTest()
{
    constexpr const char* testName = "TimeGrid_Validate_RejectTotalDurationViolation";

    // ClipEnd output != source, breaking endpoint-locked total-duration invariant
    std::vector<TimeHandle> bad = {
        makeHandle(1, 0.0,  0.0,  HandleKind::ClipStart, true),
        makeHandle(2, 12.0, 14.0, HandleKind::ClipEnd, true),  // ← out != src
    };

    juce::String err;
    if (TimeGridSnapshot::validate(bad, err)) {
        logFail(testName, "validate should reject total duration mismatch (locked endpoints)");
        return;
    }
    logPass(testName);
}

void runTimeGridValidateRejectsLockedNonEndpointTest()
{
    constexpr const char* testName = "TimeGrid_Validate_RejectLockedNonEndpoint";

    std::vector<TimeHandle> bad = {
        makeHandle(1, 0.0,  0.0,  HandleKind::ClipStart, true),
        makeHandle(2, 5.0,  5.0,  HandleKind::OnsetVoiced, true),   // ← interior locked
        makeHandle(3, 12.0, 12.0, HandleKind::ClipEnd, true),
    };

    juce::String err;
    if (TimeGridSnapshot::validate(bad, err)) {
        logFail(testName, "validate should reject locked non-endpoint handle");
        return;
    }
    logPass(testName);
}

// ============================================================================
// τ forward / inverse — anchor exactness + linear interp + identity roundtrip
// ============================================================================

void runTimeGridTauAnchorBitExactTest()
{
    constexpr const char* testName = "TimeGrid_Tau_AnchorBitExact";

    std::vector<TimeHandle> handles = {
        makeHandle(1, 0.0,  0.0,  HandleKind::ClipStart, true),
        makeHandle(2, 2.5,  3.0,  HandleKind::OnsetVoiced, false),  // R = 1.2
        makeHandle(3, 5.5,  6.5,  HandleKind::OnsetVoiced, false),  // R from prev = 1.5/3.0 = 1.166...
        makeHandle(4, 12.0, 12.0, HandleKind::ClipEnd, true),
    };
    auto snap = TimeGridSnapshot::makeFromHandles(handles, 1);
    if (snap == nullptr) {
        logFail(testName, "makeFromHandles failed");
        return;
    }

    // Anchor exactness: each handle's source → its exact output_seconds (bit-exact)
    for (const auto& h : handles) {
        const double f = snap->tauForward(h.source_seconds);
        if (f != h.output_seconds) {
            logFail(testName, ("tauForward not bit-exact at anchor src="
                               + juce::String(h.source_seconds, 9)
                               + ", got=" + juce::String(f, 9)
                               + ", expected=" + juce::String(h.output_seconds, 9)).toStdString().c_str());
            return;
        }
        const double inv = snap->tauInverse(h.output_seconds);
        if (inv != h.source_seconds) {
            logFail(testName, ("tauInverse not bit-exact at anchor out="
                               + juce::String(h.output_seconds, 9)
                               + ", got=" + juce::String(inv, 9)
                               + ", expected=" + juce::String(h.source_seconds, 9)).toStdString().c_str());
            return;
        }
    }

    logPass(testName);
}

void runTimeGridTauLinearInterpTest()
{
    constexpr const char* testName = "TimeGrid_Tau_LinearInterpInSegment";

    // Segment [src=2.0, out=2.0] -> [src=3.0, out=4.0] (R=2.0)
    std::vector<TimeHandle> handles = {
        makeHandle(1, 0.0,  0.0,  HandleKind::ClipStart, true),
        makeHandle(2, 2.0,  2.0,  HandleKind::OnsetVoiced, false),
        makeHandle(3, 3.0,  4.0,  HandleKind::OnsetVoiced, false),
        // total src = 12, total out = 13 -- breaks endpoint invariant; use 12 fence:
        // To keep total preserved, we shrink later segment:
        // src 3.0->12.0 = 9.0; out 4.0->12.0 = 8.0 (R=8/9)
        makeHandle(4, 12.0, 12.0, HandleKind::ClipEnd, true),
    };
    auto snap = TimeGridSnapshot::makeFromHandles(handles, 1);
    if (snap == nullptr) {
        logFail(testName, "makeFromHandles failed (check total-duration invariant)");
        return;
    }

    // Midpoint of stretched segment: src=2.5 → expected out=3.0
    const double mid = snap->tauForward(2.5);
    if (std::abs(mid - 3.0) > kEpsLinear) {
        logFail(testName, ("midpoint forward got " + juce::String(mid, 9)
                           + " expected 3.0").toStdString().c_str());
        return;
    }
    const double inv = snap->tauInverse(3.0);
    if (std::abs(inv - 2.5) > kEpsLinear) {
        logFail(testName, ("midpoint inverse got " + juce::String(inv, 9)
                           + " expected 2.5").toStdString().c_str());
        return;
    }

    logPass(testName);
}

void runTimeGridTauIdentityRoundtripTest()
{
    constexpr const char* testName = "TimeGrid_Tau_IdentityRoundtrip";

    auto snap = TimeGridSnapshot::makeIdentity(12.0);
    if (snap == nullptr) {
        logFail(testName, "makeIdentity failed");
        return;
    }

    // For identity τ, forward(t) == t and inverse(t) == t for all t in [0,12].
    // Test a sweep at 0.5s steps + boundary points.
    for (double t = 0.0; t <= 12.0; t += 0.5) {
        const double f = snap->tauForward(t);
        const double inv = snap->tauInverse(t);
        if (f != t || inv != t) {
            logFail(testName, ("identity τ not exact at t=" + juce::String(t, 6)
                               + ", forward=" + juce::String(f, 9)
                               + ", inverse=" + juce::String(inv, 9)).toStdString().c_str());
            return;
        }
    }

    logPass(testName);
}

void runTimeGridIsIdentityInRangeTest()
{
    constexpr const char* testName = "TimeGrid_IsIdentityInRange";

    // Mixed grid: identity in [0, 4], non-identity in [4, 8], identity in [8, 12]
    std::vector<TimeHandle> handles = {
        makeHandle(1, 0.0,  0.0,  HandleKind::ClipStart, true),
        makeHandle(2, 4.0,  4.0,  HandleKind::OnsetVoiced, false),    // identity here
        makeHandle(3, 6.0,  5.5,  HandleKind::OnsetVoiced, false),    // ← compressed (output != source)
        makeHandle(4, 8.0,  8.0,  HandleKind::OnsetVoiced, false),    // back to identity, but pre-segment was stretched
        makeHandle(5, 12.0, 12.0, HandleKind::ClipEnd, true),
    };
    auto snap = TimeGridSnapshot::makeFromHandles(handles, 1);
    if (snap == nullptr) {
        logFail(testName, "makeFromHandles failed");
        return;
    }

    // Range entirely in identity prefix [0, 4]
    if (!snap->isIdentityInRange(0.5, 3.5)) {
        logFail(testName, "isIdentityInRange should be true for [0.5, 3.5] (identity prefix)");
        return;
    }

    // Range covering the non-identity handle (h[2] at src=6, out=5.5)
    if (snap->isIdentityInRange(5.0, 7.0)) {
        logFail(testName, "isIdentityInRange should be false for [5.0, 7.0] containing non-identity handle");
        return;
    }

    // Range entirely in identity suffix [8, 12] (handle 4 at 8 is identity, handle 5 at 12 is identity)
    if (!snap->isIdentityInRange(8.5, 11.5)) {
        logFail(testName, "isIdentityInRange should be true for [8.5, 11.5] (identity suffix)");
        return;
    }

    // Empty range
    if (!snap->isIdentityInRange(5.0, 5.0)) {
        logFail(testName, "isIdentityInRange should be true for empty range");
        return;
    }

    logPass(testName);
}

void runTimeGridSnapshotPublishLoadTest()
{
    constexpr const char* testName = "TimeGrid_SnapshotPublishLoad";

    TimeGrid grid;
    auto initial = grid.getSnapshot();
    if (initial == nullptr) {
        logFail(testName, "default-constructed TimeGrid must have non-null snapshot");
        return;
    }

    auto fresh = TimeGridSnapshot::makeIdentity(20.0);
    grid.setSnapshot(fresh);

    auto loaded = grid.getSnapshot();
    if (loaded.get() != fresh.get()) {
        logFail(testName, "atomic_load did not return the published snapshot");
        return;
    }

    logPass(testName);
}

// ============================================================================
// TimeGridEditAction — affected-range 不变量 anchor 测试
// ============================================================================
//
// 与 PianoRollEditAction 同款奇安装: 构造函数接受的 affected-range 参数
// 必须被原值存到成员字段、由 getter 原值返回;
// **不得**从 before/after snapshot diff 反推
// (避免 v2.0 a122bca 重构在 PianoRollEditAction 上发生过的 “union of all segments
// min/max 倒退为全长” 的静默退化).
//
// 这里测试仅需要补 “构造 + getter 返回原值” 不依赖 processor API 的部分;
// §3.6 落地 setMaterializationTimeGridById 后可补 undo()/redo() 的
// 集成测试。

void runTimeGridEditActionAffectedRangeVerbatimTest()
{
    constexpr const char* testName = "TimeGridEditAction_AffectedRangeStoredVerbatim";

    OpenTuneAudioProcessor processor;

    auto before = TimeGridSnapshot::makeIdentity(12.0);
    auto after  = TimeGridSnapshot::makeIdentity(12.0);  // also identity (representative)

    constexpr int64_t affectedStart = 250;   // arbitrary frame indices
    constexpr int64_t affectedEnd   = 980;

    TimeGridEditAction action(processor,
                              /*materializationId=*/42,
                              "Test action",
                              before,
                              after,
                              affectedStart,
                              affectedEnd);

    if (action.getAffectedSrcStartFrame() != affectedStart) {
        logFail(testName, "getAffectedSrcStartFrame() did not return the constructor argument verbatim");
        return;
    }
    if (action.getAffectedSrcEndFrame() != affectedEnd) {
        logFail(testName, "getAffectedSrcEndFrame() did not return the constructor argument verbatim");
        return;
    }
    if (action.getMaterializationId() != 42u) {
        logFail(testName, "getMaterializationId() did not return the constructor argument verbatim");
        return;
    }
    if (action.getDescription() != "Test action") {
        logFail(testName, "getDescription() did not return the constructor argument verbatim");
        return;
    }

    logPass(testName);
}

void runTimeGridEditActionRangeIndependentOfSnapshotDiffTest()
{
    constexpr const char* testName = "TimeGridEditAction_RangeIndependentOfSnapshotDiff";

    OpenTuneAudioProcessor processor;

    // Construct two snapshots whose handle source-frame distribution covers
    // a wide range [0, 12s], yet pass affectedRange that is narrow [4.50s, 4.55s]
    // (frame indices @ 100 fps: 450..455).  The action MUST store the narrow
    // range, NOT a derived-from-snapshot wide range.
    auto identityA = TimeGridSnapshot::makeIdentity(12.0);
    auto identityB = TimeGridSnapshot::makeIdentity(12.0);

    constexpr int64_t affectedStart = 450;
    constexpr int64_t affectedEnd   = 455;

    TimeGridEditAction action(processor,
                              /*materializationId=*/7,
                              "Tiny edit",
                              identityA,
                              identityB,
                              affectedStart,
                              affectedEnd);

    if (action.getAffectedSrcStartFrame() != 450 || action.getAffectedSrcEndFrame() != 455) {
        logFail(testName,
                "getter returned wide range; constructor should NOT compute range from snapshot diff");
        return;
    }
    if (action.getAffectedSrcStartFrame() == 0 || action.getAffectedSrcEndFrame() == 1200) {
        // (1200 = 12s * 100fps)
        logFail(testName, "getter returned the full-clip range, indicating snapshot-diff fallback");
        return;
    }

    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

void runTimeGridSuite()
{
    logSection("TimeGrid");
    runTimeGridIdentityFactoryTest();
    runTimeGridIdentityRejectsZeroDurationTest();
    runTimeGridValidateLockedEndpointsTest();
    runTimeGridValidateRejectsMonotonicityViolationTest();
    runTimeGridValidateRejectsUnlockedClipStartTest();
    runTimeGridValidateRejectsTotalDurationViolationTest();
    runTimeGridValidateRejectsLockedNonEndpointTest();
    runTimeGridTauAnchorBitExactTest();
    runTimeGridTauLinearInterpTest();
    runTimeGridTauIdentityRoundtripTest();
    runTimeGridIsIdentityInRangeTest();
    runTimeGridSnapshotPublishLoadTest();
    runTimeGridEditActionAffectedRangeVerbatimTest();
    runTimeGridEditActionRangeIndependentOfSnapshotDiffTest();
}
