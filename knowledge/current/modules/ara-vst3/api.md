---
spec_version: 1.0.0
status: draft
module: ara-vst3
doc_type: api
generated_by: arch-doc-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# ara-vst3 模块 API 契约

本文档描述 `Source/ARA/` 与 `Source/Plugin/` 对外公开的类接口。ARA 术语（DocumentController、AudioSource、PlaybackRegion、MusicalContext、AudioModification、PlaybackRenderer）保持英文。

---

## 1. `OpenTune::OpenTuneDocumentController`

继承：`juce::ARADocumentControllerSpecialisation`

职责：作为 JUCE ARA SDK 桥接入口。接收宿主的所有 ARA 生命周期回调，将真正的状态管理委托给 `VST3AraSession`，并把 playback 控制请求转发给宿主的 `ARAPlaybackController`。

### 1.1 构造/析构

| 方法 | 说明 |
|------|------|
| `OpenTuneDocumentController(const ARA::PlugIn::PlugInEntry* entry, const ARA::ARADocumentControllerHostInstance* instance)` | 由 ARA SDK 调用；内部构造 `VST3AraSession`、`SourceStore`、`MaterializationStore`、`ResamplingManager`（均以 `shared_ptr` 持有，便于与 Processor 共享） |
| `~OpenTuneDocumentController()` | 析构序 critical：先 `session_->setProcessor(nullptr)` 再让 `session_` unique_ptr 析构 → 在 session 析构中 join hydration worker，避免 worker 回调已销毁的 processor |

### 1.2 Getter / 属性

| 方法 | 返回 | 说明 |
|------|------|------|
| `setProcessor(OpenTuneAudioProcessor*)` | void | 处理器注册；同时写入 `VST3AraSession::processor_` atomic，供 hydration worker 调用 auto-birth |
| `getProcessor()` const | `OpenTuneAudioProcessor*` | 注册的 processor；无锁 |
| `getSession()` | `VST3AraSession*` | 当前 session，**文档级生命周期** |
| `getSharedSourceStore() / getSharedMaterializationStore() / getSharedResamplingManager()` | `shared_ptr<...>` | Processor 通过 DC 共享这三个 store 的所有权 |
| `loadSnapshot()` const | `SnapshotHandle` | 快速读取当前 published snapshot；委托到 session |

### 1.3 ARA 回调（override）

所有方法均由宿主 DAW 在消息线程触发，大多数直接委托到 `session_` 的同名方法。

| 方法 | 调用时机 | 副作用 |
|------|----------|--------|
| `didUpdateMusicalContextProperties(ARAMusicalContext*)` | BPM/节拍变化 | **⚠️ 待补充**：当前仅 log，不回写 processor BPM |
| `willBeginEditing(ARADocument*)` | 编辑事务开始 | `session_->willBeginEditing()`；`editingDepth_++` |
| `didEndEditing(ARADocument*)` | 编辑事务结束 | `session_->didEndEditing()`；depth 归零时 publish pending snapshot |
| `didUpdatePlaybackRegionProperties(ARAPlaybackRegion*)` | 宿主改 region 起止 | 委托 session；projection 变化则 bump revision + markDirty |
| `willDestroyPlaybackRegion(ARAPlaybackRegion*)` | region 销毁前 | session 移除 region slot + `processor_->scheduleReclaimSweep()` |
| `didAddPlaybackRegionToAudioModification(ARAAudioModification*, ARAPlaybackRegion*)` | 新 region 挂载 | ensure source/region slot，更新 preferred |
| `didUpdateAudioSourceProperties(ARAAudioSource*)` | 源属性变更 | 更新 name/sampleRate/channels/numSamples；若 shape 变则 bump contentRevision + 入队 hydration |
| `doUpdateAudioSourceContent(ARAAudioSource*, ARAContentUpdateScopes)` | 源内容更新 | `scopeFlags.affectSamples()` 时 bump revision + 入队 hydration |
| `willEnableAudioSourceSamplesAccess(ARAAudioSource*, bool enable)` | 宿主宣告将启用/禁用采样访问 | enable=false 时立刻清 payload + 失效 reader lease + publish snapshot |
| `didEnableAudioSourceSamplesAccess(ARAAudioSource*, bool enable)` | 采样访问切换完成 | enable=true 时创建 `HostAudioReader` lease 并入队 hydration |
| `willRemovePlaybackRegionFromAudioModification(ARAAudioModification*, ARAPlaybackRegion*)` | region 从 modification 摘除 | 移除 region slot + `scheduleReclaimSweep()` |
| `willDestroyAudioSource(ARAAudioSource*)` | audioSource 销毁前 | 移除 source slot（延迟清理直到 reader 完成） + `scheduleReclaimSweep()` |

