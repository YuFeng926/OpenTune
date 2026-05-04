---
phase: 19-主层场景归一
updated: 2026-04-14T15:23:15+08:00
status: passed
score: 3/3 must-haves verified
gaps: []
---

# Phase 19: 主层场景归一 Verification Report

**Phase Goal:** 让 `PianoRollComponent` 重新成为唯一场景宿主，使播放头与内容主层共用同一坐标系和绘制语义。
**Updated:** 2026-04-14T15:23:15+08:00
**Verdict:** `passed`
**Gate status:** PASS

本报告以当前 live tree、`build-phase19-docs` fresh binary 和可复现命令输出为准。

## Observable Truths

| # | Truth | Status | Evidence |
| --- | --- | --- | --- |
| 1 | 播放头由主层与音符、波形、F0 共用同一绘制路径 | ✓ PASSED | `Source/Standalone/UI/PianoRollComponent.h:390` 定义 `mainLayerPlayheadSeconds_`，`Source/Standalone/UI/PianoRollComponent.h:423` 定义 `playheadColour_`；`Source/Standalone/UI/PianoRoll/PianoRollRenderer.h:59`、`:60`、`:61`、`:84` 暴露 `showPlayhead` / `playheadSeconds` / `playheadColour` / `drawPlayhead(...)`；`Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:518`、`:523`、`:528` 在主层 renderer 中绘制播放头；`Source/Standalone/UI/PianoRollComponent.cpp:698` 在内容区 clip 内调用 `renderer_->drawPlayhead(g, ctx);`；fresh L2/L6 都通过 `LAYER_01_PianoRollHasNoPlayheadOverlayChild`。 |
| 2 | 滚动、缩放、可见区调整不再同步第二视觉层坐标系 | ✓ PASSED | `rtk rg` 静态审计在 `Source/Standalone/UI/PianoRollComponent.h/cpp` 中没有命中 `playheadOverlay_` 或 `PlayheadOverlayComponent`；fresh L2/L6 通过 `LAYER_01_PianoRollContinuousScrollStillUsesProjectedPlayheadAfterOverlayRemoval` 与 `LAYER_02_PianoRollPageScrollStillUsesProjectedPlayheadAfterOverlayRemoval`。 |
| 3 | viewport 变化与播放头位置变化命中同一套 dirty 语义 | ✓ PASSED | `Source/Standalone/UI/PianoRollComponent.cpp:1288` 把播放头状态写回 `mainLayerPlayheadSeconds_`，`Source/Standalone/UI/PianoRollComponent.cpp:1761` 把该状态注入 `RenderContext`；fresh `build-phase19-docs/OpenTuneTests.exe` 输出包含 `=== Phase 19: Main Layer Scene Tests ===`，证明新的单层 contract 已编入并执行。 |

**Score:** 3/3 truths verified

## Requirements Coverage

| Requirement | Description | Status | Evidence |
| --- | --- | --- | --- |
| `LAYER-01` | 播放头由主层绘制，不再依赖独立 overlay 组件 | ✓ PASS | `build-phase19-docs/OpenTuneTests.exe` 输出包含 `[PASS] LAYER_01_PianoRollHasNoPlayheadOverlayChild`；`Source/Standalone/UI/PianoRollComponent.cpp:698` 命中 `renderer_->drawPlayhead(g, ctx);`；`Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:518` 提供主层 `drawPlayhead(...)` 实现。 |
| `LAYER-02` | 滚动、缩放、可见区调整不再同步第二个视觉层坐标系 | ✓ PASS | fresh L2/L6 输出包含 `[PASS] LAYER_01_PianoRollContinuousScrollStillUsesProjectedPlayheadAfterOverlayRemoval` 与 `[PASS] LAYER_02_PianoRollPageScrollStillUsesProjectedPlayheadAfterOverlayRemoval`；L4 静态审计确认 `PianoRollComponent.h/cpp` 不再包含 overlay 同步符号。 |

## Artifact Verification

| Artifact | Expected | Status | Details |
| --- | --- | --- | --- |
| `Tests/TestMain.cpp` | 存在 Phase 19 regressions，并接入主测试入口 | ✓ VERIFIED | `Tests/TestMain.cpp:3447` 定义 `runPhase19MainLayerSceneTests()`，`Tests/TestMain.cpp:3451`、`:3480`、`:3533` 定义三个 `LAYER_*` 用例，`Tests/TestMain.cpp:3933` 仍在 `main()` 中调用该测试节。 |
| `Source/Standalone/UI/PianoRollComponent.h` | 不再持有 overlay 成员，并暴露主层状态 | ✓ VERIFIED | `mainLayerPlayheadSeconds_` 与 `playheadColour_` 分别位于 `Source/Standalone/UI/PianoRollComponent.h:390` 与 `:423`；L4 审计未在该文件中命中 `PlayheadOverlayComponent`。 |
| `Source/Standalone/UI/PianoRollComponent.cpp` | `paint()` 走主层播放头绘制，且不再同步 overlay | ✓ VERIFIED | `Source/Standalone/UI/PianoRollComponent.cpp:698` 命中 `renderer_->drawPlayhead(g, ctx);`，`Source/Standalone/UI/PianoRollComponent.cpp:1761` 命中 `ctx.playheadSeconds = mainLayerPlayheadSeconds_;`；L4 审计未在该文件中命中 overlay 同步符号。 |
| `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h` | 存在 `drawPlayhead(...)` contract | ✓ VERIFIED | `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h:59`、`:60`、`:61`、`:84` 暴露主层播放头 contract。 |
| `.planning/phases/19-主层场景归一/19-TEST-VERIFICATION.md` | gate 结果已落盘 | ✓ VERIFIED | L1/L2/L4/L6 已全部回写为 `Passed`，且命令都绑定到 `build-phase19-docs`。 |

