---
phase: 23-snapshot-region
plan: 02
subsystem: ara
tags: [ara, snapshot, playback-region, reducer, vst3]

# Dependency graph
requires:
  - phase: 23-01
    provides: `loadSnapshot()` contract、`SourceSlot`/`RegionSlot`/`PublishedSnapshot` 骨架，以及 renderer/editor 的 snapshot-backed compile-follow 基线
provides:
  - ARA live callbacks 只更新 `sources_` / `regions_` / `preferredRegion_`，不再在每个 region/source callback 里直接发布 snapshot
  - `didEndEditing()` 统一处理 pending ARA mutable-model 变更，并整体替换 `publishedSnapshot_`
  - 保留的 source-level getter 全部退化为 published snapshot projection，不再直接暴露 mutable tables
affects: [phase-23-plan-03, phase-24-lifecycle, playback-renderer, plugin-editor]

# Tech tracking
tech-stack:
  added: []
  patterns: [batch-end snapshot publish gate, atomic snapshot handle replacement, snapshot-projection compatibility getters]

key-files:
  created:
    - .planning/phases/23-snapshot-region/23-02-SUMMARY.md
  modified:
    - Source/ARA/OpenTuneDocumentController.h
    - Source/ARA/OpenTuneDocumentController.cpp

key-decisions:
  - "ARA region/source callbacks 只负责写 mutable model 和 dirty 标记，真正的 published snapshot 统一在 `didEndEditing()` 发布。"
  - "保留的 source-level getter 必须只从 published snapshot 投影，不能再回退到 mutable model。"
  - "`publishedSnapshot_` 改用 atomic shared_ptr whole-handle replacement，让读侧只会拿到完整快照。"

patterns-established:
  - "Mutable reducer first: `didUpdatePlaybackRegionProperties()` / `didAddPlaybackRegionToAudioModification()` / source callbacks 只更新 `sources_`、`regions_`、`preferredRegion_`。"
  - "Batch-end publication: `pendingSnapshotPublication_` 累积 ARA 变更，`didEndEditing()` 再统一 `publishSnapshot()`。"
  - "Snapshot projection shell: legacy getter 仅作为 published snapshot 的只读投影。"

requirements-completed: [SNAP-01, SNAP-02, SNAP-03]

# Metrics
duration: 2 min
completed: 2026-04-16
---

# Phase 23 Plan 02: Snapshot Region Reducer And Publish Gate Summary

**`OpenTuneDocumentController` 现在把 ARA region/source 回调收敛到 dirty mutable model，并在 `didEndEditing()` 结束点一次性替换 immutable published snapshot。**

## Performance

- **Duration:** 2 min
- **Started:** 2026-04-16T03:41:55Z
- **Completed:** 2026-04-16T03:43:45Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- `didUpdatePlaybackRegionProperties()`、`didAddPlaybackRegionToAudioModification()`、`didUpdateAudioSourceProperties()` 与销毁路径不再在每个 callback 里直接 `publishSnapshot()`，而是只更新 mutable model 并维护 `preferredRegion_`
- `didEndEditing()` 现在成为 ARA 变更的统一 publish gate，`publishedSnapshot_` 改成 atomic whole-handle replacement
- 旧 `getAudioSource*` / `findAudioSourceByClipId()` 读取面全部退化为 published snapshot projection，不再从 mutable tables 直接读真相

## Task Commits

Each task was committed atomically:

1. **Task 1: 把 ARA source/region 变更收敛到 region-keyed mutable model reducer** - `0844ea5` (feat)
2. **Task 2: 在 edit-batch 结束点统一发布 immutable snapshot** - `a4c003c` (feat)

**Plan metadata:** 单独创建 `23-02-SUMMARY.md`；按用户要求不更新 `STATE.md` / `ROADMAP.md` / `REQUIREMENTS.md`。

## Files Created/Modified
- `Source/ARA/OpenTuneDocumentController.h` - 补 `didEndEditing()` override，并声明 preferred-region reconcile / dirty helper
- `Source/ARA/OpenTuneDocumentController.cpp` - 落地 reducer-only callbacks、batch-end publish gate 与 atomic snapshot replacement
- `.planning/phases/23-snapshot-region/23-02-SUMMARY.md` - 记录 23-02 执行结果、提交与 phase 边界

## Decisions Made
- 让 ARA callback 先积累 dirty mutable model，再在 `didEndEditing()` 统一发布，避免读侧在 edit-batch 中看到半更新 mixed state
- `publishedSnapshot_` 使用 atomic shared_ptr load/store，保证 renderer/editor 读到的始终是一份完整快照句柄
- 保留的 source-level getter 继续存在时，也只能从 published snapshot 投影，不能再回落到 mutable model

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- `23-03` 现在可以基于稳定的 reducer + publish gate 补 snapshot guards，并继续把 renderer/editor 往最终 snapshot-only 读侧收敛
- `Phase 24` 仍需单独处理 sample access、content dirty 与 removal lifecycle；本 plan 没有提前混入这些 callback 职责
- 本次只修改了 `Source/ARA/OpenTuneDocumentController.h` 与 `Source/ARA/OpenTuneDocumentController.cpp`，没有扩 `Source/PluginProcessor.*`，也没有提前触碰最终 consumer UX

---
*Phase: 23-snapshot-region*
*Completed: 2026-04-16*
