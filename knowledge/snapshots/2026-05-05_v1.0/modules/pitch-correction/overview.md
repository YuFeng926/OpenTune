---
spec_version: 1.0.0
status: draft
module: pitch-correction
doc_type: overview
generated_by: module-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# pitch-correction -- Module Overview

## 模块定位

`pitch-correction` 是 OpenTune 音高修正算法层，负责连接 F0 提取与声码器渲染之间的全部音高编辑、修正与调度逻辑。整个模块围绕一个核心不可变数据结构 `PitchCurveSnapshot` 与其 COW 外壳 `PitchCurve` 组织，通过 `std::atomic_store` / `std::atomic_load` 在 UI 线程（写）、Worker 线程（读写）与音频/渲染线程（只读）之间实现无锁共享。

## 职责范围

| 职责 | 负责组件 | 说明 |
|---|---|---|
| 音高曲线数据管理 | `PitchCurve` + `PitchCurveSnapshot` | COW 不可变快照，承载 originalF0、originalEnergy、correctedSegments、renderGeneration |
| 修正段落管理 | `CorrectedSegment` + `applyCorrectionToRange` | 基于音符/手绘/LineAnchor 三种来源的修正段 |
| 音高修正算法 | `PitchCurve::applyCorrectionToRange` | 斜率旋转补偿 → 音高偏移 → 颤音注入 → retune 混合 → 边界 Hermite smoothstep 过渡 |
| 音符自动分割 | `NoteGenerator` | 从 F0 曲线基于 transitionThresholdCents / gapBridge / minDuration 分段生成 Note |
| 感知音高估算 | `SimdPerceptualPitchEstimator` | PIP = VNC (Vibrato-Neutral Center) × SSA (Tukey 边缘衰减) × Energy |
| 音阶吸附 | `ScaleSnapConfig::snapMidi` | 按调式（Major/Minor/Dorian/Mixolydian/Harmonic Minor/Pentatonic 等）量化到最近的合法半音 |
| 后台任务调度 | `PianoRollCorrectionWorker` | 单槽位 pending 请求 + 条件变量唤醒 + 版本号取消策略（最新优先） |

## 架构层级

```
┌─────────────────────────────────────────────────┐
│            PianoRoll UI（编辑层）                │
│   - 发起 AsyncCorrectionRequest                  │
│   - 调用 PitchCurve setter（UI 线程同步写）     │
├─────────────────────────────────────────────────┤
│      PianoRollCorrectionWorker（调度层）         │
│   - 单 pending 槽 + version 取消                 │
│   - 在 worker 线程执行 applyCorrectionToRange    │
│     / NoteGenerator::generate                    │
├─────────────────────────────────────────────────┤
│   算法层（Source/Utils）                         │
│   - PitchCurve::applyCorrectionToRange（五阶段） │
│   - PitchUtils（freqToMidi / midiToFreq /        │
│     mixRetune）                                  │
│   - NoteGenerator（F0 → Notes）                  │
│   - SimdPerceptualPitchEstimator（PIP）          │
├─────────────────────────────────────────────────┤
│   数据层（COW）                                  │
│   - PitchCurveSnapshot（immutable，shared_ptr<  │
│     const>）                                     │
│   - renderGeneration_（每次写都递增）            │
├─────────────────────────────────────────────────┤
│   数据结构                                       │
│   - Note / NoteSequence / LineAnchor             │
│   - CorrectedSegment（startFrame/endFrame/      │
│     f0Data/source/retuneSpeed/vibrato*）         │
├─────────────────────────────────────────────────┤
│        消费层：inference / render pipeline       │
│   - Vocoder / RenderCache 通过 renderF0Range     │
│     只读消费                                     │
└─────────────────────────────────────────────────┘
```

## 上下游边界

**上游（输入源）**

- `inference` 模块 → 提供 F0 / Energy 原始曲线，调用 `PitchCurve::setOriginalF0` / `setOriginalEnergy` / `setOriginalF0Range` 写入。
- `PianoRoll` UI → 通过 Note / LineAnchor 编辑操作发起修正请求。
- 导入流程 → 触发 `AutoTuneGenerate` 自动生成音符。

**下游（消费方）**

- `inference` 模块（声码器渲染） → 通过 `PitchCurveSnapshot::renderF0Range` 拉取最终 F0（原始 + 已修正分段）。
- `PianoRoll` UI 视图 → 读取快照进行音高曲线、音符高亮、语音段边界可视化。
- `audio-thread`（实时回放） → `atomic_load` 当前快照，走 `renderF0Range` 回调。

**严格边界**

- pitch-correction **不执行** 任何神经网络推理或声码器合成（这些在 `inference` 模块）。
- pitch-correction **不直接** 写入音频缓冲区（仅产出 F0 向量供下游使用）。
- UI 交互逻辑（鼠标/键盘/绘制）不在本模块，仅提供线程安全的数据 API。

## 关键源文件