### 1.4 Playback Control 转发

调用 `getDocumentController()->getHostPlaybackController()`，将请求回送给宿主。若 DC 或 PlaybackController 为 null 则记录 error 并返回 false。

| 方法 | 返回 | 说明 |
|------|------|------|
| `requestSetPlaybackPosition(double timeInSeconds)` | bool | 请求 host 把播放头移到指定秒 |
| `requestStartPlayback()` | bool | 请求 host 开始播放 |
| `requestStopPlayback()` | bool | 请求 host 停止播放 |

### 1.5 Protected override

| 方法 | 说明 |
|------|------|
| `doRestoreObjectsFromStream(ARAInputStream&, ARARestoreObjectsFilter*)` | 当前无持久化逻辑，直接返回 `true`。**⚠️ 待补充**：ARA persistency 尚未实现 |
| `doStoreObjectsToStream(ARAOutputStream&, ARAStoreObjectsFilter*)` | 同上 |
| `doCreatePlaybackRenderer()` | 返回 `new OpenTunePlaybackRenderer(getDocumentController())` |

### 1.6 工厂函数（命名空间外部）

```cpp
const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();
```
由 VST3 插件入口调用，返回 `ARADocumentControllerSpecialisation::createARAFactory<OpenTuneDocumentController>()`。

---

## 2. `OpenTune::OpenTunePlaybackRenderer`

继承：`juce::ARAPlaybackRenderer`

职责：实时音频线程渲染回调。从 `VST3AraSession` 读一份 immutable snapshot，对每个 assigned playback region 通过 `MaterializationTimelineProjection` 把 host 块时间映射到 materialization 局部时间，然后调用 `processor->readPlaybackAudio()` 从 RenderCache（+ CrossoverMixer）取样累加到输出 buffer。

### 2.1 生命周期方法（override）

| 方法 | 线程 | 说明 |
|------|------|------|
| `prepareToPlay(double sampleRate, int maxSamplesPerBlock, int numChannels, ProcessingPrecision, AlwaysNonRealtime)` | 非 RT | 保存 host 参数；预分配 `playbackScratch_` 到 `{numChannels, maxSamplesPerBlock}` |
| `releaseResources()` | 非 RT | 当前空实现 |
| `processBlock(AudioBuffer<float>&, Realtime, const AudioPlayHead::PositionInfo&) noexcept` | **RT 线程** | 核心渲染，见下 |

### 2.2 `processBlock` 契约

- **输入**：空 buffer（由 caller 传入；Renderer 先 `buffer.clear()` 再累加）、`positionInfo.getTimeInSeconds()`
- **输出**：mix 后的 buffer；返回 `true` 表示至少一个 region 写入了样本
- **不变式**：
  1. `playbackScratch_.getNumChannels() >= buffer.getNumChannels()`（`jassert`）
  2. `playbackScratch_.getNumSamples() >= buffer.getNumSamples()`（`jassert`）
  3. `|playbackDurationSeconds - materializationDurationSeconds| <= 0.001`（否则 skip region + `jassertfalse`）
  4. 一个 block 使用一份 snapshot（`const auto snapshot = session->loadSnapshot();`）
- **短路条件**：
  - `regions.empty()` → clear + return false
  - `dc == nullptr || docController == nullptr` → clear + return false
  - `session == nullptr || processor == nullptr` → clear + return false
  - `materializationStore == nullptr` → clear + return false
  - `!snapshot || snapshot->publishedRegions.empty()` → clear + return false
- **单 region 处理**：
  1. `findRenderableRegionView(*snapshot, region)` 查找且 state 必须是 `Renderable`
  2. `computeRegionBlockRenderSpan(...)` 计算 overlap，无 overlap 直接 skip
  3. `MaterializationTimelineProjection` 投影把 host overlap start 映射到 materialization 局部时间
  4. `materializationStore->getPlaybackReadSource(matId, ...)` 取 RenderCache + audioBuffer 句柄
  5. `processor->readPlaybackAudio(request, playbackScratch_, 0, araMixer)` 写入 scratch
  6. 累加（+=）到 `buffer[destinationStartSample .. destinationStartSample+samplesToMix]`

