---
phase: 22-Clip-Aware 刷新与集成守护
updated: 2026-04-15T12:00:00Z
status: passed
score: 4/4 must-haves verified
---

# Phase 22: Clip-Aware 刷新与集成守护 Verification Report

**Phase Goal:** 让单层 PianoRoll 在 strip repaint 下仍然可承受，并确认既有播放头/滚动/时间轴行为在新结构下保持正确。
**Updated:** 2026-04-15T12:00:00Z
**Verdict:** `passed`
**Gate status:** PASS

本报告以 current live tree、`build-phase22-docs` fresh binary 和 `22-TEST-VERIFICATION.md` 中的可复现命令输出为准。

## Observable Truths

| # | Truth | Status | Evidence |
| --- | --- | --- | --- |
| 1 | `drawNotes()` 已改成 visible note window 遍历，不再对整个 `notes` 向量做 full scan | ✓ VERIFIED | `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:587` 调用 `computeVisibleTimeWindow()`；`Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:591-612` 用 `std::lower_bound` 建立 visible iterator window，遍历只限 `firstVisibleNote` 到 `lastVisibleNote`；grep 对 `for (const auto& note : notes)` 返回 0 matches。 |
| 2 | waveform / F0 / notes 共用统一 renderer visible time window 语义 | ✓ VERIFIED | `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:27` 定义 `computeVisibleTimeWindow()`；`Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:131`（waveform）、`Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:587`（notes）、`Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:696`（F0）都调用同一 helper；`Tests/TestMain.cpp:4517-4521` 静态审计断言 helper 被调用至少 4 次。 |
| 3 | strip dirty / 单入口 / 单 flush bridge 仍由 `getPlayheadDirtyBounds()` + `updateMainLayerPlayhead()` + `invalidateVisual()` + `flushPendingVisualInvalidation()` 维持 | ✓ VERIFIED | `Source/Standalone/UI/PianoRollComponent.cpp:1409` 定义 `getPlayheadDirtyBounds()`；`Source/Standalone/UI/PianoRollComponent.cpp:1421` 定义 `updateMainLayerPlayhead()` 并内部调用 `invalidateVisual()`；`Source/Standalone/UI/PianoRollComponent.cpp:1308` 定义统一 `invalidateVisual()` 入口；`Source/Standalone/UI/PianoRollComponent.cpp:1340` 定义唯一 `flushPendingVisualInvalidation()`；`Source/Standalone/UI/PianoRollComponent.cpp:1539` 在 visual tick 末尾调用一次 flush；`Tests/TestMain.cpp:4410-4484` 的 `PAINT_01_*` 测试验证 dirty strip 非 full-width、pending 在 tick 后 drain、flush 时间戳前进。 |
| 4 | `fitToScreen()`、continuous/page scroll、playhead projection 继续命中 sample-authoritative clip timeline projection | ✓ VERIFIED | `Source/Standalone/UI/PianoRollComponent.cpp:1887-1934` 的 `fitToScreen()` 优先调用 `getActiveClipTimelineProjection()`（line 1909）；`Source/Standalone/UI/PianoRollComponent.cpp:1454-1539` 的 `onVisualVBlankCallback()` 调用 `readProjectedPlayheadTime()`（line 1483）并分别处理 continuous/page scroll；`Tests/TestMain.cpp:4527-4588` 的 `FLOW_01_*` 测试断言 projected start 锚定琴键边界、projected duration 占满可见宽度、scroll offset 已推进。 |

**Score:** 4/4 truths verified

## Requirements Coverage