| 文件 | 行数 | 核心内容 |
|---|---|---|
| `Source/Utils/PitchCurve.h` | 376 | `PitchCurveSnapshot`（不可变）+ `PitchCurve`（COW 外壳，setter 全部 `atomic_store`）+ `CorrectedSegment` |
| `Source/Utils/PitchCurve.cpp` | 654 | `applyCorrectionToRange` 五阶段修正 + Hermite smoothstep 过渡段构建 + `renderF0Range` / `renderCorrectedOnlyRange` |
| `Source/Utils/PitchUtils.h` | 45 | `mixRetune` / `freqToMidi` / `midiToFreq`（纯 inline 函数） |
| `Source/Utils/Note.h` | 288 | `Note` / `NoteSequence`（含 `eraseRange` 分割 + `dirty` 标记 + `normalizeNonOverlapping`）/ `LineAnchor` |
| `Source/Utils/NoteGenerator.h` | 106 | `NoteGenerator`（静态类）+ `NoteSegmentationPolicy` + `ScaleSnapConfig` + `NoteGeneratorParams` |
| `Source/Utils/NoteGenerator.cpp` | 330 | F0 → Note 分段算法（voiced 跟踪 + gapBridge + 平均音高相对变化阈值判分） |
| `Source/Utils/SimdPerceptualPitchEstimator.h` | 157 | PIP = (VNC × SSA × Energy) / Σweight，使用 `juce::FloatVectorOperations` |
| `Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.h` | 95 | `AsyncCorrectionRequest`（含 `Kind`/`ErrorKind`）+ Worker 接口 |
| `Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.cpp` | 186 | 条件变量唤醒的 workerLoop + `enqueue` 的单槽 supersede 语义 |

## 线程模型概要

| 线程 | 访问方式 | 操作 |
|---|---|---|
| UI / Message thread | 写 | 调用 `PitchCurve::setOriginal*` / `setManualCorrectionRange` / `clearCorrectionRange` 等（原子 store） |
| `PianoRollCorrectionWorker` 线程 | 读 + 写 | 执行 `applyCorrectionToRange` / `NoteGenerator::generate`，然后 `atomic_store` 新快照 |
| Audio thread | 只读 | `getSnapshot()` 原子 load 后 `renderF0Range` 拉取 F0 |
| Render / Export 线程 | 只读 | 同 audio thread，通过快照快照一次性遍历区间 |

详细线程边界与竞争约束见 [business.md](./business.md)。

## 约束与不变式

1. **`PitchCurveSnapshot` 成员全部 `const`** — 一旦构造完成就不可变，多线程读无需锁。
2. **所有 PitchCurve 写操作必须走 `std::atomic_store`** — 构造新 snapshot 后原子替换指针。
3. **`renderGeneration_` 单调递增** — 由 `nextRenderGeneration_.fetch_add` 产生，供下游判断 F0 是否更新过。
4. **`correctedSegments_` 保持 `startFrame` 升序**（`replaceCorrectedSegments` / `insertSegmentSorted` 保证）。
5. **段落不重叠** — `clearSegmentsInRangePreserveOutside` 会裁剪或保留外侧、删除被覆盖的段。
6. **Note 序列非重叠** — `NoteSequence::normalizeNonOverlapping` 强制后者 startTime 截断前者 endTime。
7. **Worker 单 pending 槽** — 新请求进入时，旧 pending 立即标记 `VersionMismatch` 退回 completedRequest。
8. **过渡段（Hermite smoothstep）** 统一 10 帧长度（`kUnifiedTransitionFrames`），仅在两侧原始 F0 均 voiced 且无段冲突时生成。

## 文档索引

| 文档 | 内容 |
|---|---|
| [api.md](./api.md) | 编程接口契约：PitchCurve / Note / NoteGenerator / PIP / Worker 的 public 方法签名与语义 |
| [data-model.md](./data-model.md) | 数据结构详解：Snapshot 字段、Note 字段、CorrectedSegment、帧索引系统、voiced 标记 |
| [business.md](./business.md) | 业务管线：导入时音符生成 → 用户修正 → Worker 后台重算 → 音频线程读快照，含 2 张流程图 |

## 待确认汇总

共 13 项待确认事项（各文档展开）：

1. 斜率旋转角度阈值 `[10°, 30°]` 与 `slopeAt45DegSemitonesPerSecond = 7.0f` 的来源与调参口径 → [business.md]
2. `transitionThresholdCents = 80` 与 `gapBridgeMs = 10` 默认值的经验来源 → [business.md]
3. `ScaleSnapConfig.snapMidi` 是否会在 UI 交互路径被调用（除 `NoteGenerator::quantisePitch` 外） → [api.md]
4. `PianoRollCorrectionWorker` 的 `pendingRequestCv_` 是否存在虚假唤醒保护 → [api.md]
5. `renderF0Range` 在 `endFrame > maxFrame` 时的截断策略对下游是否可见 → [api.md]
6. 全 unvoiced 音符范围（所有 F0 ≤ 0）应回退原始 F0 还是产出 0 → [business.md]
7. `originalF0_` 与 `originalEnergy_` 尺寸不一致时的自动 resize / 截断策略是否覆盖所有 setter 路径 → [data-model.md]
8. `Note.dirty` 标记的清理时机（何时 `clearAllDirty`）与消费方 → [data-model.md]
9. `CorrectedSegment::Source::None` 在实际路径下是否可能被写入 → [data-model.md]
10. `materializationEpochSnapshot` / `materializationIdSnapshot` 的语义与上游来源 → [api.md]
11. Worker 内部 `try/catch` 是否涵盖所有 `applyCorrectionToRange` 的异常路径 → [business.md]
12. `getPerceptualOffset` 接口是否已被迁移或废弃 → [api.md]
13. `clear()` 在有并发读者时，`atomic_store` 之后旧 snapshot 是否被立即释放 → [data-model.md]
