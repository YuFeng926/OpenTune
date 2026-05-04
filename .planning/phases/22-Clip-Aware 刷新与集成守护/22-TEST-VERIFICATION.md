# Phase 22: Clip-Aware Paint And Integration Guards

## Scope

本文件记录 Phase 22 的验证契约与 RED gate 证据。验收真相源保持不变：PianoRoll 的播放头 strip dirty 必须继续通过主层单入口 + 单 flush bridge 命中最小脏区，renderer 必须让 waveform / F0 / notes 共用同一可见时间窗，而 `drawNotes()` 不得继续对全部 notes 做全量遍历。

Phase 18-21 的 projected playhead、主层场景归一、unified invalidation 与 single-VBlank cadence 继续作为 retained baseline。Phase 22 只允许在主层 renderer / component 内补齐 clip-aware 与集成守护，不能恢复 overlay、editor cadence 或 direct invalidate。

**Confirmed Toolchain Setup Command:** `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase22-docs -Target OpenTuneTests`

## Verification Levels

| Level | Goal | Command | Status | Pass Criteria |
| --- | --- | --- | --- | --- |
| L1 | 先确认真实 toolchain，再 fresh build `build-phase22-docs` 的 `OpenTuneTests` | `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase22-docs -Target OpenTuneTests` | PASS | 已成功产出 fresh `build-phase22-docs/OpenTuneTests.exe`，toolchain 通过动态发现命令在 live shell 中证实可用。 |
| L2 | 运行 Phase 22 journeys 与 retained guards | `& ".\build-phase22-docs\OpenTuneTests.exe"` | PASS | 输出包含 `=== Phase 22: Clip-Aware Paint Tests ===`，四个 `PAINT_* / FLOW_01_*` journeys 与 retained guards 全部为绿。 |
| L4 | 静态审计 renderer / component / editor shell 的 clip-aware 与 cadence 边界 | `rg -n "computeVisibleTimeWindow\(|std::lower_bound|for \(const auto& note : notes\)|drawWaveform\(|drawNotes\(|drawF0Curve\(" "Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp"` + `rg -n "^void PianoRollComponent::onVisualVBlankCallback\(|getPlayheadDirtyBounds\(|updateMainLayerPlayhead\(|invalidateVisual\(|flushPendingVisualInvalidation\(|fitToScreen\(" "Source/Standalone/UI/PianoRollComponent.cpp"` + `rg -n "pianoRoll_\.onVisualVBlankCallback|pianoRoll_\.invalidateVisual|requestInvalidate\(.*pianoRoll_" "Source/Standalone/PluginEditor.cpp" "Source/Plugin/PluginEditor.cpp"` | PASS | renderer 已命中 `computeVisibleTimeWindow(` + `std::lower_bound`，且不再存在 `for (const auto& note : notes)`；component 单入口/单 flush bridge 结构仍在，editor shell 也没有新增 cadence/direct invalidate residue。 |
| L6 | 重复运行 fresh Phase 22 regression binary | `& ".\build-phase22-docs\OpenTuneTests.exe"` | PASS | 第二次 full-suite run 与 L2 一致，Phase 22 journeys 和 retained guards 全部再次通过。 |

## Required Journeys

- `PAINT_02_PianoRollDrawNotesUsesVisibleNoteWindow`
- `PAINT_02_PianoRollRendererUsesUnifiedVisibleTimeWindowForNotesWaveformAndF0`
- `PAINT_01_PianoRollPlayheadDirtyStripStillUsesSingleFlushBridge`
- `FLOW_01_PianoRollFitToScreenUsesClipTimelineProjection`
- `TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead`
- `TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead`
- `LAYER_01_PianoRollHasNoPlayheadOverlayChild`
- `INVAL_01_PianoRollVisualVBlankFlushesPendingInteractiveInvalidation`
- `CLOCK_01_PianoRollSingleVisualTickFlushesPlayheadAndScroll`

## L5 Applicability

- L5: Not applicable.
- Reason: 当前仓库没有独立的 PianoRoll UI E2E harness；Phase 22 继续依赖 `OpenTuneTests` + 静态审计证明主层 clip-aware 与时间轴行为守护。

## Gate Policy

- 不允许默认跳过任何 blocking gate。
- `visible note window` 不等于“full scan 内加 if”；如果 `drawNotes()` 仍以 `for (const auto& note : notes)` 为主循环，`PAINT-02` 不能判定通过。
- `drawWaveform()` / `drawF0Curve()` 已经是 visible-range 驱动；Phase 22 的目标是把 notes 收敛到同一 visible time window，而不是新建第二套 painter/cadence。
- 不能恢复 overlay、editor cadence、direct invalidate 或第二条 flush bridge。

