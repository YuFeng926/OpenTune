# Phase 21: Single VBlank Visual Loop Tests

## Scope

本文件记录 Phase 21 的 fresh gate 证据。验收真相源保持不变：PianoRoll 的播放头、自动滚动、spinner、render decoration 和波形增量刷新必须全部收敛到同一个 `onVisualVBlankCallback(...)` cadence；Standalone 与 VST3 editor 只提供 transport/state 与业务壳层，不再承担 PianoRoll 视觉时钟或 direct invalidate 责任。

Phase 18/19/20 的 projected playhead、主层场景归一与 unified invalidation 继续作为回归底线。任何实现都必须同时守住 `CLOCK-01`、`CLOCK-02` 和 `FLOW-02`，不能把“已经有 `VBlankAttachment`”误判成收敛完成。

## Verification Levels

| Level | Goal | Command | Status | Pass Criteria |
| --- | --- | --- | --- | --- |
| L1 | 构建 fresh `OpenTuneTests` binary，确保 Phase 21 single-VBlank regressions 已进入可执行产物 | `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase21-docs -Target OpenTuneTests` | PASS | 2026-04-15 fresh configure/build 完成并产出 `build-phase21-docs/OpenTuneTests.exe`。 |
| L2 | 运行 full regression binary，确认 single-VBlank cadence、editor 非依赖与 Phase 18/19/20 guards 同时为绿 | `& ".\build-phase21-docs\OpenTuneTests.exe"` | PASS | 输出包含 `Phase 21: Single VBlank Visual Loop Tests`；三个 `CLOCK_*` journeys 通过；`TIME_01_*`、`LAYER_01_*`、`INVAL_02_*` 与 `INVAL_01_*` 继续为绿。 |
| L4 | 静态审计单一 visual tick、单一 `juce::VBlankAttachment`、editor visual-clock 残留与 direct invalidate 旁路 | `rtk rg -n "^void PianoRollComponent::onVisualVBlankCallback\(" "Source/Standalone/UI/PianoRollComponent.cpp"` + `rtk rg -n "juce::VBlankAttachment" "Source/Standalone/UI/PianoRollComponent.cpp"` + `rtk rg -n "pianoRoll_\.onVisualVBlankCallback|pianoRoll_\.flushPendingVisualInvalidation|pianoRoll_\.invalidateVisual|FrameScheduler::instance\(\)\.requestInvalidate\(safeThis->pianoRoll_" "Source/Standalone/PluginEditor.cpp" "Source/Plugin/PluginEditor.cpp"` | PASS | `Source/Standalone/UI/PianoRollComponent.cpp` 中 `onVisualVBlankCallback(` 定义恰好 1 处；`juce::VBlankAttachment` 恰好 1 处；两套 editor shell 对 PianoRoll visual API / direct invalidate 审计结果为 0 matches。 |
| L6 | 再次运行 full regression binary，确认 Phase 21 gate 可重复通过 | `& ".\build-phase21-docs\OpenTuneTests.exe"` | PASS | 第二次 full-suite run 与 L2 一致，Phase 21 / 20 / 19 / 18 sections 全绿。 |

## Required Journeys

- `CLOCK_01_PianoRollSingleVisualTickFlushesPlayheadAndScroll`
- `CLOCK_01_PianoRollSingleVisualTickDrainsDecorationAndWaveformWork`
- `CLOCK_02_PianoRollVisualLoopDoesNotNeedEditorTimerToFlush`
- `TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead`
- `TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead`
- `LAYER_01_PianoRollHasNoPlayheadOverlayChild`
- `INVAL_02_PianoRollVisualInvalidationMergesDirtyRectsInSameFrame`
- `INVAL_01_PianoRollVisualVBlankFlushesPendingInteractiveInvalidation`

## L5 Applicability

- L5: Not applicable.
- Reason: 当前仓库没有独立的 PianoRoll UI E2E harness；Phase 21 继续依赖 `OpenTuneTests` + 静态审计证明 visual cadence 与 editor 边界。

## Gate Policy

- 不允许默认跳过任何 blocking gate。
- 失败必须通过进程退出码或命令失败暴露，而不是只记日志。
- 文档只记录实际执行的命令和直接证据，不能把未验证的 editor 行为写成已收敛事实。