| Requirement | Description | Status | Evidence |
| --- | --- | --- | --- |
| `PAINT-01` | 用户看到播放头移动时，PianoRoll 主层只重绘必要 strip 脏区，不会因为单层化而退回第二层播放头方案 | ✓ PASS | `Tests/TestMain.cpp:4410` 的 `PAINT_01_PianoRollPlayheadDirtyStripStillUsesSingleFlushBridge` 验证 dirty strip 非 full-width、pending drain、flush timestamp 前进；live tree 的 `getPlayheadDirtyBounds()` / `updateMainLayerPlayhead()` / `invalidateVisual()` / `flushPendingVisualInvalidation()` 构成完整 strip dirty chain。 |
| `PAINT-02` | 用户在可见范围外的音符、波形和 F0 不会在 strip repaint 中被无意义全量遍历，主层绘制持续按可见范围裁剪 | ✓ PASS | `Tests/TestMain.cpp:4487` 的 `PAINT_02_PianoRollDrawNotesUsesVisibleNoteWindow` 静态审计排除 full scan 并确认 `std::lower_bound` 存在；`Tests/TestMain.cpp:4509` 的 `PAINT_02_PianoRollRendererUsesUnifiedVisibleTimeWindowForNotesWaveformAndF0` 确认 helper 被三条主内容路径共享；live tree 的 `PianoRollRenderer.cpp` 已落实 visible window binary search + iterator range。 |
| `FLOW-01` | 用户继续获得现有播放头投影、continuous scroll、page scroll 和 fit-to-screen 行为，不因 repaint 架构收敛而回归 | ✓ PASS | `Tests/TestMain.cpp:4527` 的 `FLOW_01_PianoRollFitToScreenUsesClipTimelineProjection` 验证 fit-to-screen 正确锚定并缩放到 clip projection；retained `TIME_01_*`（continuous/page scroll）与 `LAYER_01_*`（no overlay child）在同一 fresh binary 中全绿；live tree 的 `fitToScreen()` 与 `onVisualVBlankCallback()` 都优先命中 processor clip timeline projection。 |

## Artifact Verification

| Artifact | Expected | Status | Details |
| --- | --- | --- | --- |
| `.planning/phases/22-Clip-Aware 刷新与集成守护/22-TEST-VERIFICATION.md` | 记录 fresh L1/L2/L4/L6 命令、结果和关键输出 | ✓ VERIFIED | 已回写 `build-phase22-docs` 工具链命令、两次 full regression 输出、L4 静态审计 grep 结果与最终 PASS 判定。 |
| `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp` | 统一 visible time window helper；drawNotes 用 binary search + visible iterator window | ✓ VERIFIED | line 27 定义 `computeVisibleTimeWindow()`；lines 587-612 的 `drawNotes()` 完整实现 visible note window；lines 131/587/696 分别在 waveform/notes/F0 中调用 helper；grep 确认无 full scan。 |
| `Source/Standalone/UI/PianoRollComponent.cpp` | strip dirty + single flush bridge 完整闭环；fitToScreen 命中 clip projection | ✓ VERIFIED | lines 1409-1433 定义 playhead dirty contract；line 1909 的 `fitToScreen()` 调用 `getActiveClipTimelineProjection()`；line 1539 在 visual tick 末尾唯一 flush；lines 1454-1539 的 `onVisualVBlankCallback()` 消费 projected playhead 并推进 continuous/page scroll。 |
| `Source/Standalone/PluginEditor.cpp` | shell 不新增 cadence / direct invalidate residue | ✓ VERIFIED | grep 对 `pianoRoll_.onVisualVBlankCallback` / `pianoRoll_.invalidateVisual` / `requestInvalidate(pianoRoll_)` 均返回 0 matches；仅保留少量 `pianoRoll_.repaint()` 在 theme change / undo / sync 等非 cadence 场景。 |
| `Source/Plugin/PluginEditor.cpp` | VST3 shell 不调用 PianoRoll visual-clock API | ✓ VERIFIED | grep 对 `pianoRoll_.onVisualVBlankCallback` / `pianoRoll_.invalidateVisual` / `requestInvalidate(pianoRoll_)` 均返回 0 matches；`timerCallback()` 只做 transport/state 同步与业务队列调度。 |
| `Tests/TestMain.cpp` | Phase 22 regressions 与 retained guards 都在 fresh binary 中为绿 | ✓ VERIFIED | lines 4410-4588 定义四个 Phase 22 journeys；line 4942 接入主测试入口；fresh binary 运行结果为全部 PASS，包括 retained `TIME_01_*` / `LAYER_01_*` / `INVAL_01_*` / `CLOCK_01_*`。 |

## Key Link Verification

