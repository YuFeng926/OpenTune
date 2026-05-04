---
phase: 20-统一失效入口
updated: 2026-04-15T01:48:31.2099072+08:00
status: passed
score: 5/5 must-haves verified
---

# Phase 20: 统一失效入口 Verification Report

**Phase Goal:** 让所有 PianoRoll 视觉变化都先进入一个统一 invalidation 入口，再由同一套待刷状态决定 flush 范围。
**Updated:** 2026-04-15T01:48:31.2099072+08:00
**Verdict:** `passed`
**Gate status:** PASS

本报告以修复 WR-01 / WR-02 / WR-03 之后的 current live tree、`build-phase20-docs` fresh binary 和 `20-TEST-VERIFICATION.md` 中的可复现命令输出为准。

## Observable Truths

| # | Truth | Status | Evidence |
| --- | --- | --- | --- |
| 1 | 用户触发编辑、拖拽、滚动、缩放或数据更新时，`PianoRoll` 只经由统一 `invalidateVisual(...)` 入口登记视觉变化 | ✓ PASSED | `Source/Standalone/UI/PianoRollComponent.h:214`、`Source/Standalone/UI/PianoRollComponent.h:215`、`Source/Standalone/UI/PianoRollComponent.h:217` 暴露唯一 public invalidation API；`Source/Standalone/UI/PianoRollComponent.cpp:1190`、`Source/Standalone/UI/PianoRollComponent.cpp:1199`、`Source/Standalone/UI/PianoRollComponent.cpp:1209` 实现统一入口；`Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h:89` 与 `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp:28`、`Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp:171`、`Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp:209` 把工具交互都回到 `ctx_.invalidateVisual()`。 |
| 2 | 同一显示帧内的多个 dirty request 会先合并到 `pendingVisualInvalidation_`，最终只通过一个 flush 出口申请重绘 | ✓ PASSED | `Source/Standalone/UI/PianoRollComponent.h:418` 持有 `pendingVisualInvalidation_`；`Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp:34`、`Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp:73` 定义 merge/flush rule；`Source/Standalone/UI/PianoRollComponent.cpp:1222` 统一 flush，`Source/Standalone/UI/PianoRollComponent.cpp:1234` 是唯一 `FrameScheduler::instance().requestInvalidate(...)` 命中。 |
| 3 | paused state 下的 interactive invalidation 已经能在下一次 visual VBlank 被 component-level wiring drain，不再依赖 PianoRoll heartbeat / timer | ✓ PASSED | `Source/Standalone/UI/PianoRollComponent.h:87` 与 `Source/Standalone/UI/PianoRollComponent.cpp:1363` 定义当前 visual VBlank callback；`Tests/TestMain.cpp:3892` 到 `Tests/TestMain.cpp:3939` 的 `INVAL_01_PianoRollVisualVBlankFlushesPendingInteractiveInvalidation` 直接实例化桌面 host 中的 `PianoRollComponent` 并验证 pending invalidation 会被 VBlank flush；`Source/Standalone/UI/PianoRollComponent.h/cpp` 对 `timerCallback(` 的静态审计为 0 matches，`Source/Plugin/PluginEditor.cpp` 对 `onHeartbeatTick(` 的静态审计为 0 matches。 |
| 4 | render decoration 的持续视觉刷新现在由同一个 visual VBlank loop 推进，而不是 PianoRoll 自己的 timer | ✓ PASSED | `Source/Standalone/UI/PianoRollComponent.cpp:1374` 在 visual VBlank 中登记 decoration invalidation；`Tests/TestMain.cpp:3943` 到 `Tests/TestMain.cpp:3984` 的 `INVAL_01_PianoRollRenderingDecorationTicksOnVisualVBlank` 证明连续两个 VBlank tick 都会推进 flush timestamp。 |
| 5 | Phase 20 的 fresh regression gate 现在是 blocking 的，verification 文档也不再 overclaim 审计范围 | ✓ PASSED | `Tests/TestMain.cpp:108`、`Tests/TestMain.cpp:121`、`Tests/TestMain.cpp:4346` 把 failure flag 与 `main()` 返回值接线；fresh L4 明确把 `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp`、`Source/Standalone/PluginEditor.cpp`、`Source/Plugin/PluginEditor.cpp` 和两条 `INVAL_01_*` regressions 全部纳入审计；`20-TEST-VERIFICATION.md` 现已与实际执行命令一致。 |

**Score:** 5/5 truths verified

## Requirements Coverage

| Requirement | Description | Status | Evidence |
| --- | --- | --- | --- |
| `INVAL-01` | 用户触发任何会改变 PianoRoll 视觉状态的操作时，刷新请求只通过一个统一的 `invalidateVisual(...)` 入口进入 | ✓ PASS | unified invalidation public API、tool hook 和所有 fresh static audit 结果都只命中 `invalidateVisual(...)`；旧 `requestInteractiveRepaint` / `requestRepaint` 未再出现。 |
| `INVAL-02` | 用户在同一显示帧内触发的多种视觉变化会先合并到统一待刷状态，再决定最小必要 dirty 区域 | ✓ PASS | `PianoRollVisualInvalidationState` + `makeVisualFlushDecision(...)` 继续提供 merge/promotion/dirty clipping；`Tests/TestMain.cpp` 中三个 `INVAL_02_*` reducer journeys 和两个 `INVAL_01_*` component-level journeys 同时通过。 |

## Artifact Verification

