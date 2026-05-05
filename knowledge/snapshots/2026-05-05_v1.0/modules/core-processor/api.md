---
spec_version: 1.0.0
status: draft
module: core-processor
doc_type: api
generated_by: arch-doc-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# Core-Processor 模块 — API 接口契约

> 本模块无 HTTP Controller。此文档记录 Core-Processor 对外暴露的 C++ 编程接口契约（类/结构体/公共方法签名 + 线程安全）。

---

## 1. OpenTuneAudioProcessor（核心处理器）

**文件**：`Source/PluginProcessor.h` / `.cpp`
**命名空间**：`OpenTune`
**继承**：`juce::AudioProcessor`, `juce::AsyncUpdater`, （可选）`juce::AudioProcessorARAExtension`

JUCE 插件运行时外壳，组合三层 Store、推理服务、渲染调度。

### 构造 / 析构

```cpp
OpenTuneAudioProcessor();
~OpenTuneAudioProcessor() override;
```

- 构造：初始化 `AppLogger`、创建 `editVersion` AudioParameterInt、构造 `SourceStore` / `MaterializationStore` / `StandaloneArrangement` / `ResamplingManager`；**不**触发 ONNX 初始化。
- 析构：取消 `AsyncUpdater`、停止 chunk render worker（join）、`vocoderDomain_->shutdown()` → `f0Service_->shutdown()`、`AppLogger::shutdown()`。

### JUCE AudioProcessor 覆盖

| 方法 | 签名 | 描述 |
|------|------|------|
| `prepareToPlay` | `void prepareToPlay(double sampleRate, int samplesPerBlock)` | 写入 `currentSampleRate_`；fadeOut=200ms；准备 `trackMixScratch_`/`clipReadScratch_`/`doublePrecisionScratch_`；调用 `materializationStore_->prepareAllCrossoverMixers`；加载 PianoKeyAudition 样本 |
| `releaseResources` | `void releaseResources()` | `isPlaying_=false`；若 ARA 开启则 `releaseResourcesForARA()` |
| `processBlock` | `void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&)` | 实时混音主链路，详见 business.md |
| `processBlock` | `void processBlock(juce::AudioBuffer<double>&, juce::MidiBuffer&)` | 借 `doublePrecisionScratch_` 转 float 调上面的实现 |
| `supportsDoublePrecisionProcessing` | `bool` | 返回 true |
| `isBusesLayoutSupported` | `bool (const BusesLayout&) const` | Output 必须 stereo；Input 允许 mono/stereo |
| `createEditor` | `juce::AudioProcessorEditor*` | 委托给 `createOpenTuneEditor(*this)` |
| `hasEditor` | `bool` | true |
| `getStateInformation` | `void (juce::MemoryBlock&)` | 项目状态序列化（含版本魔数，详见 data-model.md） |
| `setStateInformation` | `void (const void*, int)` | 反序列化并重建 Sources/Materializations/Placements |
| `acceptsMidi` / `producesMidi` / `isMidiEffect` | `bool` | 均 false |
| `getTailLengthSeconds` | `double` | 0.0 |
| `getNumPrograms` / `getCurrentProgram` / `setCurrentProgram` / `getProgramName` / `changeProgramName` | — | 单程序占位实现 |

### Import API（两阶段）

```cpp
struct PreparedImport {
    juce::String displayName;
    juce::AudioBuffer<float> storedAudioBuffer;   // 已重采样到 44.1kHz
    std::vector<SilentGap> silentGaps;
    SourceWindow sourceWindow;
};
struct ImportPlacement { int trackId{-1}; double timelineStartSeconds{0.0}; bool isValid() const noexcept; };
struct CommittedPlacement { uint64_t sourceId, materializationId, placementId; bool isValid() const noexcept; };
struct MaterializationRefreshRequest {
    uint64_t materializationId{};
    bool preserveCorrectionsOutsideChangedRange{false};
    double changedStartSeconds{0.0}, changedEndSeconds{0.0};
};
```

| 方法 | 签名 | 线程 | 描述 |
|------|------|------|------|
| `prepareImport` | `bool prepareImport(juce::AudioBuffer<float>&&, double inSampleRate, const juce::String& displayName, PreparedImport& out)` | Worker | 拒绝 0 采样 / 0 通道 / 非正采样率；重采样到 `kRenderSampleRate=44100` 并填充 `storedAudioBuffer`；`silentGaps` 清空延后异步计算 |
| `commitPreparedImportAsPlacement` | `CommittedPlacement commitPreparedImportAsPlacement(PreparedImport&&, const ImportPlacement&, uint64_t sourceId=0)` | Main | 调 `ensureSourceAndCreateMaterialization` + `StandaloneArrangement::insertPlacement`；失败时级联回滚 |
| `commitPreparedImportAsMaterialization` | `uint64_t commitPreparedImportAsMaterialization(PreparedImport&&, uint64_t sourceId=0)` | Main | 只生 source+materialization，不生 placement（ARA 场景） |
| `requestMaterializationRefresh` | `bool requestMaterializationRefresh(const MaterializationRefreshRequest&)` | Main | 按 changed range 擦除旧 notes/curve；设 `OriginalF0State::Extracting`；提交到 `materializationRefreshService_`；commit 回 main thread |
| `ensureSourceById` | `bool ensureSourceById(uint64_t, const juce::String&, std::shared_ptr<const juce::AudioBuffer<float>>, double sampleRate)` | Main | 若 source 不存在则创建 |
| `ensureAraRegionMaterialization` | `std::optional<AraRegionMaterializationBirthResult> ensureAraRegionMaterialization(juce::ARAAudioSource*, uint64_t sourceId, std::shared_ptr<const juce::AudioBuffer<float>>, double sampleRate, const SourceWindow&, double playbackStartSeconds)` | Main (ARA) | 先调 `findMaterializationBySourceWindow` 复用；否则裁剪 window 后走导入 pipeline |