## Execution Evidence

### L1 Fresh Build

- Command: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase21-docs -Target OpenTuneTests`
- Result: PASS
- Key output:
  - `Build files have been written to: build-phase21-docs`
  - `Linking CXX executable OpenTuneTests.exe`
- Notes: 仅出现既有 `LockFreeQueue` / Standalone deprecated API warnings，无新的 Phase 21 blocker。

### L2 Full Regression

- Command: `& ".\build-phase21-docs\OpenTuneTests.exe"`
- Result: PASS
- Key output:
  - `=== Phase 21: Single VBlank Visual Loop Tests ===`
  - `[PASS] CLOCK_01_PianoRollSingleVisualTickFlushesPlayheadAndScroll`
  - `[PASS] CLOCK_01_PianoRollSingleVisualTickDrainsDecorationAndWaveformWork`
  - `[PASS] CLOCK_02_PianoRollVisualLoopDoesNotNeedEditorTimerToFlush`
  - `[PASS] TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead`
  - `[PASS] TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead`
  - `[PASS] LAYER_01_PianoRollHasNoPlayheadOverlayChild`
  - `[PASS] INVAL_02_PianoRollVisualInvalidationMergesDirtyRectsInSameFrame`
  - `[PASS] INVAL_01_PianoRollVisualVBlankFlushesPendingInteractiveInvalidation`

### L4 Static Audit

- Command 1: `rtk rg -n "^void PianoRollComponent::onVisualVBlankCallback\(" "Source/Standalone/UI/PianoRollComponent.cpp"`
  - Result: PASS
  - Output: `Source/Standalone/UI/PianoRollComponent.cpp:1336:void PianoRollComponent::onVisualVBlankCallback(double timestampSec)`
- Command 2: `rtk rg -n "juce::VBlankAttachment" "Source/Standalone/UI/PianoRollComponent.cpp"`
  - Result: PASS
  - Output: `Source/Standalone/UI/PianoRollComponent.cpp:67:    visualVBlankAttachment_ = std::make_unique<juce::VBlankAttachment>(`
- Command 3: `rtk rg -n "pianoRoll_\.onVisualVBlankCallback|pianoRoll_\.flushPendingVisualInvalidation|pianoRoll_\.invalidateVisual|FrameScheduler::instance\(\)\.requestInvalidate\(safeThis->pianoRoll_" "Source/Standalone/PluginEditor.cpp" "Source/Plugin/PluginEditor.cpp"`
  - Result: PASS
  - Output: no matches
- Additional shell evidence:
  - `Source/Standalone/PluginEditor.cpp:828` still defines `timerCallback()` but `Source/Standalone/PluginEditor.cpp:837` shows it only advancing `arrangementView_.onHeartbeatTick()` and business queue work.
  - `Source/Standalone/PluginEditor.cpp:1493` keeps shell-local `requestInvalidate(arrangementView_)`; `FrameScheduler::instance().requestInvalidate(safeThis->pianoRoll_` audit result is 0 matches.
  - `Source/Plugin/PluginEditor.cpp:330` still defines `timerCallback()` for transport/state polling, with no PianoRoll visual API hits.

### L6 Repeat Regression

- Command: `& ".\build-phase21-docs\OpenTuneTests.exe"`
- Result: PASS
- Key output:
  - 第二次输出再次包含 `=== Phase 21: Single VBlank Visual Loop Tests ===`
  - 三个 `CLOCK_*` journeys 与 retained `TIME_01_*` / `LAYER_01_*` / `INVAL_*` guards 全部再次通过

## Evidence To Capture

- `PianoRoll` 的播放头、scroll、spinner、render decoration 与波形增量都从 `onVisualVBlankCallback(...)` 推进。
- editor timer 不直接驱动 PianoRoll flush。
- Standalone/VST3 editor 只剩状态同步与异步业务壳层职责。

## Final Gate

- PASS: L1 / L2 / L4 / L6 全部通过，Phase 21 gate 可以关闭。
- Guard: 如果重新引入 editor timer -> PianoRoll visual flush、重新出现 `FrameScheduler::instance().requestInvalidate(safeThis->pianoRoll_`，或让 PianoRoll 存在第二个 visual tick 入口，必须重新打开本 phase gate。
