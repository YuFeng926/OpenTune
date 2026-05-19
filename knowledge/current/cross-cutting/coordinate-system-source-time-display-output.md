---
spec_version: 1.0.0
status: draft
doc_type: cross-cutting/coordinate-system-source-time-display-output
generated_by: opsx-apply
generated_at: 2026-05-12
last_updated: 2026-05-12
---

# 源时间 / 输出时间 / 屏幕坐标 三层坐标系不变量

OpenTune v7 引入 `TimeGrid` 后，时间从单层标量演化为三层坐标系：

```
┌──────────────────────────┐  屏幕 X 像素（pixelX）
│   Display layer          │  ←── 用户看到的（通过 viewOffsetSeconds × pixelsPerSecond 投影）
└──────────────────────────┘
            │   τ
            ▼   τ⁻¹
┌──────────────────────────┐  输出时间（output_seconds）
│   Output time            │  ←── ARA region / Standalone 时间轴上的位置
└──────────────────────────┘
            │   τ
            ▼   τ⁻¹
┌──────────────────────────┐  源时间（source_seconds）
│   Source time            │  ←── 原始 PCM / Note / PitchCurve / WaveformMipmap 锚点
└──────────────────────────┘
```

`τ` (`TimeGridSnapshot::tauForward`) 与 `τ⁻¹` (`tauInverse`) 通过 `TimeGrid` 的
piecewise-linear 插值在源时间与输出时间之间映射。`TimeGrid` 不存在时（v6 及以前）
等价于恒等 `τ`。

## 不变量正文

### 数据层永远存源时间

所有持久化、跨编辑生存的数据**必须**以源时间为锚：

- `Note::startTime` / `Note::endTime` (`Source/Utils/Note.h`)
- `PitchCurve` 帧索引（100 fps × 源时间）
- `CorrectedSegment::startFrame` / `endFrameExclusive` (`Source/Utils/PitchCurve.h`)
- `WaveformMipmap` peaks 数组索引（`samples / WaveformMipmap::kBaseSampleRate`）
- `PhonemeClass[]` / `OnsetEvent[]` (vocal-time-stretch 检测管线输出)
- `TimeHandle::source_seconds`（自身就是源时间锚，**不可变**）

理由：用户做时间编辑（拖 handle）时，所有数据**保持不动**，仅 `TimeGrid` 的
output_seconds 字段变。这避免了"shift around"导致的级联失效——音符、PitchCurve、
WaveformMipmap 都不需要重新计算。

### 显示层在渲染瞬间应用 τ

`PianoRollRenderer::RenderContext` 持 `timeGridSnapshot` 字段；所有把数据投影到
屏幕 X 的代码 SHALL 在渲染当下做 `τ → output_time → screenX`：

```cpp
// 绘制 Note (Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp)
const double t_out_start = ctx.timeGridSnapshot
    ? ctx.timeGridSnapshot->tauForward(note.startTime)
    : note.startTime;
const double screenX_start = ctx.timeToX(t_out_start);

// 绘制 Waveform (drawWaveform)
double matTime = projectTimelineTimeToMaterialization(ctx.xToTime(screenX));
if (timeGridSnapshot != nullptr && !timeGridSnapshot->isIdentity()) {
    matTime = timeGridSnapshot->tauInverse(matTime);   // ← screen→output→source
}
peakIndex = static_cast<int64_t>(matTime / timePerPeak);
```

恒等 `TimeGrid` 时 `tauForward(t) == t == tauInverse(t)` —— 路径退化为零成本
passthrough，与 v6 行为一致。

### 输入层在写回前做 τ⁻¹

用户拖 Note 起点到屏幕 X 时，写回 `note.startTime` 必须经 `τ⁻¹` 反投影：

```cpp
const double t_out  = ctx.xToTime(pixelX);             // screen → output
const double t_src  = ctx.timeGridSnapshot
    ? ctx.timeGridSnapshot->tauInverse(t_out)
    : t_out;                                            // output → source
note.startTime = t_src;                                 // 写回数据层（源时间）
```

任何遗漏 `τ⁻¹` 的写入路径都会**直接污染数据层**——之后再做时间编辑会导致音符
被错误地 "shift around"。

### `τ` 的边界条件

由 `TimeGrid` 的端点锁定（`ClipStart` / `ClipEnd`，`locked = true`）保证：

- `τ(0) = 0`，`τ(srcEnd) = srcEnd`（端点处 src == out）
- ARA region 长度 = `srcEnd - srcStart`，与 source 长度恒等
  （详见 `Invariant_AraRegionLength_OutputDurationEqualsSourceDuration` 单测）

总时长守恒是 v7 的**硬约束**：UI 层不暴露端点解锁路径，`TimeGrid::validate`
拒绝 unlock 端点的 snapshot。

## 实施策略

### 推荐：在 RenderContext 注入 snapshot，渲染层统一应用

PianoRollComponent 在 `buildRenderContext()` 把当前发布的（或拖动中的工作）
`TimeGridSnapshot` 写入 `RenderContext::timeGridSnapshot`，渲染层每次访问数据时
查询 ctx.timeGridSnapshot 来决定是否做 τ。识别 `nullptr` 与 `isIdentity()` 时
退化为 passthrough。

### 反模式（**禁止**）

- ❌ 在 `Note::startTime` 写入路径之外把 τ⁻¹ 当成"装饰"省略——一旦遗漏，时间编辑
     之后该数据会被静默错位
- ❌ 把 τ 计算下沉到 getter（如 `Note::getStartTimeOnScreen()`）——getter 隐藏了
     coordinate-system 切换点，让 review 难以发现遗漏
- ❌ 让 `PitchCurve` 的帧索引随 TimeGrid 编辑而平移——破坏 v7 核心目标"拖 handle
     不让 NSF 重跑"，因为 RenderCache 命中要求 PitchCurve 帧索引稳定

## 重构者预警 checklist

修改任意以下路径时**必须**核对本不变量：

- [ ] `Note` / `CorrectedSegment` / `PitchCurve` 的字段语义（time vs frame）改动
- [ ] `PianoRollRenderer::drawNote` / `drawWaveform` 等绘制函数签名（特别是
      `RenderContext::timeGridSnapshot` 字段）
- [ ] `PianoRollToolHandler::handleNoteToolMouseDrag/Up` 等写回 `note.startTime` 的
      路径（确认 `τ⁻¹` 在写回前应用，且 snapshot 来源正确）
- [ ] 新增的"波形 / 钢琴键 / piano roll cell" 等绘制层（必须读取 ctx.timeGridSnapshot
      并应用 `τ`）

如修改不可避免，**必须**：

1. 跑 `Integration_WaveformRenderer_StretchedSegmentVisualWidth` 单测（覆盖 τ⁻¹ 用于
   屏幕像素查 source PCM 的核心契约）
2. 跑 `Invariant_AraRegionLength_OutputDurationEqualsSourceDuration`（端点锁定 +
   总长守恒）
3. L5 用户旅程：拖动 handle 到非恒等 → 切换 Note tool → 拖动 Note → 再切回 Time tool
   验证 Note 屏幕位置随 TimeGrid 联动且数据层 startTime 仍为源时间

## 外部引用

- TimeGrid 实现: `Source/Utils/TimeGrid.{h,cpp}`
- 渲染应用: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp::drawWaveform`
- 设计冻结: `research/p0_time_stretch/DESIGN.md` v7 §6.2 / §11.7
- vocal-time-stretch change: `openspec/changes/vocal-time-stretch/specs/two-view-piano-roll/spec.md`