### Playback Read API（统一读取）

```cpp
struct PlaybackReadSource {
    std::shared_ptr<RenderCache> renderCache;
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    bool hasAudio() const;   // buffer 非空且样本 > 0
    bool canRead() const;    // hasAudio()
};
struct PlaybackReadRequest {
    PlaybackReadSource source;
    double readStartSeconds{0.0};
    double targetSampleRate{44100.0};
    int numSamples{0};
};
int readPlaybackAudio(const PlaybackReadRequest&,
                     juce::AudioBuffer<float>& destination,
                     int destinationStartSample,
                     CrossoverMixer* mixer = nullptr) const;
```

- **线程安全**：`readPlaybackAudio` 仅读，无外部锁；依赖 `RenderCache::overlayPublishedAudioForRate` 自身的 `ScopedTryLock` 保证非阻塞。
- **流程**：源 buffer 线性插值到 `targetSampleRate` 写入 destination（dry）；若 `renderCache != nullptr` 叠加已发布 chunk 音频（wet）；若提供 `mixer` 则用其对 dry/wet 做频段分离混合（HPF(dry)+LPF(wet)）。
- **返回**：实际写入采样数（0 = 读取失败或超出源时长）。

### Chunk 渲染 API

| 方法 | 签名 | 描述 |
|------|------|------|
| `enqueueMaterializationPartialRenderById` | `bool (uint64_t materializationId, double relStartSeconds, double relEndSeconds)` | 调用 `materializationStore_->enqueuePartialRender`（hopSize 取 `vocoderDomain_->getVocoderHopSize()` 或默认 512），唤醒 worker |
| `getMaterializationChunkStatsById` | `RenderCache::ChunkStats (uint64_t) const` | 委托给 materialization 对应的 RenderCache |
| `getMaterializationChunkBoundariesById` | `bool (uint64_t, std::vector<double>& outSeconds) const` | 返回 chunk 边界时间戳 |
| `freezeRenderBoundaries` (静态) | `bool (const MaterializationSampleRange&, int64_t startSample, int64_t endSampleExclusive, int hopSize, FrozenRenderBoundaries& out)` | 将请求范围对齐到 hopSize；最后 chunk 允许 partial，中间必须对齐；返回 synth/publish 分离 |
| `preparePublishedAudioFromSynthesis` (静态) | `bool (const FrozenRenderBoundaries&, const std::vector<float>& synthesizedAudio, std::vector<float>& publishedAudio)` | 从 synthSampleCount 裁剪出 publishSampleCount 部分 |

### Materialization 读写代理（委托 MaterializationStore）

| 方法 | 签名 | 线程安全 |
|------|------|----------|
| `getMaterializationAudioBufferById` | `std::shared_ptr<const juce::AudioBuffer<float>> (uint64_t) const` | 是 |
| `getMaterializationAudioDurationById` | `double (uint64_t) const noexcept` | 是 |
| `getMaterializationSnapshotById` | `bool (uint64_t, MaterializationSnapshot&) const` | 是 |
| `getPlaybackReadSourceByMaterializationId` | `bool (uint64_t, PlaybackReadSource&) const` | 是 |
| `getMaterializationPitchCurveById` / `setMaterializationPitchCurveById` | 见 .h | 是 |
| `getMaterializationOriginalF0StateById` / `setMaterializationOriginalF0StateById` | 见 .h | 是 |
| `getMaterializationDetectedKeyById` / `setMaterializationDetectedKeyById` | 见 .h | 是 |
| `getMaterializationNotesById` / `getMaterializationNotesSnapshotById` / `setMaterializationNotesById` | 见 .h | 是 |
| `setMaterializationCorrectedSegmentsById` | `bool (uint64_t, const std::vector<CorrectedSegment>&)` | 是 |
| `commitMaterializationNotesAndSegmentsById` | `bool (uint64_t, notes, segments)` | 是 |
| `commitAutoTuneGeneratedNotesByMaterializationId` | `bool (uint64_t, notes, startFrame, endFrameExclusive, retuneSpeed, vibratoDepth, vibratoRate, audioSampleRate)` | 是 |
| `replaceMaterializationAudioById` | `bool (uint64_t, std::shared_ptr<const juce::AudioBuffer<float>>, std::vector<SilentGap>)` | 是 |
| `replaceMaterializationWithNewLineage` | `uint64_t (uint64_t oldId, MaterializationStore::CreateMaterializationRequest)` | 是 |
| `extractImportedClipOriginalF0` | `bool (const MaterializationSnapshot&, F0ExtractionService::Result&, std::string& errorMessage)` | 调用方保证 F0 服务可用 |

