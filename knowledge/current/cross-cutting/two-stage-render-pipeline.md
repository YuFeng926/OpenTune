---
spec_version: 1.0.0
status: draft
doc_type: cross-cutting/two-stage-render-pipeline
generated_by: opsx-apply
generated_at: 2026-05-12
last_updated: 2026-05-12
---

# 双阶段渲染管线（v7+：Pitch + Time）

vocal-time-stretch v7 在原有"Pitch correction → vocoder → playback"单阶段管线之上
新增了**Stage 2 时间拉伸**阶段，使用 SoundTouch (WSOLA) 时域引擎。本文档
记录两个阶段的契约、cache 协议、edit 失效规则与 bypass 不变量。

## 阶段总览

```
                                    PitchCurve edit         TimeGrid edit
                                          │                      │
                                          ▼                      ▼
┌────────────────┐  source PCM  ┌──────────────────┐  ┌────────────────────┐
│  Materializa-  │ ───────────▶ │   Stage 1        │  │   Stage 2          │
│  tionStore     │              │   (chunk-wise)   │  │   (clip-wide)      │
│                │              │   NSF + LR4 mix  │  │   SoundTouch WSOLA   │
│  audioBuffer   │              │   ───────────▶   │  │   ───────────▶     │
│  pitchCurve    │              │   PitchCache     │  │   TimeStretchCache │
│  timeGrid      │              │   (= RenderCache)│  │                    │
└────────────────┘              └──────────────────┘  └────────────────────┘
                                          │                      │
                                          ▼                      ▼
                                   chunk-wise PCM         clip-wide PCM
                                   (per-chunk revision)   (matId-wide revision pair)
                                          │                      │
                                          └──────────┬───────────┘
                                                     ▼
                                  ┌──────────────────────────────────┐
                                  │   readPlaybackAudio              │
                                  │   1) Stage 2 fast-path           │
                                  │      (TimeStretchCache hit)      │
                                  │   2) miss → dry+overlay+LR4 mix  │
                                  └──────────────────────────────────┘
                                                     ▼
                                              processBlock 输出
```

## 阶段 1：Pitch correction → NSF + LR4 → PitchCache

**Owner**：`OpenTuneAudioProcessor::chunkRenderWorkerLoop`，独立 worker thread。
**输入**：源 PCM + 当前 `PitchCurveSnapshot`。
**输出**：chunk-wise PCM 写入 `RenderCache`（即 `PitchCache`，alias from
`Source/Inference/PitchCache.h`）。

每个 chunk 独立带 `desiredRevision` / `publishedRevision` 双版本号；只 NSF 推理
跨编辑事件保留。chunk 粒度 = `SilentGapDetector` 输出的自然 chunk。

**LR4 mix 当前位置**：仍在 audio thread (`processBlock` → `CrossoverMixer::processSample`
单 sample 风格)。Phase F+ 计划下沉到 chunkRenderWorker 内部，让 PitchCache 直接
存全频段 mix 后 PCM——但 v7 当前实现为渐进迁移，audio thread 仍执行 LR4 mix。

## 阶段 2：Time stretch → SoundTouch WSOLA → TimeStretchCache

**Owner**：`OpenTuneAudioProcessor::stage2WorkerLoop`，独立 worker thread（独立于
chunkRenderWorker，避免 RB ~5× 实时 CPU 阻塞 Stage 1 路径）。
**输入**：Stage 1 的输出（PitchCache + dry + LR4 mix），通过 `readPlaybackAudio` 读取
（`timeStretchCache=nullptr` 模式 + 专用 `CrossoverMixer`），完整拼接为 clip-wide
buffer 后送入 RB。
**输出**：clip-wide stretched PCM 写入 `TimeStretchCache`（一 materialization 一
单条目）。

**SoundTouch 生命周期 per edit**（`runStage2RebuildForMaterialization`）：

```
1. snap = store.getSnapshot(matId)
2. 若 snap.timeGrid->isIdentity() → 跳过 + invalidate Stage 2 cache + return
3. st = store.getOpenTuneStretcher(matId, sr, ch)              // lazy construct
4. schedule = st->buildTempoScheduleFromTimeGrid(*snap.timeGrid)
5. st->beginRebuild(schedule)                                   // clear + cache schedule
6. for chunk in input chunks:                                   // single pass
       st->push(chunk, n, isLast)                                // setTempo per-segment + putSamples
       drain st->pull(...) into output buffer
7. (push(isLast=true) already calls SoundTouch::flush internally)
8. truncate-or-zero-pad output to expectedOutputSamples (端点严格守恒)
9. tsCache.store(matId, output, snap.pitchRevision, snap.timeGridRevision, sr)
```

详见 SoundTouch 替换 RB 的 `openspec/changes/swap-time-stretch-to-soundtouch/design.md`
（chorus 根因诊断 + WSOLA 时域算法选型 + TempoSchedule 数学）。

## 双 cache 命中规则

| Cache | Key | Hit 条件 |
|-------|-----|---------|
| PitchCache (RenderCache) | `(matId, chunkKey)` | `chunk.publishedRevision == chunk.desiredRevision` |
| TimeStretchCache | `matId` | `entry.pitchRevision == matEntry.pitchRevision && entry.timeGridRevision == matEntry.timeGridRevision` |

