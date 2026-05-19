/**
 * Tests/HandleNoteMergerTests.cpp — note-confirmed-handles capability tests
 *
 * Coverage (per add-note-confirmed-handles specs/time-grid-note-confirmation/spec.md):
 *
 *   L2 Unit (merge algorithm):
 *   - Pass 1 双源命中 → confidence=High
 *   - Pass 2 失配 note → NoteOnly handle
 *   - Closest-note-wins (one note per handle)
 *   - 30ms min-segment 钳制
 *   - Endpoints (ClipStart/ClipEnd) 不参与 Pass 1
 *
 *   L2 Unit (TimeHandle invariants):
 *   - source_seconds + confidence 双 immutable after drag
 *
 *   L3 Integration (Barrier):
 *   - WordSegmenter 先到 / GameNoteGenerator 先到 / legacy 不触发
 *   - Manual reSeed 保留 UserAdded + 用户编辑过的 output
 *
 * Suite aggregator: runHandleNoteMergerSuite() — registered in TestMain.cpp.
 */
#include "TestSupport.h"
#include "DSP/HandleNoteMerger.h"
#include "Utils/TimeGrid.h"
#include "Utils/Note.h"
#include <cmath>
#include <vector>

namespace {

using OpenTune::Confidence;
using OpenTune::HandleKind;
using OpenTune::HandleNoteMerger;
using OpenTune::Note;
using OpenTune::TimeGridSnapshot;
using OpenTune::TimeHandle;

// 构造单个 handle 的便捷 helper
TimeHandle makeHandle(double sourceSec, HandleKind kind,
                       Confidence conf = Confidence::Default,
                       bool locked = false) {
    TimeHandle h;
    h.id = static_cast<uint64_t>(sourceSec * 1000.0); // 简单稳定 id
    h.source_seconds = sourceSec;
    h.output_seconds = sourceSec;
    h.kind = kind;
    h.locked = locked;
    h.confidence = conf;
    return h;
}

TimeHandle makeClipStart() {
    TimeHandle h;
    h.id = 1;
    h.source_seconds = 0.0;
    h.output_seconds = 0.0;
    h.kind = HandleKind::ClipStart;
    h.locked = true;
    h.confidence = Confidence::Default;
    return h;
}

TimeHandle makeClipEnd(double totalSec) {
    TimeHandle h;
    h.id = 2;
    h.source_seconds = totalSec;
    h.output_seconds = totalSec;
    h.kind = HandleKind::ClipEnd;
    h.locked = true;
    h.confidence = Confidence::Default;
    return h;
}

Note makeNote(double startTime, double endTime = 0.0) {
    Note n;
    n.startTime = startTime;
    n.endTime = (endTime > 0.0) ? endTime : startTime + 0.5;
    n.pitch = 220.0f;
    return n;
}

const TimeHandle* findBySource(const std::vector<TimeHandle>& v, double src) {
    for (const auto& h : v) {
        if (std::abs(h.source_seconds - src) < 1e-9) return &h;
    }
    return nullptr;
}

} // namespace

// ============================================================================
// L2: TimeHandle invariants
// ============================================================================

void runTimeHandle_SourceImmutableAfterDragTest() {
    constexpr const char* testName = "TimeHandle_SourceImmutableAfterDrag";
    TimeHandle h = makeHandle(1.0, HandleKind::OnsetVoiced);
    const double srcBefore = h.source_seconds;
    const Confidence confBefore = h.confidence;
    h.output_seconds = 1.3; // 模拟用户拖动
    if (h.source_seconds != srcBefore) {
        logFail(testName, "source_seconds mutated after drag");
        return;
    }
    if (h.confidence != confBefore) {
        logFail(testName, "confidence mutated after drag");
        return;
    }
    logPass(testName);
}

void runTimeHandle_ConfidenceImmutableAfterDragTest() {
    constexpr const char* testName = "TimeHandle_ConfidenceImmutableAfterDrag";
    TimeHandle h = makeHandle(1.0, HandleKind::OnsetVoiced, Confidence::High);
    h.output_seconds = 1.5;
    if (h.confidence != Confidence::High) {
        logFail(testName, "High confidence lost after drag");
        return;
    }
    logPass(testName);
}

// ============================================================================
// L2: Merge algorithm
// ============================================================================