## Key Link Verification

| From | To | Via | Status | Details |
| --- | --- | --- | --- | --- |
| `Tests/TestMain.cpp` | `Source/Standalone/UI/PianoRollComponent.h` | 测试直接实例化 PianoRoll 接口验证 single-host 目标 | ✓ WIRED | `Tests/TestMain.cpp` 继续用 `PianoRollComponent` 驱动 Phase 19 回归；`PlayheadOverlayComponent` 只保留在测试里做负向断言。 |
| `Source/Standalone/UI/PianoRollComponent.cpp` | `Source/PluginProcessor.cpp` | `readProjectedPlayheadTime()` 保持 sample-authoritative 播放头时间源 | ✓ WIRED | `Source/Standalone/UI/PianoRollComponent.cpp:1254` 定义 `readProjectedPlayheadTime()`，`Source/Standalone/UI/PianoRollComponent.cpp:1322` 与 `:1414` 继续在滚动路径读取投影播放头时间。 |
| `Source/Standalone/UI/PianoRollComponent.cpp` | `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp` | `paint()` 在内容区 clip 内直接绘制播放头 | ✓ WIRED | `Source/Standalone/UI/PianoRollComponent.cpp:698` -> `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:518`。 |

## Behavioral Spot-Checks

| Behavior | Command | Result | Status |
| --- | --- | --- | --- |
| fresh configure/build 是否能生成新的 Phase 19 二进制 | `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase19-docs -Target OpenTuneTests` | 成功生成 `build-phase19-docs/OpenTuneTests.exe`，并从 `ThirdParty/` 解析 ARA / ONNX Runtime | ✓ PASS |
| fresh 回归套件是否执行 Phase 19 主层场景测试 | `& ".\build-phase19-docs\OpenTuneTests.exe"` | 输出包含 `=== Phase 19: Main Layer Scene Tests ===` 和三个 passing `LAYER_*` 用例 | ✓ PASS |
| Phase 18 projected playhead 语义是否继续守住 | `& ".\build-phase19-docs\OpenTuneTests.exe"` | `[PASS] TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead` 与 `[PASS] TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead` 继续通过 | ✓ PASS |
| PianoRoll 单层 contract 是否通过静态审计 | `rtk rg -n "playheadOverlay_|PlayheadOverlayComponent|drawPlayhead|runPhase19MainLayerSceneTests|LAYER_01_PianoRollHasNoPlayheadOverlayChild|LAYER_01_PianoRollContinuousScrollStillUsesProjectedPlayheadAfterOverlayRemoval|LAYER_02_PianoRollPageScrollStillUsesProjectedPlayheadAfterOverlayRemoval|TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead|TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead" "Source/Standalone/UI/PianoRollComponent.h" "Source/Standalone/UI/PianoRollComponent.cpp" "Source/Standalone/UI/PianoRoll/PianoRollRenderer.h" "Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp" "Tests/TestMain.cpp"` | 只命中 renderer 与 tests 中的正向/负向证明点，`PianoRollComponent.h/cpp` 不再命中 overlay 持有与同步符号 | ✓ PASS |

## Consistency Check

- `19-TEST-VERIFICATION.md`、`ROADMAP.md`、`STATE.md` 现在都绑定到同一份 fresh `build-phase19-docs` 证据，不再复用 stale `build/` 产物。
- Phase 19 先前的 `gaps_found` 结论已被新的 live tree 和 fresh gate 结果替换，文档不再保留“源码未落地 / 环境阻断”的旧状态描述。

## Remaining Risks

- `requestInteractiveRepaint()` / `FrameScheduler` 仍在当前树中，属于 Phase 20 的统一失效入口范围，不作为 Phase 19 失败项。
- `onHeartbeatTick()` 与 `timerCallback()` 仍在当前树中，属于 Phase 21 的单 VBlank 视觉循环范围，不作为 Phase 19 失败项。
- `PlayheadOverlayComponent` 仍存在于仓库中，因为 `ArrangementViewComponent` 与 tests 仍依赖它；Phase 19 的 requirement 只要求 `PianoRollComponent` 不再持有它。