### 2.3 自由函数

| 函数 | 说明 |
|------|------|
| `std::optional<RenderBlockSpan> computeRegionBlockRenderSpan(double blockStart, int blockSamples, double hostSR, double playbackStart, double playbackEnd) noexcept` | 计算 host block 与 region 时间轴的交集，返回 destination sample range + overlap 起始秒；无交集返回 `nullopt` |

`RenderBlockSpan` 字段：`destinationStartSample`、`samplesToCopy`、`overlapStartSeconds`。

---

## 3. `OpenTune::VST3AraSession`

职责：ARA session 核心状态机。拥有所有 `SourceSlot` / `RegionSlot`、后台 hydration worker 线程、immutable `PublishedSnapshot` 发布通道。Renderer 和 Editor 都通过 `loadSnapshot()` 读取而不加锁。

### 3.1 内嵌类型

| 类型 | 关键字段 | 说明 |
|------|----------|------|
| `RegionIdentity` | `playbackRegion, audioSource` | 指针对作为 region 身份；`isValid()` / `operator==` |
| `AppliedMaterializationProjection` | `sourceId, materializationId, appliedMaterializationRevision, appliedProjectionRevision, appliedSourceWindow, playbackStartSeconds, appliedRegionIdentity` | 记录 region 被绑定到哪个 materialization 以及绑定时的版本 |
| `SourceSlot` | `audioSource, sourceId, name, sampleRate, numChannels, numSamples, copiedAudio, readerLease, retiringReaderLease, contentRevision, hydratedContentRevision, leaseGeneration, sampleAccessEnabled, hostReadInFlight, queuedForHydration, readingFromHost, cancelRead, enablePendingHydration, pendingLeaseReset, pendingRemoval` | 每个 `ARAAudioSource` 对应一份；`hasAudio()` 要求 `copiedAudio != nullptr && numSamples>0 && hydratedContentRevision==contentRevision` |
| `RegionSlot` | `identity, appliedProjection, playbackStartSeconds, playbackEndSeconds, sourceWindow, materializationDurationSeconds, projectionRevision` | `isValid()`: identity valid + end>start + sourceWindow.isValid() |
| `BindingState` (enum uint8_t) | `Unbound, HydratingSource, BoundNeedsRender, Renderable` | 派生逻辑详见 data-model.md |
| `PublishedRegionView` | 所有上面字段的 **不可变只读投影** + `bindingState` | Renderer 只看这个视图 |
| `PublishedSnapshot` | `epoch, preferredRegion, publishedRegions` | `findRegion(playbackRegion)` / `findPreferredRegion()` |
| `SnapshotHandle` | `shared_ptr<const PublishedSnapshot>` | 原子发布 / 订阅单元 |

### 3.2 构造 / 析构

| 方法 | 说明 |
|------|------|
| `VST3AraSession()` | 启动 `hydrationWorkerThread_` |
| `~VST3AraSession()` | 加锁设 `hydrationWorkerRunning_ = false`，对所有 source 设 `cancelRead=true`，`notify_all`，join worker |

### 3.3 静态辅助

| 方法 | 说明 |
|------|------|
| `buildSnapshotForPublication(const std::vector<SourceSlot>&, const std::vector<RegionSlot>&, const RegionIdentity& preferred, uint64_t epoch)` | 从值拷贝构造 snapshot；首先 `reconcilePreferredRegionFromState` 选 fallback，然后逐个 region 调 `buildPublishedRegionViewFromState` 派生 `bindingState` |
| `publishPendingSnapshot(...)` | 仅当 `pendingSnapshotPublication==true` 时重新构建，否则返回 currentSnapshot |
| `makeRegionIdentity(const ARAPlaybackRegion*)` | 安全地 traverse `playbackRegion → audioModification → audioSource`，任一级 null 返回空 identity |

### 3.4 RT-safe 读取

| 方法 | 线程 | 说明 |
|------|------|------|
| `loadSnapshot()` const | 任何线程，包括 RT | `std::atomic_load(&publishedSnapshot_)` — 零拷贝，仅原子 shared_ptr 复制 |

### 3.5 ARA 事件入口（由 DocumentController 调用）

下列方法均先 `lock_guard(stateMutex_)` + `drainDeferredSourceCleanupLocked()`：