void runHandleNoteMerger_DoubleHitMarksHighTest() {
    constexpr const char* testName = "HandleNoteMerger_DoubleHitMarksHigh";
    std::vector<TimeHandle> pre = {
        makeClipStart(),
        makeHandle(1.020, HandleKind::OnsetVoiced),
        makeClipEnd(5.0),
    };
    std::vector<Note> notes = { makeNote(1.000, 1.500) };
    auto merged = HandleNoteMerger::merge(pre, notes);

    if (merged.size() != 3) {
        logFail(testName, "expected 3 handles (no NoteOnly added)");
        return;
    }
    const auto* h = findBySource(merged, 1.020);
    if (!h || h->confidence != Confidence::High) {
        logFail(testName, "1.020s handle not marked High");
        return;
    }
    logPass(testName);
}

void runHandleNoteMerger_UnmatchedNoteCreatesNoteOnlyTest() {
    constexpr const char* testName = "HandleNoteMerger_UnmatchedNoteCreatesNoteOnly";
    std::vector<TimeHandle> pre = {
        makeClipStart(),
        makeClipEnd(5.0),
    };
    std::vector<Note> notes = { makeNote(2.500) };
    auto merged = HandleNoteMerger::merge(pre, notes);

    if (merged.size() != 3) {
        logFail(testName, "expected ClipStart + NoteOnly + ClipEnd");
        return;
    }
    const auto* h = findBySource(merged, 2.500);
    if (!h) {
        logFail(testName, "no handle at 2.500s");
        return;
    }
    if (h->kind != HandleKind::NoteOnly) {
        logFail(testName, "wrong kind for unmatched-note handle");
        return;
    }
    if (h->confidence != Confidence::Default) {
        logFail(testName, "NoteOnly should be Default confidence");
        return;
    }
    logPass(testName);
}

void runHandleNoteMerger_ClosestNoteWinsTest() {
    constexpr const char* testName = "HandleNoteMerger_ClosestNoteWins";
    // 两个 handle 都在 50ms threshold 内,note 应配给最近 (h2 距 10ms)
    std::vector<TimeHandle> pre = {
        makeClipStart(),
        makeHandle(1.000, HandleKind::OnsetVoiced),
        makeHandle(1.030, HandleKind::OnsetVoiced),
        makeClipEnd(5.0),
    };
    std::vector<Note> notes = { makeNote(1.020) };  // closer to h@1.030 (10ms) than h@1.000 (20ms)
    auto merged = HandleNoteMerger::merge(pre, notes);

    const auto* h1 = findBySource(merged, 1.000);
    const auto* h2 = findBySource(merged, 1.030);
    if (!h1 || !h2) {
        logFail(testName, "missing pre-existing handles");
        return;
    }
    if (h2->confidence != Confidence::High) {
        logFail(testName, "closest handle (1.030) not marked High");
        return;
    }
    if (h1->confidence != Confidence::Default) {
        logFail(testName, "non-closest handle (1.000) should remain Default");
        return;
    }
    logPass(testName);
}

void runHandleNoteMerger_UnmatchedNoteWithinMinSegmentDroppedTest() {
    constexpr const char* testName = "HandleNoteMerger_UnmatchedNoteWithinMinSegmentDropped";
    // h at 2.100, note at 2.150 (50ms away — Pass 1 marks High; Pass 2 not needed).
    // To test Pass 2 30ms min-segment: use multiple handles + a separate note with no match.
    // Setup: h at 2.100, h at 2.500. Note at 2.115 (15ms from 2.100, within 50ms → Pass 1).
    //        Another note at 2.520 (20ms from 2.500, within 50ms → Pass 1, marks h@2.500 High).
    //        Another note at 2.118 (3ms from 2.100, within 50ms but 2.100 already matched
    //                              by closer note. Wait — 2.115 is 15ms, 2.118 is 18ms. 2.115 wins.
    //                              2.118 unmatched → Pass 2 candidate, but 2.118 within 30ms of h@2.100 (18ms) → drop.
    std::vector<TimeHandle> pre = {
        makeClipStart(),
        makeHandle(2.100, HandleKind::OnsetVoiced),
        makeHandle(2.500, HandleKind::OnsetVoiced),
        makeClipEnd(5.0),
    };
    std::vector<Note> notes = {
        makeNote(2.115),  // matches h@2.100 (15ms)
        makeNote(2.118),  // unmatched (h@2.100 already taken by 2.115); Pass 2 drops (18ms < 30ms)
        makeNote(2.520),  // matches h@2.500 (20ms)
    };
    auto merged = HandleNoteMerger::merge(pre, notes);

    // expected: 4 handles (no NoteOnly added)
    if (merged.size() != 4) {
        logFail(testName, "expected 4 handles, no NoteOnly insertion");
        return;
    }
    const auto* h1 = findBySource(merged, 2.100);
    const auto* h2 = findBySource(merged, 2.500);
    if (!h1 || h1->confidence != Confidence::High) {
        logFail(testName, "h@2.100 should be High (matched by 2.115)");
        return;
    }
    if (!h2 || h2->confidence != Confidence::High) {
        logFail(testName, "h@2.500 should be High (matched by 2.520)");
        return;
    }
    logPass(testName);
}

