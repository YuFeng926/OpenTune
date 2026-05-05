---
spec_version: 1.0.0
status: draft
module: core-processor
doc_type: data-model
generated_by: arch-doc-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# Core-Processor 模块 — 数据模型

本文档描述内存数据结构、它们之间的关系，以及持久化到项目文件的序列化布局。

---

## 1. 三层身份模型

OpenTune 的编辑状态按三层事实组织：

```mermaid
graph LR
    subgraph "SourceStore"
      S[Source\n原始音频不可变]
    end
    subgraph "MaterializationStore"
      M1[Materialization A\nsourceWindow ⊆ Source\npitchCurve/notes/detectedKey]
      M2[Materialization B\n(split 后 leading)]
      M3[Materialization C\n(split 后 trailing)]
    end
    subgraph "StandaloneArrangement"
      P1[Placement 1\ntimelineStart/Duration]
      P2[Placement 2]
      T1[Track 0]
      T2[Track 1]
    end
    S --> M1
    S --> M2
    S --> M3
    M1 --> P1
    M2 --> P2
    T1 --> P1
    T2 --> P2
    M2 -. lineageParent .-> M1
    M3 -. lineageParent .-> M1
```

**设计决策**：
- **Source**：原始导入音频的身份，**不**持有任何编辑状态，不可变
- **Materialization**：从 Source 的一个 `SourceWindow` 派生；持有所有可编辑载荷（音频 buffer、pitchCurve、notes、detectedKey、silentGaps、RenderCache）
- **Placement**：只持有 `timelineStartSeconds + durationSeconds + gain + fade`；引用一个 Materialization

---

## 2. 核心内存结构

### 2.1 SourceEntry（`SourceStore` 内部）

| 字段 | 类型 | 描述 |
|------|------|------|
| `sourceId` | `uint64_t` | 非零唯一 id |
| `displayName` | `juce::String` | 源文件名/ARA 源名 |
| `audioBuffer` | `std::shared_ptr<const juce::AudioBuffer<float>>` | 不可变原始 PCM |
| `sampleRate` | `double` | 原始采样率（可能非 44.1kHz） |
| `numChannels` | `int` | 通道数 |
| `numSamples` | `int64_t` | 样本数 |
| `isRetired_` | `bool` | 软删除标记 |

对外暴露 `SourceSnapshot`（字段相同，去掉 retired 标记）。

### 2.2 MaterializationEntry（`MaterializationStore` 内部）

| 字段 | 类型 | 描述 |
|------|------|------|
| `materializationId` | `uint64_t` | 非零唯一 id |
| `sourceId` | `uint64_t` | 关联 Source |
| `lineageParentMaterializationId` | `uint64_t` | 派生父（split 产物指向 original；0=根） |
| `sourceWindow` | `SourceWindow` | Source-absolute 时间窗 `[start, end)` |
| `renderRevision` | `uint64_t` | 渲染版本号 |
| `notesRevision` | `uint64_t` | notes 版本号（`createMaterialization` 时为 1） |
| `audioBuffer` | `shared_ptr<const juce::AudioBuffer<float>>` | **统一 44.1kHz**，等同于 `sourceWindow` 对应区间 |
| `pitchCurve` | `shared_ptr<PitchCurve>` | 音高曲线（含 original + corrected） |
| `originalF0State` | `OriginalF0State` | NotRequested / Extracting / Ready / Failed |
| `detectedKey` | `DetectedKey` | 调式检测结果 |
| `renderCache` | `shared_ptr<RenderCache>` | 分 Chunk 渲染缓存，`retireMaterialization` 会清空 |
| `notes` | `std::vector<Note>` | 音符列表 |
| `silentGaps` | `std::vector<SilentGap>` | 静音段（用于 chunk 边界） |
| `isRetired_` | `bool` | 软删除标记 |

对外暴露 `MaterializationSnapshot`（不含 retired 标记）与 `MaterializationNotesSnapshot`（只有 `notes` + `notesRevision`）。

### 2.3 Placement（`StandaloneArrangement::Placement`）

| 字段 | 类型 | 描述 |
|------|------|------|
| `placementId` | `uint64_t` | 非零唯一 id |
| `materializationId` | `uint64_t` | 引用的 Materialization |
| `mappingRevision` | `uint64_t` | ARA 映射修订 |
| `timelineStartSeconds` | `double` | 在 timeline 上的起点 |
| `durationSeconds` | `double` | 播放时长（可与 materialization 时长不同？⚠️ 待补充是否支持 stretch） |
| `gain` | `float` | 线性增益，默认 1.0 |
| `fadeInDuration` / `fadeOutDuration` | `double` | 秒 |
| `name` / `colour` | juce | 显示元信息 |
| `isRetired` | `bool` | 软删除 |