| 方法 | 核心行为 |
|------|----------|
| `willBeginEditing()` | `editingDepth_++` |
| `didEndEditing()` | `editingDepth_--`；当 depth==0 且 pendingSnapshotPublication_ 时 `publishSnapshotLocked()` |
| `didUpdatePlaybackRegionProperties(ARAPlaybackRegion*)` | ensure slots, update projection from region, bump projectionRevision (若变化), markDirty |
| `didAddPlaybackRegionToAudioModification(ARAAudioModification*, ARAPlaybackRegion*)` | 同上，并把 region 设为 preferred |
| `didUpdateAudioSourceProperties(ARAAudioSource*)` | 更新 name/sr/channels/numSamples；shape 变 → bump contentRevision + clear payload + 入队 hydration |
| `doUpdateAudioSourceContent(ARAAudioSource*, ARAContentUpdateScopes)` | 仅 `affectSamples()` 时 bump contentRevision + 入队 hydration + markDirty |
| `willEnableAudioSourceSamplesAccess(ARAAudioSource*, bool)` | enable=false 时清 payload + `invalidateSourceReaderLeaseLocked` + markDirty + publish |
| `didEnableAudioSourceSamplesAccess(ARAAudioSource*, bool)` | enable=true 时创建 `HostAudioReader` lease + 入队 hydration；若正在 readingFromHost 则设 `enablePendingHydration` 等 worker 完成后重新入队 |
| `willRemovePlaybackRegionFromAudioModification(ARAPlaybackRegion*)` | `removePlaybackRegionFromStateLocked`；若 editingDepth==0 立即 publish |
| `willDestroyAudioSource(ARAAudioSource*)` | `removeAudioSourceFromStateLocked`；若 editingDepth==0 立即 publish |

### 3.6 Materialization 绑定接口（由 Editor 调用）

| 方法 | 说明 |
|------|------|
| `bindPlaybackRegionToMaterialization(ARAPlaybackRegion*, uint64_t materializationId, uint64_t materializationRevision, uint64_t projectionRevision, SourceWindow, double materializationDurationSeconds, double playbackStartSeconds)` | 填充 regionSlot 的 appliedProjection；立即 `publishSnapshotLocked()` |
| `updatePlaybackRegionMaterializationRevisions(ARAPlaybackRegion*, uint64_t materializationRevision, uint64_t projectionRevision)` | 仅更新 revision 字段（不变 materializationId / window / playbackStart） |
| `clearPlaybackRegionMaterialization(ARAPlaybackRegion*)` | 清空 appliedProjection，region 回到 Unbound 状态 |

### 3.7 辅助接口

| 方法 | 说明 |
|------|------|
| `setProcessor(OpenTuneAudioProcessor*) noexcept` | atomic release-store；供 DC 在构造/析构时同步 |

### 3.8 线程安全摘要

| 数据 | 保护机制 |
|------|----------|
| `sources_ / regions_ / preferredRegion_ / editingDepth_ / pendingSnapshotPublication_ / nextXxxRevision` | `stateMutex_` (`std::mutex`) |
| `publishedSnapshot_` | `std::atomic_load / std::atomic_store` 配合 `shared_ptr<const PublishedSnapshot>` |
| `processor_` | `std::atomic<OpenTuneAudioProcessor*>` (acquire / release) |
| `hydrationQueue_` | `stateMutex_` + `hydrationCv_` (`std::condition_variable`) |
| `hydrationWorkerThread_` | 独占成员，析构时 join |

---

## 4. `OpenTune::PluginUI::OpenTuneAudioProcessorEditor`（VST3 专属）

继承：`juce::AudioProcessorEditor`、`ParameterPanel::Listener`、`MenuBarComponent::Listener`、`TransportBarComponent::Listener`、`PianoRollComponent::Listener`、`LanguageChangeListener`、`juce::Timer`

**编译守卫**：整个文件由 `#if JucePlugin_Build_VST3` 包裹。ARA 特定代码再由 `#if JucePlugin_Enable_ARA` 内嵌守卫。

### 4.1 关键常量

| 常量 | 值 | 说明 |
|------|----|------|
| `TOP_BAR_HEIGHT` | 96 | 顶栏高度（px） |
| `PARAMETER_PANEL_WIDTH` | 240 | 右侧参数栏宽度 |
| `kHeartbeatHz` | 30 | Timer 心跳频率 |

