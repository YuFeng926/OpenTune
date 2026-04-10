---
spec_version: 1.0.0
status: draft
module: core-processor
doc_type: api
generated_by: arch-doc-agent
generated_at: 2026-04-10
last_updated: 2026-04-10
---

> **本模块无 HTTP Controller，此文档记录核心 AudioProcessor 及相关 Service 层对外暴露的编程接口契约**

> ⚠️ 基于源码扫描生成，准确性待人工验证

# core-processor — API 接口契约

---

## 1. OpenTuneAudioProcessor（核心处理器）

`Source/PluginProcessor.h` | 命名空间: `OpenTune`

### 1.1 JUCE AudioProcessor 生命周期

#### void OpenTuneAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
**描述**：音频引擎准备播放时由宿主/独立模式调用，设置设备采样率、block 大小，并在采样率变化时重新采样所有 clip 的 drySignalBuffer 及清理 resampled cache。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：由宿主音频线程调用（生命周期方法）

| 参数 | 类型 | 说明 |
|------|------|------|
| sampleRate | double | 设备采样率 |
| samplesPerBlock | int | 每次回调的最大采样数 |

#### void OpenTuneAudioProcessor::releaseResources()
**描述**：音频引擎释放资源时调用，停止播放状态。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：由宿主音频线程调用

#### void OpenTuneAudioProcessor::processBlock(juce::AudioBuffer\<float\>&, juce::MidiBuffer&)
**描述**：实时音频处理回调。遍历所有轨道和 clip，混合 dry signal 与 rendered (AI 修音) 音频到输出 buffer。处理 Solo/Mute、Fade-in/out、RMS 电平计算。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：音频线程安全（ScopedReadLock on tracksLock_，ScopedNoDenormals）

| 参数 | 类型 | 说明 |
|------|------|------|
| buffer | juce::AudioBuffer\<float\>& | 输入/输出音频缓冲区 |
| midiMessages | juce::MidiBuffer& | MIDI 消息（本项目未使用） |

#### void OpenTuneAudioProcessor::processBlock(juce::AudioBuffer\<double\>&, juce::MidiBuffer&)
**描述**：双精度 processBlock 重载，内部将 double 转为 float 后委托给 float 版本。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：音频线程安全

#### bool OpenTuneAudioProcessor::supportsDoublePrecisionProcessing() const
**描述**：返回 true，表示支持双精度处理。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：任意线程

#### bool OpenTuneAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
**描述**：验证总线布局：输出必须为立体声，输入可以是单声道或立体声。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：任意线程

### 1.2 两阶段导入 API (Two-Phase Import)

#### bool OpenTuneAudioProcessor::prepareImportClip(int trackId, juce::AudioBuffer\<float\>&& inBuffer, double inSampleRate, const juce::String& clipName, PreparedImportClip& out)
**描述**：导入预处理阶段（后台线程调用）。将原始音频重采样到固定 44100Hz 存储格式。不持有 tracksLock_。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：任意线程（不持有共享锁）

| 参数 | 类型 | 说明 |
|------|------|------|
| trackId | int | 目标轨道 ID (0-based) |
| inBuffer | juce::AudioBuffer\<float\>&& | 原始音频 buffer（移动语义） |
| inSampleRate | double | 原始采样率 |
| clipName | const juce::String& | clip 名称 |
| out | PreparedImportClip& | 输出预处理结果 |

#### bool OpenTuneAudioProcessor::commitPreparedImportClip(PreparedImportClip&& prepared)
**描述**：提交预处理的 clip（主线程调用）。写锁内只做轻量对象挂载，创建 RenderCache，重采样 drySignalBuffer。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：仅 UI 线程（持有 ScopedWriteLock on tracksLock_）

| 参数 | 类型 | 说明 |
|------|------|------|
| prepared | PreparedImportClip&& | 预处理结果（移动语义） |

#### bool OpenTuneAudioProcessor::prepareDeferredClipPostProcess(int trackId, uint64_t clipId, PreparedClipPostProcess& out) const
**描述**：延迟后处理准备阶段。在后台线程中计算 clip 的静音间隔检测。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：任意线程（只读取 clip 的副本）