`isValid()` = `placementId!=0 && materializationId!=0 && durationSeconds>0`。

### 2.4 Track / PlaybackSnapshot

```mermaid
graph TD
    Snap[PlaybackSnapshot\nepoch + anySoloed]
    Snap --> T0[tracks[0]: PlaybackTrack]
    Snap --> T1[tracks[1]: PlaybackTrack]
    Snap --> T11[...tracks[11]]
    T0 --> PL0[placements: vector<Placement>]
    T0 --> MB0[isMuted/isSolo/volume]
```

`PlaybackSnapshot` 是 `shared_ptr<const>`，音频线程 SpinLock 取指针副本后无锁读整个快照。每次状态变更由 `publishPlaybackSnapshotLocked()` 递增 `epoch` 并发布新实例（旧实例自然引用计数回收）。

`anySoloed` 为预计算字段（简化 processBlock 逻辑）。

### 2.5 SourceWindow

```cpp
struct SourceWindow {
    uint64_t sourceId;
    double sourceStartSeconds;   // source-absolute
    double sourceEndSeconds;
};
```

**约束**：
- 合并时必须 `leading.end ≈ trailing.start`（epsilon=0.001s）
- split 时 leading = `[s, s+splitOffset)`，trailing = `[s+splitOffset, e)`
- 导入时 `sourceStart=0, sourceEnd=duration`

### 2.6 RenderCache 接口（详见 inference 模块文档）

Core-Processor 通过以下方式消费 RenderCache：
- `requestRenderPending(startSeconds, endSeconds, startSample, endSampleExclusive)` 提交渲染意图，递增 desiredRevision
- `getNextPendingJob(PendingJob&)` worker 拉取
- `addChunk(trueStartSample, trueEndSample, audio, targetRevision)` 发布完成音频
- `completeChunkRender(startSeconds, revision, CompletionResult)` 状态机转移
- `markChunkAsBlank(startSeconds)` 无 F0 时标记
- `overlayPublishedAudioForRate(destBuffer, destStart, numSamples, readStartSeconds, targetSr)` 音频线程非阻塞读
- `prepareCrossoverMixer(sr, blockSize, channels)` / `getCrossoverMixer()` LR4 分频混音器

### 2.7 PendingRenderJob / PendingRenderEntry

```cpp
// 队列头（仅 id + 时间/样本范围）
struct PendingRenderEntry {
    uint64_t materializationId;
    double startSeconds, endSeconds;
    int64_t startSample, endSampleExclusive;
};
// 拉取后（带完整资源指针 + 目标版本号）
struct PendingRenderJob {
    uint64_t materializationId;
    std::shared_ptr<RenderCache> renderCache;
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    std::shared_ptr<PitchCurve> pitchCurve;
    std::vector<SilentGap> silentGaps;
    double startSeconds, endSeconds;
    int64_t startSample, endSampleExclusive;
    uint64_t targetRevision;
};
```

`pullNextPendingRenderJob` 从 `pendingRenderQueue_` 弹头 + 从 `RenderCache` 拉 `PendingJob` 填入 `targetRevision`。

### 2.8 FrozenRenderBoundaries（渲染边界冻结）

```cpp
struct FrozenRenderBoundaries {
    int64_t trueStartSample;      // 对齐后真实起点
    int64_t trueEndSample;        // 对齐后真实终点（publish 部分）
    int64_t synthEndSample;       // synthesize 的终点（=trueStart + synthSampleCount）
    int64_t publishSampleCount;   // 发布到 RenderCache 的样本数
    int64_t synthSampleCount;     // 声码器 synthesize 的样本数 (≥ publishSampleCount)
    int frameCount;               // mel/f0 帧数
    int hopSize;
};
```

**规则**（见 `freezeRenderBoundaries` 实现）：
- 中间 chunk：`publishSampleCount` 必须整除 `hopSize`，`synthSampleCount == publishSampleCount`
- 最后 chunk：`publishSampleCount` 可能 < `frameCount*hopSize`，此时 `synthSampleCount = frameCount*hopSize` 大于 publish；声码器输出裁剪到 publish

### 2.9 F0Timeline / PitchCurve 时间坐标