### Placement 读写代理 / 操作

| 方法 | 描述 |
|------|------|
| `getPlacementId(trackId, placementIndex)` | 通过 active index 查 id |
| `findPlacementIndexById(trackId, placementId)` | 通过 id 查 active index |
| `getPlacementByIndex` / `getPlacementById` | 返回 `StandaloneArrangement::Placement` |
| `splitPlacementAtSeconds(trackId, placementIndex, splitSeconds)` | 返回 `std::optional<SplitOutcome>`；校验 ≥0.1s 余量、分割采样在合法范围；创建 leading/trailing materialization 并 retire 原始 |
| `mergePlacements(trackId, leadingId, trailingId, targetIndex)` | 返回 `std::optional<MergeOutcome>`；要求时间邻接（epsilon=1/44100）、同 source、连续 sourceWindow、相同 gain/name/colour、detectedKey 一致、非 Extracting 态 |
| `deletePlacement(trackId, placementIndex)` | 返回 `std::optional<DeleteOutcome>`；retire placement + materialization |
| `movePlacementToTrack(sourceTrackId, targetTrackId, placementId, newTimelineStartSeconds)` | 跨轨/同轨移动 |
| `scheduleReclaimSweep()` | `triggerAsyncUpdate()` 到消息线程 |
| `runReclaimSweepOnMessageThread()` | 三阶段物理回收（placement → materialization → source） |

### Transport / 状态

| 方法 | 签名 | 说明 |
|------|------|------|
| `setPlaying(bool)` / `isPlaying()` | — | 带 200ms fade-out 停止；Play 重置 fade 状态 |
| `setPlayingStateOnly(bool)` | — | 直接写 atomic，不处理 fade |
| `setLoopEnabled(bool)` / `isLoopEnabled()` | — | loop 原子开关 |
| `setPosition(double seconds)` / `getPosition()` | — | 写 `positionAtomic_` |
| `getPositionAtomic()` | `std::shared_ptr<std::atomic<double>>` | UI 共享指针实时读取 |
| `setBpm(double)` / `getBpm()` | — | Standalone 返回 `bpm_`；ARA/VST3 返回 `hostTransportSnapshot.bpm` |
| `getTimeSigNumerator` / `getTimeSigDenominator` | — | 同上；Standalone 固定 4/4 |
| `getPlayStartPosition()` / `setPlayStartPosition(double)` | — | 记录 Play 按下时的位置 |
| `getHostTransportSnapshot()` | `HostTransportSnapshot` | SpinLock 保护的 host 传输快照 |
| `recordControlCall(DiagnosticControlCall)` | — | 记录最近一次 Play/Pause/Stop/Seek 供诊断 |
| `getDiagnosticInfo(trackId=0, placementId=0) const` | `DiagnosticInfo` | 合并 edit version / materialization id / published revision / chunk stats |

`HostTransportSnapshot`：isPlaying、timeSeconds、bpm、ppqPosition、loopEnabled、loopPpqStart/End、isRecording、timeSignatureNumerator/Denominator。

### 推理后端控制

| 方法 | 描述 |
|------|------|
| `resetInferenceBackend(bool forceCpu)` | 停 worker → 释放 `vocoderDomain_`/`f0Service_` → `AccelerationDetector::getInstance().reset().detect(forceCpu)`；worker 下次入队时惰性重启 |
| `isInferenceReady() const` | `f0Ready_.load()` |
| `getF0Service()` / `getVocoderDomain()` | 原始指针（可能为 nullptr，未初始化时） |

### 导出 API

| 方法 | 描述 |
|------|------|
| `exportPlacementAudio(trackId, placementIndex, juce::File)` | 单片段导出（见 business.md） |
| `exportTrackAudio(trackId, juce::File)` | 单轨道导出，时长以最晚 placement 结束为准 |
| `exportMasterMixAudio(juce::File)` | 总线混音导出 |
| `getLastExportError() const` | 最近一次导出错误信息 |

### 其他访问器

- `getSampleRate()` / `getStoredAudioSampleRate()` (static, =44100)
- `getSourceStore() / getMaterializationStore() / getStandaloneArrangement()`（const 与非 const 重载）
- `setTrackHeight / getTrackHeight`（UI 共享）
- `setShowWaveform / getShowWaveform / setShowLanes / getShowLanes`
- `setZoomLevel / getZoomLevel`
- `getUndoManager() / getPianoKeyAudition()`
- `#if JucePlugin_Enable_ARA`: `getDocumentController()`, `didBindToARA()`（替换 shared stores）

### 关键 Outcome 结构