| 参数 | 类型 | 说明 |
|------|------|------|
| trackId | int | 轨道 ID |
| clipId | uint64_t | clip 唯一标识 |
| out | PreparedClipPostProcess& | 输出后处理结果 |

#### bool OpenTuneAudioProcessor::commitDeferredClipPostProcess(int trackId, uint64_t clipId, PreparedClipPostProcess&& prepared)
**描述**：提交延迟后处理结果到 clip（主线程调用）。写锁内写入 silentGaps。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：仅 UI 线程

### 1.3 Clip 管理 API

#### bool OpenTuneAudioProcessor::moveClipToTrack(int sourceTrackId, int targetTrackId, uint64_t clipId, double newStartSeconds)
**描述**：跨轨道移动 clip。更新颜色为目标轨道颜色。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：仅 UI 线程（ScopedWriteLock）

| 参数 | 类型 | 说明 |
|------|------|------|
| sourceTrackId | int | 源轨道 |
| targetTrackId | int | 目标轨道 |
| clipId | uint64_t | clip 唯一标识 |
| newStartSeconds | double | 新起始时间（秒） |

#### bool OpenTuneAudioProcessor::splitClipAtSeconds(int trackId, int clipIndex, double splitSeconds)
**描述**：在指定时间点分割 clip 为左右两部分。最小分割长度 100ms。自动设置 crossfade（0.25s）。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：仅 UI 线程（ScopedWriteLock）

| 参数 | 类型 | 说明 |
|------|------|------|
| trackId | int | 轨道 ID |
| clipIndex | int | clip 索引 |
| splitSeconds | double | 分割时间点（绝对时间，秒） |

#### bool OpenTuneAudioProcessor::mergeSplitClips(int trackId, uint64_t originalClipId, uint64_t newClipId, int targetClipIndex)
**描述**：合并两个分割的 clip 为一个。恢复 fadeOut 设置。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：仅 UI 线程（ScopedWriteLock）

#### bool OpenTuneAudioProcessor::deleteClip(int trackId, int clipIndex)
**描述**：删除指定 clip，自动调整选中索引。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：仅 UI 线程（ScopedWriteLock）

#### bool OpenTuneAudioProcessor::deleteClipById(int trackId, uint64_t clipId, ClipSnapshot* deletedOut, int* deletedIndexOut)
**描述**：按 clipId 删除 clip，可选输出已删除的快照（用于 Undo）。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：仅 UI 线程（ScopedWriteLock）

#### bool OpenTuneAudioProcessor::insertClipSnapshot(int trackId, int insertIndex, const ClipSnapshot& snap, uint64_t forcedClipId)
**描述**：从快照插入 clip（用于 Undo/Redo 恢复）。验证 audioBuffer 有效性。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：仅 UI 线程（ScopedWriteLock）

#### bool OpenTuneAudioProcessor::getClipSnapshot(int trackId, uint64_t clipId, ClipSnapshot& out) const
**描述**：获取 clip 的快照副本。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：任意线程（ScopedReadLock）

### 1.4 Clip 属性读写