```
secondsPerFrame = hopSize / sampleRate    (RMVPE: 160 / 16000 = 10ms)
frameAtOrBefore(seconds) = clamp(floor(seconds / secondsPerFrame), 0, frameCount-1)
exclusiveFrameAt(seconds) = clamp(ceil(seconds / secondsPerFrame), 0, frameCount)
```

### 2.10 HostTransportSnapshot

ARA/VST3 模式下，从 `AudioPlayHead::PositionInfo` 每次 processBlock 更新一次，SpinLock 保护。

| 字段 | 类型 | 来源 |
|------|------|------|
| `isPlaying` / `isRecording` / `loopEnabled` | `bool` | host playhead |
| `timeSeconds` / `ppqPosition` | `double` | host playhead |
| `bpm` | `double` | host |
| `loopPpqStart` / `loopPpqEnd` | `double` | host loop points |
| `timeSignatureNumerator` / `Denominator` | `int` | host |

Standalone 模式下 `getBpm()` 直接用 `bpm_`（默认 120），`getTimeSig*` 返回固定 4/4。

### 2.11 F0ExtractionService::Result

供 `F0ExtractionService` 与 `ImportedClipF0Extraction.h` 共用，带 F0Alignment 诊断字段。详见 [api.md §5](./api.md)。

---

## 3. 数据关系总览（ER 风格）

```
Source (1) ─── (N) Materialization (1) ─── (N) Placement
                       │ lineageParent (0..1)
                       └─── (self-ref) Materialization
Track (12) ─── (N) Placement
Materialization ─── (1) RenderCache
Materialization ─── (0..1) PitchCurve
```

**基数约束**：
- 每个 Track 可含多个 Placement；`MAX_TRACKS=12`
- 每个 Placement 引用恰好一个 Materialization
- 每个 Materialization 引用恰好一个 Source
- 一个 Source 可被多个 Materialization 引用（split 产生两个子 Materialization，均指向同一 source）
- 一个 Materialization 可被零到多个 Placement 引用（零引用时触发 GC）

---

## 4. 软删除与 GC 生命周期

三层 Store 均有 `isRetired_` 标记，通过 `retire*` / `revive*` 切换，由 `runReclaimSweepOnMessageThread` 三阶段推进：

| 阶段 | 动作 |
|------|------|
| Phase 1 | 遍历 `StandaloneArrangement::getRetiredPlacements` → 物理 erase placement |
| Phase 2 | 遍历 `MaterializationStore::getRetiredIds`，若 `arrangement.referencesMaterializationAnyState(id)` 或 ARA published-region 引用非零 → 保留；否则物理删除 + 当 source 无任何 active materialization 时级联 retire source |
| Phase 3 | 遍历 `SourceStore::getRetiredSourceIds`，若 `materializationStore.hasMaterializationForSourceAnyState(sourceId)` → 保留；否则物理删除 |

Undo/Redo 通过 retire/revive 实现可逆切换，**不**破坏数据。

---

## 5. 项目状态序列化（setStateInformation / getStateInformation）

仅 **Standalone 构建**使用，VST3/ARA 由 host 管理状态。从 `Source/PluginProcessor.cpp` L1710-L2029 的实现可推导出以下结构（⚠️ 字段顺序与 tag 值未在此次扫描中完全列出，需查阅 `writeAudioBuffer`/`readAudioBuffer` 等 helper）。

### 序列化元素

- **Version magic**：头部 4 字节魔数（⚠️ 待查阅具体值）
- **EditVersion**：`editVersionParam_` 当前值
- **Sources**：每个 active source → sourceId + displayName + sampleRate + numChannels + AudioBuffer PCM
- **Materializations**：每个 active materialization →
  - materializationId / sourceId / lineageParentMaterializationId
  - sourceWindow (sourceId + start + end)
  - renderRevision / notesRevision
  - OriginalF0State / DetectedKey
  - PitchCurve（含 corrected segments）
  - Notes 列表（每条 note 字段，见 `writeNotes`）
  - SilentGaps（每段 start/end sample）
  - AudioBuffer
- **Arrangement**：每个 track → mute/solo/volume/name/colour → placements (placementId, materializationId, mappingRevision, timelineStart, duration, gain, fadeIn/Out, name, colour)
- **UI 状态**：zoomLevel、trackHeight、showWaveform、showLanes、activeTrackId 等

### 序列化 Helper（定义于 .cpp 匿名命名空间）

