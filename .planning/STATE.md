---
gsd_state_version: 1.0
milestone: v1.5
milestone_name: PianoRoll Undo/Redo + Async Correction + Playhead Isolation
status: active
last_updated: "2026-05-29"
---

# Project State

## Project Reference

**Core value:** 双格式独立编译，零交叉影响  
**Current focus:** v1.5 累积功能收口 + 代码质量清理  
**Test strategy:** 先 focused contract，再 focused build/test，再 runtime diagnostics，再 visual/L5

## Current Position

Milestone `v1.5`。时间线渲染架构已收敛到正确状态（2026-05-29 验证）。

### Timeline Architecture — 已完成 ✓

经代码审计确认，当前 live tree 已实现正确的时间线渲染架构：

- **超扫描渲染带**：`renderBand_` = 视口 + 左右各 1 屏超扫描。滚动在带内时只做
  `setBounds` 移动子组件位置（O(1)），不重建内容。
- **离屏内容表面**：`contentSurface_` / `rulerSurface_` 是 `ArrangementCachedSurface`
  子组件，持有预渲染 `juce::Image`。
- **PlayheadOverlay 独立**：自己的 VBlank 驱动，窄脏矩形重绘，不触发内容重绘。
- **FrameScheduler 合并**：多次请求合并为一次 repaint，播放时丢弃低优先级动画。
- **Continuous follow**：pinned playhead + `setScrollOffset` → 视口内只移动子组件。
- **`smoothScrollCurrent_` 已不存在**：即时位置跳转，无追赶式平滑。
- **render model key**：绑定 `revision + geometry`，不绑定 `scrollOffsetPx`。

STATE.md 之前描述的"steady scroll 触发 full content repaint / render model rebuild"
等问题已在之前的重构中解决。旧路径已删除，无并行兜底。

### UI 外观修复 — 已完成 ✓（2026-05-29）

- 编排视图：Time/Cont 按钮被标尺背景遮挡 → `drawTimeRulerBackdrop` 添加 `excludeClipRegion`
- 钢琴卷帘：标尺顶部 12px 背景割裂 → `paint()` 预填充 `fillAll(rollBackground)`

### 代码质量清理 — 已完成 ✓（2026-05-29）

- Issue #2：结构性重复消除 → `StandaloneArrangementHelpers.h`（20 个共享 inline 辅助）
- Issue #3：常量统一 → `TrackConstants.h`（`MaxTracks = 12` 单一真值）

## Landed Mainline Context

- UndoManager + PianoRollEditAction
- PianoRollCorrectionWorker
- PlayheadOverlayComponent（独立 VBlank overlay）
- RenderBadgeComponent
- F0Timeline
- ONNX 内存优化
- GPU/CPU 重构
- VST3 ARA lifecycle / regular-VST3 分流 / multi-item birth
- Standalone 累积功能
- AUTO(REF)
- TimeTool identity seed
- Timeline rendering architecture（超扫描渲染带 + 离屏表面 + FrameScheduler）
- StandaloneArrangementHelpers.h + TrackConstants.h 代码质量清理

## Pending Todos

- [ ] Undo/Redo 边界测试（空栈、redo 裁剪、500 层溢出）。
- [ ] CorrectionWorker 取消/覆盖验证。
- [ ] `OpenTuneTests.exe ui` exit=1 解释修复。
- [ ] Standalone visual smoke（含 UI 修复验证）。
- [ ] L5 手工旅程。
- [ ] Arrangement min-zoom waveform + cross-track drag preview。
- [ ] macOS bundle inspection。

## Verification Notes

- 2026-05-29：OpenTune + OpenTuneTests 编译零错误（MSVC Release）。
- Timeline 架构通过代码审计确认正确（非运行时验证）。
- UI 修复需要 visual smoke 确认。

## Main Risks For Next Thread

1. UI 修复未经 visual smoke 验证，可能有边缘主题下的回归。
2. `OpenTuneTests.exe ui` exit=1 仍未解释。
3. Arrangement min-zoom waveform 功能尚未实现。

---

*本文件只保留当前决策摘要和下一步风险。完整历史见 `.planning/plans/` 与 `.planning/archive/plans/`。*