| 方法签名 | 说明 | 线程安全 |
|---------|------|---------|
| `int getNumClips(int trackId) const` | 获取轨道中 clip 数量 | ScopedReadLock |
| `int getSelectedClip(int trackId) const` | 获取选中 clip 索引 | ScopedReadLock |
| `void setSelectedClip(int trackId, int clipIndex)` | 设置选中 clip | ScopedWriteLock |
| `shared_ptr<const AudioBuffer<float>> getClipAudioBuffer(int, int) const` | 获取 clip 音频缓冲区 | ScopedReadLock |
| `uint64_t getClipId(int, int) const` | 获取 clip 唯一 ID | ScopedReadLock |
| `int findClipIndexById(int, uint64_t) const` | 按 ID 查找 clip 索引 | ScopedReadLock |
| `double getClipStartSeconds(int, int) const` | 获取 clip 起始时间 | ScopedReadLock |
| `void setClipStartSeconds(int, int, double)` | 设置 clip 起始时间 | ScopedWriteLock |
| `juce::String getClipName(int, int) const` | 获取 clip 名称 | ScopedReadLock |
| `float getClipGain(int, int) const` | 获取 clip 增益 | ScopedReadLock |
| `void setClipGain(int, int, float)` | 设置 clip 增益 | ScopedWriteLock |
| `void setClipGainById(int, uint64_t, float)` | 按 ID 设置 clip 增益 | ScopedWriteLock |
| `shared_ptr<PitchCurve> getClipPitchCurve(int, int) const` | 获取 clip 的 PitchCurve | ScopedReadLock |
| `void setClipPitchCurve(int, int, shared_ptr<PitchCurve>)` | 设置 clip 的 PitchCurve | ScopedWriteLock |
| `OriginalF0State getClipOriginalF0State(int, int) const` | 获取 F0 提取状态 | ScopedReadLock |
| `void setClipOriginalF0State(int, int, OriginalF0State)` | 设置 F0 提取状态 | ScopedWriteLock |
| `bool setClipOriginalF0StateById(int, uint64_t, OriginalF0State)` | 按 ID 设置 F0 状态 | ScopedWriteLock |
| `DetectedKey getClipDetectedKey(int, int) const` | 获取检测到的调性 | ScopedReadLock |
| `void setClipDetectedKey(int, int, const DetectedKey&)` | 设置检测到的调性 | ScopedWriteLock |
| `vector<Note> getClipNotes(int, int) const` | 获取 clip 音符（副本） | ScopedReadLock |
| `vector<Note>& getClipNotesRef(int, int)` | 获取 clip 音符引用 | ScopedWriteLock |
| `void setClipNotes(int, int, const vector<Note>&)` | 设置 clip 音符 | ScopedWriteLock |
| `bool setClipNotesById(int, uint64_t, const vector<Note>&)` | 按 ID 设置 clip 音符 | ScopedWriteLock |
| `double getClipStartSecondsById(int, uint64_t) const` | 按 ID 获取起始时间 | ScopedReadLock |
| `bool setClipStartSecondsById(int, uint64_t, double)` | 按 ID 设置起始时间 | ScopedWriteLock |

### 1.5 轨道控制 API

| 方法签名 | 说明 | 线程安全 |
|---------|------|---------|
| `void setActiveTrack(int trackId)` | 设置当前活跃轨道 | 仅 UI 线程 |
| `int getActiveTrackId() const` | 获取当前活跃轨道 ID | 任意线程 |
| `int getNumTracks() const` | 获取最大轨道数 (MAX_TRACKS=12) | 任意线程 |
| `void setTrackMuted(int, bool)` | 设置轨道静音 | ScopedWriteLock |
| `bool isTrackMuted(int) const` | 查询轨道是否静音 | ScopedReadLock |
| `void setTrackSolo(int, bool)` | 设置轨道独奏（更新全局 anyTrackSoloed_ 标志） | ScopedWriteLock |
| `bool isTrackSolo(int) const` | 查询轨道是否独奏 | ScopedReadLock |
| `void setTrackVolume(int, float)` | 设置轨道音量 | ScopedWriteLock |
| `float getTrackVolume(int) const` | 获取轨道音量 | ScopedReadLock |
| `float getTrackRMS(int) const` | 获取轨道实时 RMS 电平 (dB) | 原子读取 |
| `void setTrackHeight(int)` | 设置轨道高度（UI 状态） | 仅 UI 线程 |
| `int getTrackHeight() const` | 获取轨道高度 | 任意线程 |
| `bool hasTrackAudio(int) const` | 查询轨道是否有音频 | ScopedReadLock |

### 1.6 播放控制 API (Transport)