## Execution Evidence

### L1 Fresh Build

- Command: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase22-docs -Target OpenTuneTests`
- Result: PASS
- Key output:
  - `vswhere.exe resolved the active Visual Studio installation and loaded VsDevCmd.bat`
  - `Build files have been written to: [workspace-root]/build-phase22-docs`
  - `Linking CXX executable OpenTuneTests.exe`
  - Fresh binary path verified at `[workspace-root]/build-phase22-docs/OpenTuneTests.exe`

### L2 Full Regression

- Command: `& ".\build-phase22-docs\OpenTuneTests.exe"`
- Result: PASS
- Key output:
  - `=== Phase 22: Clip-Aware Paint Tests ===`
  - `[PASS] PAINT_01_PianoRollPlayheadDirtyStripStillUsesSingleFlushBridge`
  - `[PASS] PAINT_02_PianoRollDrawNotesUsesVisibleNoteWindow`
  - `[PASS] PAINT_02_PianoRollRendererUsesUnifiedVisibleTimeWindowForNotesWaveformAndF0`
  - `[PASS] FLOW_01_PianoRollFitToScreenUsesClipTimelineProjection`
  - retained `TIME_01_*` / `LAYER_01_*` / `INVAL_01_*` / `CLOCK_01_*` 同次 full regression run 继续为绿

### L4 Static Audit

- Command 1: `rg -n "computeVisibleTimeWindow\(|std::lower_bound|for \(const auto& note : notes\)|drawWaveform\(|drawNotes\(|drawF0Curve\(" "Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp"`
  - Result: PASS
  - Output:
    - `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:27:VisibleTimeWindow computeVisibleTimeWindow(...)`
    - `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:131:    const auto visibleWindow = computeVisibleTimeWindow(ctx, ctx.trackOffsetSeconds);`
    - `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:587:    const auto visibleWindow = computeVisibleTimeWindow(ctx, trackOffsetSeconds);`
    - `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:591:    auto firstVisibleNote = std::lower_bound(...)`
    - `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:606:    const auto lastVisibleNote = std::lower_bound(...)`
    - `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:696:    const auto visibleWindow = computeVisibleTimeWindow(ctx, ctx.trackOffsetSeconds);`
    - no matches for `for (const auto& note : notes)`
- Command 2: `rg -n "getPlayheadDirtyBounds\(|updateMainLayerPlayhead\(|invalidateVisual\(|flushPendingVisualInvalidation\(|onVisualVBlankCallback\(|fitToScreen\(" "Source/Standalone/UI/PianoRollComponent.cpp"`
  - Result: PASS
  - Output includes `getPlayheadDirtyBounds(...)`, `updateMainLayerPlayhead(...)`, `invalidateVisual(...)`, `flushPendingVisualInvalidation(...)`, `onVisualVBlankCallback(...)`, `fitToScreen(...)`
- Command 3: `rg -n "pianoRoll_\.onVisualVBlankCallback|pianoRoll_\.invalidateVisual|requestInvalidate\(.*pianoRoll_" "Source/Standalone/PluginEditor.cpp" "Source/Plugin/PluginEditor.cpp"`
  - Result: PASS
  - Output: no matches

### L6 Repeat Regression

- Command: `& ".\build-phase22-docs\OpenTuneTests.exe"`
- Result: PASS
- Key output:
  - second run again printed `=== Phase 22: Clip-Aware Paint Tests ===`
  - four `PAINT_* / FLOW_01_*` journeys all passed again
  - retained `TIME_01_*` / `LAYER_01_*` / `INVAL_01_*` / `CLOCK_01_*` all remained green

## Evidence To Capture

- `drawNotes()` 已从 visible note window 起步，而不是在 full scan 内用 `continue` 过滤。
- `drawWaveform()` / `drawF0Curve()` / `drawNotes()` 共用同一 renderer visible time window 语义。
- `getPlayheadDirtyBounds()` + `updateMainLayerPlayhead()` + `invalidateVisual(...)` + `flushPendingVisualInvalidation()` 仍是 strip dirty / 单入口 / 单 flush bridge。
- `fitToScreen()`、continuous/page scroll、playhead projection 继续命中 sample-authoritative clip timeline projection。
- Phase 22 不恢复 overlay、editor cadence、direct invalidate。

## Final Gate

- PASS: L1 / L2 / L4 / L6 全部通过，Phase 22 的 fresh gate 已具备 closure 证据。
- Historical note: Wave 1 已经在同一 fresh build 流中拿到过 `drawNotes()` full scan 的真实 RED failure；当前文档保留最终 PASS 证据，RED 历史保存在 `22-01-SUMMARY.md`。