`readPlaybackAudio` 优先消费 TimeStretchCache（fast path），miss 时回退到 dry +
RenderCache overlay + LR4 mix（与 v6 行为兼容）。

## 失效规则

| 用户操作 | PitchCache(RenderCache) | TimeStretchCache |
|---------|-------------------------|------------------|
| **编辑 PitchCurve / Note** | 仅相交 chunk 失效（chunk-wise revision++）| **整 matId 条目失效** |
| **拖 TimeHandle**（`setTimeGrid`）| **不变** ⚡（v7 核心保证）| **整 matId 条目失效** + Stage 2 重建入队 |
| **删除 materialization** | 全部失效（entry 销毁）| 全部失效（`tsCache.invalidate(matId)`）|
| **clear()** | 全部失效 | 全部失效 |

实现参见 `MaterializationStore::setPitchCurve` / `setTimeGrid` /
`deleteMaterialization`，每条路径同步调用 `timeStretchCache_.invalidate(...)`。
锁定的核心保证：**拖 handle 永不让 NSF 重跑**——由 `Integration_HandleDrag_KeepsRenderCacheChunks`
单测固化。

## Bypass 不变量

恒等 PitchCurve + 恒等 TimeGrid → `readPlaybackAudio` 返回 byte-equal 源 PCM：

- Stage 2 bypass：`snap.timeGrid->isIdentity()` 时 worker 跳过 RB，
  `PlaybackReadSource::timeGridIsIdentity = true`，readPlaybackAudio 不查 TimeStretchCache
- Stage 1 bypass：PitchCurve 恒等 + RenderCache 空时 `overlayPublishedAudioForRate`
  no-op，dry signal 直接写入 destination

由 `Invariant_BypassInvariant_DualBypassEqualsSourcePCM` 单测固化（8192 样本 sine
读回 byte-equal source）。

## RB Offline 模式硬约束

`Source/Inference/SoundTouchStretcher.cpp` 包装 `soundtouch::SoundTouch`
(`OptionProcessOffline + EngineFiner + TransientsCrisp + FormantPreserved +
PitchHighQuality + WindowStandard`)：

- `setKeyFrameMap` **只能** 在 reset 后、第一次 process 前调用
- 每次编辑（PitchCurve 或 TimeGrid revision++）都必须调用 `beginRebuild` 走
  reset → setTimeRatio → setKeyFrameMap → study → process 全周期
- Phase 状态机 (`Idle / Studying / Processing`) 拒绝调用顺序违反

由 `Invariant_SoundTouchStretcher_ResetReappliesSchedule` 单测固化。

## 关键性能预算

| 阶段 | 耗时（M1 Pro / 30s vocal） | 来源 |
|------|---------------------------|------|
| Stage 1 NSF + LR4 | ~6 秒 / 30 秒（5× 实时）| 现有 baseline |
| Stage 2 RB study + process | ~6–7 秒 / 30 秒（~5× 实时）| DESIGN.md §11.6 |
| Cache LRU 容量 | 256 MB（共享池）| `RenderCache::globalCacheLimitBytes()` |

UX 上 stage 2 在 drag-release 时触发，不在 drag move 中实时跑——避免连续编辑
时 worker 队列堆积。

## 重构者预警 checklist

修改下面任一路径时**必须**核对本管线契约：

- [ ] `MaterializationStore::setPitchCurve / setTimeGrid / deleteMaterialization`
      （TimeStretchCache invalidate 时机不能漏）
- [ ] `OpenTuneAudioProcessor::readPlaybackAudio`（Stage 2 fast-path / Stage 1 fallback
      顺序不能颠倒）
- [ ] `OpenTuneAudioProcessor::runStage2RebuildForMaterialization` 与
      `requestStage2Rebuild` (worker 入队 + identity skip + revision 重读)
- [ ] `SoundTouchStretcher::beginRebuild` 调用顺序（reset → setRatio → setKeyFrameMap）
- [ ] `RenderCache::globalCacheCurrentBytes()` 共享 LRU 计数（TimeStretchCache 进出
      也必须同步累加/减去）

跑全部 vocal-time-stretch suite（time-grid + dsp-detection + soundtouch +
time-stretch-cache + matstore-timegrid + stage2-worker + timetool-handler +
integration-pipeline + invariant-contract，共 9 suite 72 tests）确认本管线
契约不被新逻辑污染。

## 外部引用

- 实现: `Source/PluginProcessor.cpp::stage2WorkerLoop` / `runStage2RebuildForMaterialization`
- 缓存: `Source/Inference/RenderCache.{h,cpp}` (PitchCache alias 在
  `Source/Inference/PitchCache.h`) + `Source/Inference/TimeStretchCache.{h,cpp}`
- 拉伸引擎: `Source/Inference/SoundTouchStretcher.{h,cpp}`
- 设计冻结: `research/p0_time_stretch/DESIGN.md` v7 §5
- vocal-time-stretch change: `openspec/changes/vocal-time-stretch/specs/two-stage-render-pipeline/spec.md`