| 方法签名 | 说明 | 线程安全 |
|---------|------|---------|
| `void setPlaying(bool)` | 开始/停止播放（停止时触发 200ms fade-out） | 仅 UI 线程 |
| `bool isPlaying() const` | 查询播放状态 | 原子读取 |
| `void setLoopEnabled(bool)` | 设置循环播放 | 原子写入 |
| `bool isLoopEnabled() const` | 查询循环状态 | 原子读取 |
| `void setPosition(double seconds)` | 设置播放位置（秒） | 原子写入 |
| `double getPosition() const` | 获取播放位置（秒） | 原子读取 |
| `double getPlayStartPosition() const` | 获取播放起始位置 | 原子读取 |
| `void setPlayStartPosition(double)` | 设置播放起始位置 | 原子写入 |
| `shared_ptr<atomic<double>> getPositionAtomic()` | 获取原子播放位置指针（lock-free 共享） | 任意线程 |
| `void setBpm(double)` | 设置 BPM | 仅 UI 线程 |
| `double getBpm() const` | 获取 BPM（独立模式返回本地值，插件模式返回宿主值） | 任意线程 |
| `int getTimeSigNumerator() const` | 获取拍号分子 | 任意线程 |
| `int getTimeSigDenominator() const` | 获取拍号分母 | 任意线程 |
| `double getPpqPosition() const` | 获取 PPQ 位置（插件模式） | 任意线程 |
| `bool isRecording() const` | 查询宿主录音状态 | 任意线程 |
| `bool isHostLooping() const` | 查询宿主循环状态 | 任意线程 |

### 1.7 导出 API

| 方法签名 | 说明 | 线程安全 |
|---------|------|---------|
| `bool exportClipAudio(int trackId, int clipIndex, const File& file)` | 导出单个 clip 为 WAV（44100Hz, 32-bit float, mono） | ScopedReadLock |
| `bool exportTrackAudio(int trackId, const File& file)` | 导出整个轨道为 WAV（所有 clip 按时间轴合并） | ScopedReadLock |
| `bool exportMasterMixAudio(const File& file)` | 导出总线混音为 WAV（所有轨道混合，立体声） | ScopedReadLock |
| `String getLastExportError() const` | 获取上次导出错误信息 | 任意线程 |
| `void clearLastExportError()` | 清除导出错误信息 | 任意线程 |

### 1.8 渲染 & 推理 API

| 方法签名 | 说明 | 线程安全 |
|---------|------|---------|
| `void enqueuePartialRender(int trackId, int clipIndex, double relStartSeconds, double relEndSeconds)` | 提交部分渲染请求。按 silentGaps 分 chunk，更新 RenderCache 版本，唤醒渲染 Worker。 | ScopedReadLock（读取 clip 数据） |
| `bool isInferenceReady() const` | 查询 F0 推理引擎是否就绪 | 原子读取 |
| `bool initializeInferenceIfNeeded()` | 懒初始化 F0 推理引擎 | 互斥锁（f0InitMutex_） |
| `F0InferenceService* getF0Service() const` | 获取 F0 推理服务指针 | 任意线程 |
| `VocoderDomain* getVocoderDomain() const` | 获取 Vocoder 域指针 | 任意线程 |
| `RenderCache::ChunkStats getClipChunkStats(int, int) const` | 获取 clip 渲染缓存统计 | ScopedReadLock |
| `bool isBuffering() const` | 查询是否正在缓冲 | 原子读取 |
| `bool isDrySignalFallback() const` | 查询是否回退到 dry signal | 原子读取 |

### 1.9 Undo/Redo API

| 方法签名 | 说明 | 线程安全 |
|---------|------|---------|
| `UndoManager& getUndoManager()` | 获取全局 UndoManager 引用 | 仅 UI 线程 |
| `void performUndo()` | 执行撤销 | 仅 UI 线程 |
| `void performRedo()` | 执行重做 | 仅 UI 线程 |
| `bool canUndo() const` | 是否可撤销 | 任意线程 |
| `bool canRedo() const` | 是否可重做 | 任意线程 |
| `String getUndoDescription() const` | 获取撤销操作描述 | 任意线程 |
| `String getRedoDescription() const` | 获取重做操作描述 | 任意线程 |

### 1.10 性能探测 API

#### PerfProbeSnapshot OpenTuneAudioProcessor::getPerfProbeSnapshot() const
**描述**：获取性能探测快照，包括音频回调 P99 延迟、cache miss 率、渲染队列深度。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：任意线程（原子读取 + ScopedReadLock）

#### void OpenTuneAudioProcessor::resetPerfProbeCounters()
**描述**：重置所有性能计数器。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：任意线程（原子操作）

### 1.11 其他

