---
phase: 23-snapshot-region
plan: 01
subsystem: ara
tags: [ara, snapshot, playback-region, vst3, juce]

# Dependency graph
requires:
  - phase: 22-clip-aware-paint
    provides: PianoRoll 与共享 processor 的稳定基线，允许本计划只收敛 ARA adapter contract
provides:
  - Phase 23 verification source of truth (`23-TEST-VERIFICATION.md`)
  - `OpenTuneDocumentController` 的 `SourceSlot` / `RegionSlot` / `PublishedSnapshot` contract
  - snapshot-backed `loadSnapshot()` 读取入口与 region-level published view scaffolding
affects: [phase-23-plan-02, phase-23-plan-03, playback-renderer, plugin-editor]

# Tech tracking
tech-stack:
  added: []
  patterns: [single mutable model plus immutable published snapshot, region-keyed published views, snapshot-backed compile-follow read side]

key-files:
  created:
    - .planning/phases/23-snapshot-region/23-TEST-VERIFICATION.md
    - .planning/phases/23-snapshot-region/23-01-SUMMARY.md
  modified:
    - Source/ARA/OpenTuneDocumentController.h
    - Source/ARA/OpenTuneDocumentController.cpp
    - Source/ARA/OpenTunePlaybackRenderer.cpp
    - Source/Plugin/PluginEditor.cpp

key-decisions:
  - "Controller mutable state 以 `sources_ + regions_ + preferredRegion_ + publishedSnapshot_` 为主轴，不再保留 source-keyed 半状态表作为 header contract。"
  - "renderer 与 VST3 editor 的 compile-follow 直接切到 `loadSnapshot()`，而不是再引入一层兼容 getter。"
  - "PublishedRegionView 即使还没拿到 copied audio，也必须保留 region identity 与 mapping truth，避免 preferred-region 语义再次退回 source-level。"

patterns-established:
  - "Snapshot-first controller contract: 所有对外读取从 `loadSnapshot()` 进入。"
  - "Region-keyed truth: playback/source mapping 与 preferred identity 都绑定到 `ARAPlaybackRegion`。"
  - "Compile-follow via published view: renderer/editor 的跟随只消费 `PublishedRegionView`。"

requirements-completed: [SNAP-01, SNAP-02, SNAP-03]

# Metrics
duration: 13 min
completed: 2026-04-16
---

# Phase 23 Plan 01: Snapshot Model And Region Truth Guards Summary

**`OpenTuneDocumentController` 现在发布 immutable、region-keyed snapshot，并让 renderer/editor 跟随到 snapshot-backed 读取面，同时先锁定了 Phase 23 的验证门。**

## Performance

- **Duration:** 13 min
- **Started:** 2026-04-16T03:14:06Z
- **Completed:** 2026-04-16T03:27:40Z
- **Tasks:** 3
- **Files modified:** 5

## Accomplishments
- 提前创建 `23-TEST-VERIFICATION.md`，把 Phase 23 的 L1/L2/L6 gate 和 required journeys 固化成 source of truth
- 在 `Source/ARA/OpenTuneDocumentController.h` 定义 `SourceSlot`、`RegionSlot`、`PublishedRegionView`、`PublishedSnapshot` 与 `loadSnapshot()`
- 在 `Source/ARA/OpenTuneDocumentController.cpp` 建立 snapshot builder / publish scaffolding，并让 `Source/ARA/OpenTunePlaybackRenderer.cpp`、`Source/Plugin/PluginEditor.cpp` 跟随读取 snapshot
- 通过 `build-phase23-docs` 的 `OpenTuneTests` 编译 gate，并成功运行一次 `build-phase23-docs\OpenTuneTests.exe`

## Task Commits

Each task was committed atomically:

1. **Task 1: 创建 Phase 23 verification source of truth** - `ad6d9d0` (docs)
2. **Task 2: 冻结 source/region slot 与 immutable snapshot header contract** - `01fe267` (feat)
3. **Task 3: 落地 snapshot builder / load scaffolding 并守住 build gate** - `5a5b00f` (feat)

**Plan metadata:** 未单独提交；按用户要求未更新 `STATE.md` / `ROADMAP.md`。

## Files Created/Modified
- `.planning/phases/23-snapshot-region/23-TEST-VERIFICATION.md` - Phase 23 的验证源文档与 L1/L2/L6 gate
- `Source/ARA/OpenTuneDocumentController.h` - Source/region slot、published snapshot 与 `loadSnapshot()` contract
- `Source/ARA/OpenTuneDocumentController.cpp` - mutable model、snapshot builder、publish scaffolding 与 transition shell
- `Source/ARA/OpenTunePlaybackRenderer.cpp` - 改成按 playback region 读取 published snapshot view
- `Source/Plugin/PluginEditor.cpp` - 改成按 preferred published region 读取 snapshot-backed import/sync 数据

## Decisions Made
- 用 `PublishedSnapshot` 直接承接读侧 contract，而不是保留旧 getter 再慢慢迁移；否则 Phase 23 的 contract 冻结仍会围着旧 source truth 打补丁
- `RegionIdentity` 明确包含 `playbackRegion + audioSource`，让 preferred region 的 identity 不再退化成“最后一个 source”
- `OpenTunePlaybackRenderer` 与 `PluginEditor` 的 compile-follow 直接接 published view，避免再引入一套 snapshot projection helper API

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] 旧读侧调用在 header contract 冻结后无法继续编译**
- **Found during:** Task 3（snapshot builder / load scaffolding）
- **Issue:** `Source/ARA/OpenTunePlaybackRenderer.cpp` 与 `Source/Plugin/PluginEditor.cpp` 仍调用已退场的 source-level getter；如果不跟随，L1 build gate 无法通过
- **Fix:** 直接把两处消费者切到 `loadSnapshot()` + `PublishedRegionView`，只做保持编译与 contract 一致性的最小跟随，不提前实现 Phase 25 的额外 UX 逻辑
- **Files modified:** `Source/ARA/OpenTunePlaybackRenderer.cpp`, `Source/Plugin/PluginEditor.cpp`
- **Verification:** `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase23-docs -Target OpenTuneTests`；`& ".\build-phase23-docs\OpenTuneTests.exe"`
- **Committed in:** `5a5b00f` (part of task commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** 跟随修改只为守住 snapshot-first contract 与 build gate，没有扩 shared processor，也没有引入兼容层或并行旧新结构。

## Issues Encountered
- `.planning/` 被 `.gitignore` 忽略，因此 task 1 的 phase 文档需要 `git add -f` 才能纳入任务提交；其余实现无额外阻塞

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- `23-02` 可以直接基于新的 `sources_ / regions_ / preferredRegion_ / publishedSnapshot_` 结构继续实现 reducer 与 batch-end publish gate
- renderer/editor 已经有 snapshot-backed compile-follow 基线，后续只需继续收敛到 Phase 25 的最终读侧语义
- 本次没有触碰 `Source/PluginProcessor.*`，shared processor 边界保持不变

---
*Phase: 23-snapshot-region*
*Completed: 2026-04-16*