| Artifact | Expected | Status | Details |
| --- | --- | --- | --- |
| `.planning/phases/20-统一失效入口/20-TEST-VERIFICATION.md` | 记录 fresh L1/L2/L4/L6 证据，且命令与结果一致 | ✓ VERIFIED | 已回写 fresh build/test commands、两条新 component-level regressions、reducer `.cpp` 审计范围以及 blocking harness evidence。 |
| `Source/Standalone/UI/PianoRollComponent.h` | 暴露统一 invalidation contract 与 visual VBlank callback | ✓ VERIFIED | `invalidateVisual(...)` overload、`flushPendingVisualInvalidation()` 和 `onVisualVBlankCallback(...)` 都已在头文件中明确声明。 |
| `Source/Standalone/UI/PianoRollComponent.cpp` | 所有视觉变化都登记到 pending state，再在 visual VBlank 中统一 flush | ✓ VERIFIED | `invalidateVisual(...)` 只 merge；`onVisualVBlankCallback(...)` 负责消费 correction/render/playhead invalidation；`flushPendingVisualInvalidation()` 仍是唯一 scheduler bridge。 |
| `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h` / `.cpp` | reducer contract 与实现都已纳入审计 | ✓ VERIFIED | `PianoRollVisualInvalidationState` 与 `makeVisualFlushDecision(...)` 的 `.h` / `.cpp` 都已在 fresh L4 command 中直接命中。 |
| `Source/Standalone/PluginEditor.cpp` / `Source/Plugin/PluginEditor.cpp` | editor 不再承担 PianoRoll heartbeat flush | ✓ VERIFIED | `Source/Standalone/PluginEditor.cpp:837` 只剩 `arrangementView_.onHeartbeatTick()`；`Source/Plugin/PluginEditor.cpp` 对 `onHeartbeatTick(` 的 fresh static audit 为 0 matches。 |
| `Tests/TestMain.cpp` | Phase 20 journeys + blocking harness | ✓ VERIFIED | `PianoRollComponentTestProbe`、两条 `INVAL_01_*` journeys、`gHasTestFailure`、`return gHasTestFailure...` 全部在 fresh L4 中直接命中。 |

## Key Link Verification

| From | To | Via | Status | Details |
| --- | --- | --- | --- | --- |
| `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp` | `Source/Standalone/UI/PianoRollComponent.cpp` | `ctx_.invalidateVisual()` -> component invalidation entry | ✓ WIRED | `PianoRollToolHandler` 继续只通过 `Context::invalidateVisual` 回调 component。 |
| `Source/Standalone/UI/PianoRollComponent.cpp` | `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp` | pending state -> `makeVisualFlushDecision(...)` | ✓ WIRED | `flushPendingVisualInvalidation()` 直接消费 reducer decision。 |
| `Source/Standalone/UI/PianoRollComponent.cpp` | current visual cadence | `onVisualVBlankCallback(...)` | ✓ WIRED | pending invalidation、playhead、render decoration 和 waveform incremental update 全都在同一个 visual VBlank callback 中推进。 |
| `Tests/TestMain.cpp` fail path | process exit code | `gHasTestFailure` -> `main()` | ✓ WIRED | 测试失败现在会影响进程返回值；fresh run 输出为绿且 `EXITCODE=0` 因此有意义。 |

## Behavioral Spot-Checks

| Behavior | Command | Result | Status |
| --- | --- | --- | --- |
| fresh build dir 是否生成新的 regression binary | `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase20-docs -Target OpenTuneTests` | 成功生成 `build-phase20-docs/OpenTuneTests.exe` | ✓ PASS |
| remediated Phase 20 journeys 是否在 fresh binary 上通过 | `& ".\build-phase20-docs\OpenTuneTests.exe"` | 输出包含 3 个 `INVAL_02_*` 和 2 个 `INVAL_01_*` passing tests | ✓ PASS |
| test harness 退出码是否已接线 | `& ".\build-phase20-docs\OpenTuneTests.exe" > $null; Write-Output "EXITCODE=$LASTEXITCODE"` | 输出 `EXITCODE=0`，且 `Tests/TestMain.cpp` 已把失败状态接到 `main()` 返回值 | ✓ PASS |
| static audit 是否覆盖 reducer `.cpp`、visual VBlank cadence 和 blocking harness | `rtk grep -n "invalidateVisual\(|flushPendingVisualInvalidation\(|onVisualVBlankCallback\(|FrameScheduler::instance\(\)\.requestInvalidate|onHeartbeatTick\(|timerCallback\(|gHasTestFailure|PianoRollComponentTestProbe|INVAL_01_PianoRollVisualVBlankFlushesPendingInteractiveInvalidation|INVAL_01_PianoRollRenderingDecorationTicksOnVisualVBlank" ...` | 命中 reducer `.cpp`、`onVisualVBlankCallback(...)`、`gHasTestFailure`、`PianoRollComponentTestProbe`，并证明 `timerCallback(` / plugin `onHeartbeatTick(` 归零 | ✓ PASS |

## Remaining Risks

- `FrameScheduler` 仍作为当前 flush bridge 存在，但它已被压缩到 `PianoRollComponent.cpp:1234` 的单一出口；它不再构成 WR-01 中的 cadence 退化问题。
- Phase 21 / Phase 22 仍有自己的独立目标（例如更彻底的单循环表述和 clip-aware repaint 性能守护），本次修复只关闭 review 中的 3 条 warning，不对后续 phase 成功条件做超额认领。