```cpp
struct SplitOutcome { int trackId; uint64_t sourceId;
    uint64_t originalPlacementId, originalMaterializationId;
    uint64_t leadingPlacementId, trailingPlacementId;
    uint64_t leadingMaterializationId, trailingMaterializationId; };
struct MergeOutcome { int trackId; uint64_t sourceId;
    uint64_t leadingPlacementId, trailingPlacementId,
             leadingMaterializationId, trailingMaterializationId;
    uint64_t mergedPlacementId, mergedMaterializationId; };
struct DeleteOutcome { int trackId; uint64_t sourceId, placementId, materializationId; };
struct AraRegionMaterializationBirthResult { uint64_t sourceId, materializationId, materializationRevision;
    double materializationDurationSeconds; };
struct FrozenRenderBoundaries { int64_t trueStartSample, trueEndSample, synthEndSample,
    publishSampleCount, synthSampleCount; int frameCount; int hopSize; };
struct MaterializationSampleRange { int64_t startSample, endSampleExclusive;
    int64_t sampleCount() const; bool isValid() const; };
struct DiagnosticInfo { int editVersion; uint64_t materializationId, placementId,
    publishedRevision, desiredRevision;
    juce::String lastControlCall; juce::int64 lastControlTimestamp;
    RenderCache::ChunkStats chunkStats; };
enum class DiagnosticControlCall : uint8_t { None, Play, Pause, Stop, Seek };
```

### 自由函数

```cpp
void fillF0GapsForVocoder(std::vector<float>& f0,
                          const std::shared_ptr<const PitchCurveSnapshot>& snap,
                          double frameStartTimeSec, double frameEndTimeSec,
                          double hopDuration, double f0FrameRate,
                          bool allowTrailingExtension);
```
填充渲染前 F0 曲线的无声间隙，确保声码器输入连续。 ⚠️ 待补充：gap 填充策略（最近邻 vs 线性插值）需查 `.cpp`。

---

## 2. SourceStore（源音频仓库）

**文件**：`Source/SourceStore.h` / `.cpp`
**命名空间**：`OpenTune`
**线程安全**：`juce::ReadWriteLock` 保护 `sources_` map

### 嵌套类型

```cpp
struct CreateSourceRequest {
    juce::String displayName;
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    double sampleRate{0.0};
};
struct SourceSnapshot {
    uint64_t sourceId;
    juce::String displayName;
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    double sampleRate; int numChannels; int64_t numSamples;
};
```

### 方法

| 方法 | 返回 / 语义 |
|------|-------------|
| `createSource(CreateSourceRequest, uint64_t forcedSourceId=0)` | 返回新 sourceId；forcedSourceId 非零且已存在时直接返回；拒绝空 buffer / 0 channel / 0 samples |
| `clear()` | 清空所有 source（含 retired）；nextSourceId_=1 |
| `deleteSource(uint64_t)` | 物理删除（不检查 retired） |
| `containsSource(uint64_t) const` | active（非 retired）时 true |
| `getSnapshot(uint64_t, SourceSnapshot&) const` | bool；无论 retired 与否均返回 |
| `getAudioBuffer(uint64_t, std::shared_ptr<const juce::AudioBuffer<float>>&) const` | 同上 |
| `retireSource(uint64_t)` / `reviveSource(uint64_t)` | 软删除/恢复 |
| `isRetired(uint64_t) const` | 查询 retired 状态 |
| `physicallyDeleteIfReclaimable(uint64_t)` | 仅 retired 的可物理删除 |
| `getRetiredSourceIds() const` | 遍历返回所有 retired id |

### 不变量

- `sourceId != 0`（0 保留为 null）
- `nextSourceId_` 单调递增
- `forcedSourceId` 路径用于 Undo 恢复，会更新 `nextSourceId_` 到 `max+1`

---

## 3. MaterializationStore（可编辑载荷仓库）

**文件**：`Source/MaterializationStore.h` / `.cpp`
**命名空间**：`OpenTune`
**线程安全**：`juce::ReadWriteLock` 保护 map；独立 `std::mutex renderQueueMutex_` 保护 render queue

### 嵌套类型