| From | To | Via | Status | Details |
| --- | --- | --- | --- | --- |
| `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp` | `Tests/TestMain.cpp` | visible note window implementation -> `PAINT_02_*` regressions | ✓ WIRED | 静态审计与 runtime host 测试共同验证 `drawNotes()` 已切换到 visible iterator window，不再 full scan。 |
| `Source/Standalone/UI/PianoRollComponent.cpp` | `Tests/TestMain.cpp` | strip dirty + fit-to-screen -> `PAINT_01_*` / `FLOW_01_*` regressions | ✓ WIRED | 组件层 dirty strip、projected playhead、clip timeline projection 与 fit-to-screen 行为全部被自动化测试覆盖并验证通过。 |
| `Source/Standalone/PluginEditor.cpp` / `Source/Plugin/PluginEditor.cpp` | `Source/Standalone/UI/PianoRollComponent.cpp` | shell 不调用 PianoRoll visual-clock / direct invalidate API | ✓ WIRED | fresh L4 对 `pianoRoll_.onVisualVBlankCallback` / `pianoRoll_.invalidateVisual` / `requestInvalidate(pianoRoll_)` 的 grep 审计均为 0 matches，证明 editor shell 未引入新的 visual-clock 或 direct invalidate residue。 |
| `.planning/phases/22-Clip-Aware 刷新与集成守护/22-TEST-VERIFICATION.md` | `.planning/phases/22-Clip-Aware 刷新与集成守护/22-VERIFICATION.md` | fresh gate result -> requirement verdict | ✓ WIRED | 本报告直接消费 L1/L2/L4/L6 的命令与输出，不再复用旧 binary 或口头结论。 |

## Behavioral Spot-Checks

| Behavior | Command | Result | Status |
| --- | --- | --- | --- |
| fresh build dir 重新生成 Phase 22 regression binary | `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase22-docs -Target OpenTuneTests` | 成功生成并更新 `build-phase22-docs/OpenTuneTests.exe`，fresh configure/build 与当前工作区路径一致 | ✓ PASS |
| Phase 22 journeys 与 retained guards 在 fresh binary 上通过 | `& ".\build-phase22-docs\OpenTuneTests.exe"` | 输出包含 `=== Phase 22: Clip-Aware Paint Tests ===`，四个 `PAINT_* / FLOW_01_*` journeys 与 retained `TIME_01_*` / `LAYER_01_*` / `INVAL_01_*` / `CLOCK_01_*` 全部为 `[PASS]` | ✓ PASS |
| drawNotes 已切换到 visible note window，不再 full scan | `rg -n "computeVisibleTimeWindow\(|std::lower_bound|for \(const auto& note : notes\)" "Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp"` | 命中 `computeVisibleTimeWindow(`（line 27）、`std::lower_bound`（lines 591/606），无 `for (const auto& note : notes)` 匹配 | ✓ PASS |
| editor shell 不调用 PianoRoll visual-clock / direct invalidate API | `rg -n "pianoRoll_\.onVisualVBlankCallback|pianoRoll_\.invalidateVisual|requestInvalidate\(.*pianoRoll_" "Source/Standalone/PluginEditor.cpp" "Source/Plugin/PluginEditor.cpp"` | 输出为空，证明审计结果为 0 matches | ✓ PASS |

## Remaining Risks

- `timerCallback()` 仍保留在两套 editor shell 中作为 transport/state polling 与业务队列调度，这一边界必须在后续维护中继续守住，不能再次扩张回 PianoRoll visual flush。
- `FrameScheduler` 仍是 `PianoRollComponent.cpp:1352` 的单一 flush bridge；后续若引入新的 repaint 路径，仍需确保不会旁路该 bridge。
- Phase 22 的 editor shell 审计只覆盖 `pianoRoll_.onVisualVBlankCallback` / `pianoRoll_.invalidateVisual` / `requestInvalidate(pianoRoll_)`；少量 `pianoRoll_.repaint()` 调用在 theme change / undo / sync 场景中存在，但这些调用不属于 visual-clock / direct invalidate residue，不影响单入口闭环。

---

_Verified: 2026-04-15T12:00:00Z_
_Verifier: the agent (gsd-verifier)_
