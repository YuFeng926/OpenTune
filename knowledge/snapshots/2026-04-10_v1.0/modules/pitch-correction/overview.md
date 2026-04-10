---
module: pitch-correction
type: overview
version: 1.0
updated: 2026-04-10
status: extracted
---

# pitch-correction -- Module Overview

## 模块定位

**pitch-correction** 是 OpenTune 的核心算法层，负责从 F0 提取结果到声码器合成之间的全部音高修正逻辑。这是一个纯数学/DSP 模块，不涉及 AI 模型推理。

## 职责范围

| 职责 | 说明 |
|---|---|
| 音高曲线数据管理 | `PitchCurve` + COW 不可变快照，连接编辑层与渲染层 |
| 五阶段音高修正 | 斜率旋转补偿 -> 音高偏移 -> 颤音注入 -> Retune 混合 -> 过渡平滑 |
| 音符自动分割 | `NoteGenerator` 从 F0 曲线分割生成音符序列 |
| 感知音高估算 | `SimdPerceptualPitchEstimator` PIP 算法 (VNC + SSA + Energy) |
| 手动修正支持 | HandDraw / LineAnchor 修正段管理 |
| 后台任务调度 | `PianoRollCorrectionWorker` 异步执行修正，最新优先策略 |

## 架构层级

```
┌───────────────────────────────────────────┐
│           PianoRoll UI (编辑层)             │
├───────────────────────────────────────────┤
│      PianoRollCorrectionWorker (调度层)     │
├───────────────────────────────────────────┤
│  PitchCurve ← applyCorrectionToRange      │
│  ├─ PitchUtils (freqToMidi, mixRetune)    │  算法层
│  ├─ NoteGenerator (F0 → Notes)            │
│  └─ SimdPerceptualPitchEstimator (PIP)    │
├───────────────────────────────────────────┤
│  PitchCurveSnapshot (COW 不可变快照)       │  数据层
│  ├─ originalF0_ / originalEnergy_         │
│  └─ correctedSegments_                    │
├───────────────────────────────────────────┤
│  Note / NoteSequence / LineAnchor         │  数据结构
├───────────────────────────────────────────┤
│           RenderCache / Vocoder (消费层)    │
└───────────────────────────────────────────┘
```

## 关键源文件

| 文件 | 行数 | 核心内容 |
|---|---|---|
| `Source/Utils/PitchCurve.h` | 383 | PitchCurveSnapshot (不可变), PitchCurve (COW 外壳), CorrectedSegment |
| `Source/Utils/PitchCurve.cpp` | 757 | 五阶段修正算法, 过渡平滑, 段管理, renderF0Range |
| `Source/Utils/PitchUtils.h` | 45 | mixRetune, freqToMidi, midiToFreq |
| `Source/Utils/Note.h` | 288 | Note, NoteSequence, LineAnchor |
| `Source/Utils/NoteGenerator.h` | 91 | NoteGenerator 接口, NoteSegmentationPolicy, ScaleSnapConfig |
| `Source/Utils/NoteGenerator.cpp` | 300 | F0 → Note 分割算法, 代表音高计算, 半音量化 |
| `Source/Utils/SimdPerceptualPitchEstimator.h` | 157 | PIP 估算 (VNC + SSA + Energy) |
| `Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.h` | 89 | Worker 接口, AsyncCorrectionRequest |
| `Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.cpp` | 191 | 后台线程循环, 版本控制, 错误处理 |

## 线程模型

| 线程 | 访问方式 | 操作 |
|---|---|---|
| **Message thread (UI)** | 写入 | 调用 PitchCurve setter, enqueue 修正请求 |
| **CorrectionWorker thread** | 读写 | 执行 applyCorrectionToRange, atomic_store 新快照 |
| **Audio thread** | 只读 | atomic_load 快照, renderF0Range |
| **Chunk render thread** | 只读 | atomic_load 快照, renderF0Range |

## 依赖关系

**上游 (输入)**:
- `F0ExtractionService` -> 提供 originalF0 + energy
- `PianoRoll UI` -> 提供用户编辑的 Note 列表和修正参数

**下游 (输出)**:
- `RenderCache` / `RenderingManager` -> 消费 PitchCurveSnapshot.renderF0Range
- `processBlock` (音频线程) -> 消费 PitchCurveSnapshot 判断是否有修正

## 文档索引

| 文档 | 内容 |
|---|---|
| [api.md](./api.md) | 编程接口契约: 所有 public 方法签名和语义 |
| [data-model.md](./data-model.md) | 数据结构详解 + Mermaid 关系图 |
| [business.md](./business.md) | 核心业务规则 + 五阶段修正流程 + 算法细节 |

## 待确认汇总

共 10 项待确认事项，分布在各文档中:

1. **斜率旋转角度阈值** `[10°, 30°]` 来源和可调性 → [business.md]
2. **transitionThresholdCents = 80** 适用范围 → [business.md]
3. **getPerceptualOffset** 方法缺失/迁移 → [api.md, business.md]
4. **ScaleSnapConfig.snapMidi** 调用位置 → [api.md, business.md]
5. **PitchCurve setter 线程安全** (无并发 setter 保护) → [business.md]
6. **Worker 4ms 轮询 vs 条件变量** → [api.md]
7. **renderF0Range 越界截断行为** → [api.md]
8. **全 unvoiced 音符范围的输出** → [business.md]
9. **originalF0/Energy 尺寸不一致时的自动填充** → [data-model.md]
10. **Note.dirty 标记用途范围** → [data-model.md]