```cpp
struct CreateMaterializationRequest {
    uint64_t sourceId;
    uint64_t lineageParentMaterializationId{0};
    SourceWindow sourceWindow;
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    std::shared_ptr<PitchCurve> pitchCurve;
    OriginalF0State originalF0State{OriginalF0State::NotRequested};
    DetectedKey detectedKey;
    std::shared_ptr<RenderCache> renderCache;
    std::vector<Note> notes;
    std::vector<SilentGap> silentGaps;
    uint64_t renderRevision{0};
};
struct PlaybackReadSource { /* renderCache + audioBuffer + hasAudio/canRead() */ };
struct MaterializationSnapshot { /* 完整只读快照，见 data-model.md */ };
struct MaterializationNotesSnapshot { std::vector<Note> notes; uint64_t notesRevision; };
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

### 方法

| 方法 | 语义 |
|------|------|
| `createMaterialization(CreateMaterializationRequest, uint64_t forcedId=0)` | 拒绝 sourceId=0 / null buffer；无 renderCache 时自动 `std::make_shared<RenderCache>()`；notesRevision=1 |
| `clear()` | 清空 map + 清空 renderQueue；nextId=1 |
| `deleteMaterialization(uint64_t)` | 物理删除（不检查 retired） |
| `containsMaterialization(uint64_t) const` | active 才 true |
| `hasMaterializationForSource(uint64_t) const` | active 才算 |
| `hasMaterializationForSourceAnyState(uint64_t) const` | 含 retired |
| `retireMaterialization(uint64_t)` | 置 retired + 清空关联 RenderCache |
| `reviveMaterialization(uint64_t)` | 恢复 |
| `isRetired(uint64_t) const` | 查询 |
| `physicallyDeleteIfReclaimable(uint64_t)` | 仅 retired 时物理删除 |
| `getRetiredIds() const` | 遍历返回 |
| `getSourceIdAnyState(uint64_t) const` | 返回关联 sourceId（无论 retired） |
| `getAudioBuffer / getPlaybackReadSource / getSnapshot / getRenderCache` | 只读访问器 |
| `getPitchCurve / setPitchCurve` | curve 读写（写自动递增 notesRevision? ⚠️ 待补充：仅 notes 递增 revision） |
| `commitNotesAndPitchCurve(uint64_t, notes, curve)` | 原子写 |
| `getOriginalF0State / setOriginalF0State` | 枚举读写 |
| `getDetectedKey / setDetectedKey` | 读写 |
| `getNotes / getNotesSnapshot / setNotes` | setNotes 递增 notesRevision |
| `setSilentGaps(uint64_t, std::vector<SilentGap>)` | 替换 |
| `replaceAudio(uint64_t, buffer, silentGaps)` | 替换 audio + silentGaps；重置 detectedKey 与 originalF0State 为 NotRequested；sourceWindow **不变** |
| `replaceMaterializationWithNewLineage(uint64_t oldId, CreateMaterializationRequest)` | 原子：删除 oldId + 插入新 entry（新 id）；调用方需重定向所有引用 |
| `prepareAllCrossoverMixers(sampleRate, maxBlockSize)` | 遍历所有 materialization，调 `renderCache->prepareCrossoverMixer(sr, block, 2)` |
| `enqueuePartialRender(materializationId, relStartSeconds, relEndSeconds, hopSize)` | 按 silentGaps 分 chunk；调用 `RenderCache::requestRenderPending` 并入队；两步加锁 |
| `hasPendingRenderJobs() const` | queue 非空 |
| `pullNextPendingRenderJob(PendingRenderJob&)` | 从 queue 弹头 → ReadLock 取 RenderCache 的 PendingJob 填充 |
| `getMaterializationAudioDurationById(uint64_t) const noexcept` | 基于 `kRenderSampleRate=44100` 算秒数；retired 或不存在返回 0 |
| `findMaterializationBySourceWindow(sourceId, SourceWindow) const` | 在 active 中按 source + window（epsilon=0.001s）匹配 |
| `buildChunkBoundariesFromSilentGaps` (静态) | 将每个 silent gap 中心向 hopSize 对齐作为 chunk 边界；返回 `[0, ..., sampleCount]` |

### 关键约束

- Chunk 边界 = `[0, hopAlignedSplit..., totalSampleCount]`，去重 + 排序
- `enqueuePartialRender` 不持 WriteLock，两步锁规避重入
- `replaceAudio` 不改 sourceWindow；`replaceMaterializationWithNewLineage` 才改 lineage

---

## 4. StandaloneArrangement（多轨时间轴）

**文件**：`Source/StandaloneArrangement.h` / `.cpp`
**命名空间**：`OpenTune`
**线程安全**：`juce::ReadWriteLock stateLock_` 保护 Track/Placement；`juce::SpinLock snapshotLock_` 保护音频线程可见的 snapshot。

### 嵌套类型

```cpp
struct Placement {
    uint64_t placementId, materializationId, mappingRevision;
    double timelineStartSeconds, durationSeconds;
    float gain{1.0f}; double fadeInDuration, fadeOutDuration;
    juce::String name; juce::Colour colour;
    bool isRetired{false};
    bool isValid() const noexcept;
    double timelineEndSeconds() const noexcept;
};
struct Track {
    std::vector<Placement> placements;
    uint64_t selectedPlacementId{0};
    bool isMuted{false}, isSolo{false};
    float volume{1.0f};
    juce::String name; juce::Colour colour;
    std::atomic<float> currentRmsDb{-100.0f};
};
struct PlaybackTrack { bool isMuted, isSolo; float volume; std::vector<Placement> placements; };
struct PlaybackSnapshot { uint64_t epoch; bool anySoloed; std::array<PlaybackTrack, kTrackCount> tracks; };
using PlaybackSnapshotHandle = std::shared_ptr<const PlaybackSnapshot>;
```

`kTrackCount = 12`。

### 主要方法

| 方法 | 语义 |
|------|------|
| `loadPlaybackSnapshot() const` | SpinLock 保护，返回 `shared_ptr<const PlaybackSnapshot>` |
| `getNumTracks() const noexcept` | 12 |
| `getActiveTrackId / setActiveTrack(int)` | 读/写活动轨道 |
| `setTrackMuted / isTrackMuted(int, bool)` | 写后 `publishPlaybackSnapshotLocked()` |
| `setTrackSolo / isTrackSolo(int, bool)` | 同上 |
| `setTrackVolume(int, float)` / `getTrackVolume(int)` | 写后 publish snapshot；volume 非负 |
| `getTrackRmsDb(int) / setTrackRmsDb(int, float)` | 原子无锁（processBlock 高频写 UI 读） |
| `getNumPlacements(int)` | 只计 active（非 retired）placement |
| `getPlacementId(trackId, activeIndex)` | 按活动索引查 id |
| `findPlacementIndexById(trackId, placementId)` | 返回 active 索引或 -1 |
| `getPlacementByIndex / getPlacementById` | 读 placement |
| `getSelectedPlacementId / getSelectedPlacementIndex / selectPlacement / setSelectedPlacementIndex` | 选择状态 |
| `insertPlacement(trackId, Placement&)` / `insertPlacement(trackId, index, Placement&)` | 分配 `placementId`（写回 placement）；publish |
| `deletePlacementById(trackId, placementId, Placement* out=nullptr, int* outIndex=nullptr)` | 物理删除（不检查 retired） |
| `movePlacementToTrack(sourceTrackId, targetTrackId, placementId, newTimelineStartSeconds)` | 跨轨 move，保留 placementId |
| `setPlacementTimelineStartSeconds / setPlacementGain` | 写后 publish |
| `retirePlacement / revivePlacement / referencesMaterializationAnyState / getRetiredPlacements` | 软删除 API，用于 Undo/GC |
| `clear()` | 清空所有 track |

### 不变量

- 每个 placement 的 `timelineEndSeconds()` 不得重叠（**注：** ⚠️ 待补充：未在 cpp 中看到显式重叠校验）
- `insertPlacement` 分配新 `placementId = nextPlacementId_++`；外部若赋非零值则视为 Undo 恢复
- `selectedPlacementId=0` 表示无选择
- `publishPlaybackSnapshotLocked()` 在 stateLock_ 写锁内，递增 epoch 后写 snapshot

---

## 5. F0ExtractionService（后台 F0 提取任务服务）

**文件**：`Source/Services/F0ExtractionService.h` / `.cpp`
**命名空间**：`OpenTune`

多线程（工作池）任务队列，支持请求去重与 token 取消。

### 嵌套类型

```cpp
struct Result {
    bool success{false};
    int trackId{0}, placementIndexHint{-1};
    uint64_t materializationId{0}, requestKey{0}, requestToken{0};
    std::shared_ptr<const juce::AudioBuffer<float>> sourceAudioBuffer;
    int hopSize{0}, f0SampleRate{0};
    std::vector<float> f0, energy;
    std::vector<SilentGap> silentGaps;
    const char* modelName{"Unknown"};
    std::string errorMessage;
    // Alignment diagnostics
    double audioDurationSeconds{0.0};
    double firstAudibleTimeSeconds{-1.0}, firstVoicedTimeSeconds{-1.0};
    int firstVoicedFrame{-1}, expectedInferenceFrameCount{0};
};
using ExecuteFn = std::function<Result()>;
using CommitFn = std::function<void(Result&&)>;
enum class SubmitResult : uint8_t { Accepted, AlreadyInProgress, QueueFull, InvalidTask };
```

### 方法

| 方法 | 语义 |
|------|------|
| `F0ExtractionService(int workerCount=1, size_t maxQueueSize=64)` | 创建 worker 线程池，每个线程运行 `workerLoop()` |
| `~F0ExtractionService()` | 设置 `running_=false`，join 所有 worker |
| `static makeRequestKey(materializationId, trackId, placementIndex)` | materializationId 非零时用它作为 key；否则高 32 位 trackId + 低 32 位 placementIndex |
| `submit(uint64_t requestKey, ExecuteFn, CommitFn)` | 返回 `SubmitResult`；key=0 / fn null / running=false → InvalidTask；已在 activeEntries → AlreadyInProgress；队列满 → QueueFull |
| `isActive(uint64_t requestKey) const` | 查 activeEntries |
| `cancel(uint64_t requestKey)` | 从 activeEntries 移除；worker 会跳过 token 不匹配的任务 |

### Worker 行为（`workerLoop`）

- 轮询 `LockFreeQueue`（空时 sleep 2ms）
- 运行前 token 校验（防止 cancel 后仍执行 commit）
- `execute()` 抛异常时 `result.success=false` + 记录消息
- 运行后再次 token 校验；通过则从 `activeEntries_` erase 并 `juce::MessageManager::callAsync` 在 main 执行 `commit(result)`

### 全局自由函数

```cpp
inline bool extractOriginalF0ForImportedClip(
    F0InferenceService& f0Service,
    const MaterializationStore::MaterializationSnapshot& snap,
    F0ExtractionService::Result& out,
    std::string& errorMessage);