void runHandleNoteMerger_EndpointsExcludedTest() {
    constexpr const char* testName = "HandleNoteMerger_EndpointsExcluded";
    std::vector<TimeHandle> pre = {
        makeClipStart(),
        makeClipEnd(5.0),
    };
    std::vector<Note> notes = {
        makeNote(0.020), // very close to ClipStart (within T_match)
        makeNote(4.980), // very close to ClipEnd (within T_match)
    };
    auto merged = HandleNoteMerger::merge(pre, notes);

    // ClipStart/ClipEnd MUST stay Default (Pass 1 excludes endpoints).
    if (merged[0].confidence != Confidence::Default) {
        logFail(testName, "ClipStart got marked High");
        return;
    }
    if (merged.back().confidence != Confidence::Default) {
        logFail(testName, "ClipEnd got marked High");
        return;
    }
    // Pass 2: notes near endpoints excluded → no NoteOnly added.
    if (merged.size() != 2) {
        logFail(testName, "endpoint-near notes should not produce NoteOnly");
        return;
    }
    logPass(testName);
}

// ============================================================================
// L3: Barrier integration
// ============================================================================

void runHandleNoteMergerBarrier_WordSegmenterFirstTest() {
    constexpr const char* testName = "HandleNoteMergerBarrier_WordSegmenterFirst";
    HandleNoteMerger merger;
    bool fired = false;
    std::vector<TimeHandle> publishedHandles;
    merger.onMergeComplete = [&](std::vector<TimeHandle> handles) {
        fired = true;
        publishedHandles = std::move(handles);
    };

    std::vector<TimeHandle> pre = { makeClipStart(), makeHandle(1.0, HandleKind::OnsetVoiced), makeClipEnd(5.0) };
    merger.deliverHandles(pre);
    if (fired) { logFail(testName, "merge fired before notes arrived"); return; }

    std::vector<Note> notes = { makeNote(1.020) };
    merger.deliverNotes(notes);
    if (!fired) { logFail(testName, "merge did not fire after both"); return; }
    if (publishedHandles.size() != 3) { logFail(testName, "wrong publish size"); return; }
    logPass(testName);
}

void runHandleNoteMergerBarrier_GameNoteGeneratorFirstTest() {
    constexpr const char* testName = "HandleNoteMergerBarrier_GameNoteGeneratorFirst";
    HandleNoteMerger merger;
    bool fired = false;
    merger.onMergeComplete = [&](std::vector<TimeHandle>) { fired = true; };

    merger.deliverNotes({ makeNote(1.000) });
    if (fired) { logFail(testName, "merge fired without handles"); return; }

    merger.deliverHandles({ makeClipStart(), makeHandle(1.020, HandleKind::OnsetVoiced), makeClipEnd(5.0) });
    if (!fired) { logFail(testName, "merge did not fire"); return; }
    logPass(testName);
}

void runHandleNoteMergerBarrier_LegacyDisabledNoMergeTest() {
    constexpr const char* testName = "HandleNoteMergerBarrier_LegacyDisabledNoMerge";
    HandleNoteMerger merger;
    bool fired = false;
    merger.onMergeComplete = [&](std::vector<TimeHandle>) { fired = true; };

    merger.deliverHandles({ makeClipStart(), makeHandle(1.0, HandleKind::OnsetVoiced), makeClipEnd(5.0) });
    // Notes never delivered (legacy mode disabled GameNoteGenerator).
    if (fired) { logFail(testName, "merge fired without notes"); return; }
    logPass(testName);
}

// ============================================================================
// L3: Manual re-seed
// ============================================================================