| 函数 | 目的 |
|------|------|
| `writeFloatVector / readFloatVector` | `std::vector<float>` 带长度前缀 |
| `writePitchCurve / readPitchCurve` | PitchCurve 的自定义二进制（含 corrected segments） |
| `writeDetectedKey / readDetectedKey` | 调式 |
| `writeNotes / readNotes` | Note 列表 |
| `writeAudioBuffer / readAudioBuffer` | JUCE AudioBuffer 的 PCM |
| `writeSilentGaps / readSilentGaps` | SilentGap 列表 |
| `appendUniqueMaterializationId` | 避免重复序列化同 id |
| `readBytesExact` | 原始字节读取 |

⚠️ 待补充：完整字段 tag 表、字节顺序、数据字段的版本兼容策略。

---

## 6. ID 空间与分配策略

| 实体 | 类型 | 起始值 | 分配器 | 0 值语义 |
|------|------|--------|--------|---------|
| sourceId | `uint64_t` | 1 | `SourceStore::nextSourceId_ (atomic)` | null |
| materializationId | `uint64_t` | 1 | `MaterializationStore::nextMaterializationId_` | null |
| placementId | `uint64_t` | 1 | `StandaloneArrangement::nextPlacementId_` (非原子，在 stateLock_ 写锁内) | null |

`forcedId` 路径用于 Undo 恢复：直接使用指定 id，若已存在则跳过；分配器会更新 next 到 `max(next, forcedId+1)`。

---

## 7. 编辑方案与参数目标解析

**AudioEditingScheme**（inline 头文件）定义两种编辑模式：

| Scheme | 特征 |
|--------|------|
| `CorrectedF0Primary` | 直接编辑 F0 曲线；任意帧可编辑 |
| `NotesPrimary` | 编辑音符；仅有声帧（`f0 > 0`）可编辑 |

参数（RetuneSpeed / VibratoDepth / VibratoRate）的目标解析优先级（`resolveParameterTarget`）：
1. RetuneSpeed + 有 LineAnchor 选中 + 允许（非 NotesPrimary）→ `SelectedLineAnchorSegments`
2. 有选中 note → `SelectedNotes`
3. 有帧选择 → `FrameSelection`
4. 若允许整片 → `WholeClip`，否则 `None`

AutoTune 范围解析（`resolveAutoTuneRange`）：
- NotesPrimary：SelectedNotes > f0Selection > selectionArea > WholeClip
- CorrectedF0Primary：selectionArea > f0Selection > SelectedNotes > WholeClip

---

## ⚠️ 待确认

### 数据语义

1. **`Placement.durationSeconds` 可否不等于 `materialization.audioBuffer` 时长**：代码显示 split 后 `leadingPlacement.durationSeconds = splitOffsetSeconds`，且 leading materialization 的 audio 也是裁剪后的相同时长。⚠️ 是否支持 time-stretch（placement 长度 ≠ audio 长度），需查 `readPlaybackAudio` / ARA 路径。

2. **`renderRevision` 与 `notesRevision` 的递增时机**：`createMaterialization` 后 `notesRevision=1`，`setNotes` 递增；但 `renderRevision` 字段存在于 `CreateMaterializationRequest` 中可设置初值，ongoing 的递增逻辑未在本次扫描中确认。⚠️ 待查 `RenderCache::desiredRevision` 与 materialization `renderRevision` 的一致性。

3. **`mappingRevision` 在 Standalone 模式下的意义**：该字段在 ARA 场景用于 region mapping，Standalone 模式下 `commitPreparedImportAsPlacement` 初始化为 0，`split` 后不变。⚠️ 是否可直接省略或仅 ARA 使用。

4. **`DetectedKey` 的内部结构**：引用自 `DSP/ChromaKeyDetector.h`，本模块不定义。⚠️ 其字段（tonic/mode/confidence 等）需在 DSP 模块文档中描述。

### 持久化兼容性

5. **版本魔数与升级策略**：无在文件中看到显式 version 字段解析逻辑。⚠️ 待查 `setStateInformation` 开头的 magic 校验代码。

6. **retired 数据是否序列化**：序列化/反序列化流程对 retired 的 source/materialization 的处理策略未明（通常不序列化以减小项目文件）。⚠️ 待查 `getStateInformation` 的过滤逻辑。

### 关系完整性

7. **`lineageParentMaterializationId` 指向 retired 父的场景**：split 后，原 materialization 被 retire，但 leading/trailing 的 `lineageParentMaterializationId` 仍指向它。若后续 GC 物理删除原 materialization，父指针变为悬空。⚠️ GC 是否考虑 lineage 引用？当前 sweep 仅看 placement + ARA region。