```
定义于 `Services/ImportedClipF0Extraction.h`。
- 要求 `snap.audioBuffer != nullptr`，否则 `clip_snapshot_failed`
- 以 `TimeCoordinate::kRenderSampleRate=44100` 做时长换算
- Mono 混合多通道，调用 `f0Service.extractF0(...)`
- 填充 alignment 诊断字段（firstAudibleTime、firstVoicedFrame 等）
- 无有声帧时返回 `f0_empty_or_unvoiced`
- Energy 为二值（有声/无声）

---

## 6. AsyncAudioLoader（异步音频加载）

**文件**：`Source/Audio/AsyncAudioLoader.h`（header-only）
**继承**：`juce::Thread`（线程名 `"AudioLoaderThread"`）

### 类型

```cpp
struct LoadResult { bool success; juce::String errorMessage;
                    juce::AudioBuffer<float> audioBuffer; double sampleRate; };
using ProgressCallback = std::function<void(float, const juce::String&)>;
using CompletionCallback = std::function<void(LoadResult)>;
```

### 方法

| 方法 | 语义 |
|------|------|
| 构造 / 析构 | 析构时 `validityToken_ = false` + `stopThread(2000)` |
| `loadAudioFile(file, progressCallback, completionCallback)` | 首先 stopThread，然后启动 worker |
| `run()` override | 通过 `AudioFormatRegistry::registerImportFormats` 注册格式 → `createReaderFor` → 读全文件到 `AudioBuffer<float>`；失败时组装详细诊断消息（文件是否存在、大小、已注册解码器、容器 hex） |
| `cancelLoad()` | `stopThread(2000)` |

### 线程安全

- 所有回调通过 `juce::MessageManager::callAsync` 封送到消息线程
- 使用 `std::shared_ptr<std::atomic<bool>> validityToken_` 作为生命期 guard；析构后回调不会触发

---

## 7. AudioFormatRegistry（音频格式注册 / 探测）

**文件**：`Source/Audio/AudioFormatRegistry.h` / `.cpp`
**命名空间**：`OpenTune::AudioFormatRegistry`

### 类型

```cpp
struct FileProbeResult {
    bool fileExists=false, streamOpened=false;
    juce::int64 fileSize=-1;
    juce::String wildcardFilter, registeredFormats,
                 formatDiagnostics, containerDiagnostics;
};
```

### 函数

| 函数 | 语义 |
|------|------|
| `registerImportFormats(juce::AudioFormatManager&)` | `clearFormats()` → 注册 WAV(默认)、AIFF、FLAC、OggVorbis、CoreAudio(mac/iOS)、MP3(条件)、WMF(Windows) |
| `getImportWildcardFilter()` | 返回 `*.wav;*.aif;...` 形式的字符串 |
| `describeRegisteredImportFormats()` | 逗号分隔的格式名字 |
| `probeFile(const juce::File&)` | 返回 `FileProbeResult`（含 RIFF/WAVE 首 128 字节解析：formatCode、channels、sampleRate、bitsPerSample、extensibleSubFormat 等） |
| `createReaderFor(const juce::File&)` | 返回 `std::unique_ptr<juce::AudioFormatReader>` |

---

## 8. EditorFactory（双格式 Seam）

**文件**：`Source/Editor/EditorFactory.h`，`Editor/EditorFactoryPlugin.cpp`，`Standalone/EditorFactoryStandalone.cpp`
**命名空间**：`OpenTune`

### 声明

```cpp
juce::AudioProcessorEditor* createOpenTuneEditor(OpenTuneAudioProcessor& processor);
```

### 实现

- **Plugin 构建（VST3/ARA）** (`EditorFactoryPlugin.cpp`，`#if !JucePlugin_Build_Standalone`)：
  ```cpp
  return new PluginUI::OpenTuneAudioProcessorEditor(processor);
  ```