| 方法签名 | 说明 | 线程安全 |
|---------|------|---------|
| `void bumpEditVersion()` | 递增编辑版本参数（通知宿主状态已变化） | 仅 UI 线程 |
| `void showAudioSettingsDialog(AudioProcessorEditor&)` | 显示音频设置对话框（委托给 HostIntegration） | 仅 UI 线程 |
| `double getSampleRate() const` | 获取当前设备采样率 | 原子读取 |
| `static constexpr double getStoredAudioSampleRate()` | 获取存储音频采样率 (44100Hz) | 任意线程 |
| `void setShowWaveform(bool)` / `bool getShowWaveform() const` | 波形显示开关 | 仅 UI 线程 |
| `void setShowLanes(bool)` / `bool getShowLanes() const` | 泳道显示开关 | 仅 UI 线程 |
| `void setZoomLevel(double)` / `double getZoomLevel() const` | 缩放级别 | 仅 UI 线程 |
| `SilentGapDetector::DetectionConfig getSilentGapDetectionConfig() const` | 获取静音间隔检测配置 | 任意线程 |
| `void setSilentGapDetectionConfig(const DetectionConfig&)` | 设置静音间隔检测配置 | 任意线程 |

### 1.12 序列化

#### void OpenTuneAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
**描述**：将项目状态序列化为 XML（ValueTree）并编码为二进制。存储 BPM、缩放级别、轨道高度、每个 clip 的 PitchCurve（F0 + corrected segments，Base64 编码）。Schema version = 2。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：ScopedReadLock

#### void OpenTuneAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
**描述**：从二进制数据反序列化项目状态。恢复 BPM、缩放、PitchCurve（包括 corrected segments）。  
**所在类**：`OpenTuneAudioProcessor`  
**线程安全**：ScopedWriteLock

---

## 2. F0ExtractionService（F0 提取服务）

`Source/Services/F0ExtractionService.h` | 命名空间: `OpenTune`

#### explicit F0ExtractionService::F0ExtractionService(int workerCount = 2, size_t maxQueueSize = 64)
**描述**：构造函数，创建指定数量的工作线程（默认 2 个）和 lock-free 任务队列。  
**所在类**：`F0ExtractionService`  
**线程安全**：构造时单线程

| 参数 | 类型 | 说明 |
|------|------|------|
| workerCount | int | 工作线程数 |
| maxQueueSize | size_t | 队列最大容量 |

#### SubmitResult F0ExtractionService::submit(uint64_t requestKey, ExecuteFn execute, CommitFn commit)
**描述**：提交 F0 提取任务。支持请求去重（同一 requestKey 不会同时运行）。执行完成后通过 `juce::MessageManager::callAsync` 在主线程调用 commit 回调。  
**所在类**：`F0ExtractionService`  
**线程安全**：任意线程（内部使用 mutex + lock-free queue）

| 参数 | 类型 | 说明 |
|------|------|------|
| requestKey | uint64_t | 唯一请求键（通常是 clipId） |
| execute | ExecuteFn | 执行函数（在 worker 线程调用） |
| commit | CommitFn | 结果提交回调（在 message 线程调用） |

**返回值**：`SubmitResult` 枚举 — `Accepted` / `AlreadyInProgress` / `QueueFull` / `InvalidTask`

#### static uint64_t F0ExtractionService::makeRequestKey(uint64_t clipId, int trackId, int clipIndex)
**描述**：生成请求键。优先使用 clipId，若为 0 则用 trackId + clipIndex 组合。  
**所在类**：`F0ExtractionService`  
**线程安全**：任意线程（纯函数）

#### bool F0ExtractionService::isActive(uint64_t requestKey) const
**描述**：查询指定请求是否正在执行中。  
**所在类**：`F0ExtractionService`  
**线程安全**：任意线程（mutex 保护）

#### void F0ExtractionService::cancel(uint64_t requestKey)
**描述**：取消指定请求（从 activeEntries 中移除，worker 在校验 token 时会跳过已取消的任务）。  
**所在类**：`F0ExtractionService`  
**线程安全**：任意线程（mutex 保护）

---

## 3. AsyncAudioLoader（异步音频加载器）

`Source/Audio/AsyncAudioLoader.h` | 命名空间: `OpenTune`

