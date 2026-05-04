---
phase: 20-统一失效入口
plan: 02
subsystem: ui
tags: [piano-roll, invalidation, reducer, repaint, cmake]

requires:
  - phase: 20-统一失效入口
    provides: Phase 20 RED regressions that compile against the future invalidation helper contract
provides:
  - PianoRollVisualInvalidation contract compiled into OpenTune and OpenTuneTests
  - Dirty-rect union, full-repaint promotion, and priority-max reducer implementation
affects: [phase-20, piano-roll, invalidation, reducer]

tech-stack:
  added: []
  patterns:
    - "Compile reducer helpers into OpenTune before component wiring consumes them"
    - "Freeze same-frame invalidation merge semantics in a pure helper instead of duplicating if/else branches in PianoRollComponent"

key-files:
  created:
    - Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h
    - Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp
  modified:
    - CMakeLists.txt

key-decisions:
  - "D20-02-01: PianoRoll invalidation priority is pinned to FrameScheduler::Priority with static_asserts so the helper enum cannot silently drift"
  - "D20-02-02: makeVisualFlushDecision owns dirty clipping and full-repaint fallback, leaving later component work to consume one reducer result instead of rebuilding merge rules"

patterns-established:
  - "Pattern: merge invalidation reasons in a standalone state carrier, then derive repaint scope through makeVisualFlushDecision(...)"
  - "Pattern: register new PianoRoll helpers in target_sources(OpenTune ...) the same time the contract lands so fresh builds compile what tests include"

requirements-completed: []

duration: 5min
completed: 2026-04-15
---

# Phase 20 Plan 02: unified invalidation reducer contract Summary

**Standalone PianoRoll 现在有一个可编译、可测试的 visual invalidation reducer，统一负责 dirty union、full repaint 提升与 priority merge。**

## Performance

- **Duration:** 5 min
- **Started:** 2026-04-15T00:04:00+08:00
- **Completed:** 2026-04-15T00:08:40.9220589+08:00
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments

- 新增 `PianoRollVisualInvalidation.h`，把 Phase 20 RED tests 直接引用的 reason / priority / request / state / flush decision contract 固定成独立 helper
- 新增 `PianoRollVisualInvalidation.cpp`，把 dirty rect union、full repaint 提升、priority max merge 与 local-bounds flush 决策集中到唯一实现
- 更新 `CMakeLists.txt`，让 `OpenTune` / `OpenTuneTests` 的 fresh build 真正编译这份 reducer contract，而不是只在 include 路径里“看起来存在”

## Task Commits

Each task was committed atomically:

1. **Task 1: 定义 PianoRoll invalidation reducer contract 并接入构建图** - `7f15d83` (feat)
2. **Task 2: 实现 dirty union / full repaint / priority merge reducer** - `35e5e0c` (feat)

## Files Created/Modified

- `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h` - 暴露 Phase 20 reducer 的唯一 contract 类型与 helper API
- `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp` - 实现 merge/clear/flush decision 规则，并用 static_assert 锁死 priority 数值顺序
- `CMakeLists.txt` - 把新的 PianoRoll invalidation helper 纳入 `target_sources(OpenTune ...)`

## Decisions Made

- 用 `static_assert` 把 `PianoRollVisualInvalidationPriority` 与 `FrameScheduler::Priority` 的数值顺序绑死，避免 helper contract 与现有调度优先级语义漂移
- 把 dirty clipping 与 out-of-bounds full repaint fallback 收进 `makeVisualFlushDecision(...)`，后续 `PianoRollComponent` 只消费 reducer 结果，不再复制一套 flush if/else
- 按 `20-TEST-VERIFICATION.md` 的 Final Gate，`INVAL-02` 的 phase-level closure 仍要等 `20-03/20-04` 完成旧入口清理与 L4 审计后再回写 REQUIREMENTS

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] 在 Task 1 提前落最小 `.cpp` 壳以保持构建图真实可编译**
- **Found during:** Task 1 (定义 PianoRoll invalidation reducer contract 并接入构建图)
- **Issue:** 任务要求当场把 `PianoRollVisualInvalidation.cpp` 注册进 `CMakeLists.txt`；如果只写 header 和 wiring，fresh build 会直接指向不存在的源文件
- **Fix:** 在 Task 1 同步创建最小可编译 `.cpp` 壳保证 build graph 为真，再在 Task 2 填满 reducer 逻辑
- **Files modified:** `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp`
- **Verification:** `build-phase20-docs` 在 Task 1 后成功重新编译 `OpenTuneTests`，Task 2 后 full suite 转绿
- **Committed in:** `7f15d83`

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** 仅用于保证构建图立即可编译，没有引入额外作用域或兼容层。

## Issues Encountered

- 当前工作树已有多处与本计划无关的脏改动，因此两个 task commits 都只显式 stage 允许文件，未触碰 `Tests/TestMain.cpp` 与其他现有改动
- Phase 20 的 phase-level Final Gate 还缺 `20-03` 的统一入口清理与后续 L4 审计，因此本计划只交付 reducer contract 本身，不提前宣称整个 Phase 20 关闭

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- `20-03` 现在可以把 `PianoRollComponent` / `PianoRollToolHandler` 的视觉入口改成只消费 `PianoRollVisualInvalidationState` / `makeVisualFlushDecision(...)`
- `build-phase20-docs` 的 L1、L2、L6 已经为 helper contract 转绿；下一阶段只需继续完成旧 repaint 入口收敛并补齐 L4 静态审计证据

## Self-Check: PASSED

- Summary file exists at `.planning/phases/20-统一失效入口/20-02-SUMMARY.md`
- Commits `7f15d83` and `35e5e0c` are present in git history
- Stub scan on `CMakeLists.txt`, `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h`, and `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp` found no tracked placeholders
