# Phase 20: Unified Invalidation Tests

## Scope

本文件记录的是修复 WR-01 / WR-02 / WR-03 之后的 current live tree 事实，而不是最早那份 `passed` closure snapshot。当前 Phase 20 证据必须同时证明三件事：

- PianoRoll 的所有视觉变化仍只通过统一 `invalidateVisual(...)` 入口进入。
- `PianoRollVisualInvalidationState` / `makeVisualFlushDecision(...)` 继续负责同帧 dirty union、full repaint promotion 和 priority merge。
- component-level wiring 已经把 pending invalidation 的 flush 收敛到 `onVisualVBlankCallback(...)`，不再依赖 PianoRoll 自己的 heartbeat / timer 路径；测试 harness 失败时必须返回非零。

Phase 18/19 的 projected playhead 与主层场景守护继续作为回归底线。

## Verification Levels

| Level | Goal | Command | Status | Pass Criteria |
| --- | --- | --- | --- | --- |
| L1 | 构建 `OpenTuneTests` 并验证 remediated Phase 20 contract 已接入 fresh binary | `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase20-docs -Target OpenTuneTests` | Passed | fresh configure/build 完成并产出 `build-phase20-docs/OpenTuneTests.exe`。 |
| L2 | 运行 full regression binary，确认 unified invalidation reducer、component VBlank wiring、Phase 18/19 guards 全部为绿 | `& ".\build-phase20-docs\OpenTuneTests.exe"` | Passed | 输出必须包含 `Phase 20: Unified Invalidation Tests`；`INVAL_02_*` 三个 reducer 用例通过；`INVAL_01_PianoRollVisualVBlankFlushesPendingInteractiveInvalidation` 与 `INVAL_01_PianoRollRenderingDecorationTicksOnVisualVBlank` 通过；`TIME_01_*` 与 `LAYER_01_*` 继续为绿。 |
| L4 | 静态审计 unified invalidation / visual VBlank / blocking harness contract | `rtk grep -n "invalidateVisual\(|flushPendingVisualInvalidation\(|onVisualVBlankCallback\(|FrameScheduler::instance\(\)\.requestInvalidate|onHeartbeatTick\(|timerCallback\(|gHasTestFailure|PianoRollComponentTestProbe|INVAL_01_PianoRollVisualVBlankFlushesPendingInteractiveInvalidation|INVAL_01_PianoRollRenderingDecorationTicksOnVisualVBlank" "Source/Standalone/UI/PianoRollComponent.h" "Source/Standalone/UI/PianoRollComponent.cpp" "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h" "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp" "Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h" "Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp" "Source/Standalone/PluginEditor.cpp" "Source/Plugin/PluginEditor.cpp" "Tests/TestMain.cpp"` | Passed | 必须直接命中 unified invalidation entry、reducer `.cpp`、`onVisualVBlankCallback(...)`、唯一 scheduler flush 出口、两条新 component-level regressions，以及 `gHasTestFailure` / `return gHasTestFailure...`；`Source/Standalone/UI/PianoRollComponent.h/cpp` 中 `timerCallback(` 为 0 matches，`Source/Plugin/PluginEditor.cpp` 中 `onHeartbeatTick(` 为 0 matches。 |
| L6 | 重新运行 full regression suite，确认 remediated tree 的 gate 可重复通过 | `& ".\build-phase20-docs\OpenTuneTests.exe"` | Passed | 第二次 full-suite run 与 L2 一致，Phase 20 / 18 / 19 sections 全绿。 |

## Required Journeys

- `INVAL_02_PianoRollVisualInvalidationMergesDirtyRectsInSameFrame`
- `INVAL_02_PianoRollVisualInvalidationPromotesFullRepaintAcrossReasons`
- `INVAL_02_PianoRollVisualInvalidationPreservesInteractivePriorityUntilFlush`
- `INVAL_01_PianoRollVisualVBlankFlushesPendingInteractiveInvalidation`
- `INVAL_01_PianoRollRenderingDecorationTicksOnVisualVBlank`
- `TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead`
- `TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead`
- `LAYER_01_PianoRollHasNoPlayheadOverlayChild`

## L5 Applicability

- L5: Not applicable.
- Reason: 本次 closure 仍然依赖 `OpenTuneTests` + 静态审计；仓库没有针对 PianoRoll 的独立 UI E2E harness。

## Gate Policy

- 不允许默认跳过任何 blocking gate。
- 失败必须通过进程退出码暴露，而不是只打印 `[FAIL]`。
- 文档只能记录实际执行过的命令与真实命中的文件，不能再 overclaim 未审计的范围。

## Evidence To Capture

- `PianoRollComponent` 仍暴露统一 `invalidateVisual(...)` 入口，并只通过 `flushPendingVisualInvalidation()` 触达 `FrameScheduler`。
- `PianoRollVisualInvalidation.cpp` 已被纳入 L4 静态审计范围。
- `onVisualVBlankCallback(...)` 是 PianoRoll 当前 flush cadence；PianoRoll 自己不再声明 `timerCallback()`，plugin editor 不再调用 `pianoRoll_.onHeartbeatTick()`。
- `Tests/TestMain.cpp` 的 failure flag 与 `main()` 返回值已经接线。

## Final Gate

- Passed: L1 / L2 / L4 / L6 已全部在 `build-phase20-docs` fresh binary 上执行并满足 closure 条件。
- Passed: WR-01 / WR-02 / WR-03 对应的代码、测试与文档缺口都已在 current live tree 中补齐。
- Guard: 后续如果重新引入第二套 repaint API、让 PianoRoll flush 重新依赖 heartbeat/timer，或再次让测试失败不出非零退出码，必须重新打开本 phase gate。

