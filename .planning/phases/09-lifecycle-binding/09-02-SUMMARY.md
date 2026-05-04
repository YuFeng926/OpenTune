---
phase: 09-lifecycle-binding
plan: 02
subsystem: core
tags: [replace, cache-reuse, partial-invalidation, STAB-01, READ-03]

requires:
  - phase: 09-01
    provides: Core-managed AudioSource↔clipId↔renderCache binding truth source

provides:
  - replaceClipAudioById preserves clipId and renderCache instance
  - Partial invalidation via enqueuePartialRenderById for changed ranges
  - Mapping-only changes skip content re-rendering
  - Diagnostic logging for STAB-01 observability

affects: [09-03, verification]

tech-stack:
  added: []
  patterns: [cache-reuse, partial-invalidation, identity-stability]

key-files:
  created: []
  modified:
    - Source/PluginProcessor.cpp - replaceClipAudioById cache preservation + logging
    - Source/Plugin/PluginEditor.cpp - Partial invalidation call + mapping-only logging
    - Tests/TestMain.cpp - Cache semantics and invalidation tests

key-decisions:
  - "Replace must preserve renderCache instance to avoid full cache rebuild (READ-03, STAB-01)"
  - "Only content changes trigger partial render; mapping-only changes skip re-rendering"
  - "Diagnostic fields: clipId, changedStart, changedEnd, mappingOnly, requestedChunks"

patterns-established:
  - "Identity stability: clipId and renderCache instance remain stable across replace operations"
  - "Partial invalidation: Only affected chunks marked pending, no full cache recreation"
  - "Observability: All critical paths logged with structured fields for verification"

requirements-completed: [READ-03, STAB-01]

duration: 15min
completed: 2026-04-07
---

# Phase 09 Plan 02: replace 语义收敛与局部失效 Summary

**Replace 操作保留 clipId 与 renderCache 实例，通过部分失效替代全量重建，满足 READ-03 与 STAB-01**

## Performance

- **Duration:** 15 min
- **Started:** 2026-04-07T08:27:16Z
- **Completed:** 2026-04-07T08:42:30Z
- **Tasks:** 3
- **Files modified:** 3

## Accomplishments

- **Task 1:** 重构 `replaceClipAudioById` 为 cache 复用语义 - 移除 renderCache 重建，保留实例
- **Task 2:** 建立统一 diff 区间到 chunk 失效的执行路径 - 添加 `enqueuePartialRenderById` 调用
- **Task 3:** 对齐日志与诊断字段用于 STAB-01 观测 - 添加 clipId、changed range、mappingOnly 字段

## Task Commits

Each task was committed atomically:

1. **Task 1 (RED):** `c6d9524` - Add failing tests for cache semantics (test)
2. **Task 1 (GREEN):** `a22f1a8` - Implement cache reuse in replaceClipAudioById (feat)
3. **Task 2 (RED):** `f6ce142` - Add failing tests for partial invalidation (test)
4. **Task 2 (GREEN):** `4af326c` - Add partial invalidation path + diagnostic logging (feat)

**Plan metadata:** Will be committed with SUMMARY.md

## Files Created/Modified

- `Source/PluginProcessor.cpp` - remove renderCache recreation, add diagnostic logging
- `Source/Plugin/PluginEditor.cpp` - call enqueuePartialRenderById, add mapping-only logging
- `Tests/TestMain.cpp` - add cache semantics and partial invalidation tests

## Decisions Made

**D1: Replace must preserve renderCache instance**
- Rationale: READ-03 requires identity stability; STAB-01 requires avoiding full cache rebuild
- Implementation: Removed `it->renderCache = std::make_shared<RenderCache>()` line

**D2: Only content changes trigger partial render**
- Rationale: Mapping-only changes (playbackStart without content/source change) don't affect rendered content
- Implementation: Check `diff.changed || sourceRangeChanged` before calling enqueuePartialRenderById

**D3: Diagnostic fields for STAB-01 observability**
- Fields: clipId, changedStart, changedEnd, mappingOnly, requestedChunks
- Purpose: Phase 9 verification document can reference these logs to demonstrate partial invalidation

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None - all tasks completed successfully.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- **READ-03 satisfied:** clipId and renderCache instance remain stable after replace
- **STAB-01 foundation ready:** Partial invalidation path established, ready for verification in 09-03
- **Standalone unchanged:** SAFE-01 maintained - no changes to Standalone branch behavior

---
*Phase: 09-lifecycle-binding*
*Completed: 2026-04-07*
