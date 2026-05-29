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
**Current focus:** v1.5 累积功能继续收口，但时间线方向已切到 `DAW timeline follow rendering architecture`  
**Test strategy:** 先 focused contract，再 focused build/test，再 runtime diagnostics，再 visual/L5

## Current Position

Milestone 仍为 `v1.5`，但时间线子系统的状态已重新定性：

- 现在的主要问题不是 VBlank/timer 参数，而是 follow scroll 架构把播放时间、scroll 呈现、
  content rebuild 绑在一起。
- 2026-05-29 已先完成 **focused tests / kill-list 合同反转** 和 `.planning` 定锚。
- 本轮没有声称构建、测试执行或视觉验证已经完成。

## Landed Mainline Context

已落地主线仍包括：

- UndoManager + PianoRollEditAction
- PianoRollCorrectionWorker
- PlayheadOverlayComponent
- RenderBadgeComponent
- F0Timeline
- ONNX 内存优化
- GPU/CPU 重构
- VST3 ARA lifecycle / regular-VST3 分流 / multi-item birth
- Standalone 累积功能
- AUTO(REF)
- TimeTool identity seed

这些既有落地项不构成“当前 timeline follow 已正确”的证明。

## New Active Timeline Contract

当前时间线 follow 的唯一正确方向：

1. authoritative playback time
2. presentation clock
3. viewport policy
4. prepared content / render band cache
5. playhead overlay

明确禁止继续把以下旧口径当成正确行为：

- steady scroll 触发 full content repaint
- steady scroll 触发 render model rebuild
- `smoothScrollCurrent_ += diff * constant` 追赶式平滑
- render model key 绑定 `scrollOffsetPx` / exact visible window
- 新旧两套 scroll 路径并行保留

## Pending Todos

- [ ] 按 2026-05-29 新合同做 live implementation。
- [ ] 删除 steady scroll -> full invalidate / full rebuild 旧路径。
- [ ] Continuous follow 改为 pinned playhead + viewport presentation policy。
- [ ] render model key 收敛到 render band / revision / geometry 边界。
- [ ] focused build/test 执行。
- [ ] runtime diagnostics gate。
- [ ] Standalone visual smoke。
- [ ] L5 手工旅程。
- [ ] Undo/Redo 边界测试。
- [ ] CorrectionWorker 取消/覆盖验证。
- [ ] `OpenTuneTests.exe ui` exit=1 解释修复。
- [ ] Arrangement min-zoom waveform + cross-track drag preview。
- [ ] macOS bundle inspection。

## Verification Notes

- 本轮只完成合同与文档层收口：
  - `Tests/TestMain.cpp`
  - `Tests/TestTimelineRenderingPipeline.cpp`
  - `2026-05-29` 两份计划文档
  - `ROADMAP/STATE` 同步
- 本轮**未执行** build / `OpenTuneTests.exe` / visual smoke / L5。
- 因此任何“已经流畅”或“已验证高帧率低占用”的表述，在当前状态下都不成立。

## Main Risks For Next Thread

1. focused contracts 已翻面，live tree 仍大概率先 FAIL，这是预期而不是回退理由。
2. Arrangement live code 里的追赶式 scroll 平滑会与新合同正面冲突。
3. live render model key 仍可能含 visible-window / `scrollOffsetPx`，会继续触发 steady-scroll rebuild。
4. 如果后续实现试图保留新旧路径并行，必须视为违约而不是过渡成功。

---

*本文件只保留当前决策摘要和下一步风险。完整历史见 `.planning/plans/` 与 `.planning/archive/plans/`。*
