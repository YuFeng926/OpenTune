---
spec_version: 1.0.0
status: draft
doc_type: cross-cutting/undo-affected-range-invariant
generated_by: opsx-apply
generated_at: 2026-05-07
last_updated: 2026-05-07
---

# Undo / Redo affected-range 不变量

`PianoRollEditAction` 的 `affectedStartFrame_` / `affectedEndFrame_` 字段表征「该 action 触及的 PitchCurve 帧范围」，决定了 undo/redo 时 vocoder 重渲染的范围（`enqueueMaterializationPartialRenderById(matId, startSec, endSec)`）。

本文件记录这条不变量的来源、强制契约、以及它在历史上被无声丢失的教训，作为未来重构者的预警 checkpoint。

## 不变量正文

### 信息流：affected-range 由 ToolHandler 编辑时计算，沿 commit 链路透传至 undo action

```
ToolHandler.mouseUp / 工具回调:
   F0FrameRange affectedRange = f0tl.rangeForTimes(...)   ← ground truth at source
        │
        ▼
   ctx_.commitNotesAndSegments(notes, segments, affectedRange)
        │
        ▼
   PianoRollComponent::commitEditedMaterialization*(notes/segments, affectedRange)
        │
        ▼
   recordUndoAction(description, affectedRange)
        │
        ▼
   PianoRollEditAction(matId, desc, oldNotes/newNotes, oldSegs/newSegs,
                       affectedStartFrame=affectedRange.startFrame,
                       affectedEndFrame=affectedRange.endFrameExclusive - 1)
        │
        ▼
   编辑器 undoRequested / redoRequested:
       enqueueMaterializationPartialRenderById(matId,
           getAffectedStartFrame() * spf,
           getAffectedEndFrame()   * spf)
```

**核心要求**：

- `affectedRange` SHALL 在每条 commit 路径上由调用方显式传入；不接受默认参数（避免遗漏导致全长退化静默发生）
- `PianoRollEditAction` 构造函数 SHALL 把 `affectedStartFrame` / `affectedEndFrame` 作为入参直接 store 到字段；**不得**从 `oldSegments_` / `newSegments_` 反推（即不得用 union of all segments min/max）
- 当调用方无明确 affected 范围时（少数 fallback 路径），SHALL 显式传入 `currentFullF0Range()`（= `[0, totalFrames]`），表达"全长"为有意识的退化决策

### 作用域不变量（PitchCurve 侧支撑）

`PitchCurve::applyCorrectionToRange(notes, startFrame, endFrame, ...)` SHALL 保证：commit 完成后 `corrected segments` 在 `[startFrame, endFrame)` 区间外 byte-identical（由 `clearSegmentsInRangePreserveOutside` 强制实现）。

由此推得：当 PianoRollEditAction.affectedRange 与 commit 调用的 (startFrame, endFrame) 一致时，undo 只重渲染该范围 SHALL 与全量重渲染产生 byte-equal 的 RenderCache 结果（在该 range 内）。

## 历史回归：v2.0 重构无声丢失优化

`c5c6c29` (2026-04-06) "Unify selection mechanism: ... undo perf optimization" 引入了 `Source/Standalone/UI/PianoRoll/PianoRollUndoSupport.cpp` 的 `computeSegmentDiffRange()` —— 用 hash-based set difference 计算精确范围。

`a122bca v2.0` 重构删除了 `PianoRollUndoSupport.{cpp,h}` (-265 行)，新增 `Source/Utils/PianoRollEditAction.{cpp,h}` (+88 行)。新构造函数把 affected 范围算法换成了 `union of all old∪new segments min/max` —— 在多段 PitchCurve 中等同于 `[0, 全长]`。**优化在重构里被无声丢失**。

`undo-affected-range-passthrough` change (2026-05-07) 用「编辑时已知范围沿链路透传」机制重新建立了不变量，**不**复活旧的 hash-diff 算法 —— passthrough 比 reconstruct 信息密度更高、性能更稳、与 live render 路径自然对称。

## 重构者预警 checklist

未来重构 `PianoRollEditAction` / `recordUndoAction` / commit 链路时，若做以下任一动作，必须**先**确认是否会破坏本不变量：

- [ ] 修改 `PianoRollEditAction` 构造函数签名（特别是删除/重命名 `affectedStartFrame` / `affectedEndFrame` 参数）
- [ ] 在 `PianoRollEditAction` 构造函数里引入对 `oldSegments_` / `newSegments_` 的遍历（哪怕是 hash 计算，也是把"信息保留"模式倒退回"信息重建"）
- [ ] 移除 `commitNotesAndSegments` / `commitEditedMaterialization*` / `recordUndoAction` 的 `F0FrameRange` 参数
- [ ] 给 `recordUndoAction` 加默认参数（默认值会让遗漏调用点的 affectedRange 静默退化为"全长"）
- [ ] 修改 `PitchCurve::applyCorrectionToRange` / `clearSegmentsInRangePreserveOutside` 让范围外的段不再 byte-identical（直接破坏了 partial render 的语义保证）

如以上修改不可避免，**必须**：

1. 用 `runPianoRollEditActionAffectedRangeIndependentOfSegmentsTest` 等 anchor 测试确认 affected-range 的字段化语义不被新逻辑污染
2. 在 `~/Library/OpenTune/Logs/` 中通过 grep `enqueueMaterializationPartialRender matId=<m> range=[s,e]` 验证编辑触发与 undo 触发产生几乎相等的范围（除 1 帧 inclusive/exclusive 换算差）
3. 跑 L5 用户旅程：编辑一个 note → 撤销 → 体感 + 日志确认范围紧致

## 外部引用

- 实现 spec: `openspec/specs/undo-edit-action-range/spec.md`
- 历史 archive: `openspec/changes/archive/<date>-undo-affected-range-passthrough/`
- 相关 commit: `c5c6c29` (原优化)、`a122bca` (无声丢失)、本次修复 commit