- **Standalone 构建** (`EditorFactoryStandalone.cpp`，`#if JucePlugin_Build_Standalone`)：
  ```cpp
  return new OpenTuneAudioProcessorEditor(processor);
  ```

两个 `.cpp` 由条件编译互斥提供，`OpenTuneAudioProcessor::createEditor()` 无分支。

---

## 9. Undo Actions（PlacementActions）

**文件**：`Source/Utils/PlacementActions.h` / `.cpp`
**基类**：`UndoAction`（见 Utils 模块）

所有 Action 都持有 `OpenTuneAudioProcessor&` 引用，通过 retire/revive 组合实现双态切换，不做音频数据物理增删。

| 类 | undo 状态 | redo 状态 | 描述 |
|----|-----------|-----------|------|
| `SplitPlacementAction` | original active + leading/trailing retired | original retired + leading/trailing active | "分割片段" |
| `MergePlacementAction` | leading/trailing active + merged retired | leading/trailing retired + merged active | "合并片段" |
| `DeletePlacementAction` | placement+materialization active | placement+materialization retired | "删除片段" |
| `MovePlacementAction` | 原 trackId + oldStartSeconds | 目标 trackId + newStartSeconds | "移动片段"；通过 `processor.movePlacementToTrack` 实现 |
| `GainChangeAction` | oldGain | newGain | "调整增益"；通过 `arrangement->setPlacementGain` 实现 |

---

## 10. Utility Types（公开使用）

### F0Timeline (`Utils/F0Timeline.h`)

```cpp
struct F0FrameRange { int startFrame=0, endFrameExclusive=0; bool isEmpty() const noexcept; };
class F0Timeline {
public:
    F0Timeline(int hopSize, double sampleRate, int frameCount) noexcept;
    bool isEmpty() const noexcept; int endFrameExclusive() const noexcept;
    double timeAtFrame(int) const noexcept;
    int frameAtOrBefore(double seconds) const noexcept;   // floor + clamp
    int exclusiveFrameAt(double seconds) const noexcept;  // ceil + clamp
    F0FrameRange rangeForTimes(double start, double end) const noexcept;
    F0FrameRange nonEmptyRangeForTimes(double start, double end) const noexcept; // 保证 ≥1 帧
    F0FrameRange rangeForFrames(int start, int endExclusive) const noexcept;
    F0FrameRange rangeForTimesWithMargin(double start, double end, int margin) const noexcept;
};
```