#### void AsyncAudioLoader::loadAudioFile(const juce::File& file, ProgressCallback progressCallback, CompletionCallback completionCallback)
**描述**：在后台线程异步加载音频文件。支持 JUCE 标准格式（WAV, AIFF, FLAC, MP3 等）。进度和完成回调通过 `MessageManager::callAsync` 安全地在 UI 线程触发。使用 validity token 机制防止对象销毁后的悬空回调。  
**所在类**：`AsyncAudioLoader`  
**线程安全**：仅 UI 线程调用（启动后台线程）

| 参数 | 类型 | 说明 |
|------|------|------|
| file | const juce::File& | 要加载的音频文件 |
| progressCallback | ProgressCallback | 进度回调 (float progress, String status) |
| completionCallback | CompletionCallback | 完成回调 (LoadResult) |

#### void AsyncAudioLoader::cancelLoad()
**描述**：取消正在进行的加载（等待线程停止，最多 2000ms）。  
**所在类**：`AsyncAudioLoader`  
**线程安全**：仅 UI 线程

---

## 4. HostIntegration（宿主集成接口）

`Source/Host/HostIntegration.h` | 命名空间: `OpenTune`

这是一个抽象接口（ABC），定义了 Standalone 和 Plugin 两种模式的集成契约。

#### virtual void HostIntegration::configureInitialState(OpenTuneAudioProcessor& processor) = 0
**描述**：配置处理器初始状态（由构造函数调用）。独立模式实现为空操作。  
**所在类**：`HostIntegration` (interface)  
**线程安全**：构造时单线程

#### virtual bool HostIntegration::processIfApplicable(OpenTuneAudioProcessor& processor, juce::AudioBuffer\<float\>& buffer, int totalNumInputChannels, int totalNumOutputChannels, int numSamples) = 0
**描述**：如果宿主模式需要特殊处理，返回 true 并处理 buffer；否则返回 false 由 OpenTuneAudioProcessor 自行处理。独立模式始终返回 false。  
**所在类**：`HostIntegration` (interface)  
**线程安全**：音频线程

#### virtual void HostIntegration::audioSettingsRequested(juce::AudioProcessorEditor& editor) = 0
**描述**：响应音频设置请求。独立模式打开自定义 AudioDeviceSelectorComponent 对话框。  
**所在类**：`HostIntegration` (interface)  
**线程安全**：仅 UI 线程

#### std::unique_ptr\<HostIntegration\> createHostIntegration()
**描述**：工厂函数，根据构建配置创建 HostIntegrationStandalone 或 HostIntegrationPlugin。  
**线程安全**：构造时单线程

---

## 5. EditorFactory（编辑器工厂）

`Source/Editor/EditorFactory.h` | 命名空间: `OpenTune`

#### juce::AudioProcessorEditor* createOpenTuneEditor(OpenTuneAudioProcessor& processor)
**描述**：工厂函数，创建 OpenTuneAudioProcessorEditor 实例。Standalone 模式创建 `OpenTuneAudioProcessorEditor`。  
**所在类**：自由函数  
**线程安全**：仅 UI 线程

| 参数 | 类型 | 说明 |
|------|------|------|
| processor | OpenTuneAudioProcessor& | 处理器引用 |

---

## ⚠️ 待确认

1. **[LOW-CONF]** `getClipNotesRef()` 返回可变引用但使用 ScopedWriteLock 保护 — 调用方在锁释放后继续持有引用是否安全？需确认所有调用点的使用模式。
2. **[EDGE]** `exportMasterMixAudio` 中 `totalLen` 为 int 类型（通过 `juce::AudioBuffer::setSize`），超长音频（>48000s at 44.1kHz）可能溢出。
3. **[DEP]** `F0InferenceService` 和 `VocoderDomain` 接口未在 arch_layers 中列出，但被 `OpenTuneAudioProcessor` 直接依赖。这两个类的 API 契约需要补充。
4. **[BIZ-GAP]** `HostIntegrationPlugin` 实现未在 arch_layers 中提供（仅有 Standalone 实现），插件模式的 processIfApplicable 行为未知。
5. **[LOW-CONF]** `setPlaying(false)` 通过 fade-out 机制异步停止播放，调用方可能需要等待 `isPlaying()` 变为 false，但无提供同步等待机制。