### 4.2 生命周期

| 方法 | 说明 |
|------|------|
| `OpenTuneAudioProcessorEditor(OpenTuneAudioProcessor&)` | 构造：`menuBar_.setVisible(false)`（VST3 隐藏系统菜单），`transportBar_.setLayoutProfile(VST3AraSingleClip)`，`setResizeLimits(960, 640, 3000, 2000)`，`setSize(1280, 820)`，`startTimerHz(30)` |
| `~OpenTuneAudioProcessorEditor()` | stopTimer + 取消所有 listener 注册 + `setLookAndFeel(nullptr)` |

### 4.3 JUCE Component override

| 方法 | 说明 |
|------|------|
| `paint(Graphics&)` | 填背景色 `UIColors::backgroundDark` |
| `resized()` | 布局：top bar → 右侧 parameter panel → 中央 PianoRoll；overlay 与 badge 覆盖其上 |
| `mouseDown(const MouseEvent&)` | 若 PianoRoll 无焦点则 `grabKeyboardFocus()` |
| `keyPressed(const KeyPress&)` | 空格键 → playPauseToggle；Cmd+Z/Y → undo/redo |

### 4.4 Timer 心跳（`timerCallback`）

按顺序执行：
1. 首次 showing 时 grab focus 到 PianoRoll
2. `syncSharedAppPreferences()` — 语言、主题、视觉偏好下发
3. 同步 position / playing / BPM / time signature
4. RMVPE overlay latch 管理（分析完成后自动解除）
5. 驱动 `pianoRoll_.onHeartbeatTick()`
6. 选择渲染 overlay 还是 badge（基于 chunkStats）
7. **`syncMaterializationProjectionToPianoRoll()`**
8. **`syncImportedAraClipIfNeeded()`** — 核心 ARA 同步

### 4.5 ARA 相关 private 辅助

| 方法 | 说明 |
|------|------|
| `uint64_t resolveCurrentMaterializationId()` | 便捷封装，调 `resolveCurrentMaterializationProjection` |
| `bool resolveCurrentMaterializationProjection(uint64_t& matId, MaterializationTimelineProjection& projection)` | 取 preferred region view（若只有一个 region 则视为 preferred），填出 matId 和 pianoRoll 用的 projection（timelineStart=playbackStart, timelineDuration=materializationDuration） |
| `void syncMaterializationProjectionToPianoRoll()` | 把 preferred region 的 projection / pitch curve / audio buffer 推送给 `pianoRoll_`（在 timer + 事件路径 15+ 处调用） |
| `void syncImportedAraClipIfNeeded()` | **核心**：检测 snapshot epoch / preferred region / materialization revision / projection revision / source range / playback start 变化，分类型处理：mapping-only（仅 playbackStart 改）→ bind；content 变 → `replaceMaterializationAudioById`；source range 变 → `replaceMaterializationWithNewLineage`；随后 enqueue 部分渲染并重新 bind |
| `void syncParameterPanelFromSelection()` | 选区 → 参数面板的 retune/vibrato 显示同步 |
| `void syncSharedAppPreferences()` | 读取 AppPreferences，下发主题/语言/视觉偏好到各子组件 |
| `void applyThemeToEditor(ThemeId)` | 切换 `OpenTuneLookAndFeel` / `AuroraLookAndFeel` |

### 4.6 对宿主的"host-managed"回退

在 VST3 模式下，下述操作与 DAW 职能重复，因此弹出 info dialog 提示用户用 DAW 完成：
- `importAudioRequested()` — "Please import audio from your DAW"
- `exportAudioRequested(ExportType)` — "Please render/export from your DAW"
- `savePresetRequested()` / `loadPresetRequested()` — "Use your DAW preset/chunk … workflow"
- `helpRequested()` — "Open the host DAW plugin help/manual entry"

### 4.7 Transport 控制（override PianoRollComponent::Listener / TransportBarComponent::Listener）

所有 play/pause/stop/position 请求在 ARA 模式下会 **优先** 通过 `DocumentController` 转发给 host：