### MaterializationTimelineProjection (`Utils/MaterializationTimelineProjection.h`)

```cpp
struct MaterializationTimelineProjection {
    double timelineStartSeconds, timelineDurationSeconds, materializationDurationSeconds;
    bool isValid() const noexcept;
    double timelineEndSeconds() const noexcept;
    double projectTimelineTimeToMaterialization(double) const noexcept;  // 线性归一化
    double projectMaterializationTimeToTimeline(double) const noexcept;
    double clampTimelineTime(double) const noexcept;
    double clampMaterializationTime(double) const noexcept;
};
```

### SourceWindow (`Utils/SourceWindow.h`)

```cpp
struct SourceWindow {
    uint64_t sourceId{0};
    double sourceStartSeconds{0.0}, sourceEndSeconds{0.0};  // [start, end) source-absolute
    bool isValid() const noexcept;                          // sourceId!=0 && end>start
    double durationSeconds() const noexcept;
};
```

### MaterializationState (`Utils/MaterializationState.h`)

```cpp
enum class OriginalF0State : uint8_t { NotRequested=0, Extracting, Ready, Failed };
```

### AudioEditingScheme (`Utils/AudioEditingScheme.h`)

命名空间 `OpenTune::AudioEditingScheme` 的 inline 工具：

```cpp
enum class Scheme { CorrectedF0Primary=0, NotesPrimary=1 };
enum class ParameterKind { RetuneSpeed, VibratoDepth, VibratoRate };
enum class ParameterTarget { None, SelectedLineAnchorSegments, SelectedNotes,
                             FrameSelection, WholeClip };
enum class AutoTuneTarget { None, SelectedNotes, FrameSelection, WholeClip };
struct FrameRange { int startFrame, endFrameExclusive; bool isValid() const noexcept; };
struct ParameterTargetContext { bool hasSelectedNotes, hasSelectedLineAnchorSegments,
                                     hasFrameSelection, allowWholeClipFallback=true; };
struct AutoTuneTargetContext { int totalFrameCount; FrameRange selectedNotesRange,
                                     selectionAreaRange, f0SelectionRange;
                               bool allowWholeClipFallback=true; };
struct AutoTuneDecision { AutoTuneTarget target; FrameRange range; };

bool usesNotesPrimaryScheme(Scheme);
bool usesVoicedOnlyEditing(Scheme);  // == NotesPrimary
bool isEditableVoicedFrame(float f0Hz);  // f0 > 0
FrameRange clampFrameRange(FrameRange, int totalFrameCount);
bool canEditFrame(Scheme, const std::vector<float>& originalF0, int frameIndex);
FrameRange trimFrameRangeToEditableBounds(Scheme, const std::vector<float>&, FrameRange);
bool shouldSelectNotesForEditedFrameRange(Scheme);
bool allowsLineAnchorSegmentSelection(Scheme);
ParameterTarget resolveParameterTarget(Scheme, ParameterKind, const ParameterTargetContext&);
AutoTuneDecision resolveAutoTuneRange(Scheme, const AutoTuneTargetContext&);
```

---

## ⚠️ 待确认

### 接口歧义

1. **`OpenTuneAudioProcessor::resetInferenceBackend` 的线程假设**：方法注释说"UI 线程"调用，但内部通过条件变量停 worker 并 join；若调用方为音频线程/ARA 线程会阻塞。⚠️ 待明确是否需 `MessageManager::callAsync` 转发。

2. **`MaterializationStore::setPitchCurve` 是否递增 `notesRevision`**：源码显示 `setNotes` 递增 revision，但 `setPitchCurve` / `commitNotesAndPitchCurve` 行为未在本次扫描中确认。⚠️ 待查阅 `MaterializationStore.cpp` 200-400 行。

3. **`StandaloneArrangement::insertPlacement` 外部 placementId 语义**：`Placement& placement` 是引用传递，方法内会写回 `placementId`。若调用方传入非零值（Undo 恢复），是否保留该值还是覆盖？⚠️ 待查 cpp。

### 缺失契约

4. **`fillF0GapsForVocoder` 的 gap 填充策略**：自由函数签名表明对 F0 vector 做 in-place 修改，但填充算法（最近邻 / 线性插值 / clamp 到 note 边界）未在 `.h` 中说明。⚠️ 待查 `PluginProcessor.cpp` 实现。

5. **`readPlaybackAudio` 的 `mixer` 参数所有权**：签名是裸指针，但 const 方法。调用方为 `processBlock` 中的 `placementMixer = &renderCache->getCrossoverMixer()`。⚠️ 需明确 CrossoverMixer 在多 processBlock 调用间的状态持久性与跨 placement 是否共享。

### 隐式约束

6. **`replaceMaterializationWithNewLineage` 要求调用方重指向所有引用**：注释明示"调用方负责把所有指向 oldId 的 placement 重指向 newId"，但 Processor 没有提供原子的 "替换 + rewire" API。⚠️ 建议补充封装或至少在 Processor::replaceMaterializationWithNewLineage 中做此事。

7. **`commitAutoTuneGeneratedNotesByMaterializationId` 的参数语义**：`startFrame`/`endFrameExclusive` 是 F0 帧单位还是 sample 单位，需对照调用方。⚠️ 待补充。
