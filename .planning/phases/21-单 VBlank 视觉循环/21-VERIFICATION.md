---
phase: 21-单 VBlank 视觉循环
updated: 2026-04-15T02:53:42.9381452+08:00
status: passed
score: 3/3 must-haves verified
---

# Phase 21: 单 VBlank 视觉循环 Verification Report

**Phase Goal:** 让 `VBlankAttachment` 成为 PianoRoll 唯一视觉时钟，并把 editor 收敛成只提供 transport/state 的壳层。
**Updated:** 2026-04-15T02:53:42.9381452+08:00
**Verdict:** `passed`
**Gate status:** PASS

本报告以 current live tree、`build-phase21-docs` fresh binary 和 `21-TEST-VERIFICATION.md` 中的可复现命令输出为准。

## Observable Truths

| # | Truth | Status | Evidence |
| --- | --- | --- | --- |
| 1 | `PianoRoll` 的播放头、自动滚动、spinner 与波形增量都在同一个 `onVisualVBlankCallback(...)` tick 中推进，并在 tick 末尾只 flush 一次 | ✓ PASSED | `Source/Standalone/UI/PianoRollComponent.h:447` 与 `Source/Standalone/UI/PianoRollComponent.cpp:67` 绑定唯一 `visualVBlankAttachment_`；`Source/Standalone/UI/PianoRollComponent.cpp:1336` 定义唯一 visual tick；`Source/Standalone/UI/PianoRollComponent.cpp:1344`、`Source/Standalone/UI/PianoRollComponent.cpp:1352`、`Source/Standalone/UI/PianoRollComponent.cpp:1399`、`Source/Standalone/UI/PianoRollComponent.cpp:1421` 依次消费 correction、波形增量、滚动推进和最终 flush；`Tests/TestMain.cpp:3996` 与 `Tests/TestMain.cpp:4076` 的两个 `CLOCK_01_*` regressions 为绿。 |
| 2 | `PianoRoll` 的视觉刷新不再依赖 editor heartbeat / timer，直接 VBlank tick 就能 drain pending invalidation | ✓ PASSED | `Source/Standalone/UI/PianoRollComponent.h:47` 只继承 `juce::Component` 与 `juce::ScrollBar::Listener`，不再继承 `juce::Timer`；`Tests/TestMain.cpp:4132` 的 `CLOCK_02_PianoRollVisualLoopDoesNotNeedEditorTimerToFlush` 直接调用 `onVisualVBlankCallback(...)` 并通过；`21-TEST-VERIFICATION.md` 的 L4 shell 审计对 `pianoRoll_.onVisualVBlankCallback` / `pianoRoll_.flushPendingVisualInvalidation` / `pianoRoll_.invalidateVisual` 与 `FrameScheduler::instance().requestInvalidate(safeThis->pianoRoll_` 全部为 0 matches。 |
| 3 | Standalone 与 VST3 editor 继续只承担 transport/state 与业务壳层职责，不再 direct invalidate PianoRoll 或代打 visual cadence | ✓ PASSED | `Source/Standalone/PluginEditor.cpp:828` 的 `timerCallback()` 只保留 shell polling；`Source/Standalone/PluginEditor.cpp:837` 只推进 `arrangementView_.onHeartbeatTick()`；`Source/Standalone/PluginEditor.cpp:1493` 只刷新 `arrangementView_`；`Source/Plugin/PluginEditor.cpp:330` 的 `timerCallback()` 只做 transport/state 同步；fresh L4 对两套 editor 的 PianoRoll visual API 调用审计均为 0 matches。 |

**Score:** 3/3 truths verified

## Requirements Coverage

| Requirement | Description | Status | Evidence |
| --- | --- | --- | --- |
| `CLOCK-01` | 用户看到的播放头、自动滚动、spinner 和波形增量刷新，都在同一个 VBlank 驱动的视觉循环中推进并 flush | ✓ PASS | `Source/Standalone/UI/PianoRollComponent.cpp:1336`、`Source/Standalone/UI/PianoRollComponent.cpp:1344`、`Source/Standalone/UI/PianoRollComponent.cpp:1352`、`Source/Standalone/UI/PianoRollComponent.cpp:1399`、`Source/Standalone/UI/PianoRollComponent.cpp:1421` 与 `Tests/TestMain.cpp:3996`、`Tests/TestMain.cpp:4076`。 |
| `CLOCK-02` | 用户在 Standalone 与 VST3 中都不再依赖 editor heartbeat 或组件 `Timer` 回调，才能看到 PianoRoll 视觉更新 | ✓ PASS | `Source/Standalone/UI/PianoRollComponent.h:47`、`Tests/TestMain.cpp:4132`、fresh L4 对 editor visual API / direct invalidate 的 0-match 审计。 |
| `FLOW-02` | Standalone editor 与 VST3 editor 继续只提供 transport/state 等业务状态，不再承担 PianoRoll 视觉时钟职责，也不互相泄漏 UI 责任边界 | ✓ PASS | `Source/Standalone/PluginEditor.cpp:828`、`Source/Standalone/PluginEditor.cpp:837`、`Source/Standalone/PluginEditor.cpp:1493`、`Source/Plugin/PluginEditor.cpp:330`，以及 `21-TEST-VERIFICATION.md` L4 shell 审计结果。 |

## Artifact Verification