## Execution Results

### L1 - Passed

- Command: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase20-docs -Target OpenTuneTests`
- Result: fresh configure/build completed and produced `build-phase20-docs\OpenTuneTests.exe`.
- Key output:
  - `-- Build files have been written to: build-phase20-docs`
  - `Linking CXX executable OpenTuneTests.exe`
  - `Copying ONNX Runtime DLLs to unit tests...`
- Blocker: none.

### L2 - Passed

- Command: `& ".\build-phase20-docs\OpenTuneTests.exe"`
- Result: the fresh regression binary executed the remediated Phase 20 section and the retained Phase 18/19 guards all stayed green.
- Key output:
  - `=== Phase 18: Timeline Projection Tests ===`
  - `[PASS] TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead`
  - `[PASS] TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead`
  - `=== Phase 19: Main Layer Scene Tests ===`
  - `[PASS] LAYER_01_PianoRollHasNoPlayheadOverlayChild`
  - `[PASS] LAYER_01_PianoRollContinuousScrollStillUsesProjectedPlayheadAfterOverlayRemoval`
  - `=== Phase 20: Unified Invalidation Tests ===`
  - `[PASS] INVAL_02_PianoRollVisualInvalidationMergesDirtyRectsInSameFrame`
  - `[PASS] INVAL_02_PianoRollVisualInvalidationPromotesFullRepaintAcrossReasons`
  - `[PASS] INVAL_02_PianoRollVisualInvalidationPreservesInteractivePriorityUntilFlush`
  - `[PASS] INVAL_01_PianoRollVisualVBlankFlushesPendingInteractiveInvalidation`
  - `[PASS] INVAL_01_PianoRollRenderingDecorationTicksOnVisualVBlank`
  - `Tests Complete`
- Blocker: none (`EXITCODE=0`).

### L4 - Passed

- Command: `rtk grep -n "invalidateVisual\(|flushPendingVisualInvalidation\(|onVisualVBlankCallback\(|FrameScheduler::instance\(\)\.requestInvalidate|onHeartbeatTick\(|timerCallback\(|gHasTestFailure|PianoRollComponentTestProbe|INVAL_01_PianoRollVisualVBlankFlushesPendingInteractiveInvalidation|INVAL_01_PianoRollRenderingDecorationTicksOnVisualVBlank" "Source/Standalone/UI/PianoRollComponent.h" "Source/Standalone/UI/PianoRollComponent.cpp" "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h" "Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp" "Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h" "Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp" "Source/Standalone/PluginEditor.cpp" "Source/Plugin/PluginEditor.cpp" "Tests/TestMain.cpp"`
- Result: the static audit proved the reducer `.cpp` is in scope, the component flush cadence is now VBlank-driven, and the test harness exit code is wired to failures.
- Key output:
  - `Source/Standalone/UI/PianoRollComponent.h:87` -> `void onVisualVBlankCallback(double timestampSec);`
  - `Source/Standalone/UI/PianoRollComponent.cpp:1190` / `:1199` / `:1209` -> unified `invalidateVisual(...)` overloads
  - `Source/Standalone/UI/PianoRollComponent.cpp:1222` -> `flushPendingVisualInvalidation()`
  - `Source/Standalone/UI/PianoRollComponent.cpp:1234` -> sole `FrameScheduler::instance().requestInvalidate(...)`
  - `Source/Standalone/UI/PianoRollComponent.cpp:1363` -> `onVisualVBlankCallback(...)`
  - `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h:34` / `:56` and `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp:34` / `:73` -> reducer contract + implementation both audited
  - `Source/Standalone/PluginEditor.cpp:837` -> only `arrangementView_.onHeartbeatTick()` remains; no `pianoRoll_.onHeartbeatTick()` hit
  - `Source/Plugin/PluginEditor.cpp` -> `onHeartbeatTick(` returned `0 matches`
  - `Source/Standalone/UI/PianoRollComponent.h/cpp` -> `timerCallback(` returned `0 matches`
  - `Tests/TestMain.cpp:41` -> `PianoRollComponentTestProbe`
  - `Tests/TestMain.cpp:108` / `:121` / `:4346` -> `gHasTestFailure` + failure flag store + non-zero-aware `main()` return
  - `Tests/TestMain.cpp:3892` -> `INVAL_01_PianoRollVisualVBlankFlushesPendingInteractiveInvalidation`
  - `Tests/TestMain.cpp:3943` -> `INVAL_01_PianoRollRenderingDecorationTicksOnVisualVBlank`
- Blocker: none.

### L6 - Passed

- Command: `& ".\build-phase20-docs\OpenTuneTests.exe"`
- Result: the second full-suite run matched L2 on the same fresh binary and stayed green.
- Key output:
  - `=== Phase 20: Unified Invalidation Tests ===`
  - `[PASS] INVAL_02_PianoRollVisualInvalidationMergesDirtyRectsInSameFrame`
  - `[PASS] INVAL_02_PianoRollVisualInvalidationPromotesFullRepaintAcrossReasons`
  - `[PASS] INVAL_02_PianoRollVisualInvalidationPreservesInteractivePriorityUntilFlush`
  - `[PASS] INVAL_01_PianoRollVisualVBlankFlushesPendingInteractiveInvalidation`
  - `[PASS] INVAL_01_PianoRollRenderingDecorationTicksOnVisualVBlank`
  - `Tests Complete`
- Blocker: none (`EXITCODE=0`).
