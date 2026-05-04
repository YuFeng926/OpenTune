---
phase: 20-统一失效入口
plan: 03
subsystem: ui
tags: [piano-roll, invalidation, frame-scheduler, heartbeat, vblank]

requires:
  - phase: 20-统一失效入口
    provides: PianoRollVisualInvalidation reducer contract and dirty-merge helper from 20-02
provides:
  - PianoRollComponent unified invalidateVisual contract with pendingVisualInvalidation_ state
  - PianoRollToolHandler invalidation hook renamed away from repaint semantics
  - Single flushPendingVisualInvalidation exit preserving Phase 21 clock boundaries
affects: [phase-20, phase-21, piano-roll, invalidation, frame-scheduler]

tech-stack:
  added: []
  patterns:
    - "Express PianoRoll visual intent through invalidation reasons instead of direct repaint calls"
    - "Keep heartbeat/timer/VBlank alive in Phase 20, but let them only flush one shared pending invalidation state"

key-files:
  created: []
  modified:
    - Source/Standalone/UI/PianoRollComponent.h
    - Source/Standalone/UI/PianoRollComponent.cpp
    - Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h
    - Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp

key-decisions:
  - "D20-03-01: PianoRollComponent merges every visual request into pendingVisualInvalidation_ and only flushPendingVisualInvalidation() may touch FrameScheduler"
  - "D20-03-02: ToolHandler, heartbeat, timer, and VBlank all speak invalidateVisual semantics so Phase 21 can remove extra clocks without reopening a second repaint API"

patterns-established:
  - "Pattern: setters, tool callbacks, and playback updates all register reason masks into one pending invalidation state"
  - "Pattern: flushPendingVisualInvalidation() is the only scheduler bridge; clocks may trigger flush, but may not call repaint directly"

requirements-completed: []

duration: 28min
completed: 2026-04-15
---

# Phase 20 Plan 03: unified invalidation entry wiring Summary

**PianoRoll 现在把工具交互、滚动、播放头、内容回填和 spinner 更新统一登记到 `invalidateVisual(...)`，再由单一 `flushPendingVisualInvalidation()` 出口决定 dirty 区域与调度优先级。**

## Performance

- **Duration:** 28 min
- **Started:** 2026-04-15T00:18:00+08:00
- **Completed:** 2026-04-15T00:46:28.0233401+08:00
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments

- `PianoRollComponent` 删除对外 `requestInteractiveRepaint()` 语义，暴露统一 `invalidateVisual(...)` / `flushPendingVisualInvalidation()` contract，并用 `pendingVisualInvalidation_` 承接同帧 merge
- `PianoRollToolHandler` 把 `Context::requestRepaint` 全量改成 `Context::invalidateVisual`，所有工具交互都回到 component 提供的统一视觉入口
- `PianoRollComponent.cpp` 不再保留裸 `repaint(`，且 `FrameScheduler::instance().requestInvalidate(...)` 只剩 flush 出口一处；heartbeat / timer / VBlank 结构保留但只做 unified invalidation register + flush

## Verification

- `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase20-docs -Target OpenTuneTests` 成功，产出 `build-phase20-docs/OpenTuneTests.exe`
- `build-phase20-docs\OpenTuneTests.exe` 全套通过，包含 `Phase 20: Unified Invalidation Tests` 三个 `INVAL_02_*` 用例，以及 Phase 18/19 守护用例
- 静态审计确认 `requestInteractiveRepaint` / `requestRepaint` 在目标结构中归零，`PianoRollComponent.cpp` 的 `FrameScheduler::instance().requestInvalidate(...)` 只剩一处，且不再命中 `repaint(`

## Task Commits

Each task was committed atomically:

1. **Task 1: 收敛 ToolHandler 与 Component 的对外 invalidation API** - `fb1f482` (refactor)
2. **Task 2: 把 PianoRoll 内部视觉调用点全部并到单一 invalidateVisual + flush 出口** - `950d3b8` (refactor)

## Files Created/Modified

- `Source/Standalone/UI/PianoRollComponent.h` - 暴露 unified invalidation contract，并让剩余 inline setter 直接登记视觉原因而不是直接重绘
- `Source/Standalone/UI/PianoRollComponent.cpp` - 把播放头、滚动、内容更新、spinner/waveform tick 全部汇入 `pendingVisualInvalidation_`，只经由 `flushPendingVisualInvalidation()` 触达 `FrameScheduler`
- `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h` - 把工具侧 callback contract 从 `requestRepaint` 改成 `invalidateVisual`
- `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp` - 将全部工具交互重绘请求改为 `ctx_.invalidateVisual()`

## Decisions Made

- 让 `invalidateVisual(...)` 只表达“为什么变脏、哪里变脏、优先级是什么”，而不再在调用点上决定立即 repaint 或 scheduler 细节，避免第二套视觉语义重新长出来
- 保留 `timerCallback()`、`onHeartbeatTick()`、`onScrollVBlankCallback()` 作为 Phase 21 之前的时钟边界，但它们只能消费统一 pending state；Phase 20 不越权删钟，也不允许这些时钟继续拥有独立 repaint API
- 把 `FrameScheduler` 出口压缩到 `flushPendingVisualInvalidation()` 内的单一 bridge，从而让 dirty merge / full repaint 提升 / priority 决策都先在 reducer helper 中完成

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 2 - Missing Critical] 补齐外部播放/滚动模式 setter 的 unified invalidation 路径**
- **Found during:** Task 2 (把 PianoRoll 内部视觉调用点全部并到单一 invalidateVisual + flush 出口)
- **Issue:** `setIsPlaying(...)` 与 `setScrollMode(...)` 仍会改变 PianoRoll 可见状态，但原计划正文没有点名这两个 setter；若不补齐，它们仍可能留下未登记的视觉状态变化
- **Fix:** 让播放状态变化登记 `Playhead` invalidation，滚动模式变化登记 `Viewport` invalidation，并让 toolbar 按钮改为复用 `setScrollMode(...)`
- **Files modified:** `Source/Standalone/UI/PianoRollComponent.h`, `Source/Standalone/UI/PianoRollComponent.cpp`
- **Verification:** static audit 继续满足“单入口、单 flush 出口、零 direct repaint”，fresh `OpenTuneTests` build + suite 全绿
- **Committed in:** `950d3b8`

---

**Total deviations:** 1 auto-fixed (1 missing critical)
**Impact on plan:** 仅补齐仍会改变可见状态的剩余 setter，避免 Phase 20 留下未收敛的视觉入口；没有引入兼容层或并行结构。

## Issues Encountered

- 首次在 PowerShell 中复用 L1 命令时，`cmd /c` 的引号写法不对，导致 `VsDevCmd.bat` 未正确启动；改回 verification 文档里的单引号包裹形式后，fresh configure/build 正常通过

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- `20-04` 现在可以直接基于 live tree 重建 Phase 20 的 fresh gate 证据，并回写 `ROADMAP.md` / `STATE.md`
- `Phase 21` 已拥有稳定的前置结构：工具、setter、播放头、滚动和 spinner/waveform tick 都只通过 unified invalidation register + single flush exit 工作
- `INVAL-01` / `INVAL-02` 仍应等 `20-04` 按 `20-TEST-VERIFICATION.md` 完成 L1/L2/L4/L6 全部 gate 后再回写 REQUIREMENTS

## Self-Check: PASSED

- Summary file exists at `.planning/phases/20-统一失效入口/20-03-SUMMARY.md`
- Commits `fb1f482` and `950d3b8` are present in git history
- Stub scan across the four implementation files and this summary found no tracked placeholders