| Artifact | Expected | Status | Details |
| --- | --- | --- | --- |
| `.planning/phases/21-单 VBlank 视觉循环/21-TEST-VERIFICATION.md` | 记录 fresh L1/L2/L4/L6 命令、结果和关键输出 | ✓ VERIFIED | 已回写 `build-phase21-docs`、两次 full regression、精确 L4 命令以及 direct evidence。 |
| `Source/Standalone/UI/PianoRollComponent.h` / `.cpp` | `VBlankAttachment` 成为唯一视觉时钟，组件不再保留 timer/heartbeat cadence | ✓ VERIFIED | 头文件保留唯一 `onVisualVBlankCallback(...)` 与 `visualVBlankAttachment_`；实现里 visual tick 只剩一个定义和一个最终 flush。 |
| `Source/Standalone/PluginEditor.cpp` | Standalone shell 不再 direct invalidate PianoRoll | ✓ VERIFIED | 导入完成后只保留 `requestInvalidate(arrangementView_)`，`requestInvalidate(pianoRoll_)` 审计为 0 matches。 |
| `Source/Plugin/PluginEditor.cpp` | VST3 shell 不调用 PianoRoll visual-clock API | ✓ VERIFIED | fresh L4 对 `pianoRoll_.onVisualVBlankCallback` / `flushPendingVisualInvalidation` / `invalidateVisual` 审计为 0 matches。 |
| `Tests/TestMain.cpp` | Phase 21 regressions 与 retained guards 都在 fresh binary 中为绿 | ✓ VERIFIED | `Tests/TestMain.cpp:3996`、`Tests/TestMain.cpp:4076`、`Tests/TestMain.cpp:4132` 与 retained `TIME_01_*` / `LAYER_01_*` / `INVAL_*` 测试名都在 L2/L6 中通过。 |

## Key Link Verification

| From | To | Via | Status | Details |
| --- | --- | --- | --- | --- |
| `Source/Standalone/UI/PianoRollComponent.cpp` | `Tests/TestMain.cpp` | component single-tick behavior -> `CLOCK_*` regressions | ✓ WIRED | 三条 `CLOCK_*` regressions 直接 host `PianoRollComponent` 并验证 playhead/scroll、decoration/waveform drain 与 editor non-dependency。 |
| `Source/Standalone/PluginEditor.cpp` | `Source/Standalone/UI/PianoRollComponent.cpp` | shell no longer schedules direct invalidate | ✓ WIRED | shell 侧 `FrameScheduler::instance().requestInvalidate(safeThis->pianoRoll_` fresh 审计为 0 matches。 |
| `Source/Plugin/PluginEditor.cpp` | `Source/Standalone/UI/PianoRollComponent.cpp` | VST3 timer never proxies PianoRoll visual APIs | ✓ WIRED | `pianoRoll_.onVisualVBlankCallback` / `flushPendingVisualInvalidation` / `invalidateVisual` fresh 审计为 0 matches。 |
| `.planning/phases/21-单 VBlank 视觉循环/21-TEST-VERIFICATION.md` | `.planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md` | fresh gate result -> requirement verdict | ✓ WIRED | 本报告直接消费 L1/L2/L4/L6 的命令与输出，不再复用旧 binary 或口头结论。 |

## Behavioral Spot-Checks

| Behavior | Command | Result | Status |
| --- | --- | --- | --- |
| fresh build dir 重新生成 Phase 21 regression binary | `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase21-docs -Target OpenTuneTests` | 成功生成 `build-phase21-docs/OpenTuneTests.exe` | ✓ PASS |
| single-VBlank regressions 与 retained guards 在 fresh binary 上通过 | `& ".\build-phase21-docs\OpenTuneTests.exe"` | 两次 run 都输出 `Phase 21: Single VBlank Visual Loop Tests`，三个 `CLOCK_*` 与 retained `TIME_01_*` / `LAYER_01_*` / `INVAL_*` 全绿 | ✓ PASS |
| 当前 live tree 只保留一个 visual tick 定义和一个 VBlank attachment | `rtk rg -n "^void PianoRollComponent::onVisualVBlankCallback\(" "Source/Standalone/UI/PianoRollComponent.cpp"` + `rtk rg -n "juce::VBlankAttachment" "Source/Standalone/UI/PianoRollComponent.cpp"` | 分别命中 `Source/Standalone/UI/PianoRollComponent.cpp:1336` 与 `Source/Standalone/UI/PianoRollComponent.cpp:67` | ✓ PASS |
| editor shell 不再直连 PianoRoll visual API / direct invalidate | `rtk rg -n "pianoRoll_\.onVisualVBlankCallback|pianoRoll_\.flushPendingVisualInvalidation|pianoRoll_\.invalidateVisual|FrameScheduler::instance\(\)\.requestInvalidate\(safeThis->pianoRoll_" "Source/Standalone/PluginEditor.cpp" "Source/Plugin/PluginEditor.cpp"` | 输出为空，证明审计结果为 0 matches | ✓ PASS |

## Remaining Risks

- `timerCallback()` 仍保留在两套 editor shell 中作为 transport/state polling 与业务队列调度，这一边界必须在 Phase 22 继续守住，不能再次扩张回 PianoRoll visual flush。
- `FrameScheduler` 仍是 `PianoRollComponent.cpp:1234` 的单一 flush bridge；它已经不再构成第二时钟，但后续 Clip-aware repaint 优化仍需确保不会重新旁路它。
- Wave 3 的 VST3 shell 提交包含用户已批准的 Phase 21 文件边界内同文件脏改动；fresh build 和 full regression 已在该 live tree 上验证通过，因此当前 closure 以这份真实树状态为准。