void runHandleNoteMergerReSeed_PreservesUserAddedTest() {
    constexpr const char* testName = "HandleNoteMergerReSeed_PreservesUserAdded";
    // 当前 snapshot: ClipStart, OnsetVoiced (1.0, output=1.3), UserAdded (2.0, output=2.0), ClipEnd
    std::vector<TimeHandle> currentHandles = {
        makeClipStart(),
        makeHandle(1.0, HandleKind::OnsetVoiced),
        makeHandle(2.0, HandleKind::UserAdded),
        makeClipEnd(5.0),
    };
    currentHandles[1].output_seconds = 1.3;
    auto snap = TimeGridSnapshot::makeFromHandles(currentHandles, /*revision=*/2);
    if (!snap) { logFail(testName, "snapshot construction failed"); return; }

    // handlesPre 仍包含 1.0 onset (WordSegmenter 没变)
    std::vector<TimeHandle> handlesPre = {
        makeClipStart(),
        makeHandle(1.0, HandleKind::OnsetVoiced),
        makeClipEnd(5.0),
    };
    std::vector<Note> notes = { makeNote(1.020) };
    auto reseeded = HandleNoteMerger::reSeed(*snap, handlesPre, notes);

    const auto* userAdded = findBySource(reseeded, 2.0);
    if (!userAdded) { logFail(testName, "UserAdded handle lost"); return; }
    if (userAdded->kind != HandleKind::UserAdded) { logFail(testName, "UserAdded kind changed"); return; }

    // 1.0 OnsetVoiced 应该保留用户编辑的 output_seconds=1.3
    const auto* h = findBySource(reseeded, 1.0);
    if (!h) { logFail(testName, "auto handle 1.0 lost"); return; }
    if (std::abs(h->output_seconds - 1.3) > 1e-9) {
        logFail(testName, "user-edited output_seconds lost on re-seed");
        return;
    }
    logPass(testName);
}

void runHandleNoteMergerReSeed_DiscardsStaleNoteOnlyTest() {
    constexpr const char* testName = "HandleNoteMergerReSeed_DiscardsStaleNoteOnly";
    // Current: ClipStart, NoteOnly (2.5), ClipEnd
    std::vector<TimeHandle> currentHandles = {
        makeClipStart(),
        makeHandle(2.5, HandleKind::NoteOnly),
        makeClipEnd(5.0),
    };
    auto snap = TimeGridSnapshot::makeFromHandles(currentHandles, /*revision=*/2);
    if (!snap) { logFail(testName, "snapshot construction failed"); return; }

    // 当前 notes 不再含 t=2.5 的 note
    std::vector<TimeHandle> handlesPre = { makeClipStart(), makeClipEnd(5.0) };
    std::vector<Note> notes = { makeNote(3.5) };
    auto reseeded = HandleNoteMerger::reSeed(*snap, handlesPre, notes);

    // 旧 NoteOnly 应被丢弃; 新 NoteOnly 在 3.5 处生成
    const auto* old = findBySource(reseeded, 2.5);
    if (old) { logFail(testName, "stale NoteOnly@2.5 not discarded"); return; }
    const auto* fresh = findBySource(reseeded, 3.5);
    if (!fresh || fresh->kind != HandleKind::NoteOnly) {
        logFail(testName, "new NoteOnly@3.5 not created");
        return;
    }
    logPass(testName);
}

// ============================================================================
// Suite aggregator
// ============================================================================

void runHandleNoteMergerSuite()
{
    logSection("HandleNoteMerger (note-confirmation merge capability)");
    runTimeHandle_SourceImmutableAfterDragTest();
    runTimeHandle_ConfidenceImmutableAfterDragTest();
    runHandleNoteMerger_DoubleHitMarksHighTest();
    runHandleNoteMerger_UnmatchedNoteCreatesNoteOnlyTest();
    runHandleNoteMerger_ClosestNoteWinsTest();
    runHandleNoteMerger_UnmatchedNoteWithinMinSegmentDroppedTest();
    runHandleNoteMerger_EndpointsExcludedTest();
    runHandleNoteMergerBarrier_WordSegmenterFirstTest();
    runHandleNoteMergerBarrier_GameNoteGeneratorFirstTest();
    runHandleNoteMergerBarrier_LegacyDisabledNoMergeTest();
    runHandleNoteMergerReSeed_PreservesUserAddedTest();
    runHandleNoteMergerReSeed_DiscardsStaleNoteOnlyTest();
}