| 方法 | ARA 分支 | 非 ARA 分支 |
|------|----------|-------------|
| `playRequested()` | `docController->requestStartPlayback()` + `setPlayingStateOnly(true)` | `processor.setPlaying(true)` |
| `pauseRequested()` | `docController->requestStopPlayback()` + `setPlayingStateOnly(false)` | `processor.setPlaying(false)` |
| `stopRequested()` | `requestStopPlayback()` + `requestSetPlaybackPosition(0)` + `setPosition(0)` | `setPlaying(false)` + `setPosition(0)` |
| `playheadPositionChangeRequested(double)` | `requestSetPlaybackPosition(t)` + `setPosition(t)` | 仅 `setPosition(t)` |
| `playPauseToggleRequested()` | 依据 `isPlaying()` 调 pause 或 play | 同左 |

### 4.8 Undo/Redo 局部重渲染

`undoRequested()` / `redoRequested()`：
1. `getUndoManager().undo()/redo()`
2. `syncMaterializationProjectionToPianoRoll()`
3. 若 `PianoRollEditAction` 携带 affected frame 范围，换算为秒后 `enqueueMaterializationPartialRenderById(matId, startSec, endSec)`；否则对整个 materialization 触发 refresh

### 4.9 记忆字段（per-editor state）

| 字段 | 用途 |
|------|------|
| `lastConsumedAraSnapshotEpoch_` | 上次处理过的 snapshot epoch，避免重复响应同一 epoch |
| `lastConsumedPreferredAraRegion_` | 上次聚焦的 preferred region，变化时触发 clip import |
| `rmvpeOverlayLatched_` + `rmvpeOverlayTargetMaterializationId_` | F0 分析期间显示 overlay |
| `araClipImportArmed_` | ARA clip import 准备位 |

---

## 5. 编译守卫对比：VST3 Editor vs Standalone Editor

| 维度 | VST3 Editor (`Source/Plugin/PluginEditor`) | Standalone Editor (`Source/Editor/...`) |
|------|-------------------------------------------|---------------------------------------|
| 守卫 | `#if JucePlugin_Build_VST3` 全文件 | 无守卫，始终参与编译 |
| 数据源 | `VST3AraSession::PublishedSnapshot.preferredRegion` | `StandaloneArrangement` 多轨数据 |
| 文件 I/O | 弹 host-managed 提示 | 直接打开/保存文件 |
| 菜单栏 | `menuBar_.setVisible(false)`；`TopBar` 内联 File/Edit/View | 原生 macOS MenuBar 或 Windows 内嵌 |
| Transport layout | `VST3AraSingleClip` | 完整多轨 profile |
| Arrangement 视图 | 不包含（单 region 模式） | 包含多轨 Arrangement |
| Playback 控制 | 通过 DC 转发给 host `PlaybackController` | 直接驱动内部 playback |
| 主题/偏好 | 同 Standalone（共用 AppPreferences） | 同左 |

---

## ⚠️ 待确认

1. **ARA persistency 未实现**：`doRestoreObjectsFromStream` / `doStoreObjectsToStream` 当前直接返回 `true` 且忽略流参数；是否计划在后续版本中支持 pitch curve / materialization 的 ARA 档案序列化？
2. **MusicalContext 回写**：`didUpdateMusicalContextProperties` 只做 log 不更新 processor BPM；host 端 BPM/节拍变化似乎没有反向同步到 `PluginProcessor` 的 `setBpm()`（Editor 心跳也只是 `processorRef_.getBpm()` 单向读取）。
3. **快照日志上限的副作用**：`mappingLogCounter` 用静态 atomic 记录前 24 个 mapping log；多插件实例共享这个计数器 — 是否会导致多实例下漏 log？
4. **`processor_` 生命周期 vs hydration worker**：DC 析构先 `session_->setProcessor(nullptr)` 再等 session unique_ptr 析构 join worker；但在 DC 析构和 setProcessor 之间，如果 worker 此刻正准备调 processor，acquire load 仍可能拿到非 null 指针 — 确认 worker 的 `processor_.load(memory_order_acquire)` 到调用之间的窗口是否被 stateMutex_ 覆盖（看起来是的，但建议确认）。
5. **`OpenTuneDocumentController` 构造时 processor 未设置**：构造函数不接受 processor 参数，只能事后 `setProcessor`；这期间若 host 已经触发 `didEnableAudioSourceSamplesAccess` 导致 auto-birth，worklist 的 `processor_.load()` 会拿到 nullptr 并跳过绑定 — 是否需要补救路径（例如在 setProcessor 时对已 hydrate 完成的 source 重新派发 auto-birth）？
