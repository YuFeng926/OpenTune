# VST3 References 与工作区结构性差异分析

**生成时间**: 2026-04-04
**状态说明**: 这是历史对照文档，不是 current live tree 的事实源。
**当前使用方式**: 用于回看早期 `VST3 References` 与主工作区的结构冲突；当前 live tree 的事实请以 `.planning/PROJECT.md`、`.planning/REQUIREMENTS.md`、`.planning/codebase/ARCHITECTURE.md` 和源码本身为准。

**2026-04-17 注记**:
- 当前 live tree 已经不再是本文写作时的 `tracks_` / `AudioSourceState` 结构。
- 当前 shared runtime shell 已切到 `ClipCoreStore + StandaloneArrangement + VST3AraSession`。
- 因此，本文适合作为“为什么当时要重构”的背景材料，不适合作为“现在代码长什么样”的说明书。

---

## 一、数据模型根本差异

VST3 References 与工作区采用了**两套完全不同的数据模型**，这是最核心的结构性差异。

### 1.1 VST3 References：单 CLIP 模型

VST3 References 的 `PluginProcessor.h` 已重构为单 CLIP 架构：

```cpp
// VST3 References - PluginProcessor.h
struct CurrentClipState {
    uint64_t clipId{0};
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    std::shared_ptr<PitchCurve> pitchCurve;
    std::shared_ptr<RenderCache> renderCache;
    std::vector<Note> notes;
    std::vector<SilentGap> silentGaps;
    OriginalF0State originalF0State;
    DetectedKey detectedKey;
    double startSeconds{0.0};
    float gain{1.0f};
};

class OpenTuneAudioProcessor {
    CurrentClipState clip_;              // 单个 clip
    mutable juce::ReadWriteLock clipLock_;  // 保护单个 clip
    std::atomic<uint64_t> nextClipId_{1};
    // ...
};
```

### 1.2 工作区：多轨道完整模型

工作区的 `PluginProcessor.h` 保持多轨道架构：

```cpp
// 工作区 - PluginProcessor.h
class OpenTuneAudioProcessor {
    static constexpr int MAX_TRACKS = 12;

    struct TrackState {
        struct AudioClip {
            uint64_t clipId{0};
            std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
            std::shared_ptr<PitchCurve> pitchCurve;
            std::shared_ptr<RenderCache> renderCache;
            std::vector<Note> notes;
            std::vector<SilentGap> silentGaps;
            // ...
        };
        std::vector<AudioClip> clips;
        int selectedClipIndex{0};
        bool isMuted{false}, isSolo{false};
        float volume{1.0f};
        std::atomic<float> currentRMS{-100.0f};
    };

    std::array<TrackState, MAX_TRACKS> tracks_;
    mutable juce::ReadWriteLock tracksLock_;
    int activeTrackId_{0};
    bool anyTrackSoloed_{false};
    // ...
};
```

### 1.3 差异对照表

| 字段/方法 | VST3 References | 工作区 |
|-----------|---------------|--------|
| 数据容器 | `CurrentClipState clip_` (单) | `std::array<TrackState, 12> tracks_` (多) |
| 保护锁 | `clipLock_` | `tracksLock_` |
| 轨道常量 | 已废弃 MAX_TRACKS | `MAX_TRACKS = 12` |
| 活跃轨道 | 无（单 CLIP 无索引概念） | `activeTrackId_` |
| Solo 状态 | 无 | `anyTrackSoloed_` |
| 多轨道 API | **全部标记 `[[deprecated]]` | 完整实现 |

---

## 二、`chunkRenderWorkerLoop` 差异

这是最关键的差异。VST3 References 中的 `chunkRenderWorkerLoop` 已是骨架实现，但 `PluginProcessor.h` 头文件已删除多轨道 API，导致代码引用断裂。

### 2.1 工作区完整实现（311 行）

工作区的 `chunkRenderWorkerLoop` 是完整实现：

```cpp
// 工作区 - PluginProcessor.cpp:2484-2795
void OpenTuneAudioProcessor::chunkRenderWorkerLoop() {
    AppLogger::log("RenderTrace: chunkRenderWorkerLoop started");
    constexpr double kHopDuration = 512.0 / RenderCache::kSampleRate;

    struct WorkerRenderJob {
        std::shared_ptr<RenderCache> cache;
        int trackId{0};
        uint64_t clipId{0};
        double startSeconds{0.0};
        double endSeconds{0.0};
        uint64_t targetRevision{0};
    };

    while (true) {
        // 1. wait() 等待 pending jobs
        std::unique_lock<std::mutex> lock(schedulerMutex_);
        schedulerCv_.wait(lock, [this]() {
            return !chunkRenderWorkerRunning_.load(std::memory_order_acquire)
                || hasPendingRenderJobs();
        });
        if (!chunkRenderWorkerRunning_) return;

        // 2. 遍历所有 tracks_[t].clips 找 Pending Chunk
        const juce::ScopedReadLock rl(tracksLock_);
        for (int t = 0; t < MAX_TRACKS; ++t) {
            for (auto& clip : tracks_[t].clips) {
                if (clip.renderCache->getNextPendingJob(pendingJob)) {
                    // 构建 WorkerRenderJob
                }
            }
        }

        // 3. 读取音频 (mono mix down)
        // 4. 获取 PitchCurve snapshot
        // 5. 检查 F0 可渲染性
        // 6. computeLogMelSpectrogram()
        // 7. F0-to-Mel 线性插值
        // 8. fillF0GapsForVocoder() (log-domain 间隙填补)
        // 9. submit 到 VocoderDomain 串行队列
        // 10. onComplete 回调写入 RenderCache (含重采样逻辑)
    }
}
```

### 2.2 VST3 References 骨架实现（断裂）

VST3 References 的 `PluginProcessor.cpp` 中 `chunkRenderWorkerLoop` **已被破坏**：

- 头文件已删除多轨道 API（`tracks_`、`MAX_TRACTS`、`tracksLock_`、`hasPendingRenderJobs` 等）
- 但 `.cpp` 中的 `chunkRenderWorkerLoop` **仍然引用**这些已删除的符号
- 导致编译不兼容

### 2.3 关键代码段对比

#### F0 可渲染性检查

**工作区有，VST3 References 需要确认是否存在：**
```cpp
// 工作区 2619-2626
if (!snap->hasRenderableCorrectedF0()) {
    AppLogger::log("RenderTrace: Blank: no_renderable_corrected_F0");
    job->cache->markChunkAsBlank(relChunkStartSec);
    schedulerCv_.notify_one();
    continue;
}
```

#### Mel Spectrogram 计算

**工作区有，VST3 References 需要确认是否存在：**
```cpp
// 工作区 2689-2706
MelSpectrogramConfig melConfig;
melConfig.sampleRate = static_cast<int>(RenderCache::kSampleRate);
melConfig.nMels = vocoderDomain_->getMelBins();
auto melResult = computeLogMelSpectrogram(
    monoAudio.data(), static_cast<int>(monoAudio.size()), numFrames, melConfig);
```

#### F0-to-Mel 插值

**工作区有，VST3 References 需要确认是否存在：**
```cpp
// 工作区 2711-2733
std::vector<float> correctedF0(static_cast<size_t>(actualFrames), 0.0f);
for (int i = 0; i < actualFrames; ++i) {
    const double melTimeSec = relChunkStartSec + i * kHopDuration;
    const double srcPos = melTimeSec * f0FrameRate - static_cast<double>(f0StartFrame);
    // log-domain 线性插值
}
fillF0GapsForVocoder(correctedF0, snap, relChunkStartSec, relChunkEndSec,
                     kHopDuration, f0FrameRate);
```

#### Vocoder Job 回调（完整版含重采样）

**工作区有，VST3 References 需要确认是否存在：**
```cpp
// 工作区 2745-2790
vocoderJob.onComplete = [this, renderCache, targetRevision, jobStartSeconds](
    bool success, const juce::String& error, const std::vector<float>& audio) {
    if (success && !audio.empty()) {
        // 1. addChunk
        const bool baseChunkStored = renderCache->addChunk(jobStartSeconds, chunkEndSeconds, ...);
        // 2. 重采样到设备采样率
        if (baseChunkStored && resamplingManager_ && deviceSampleRate != renderSampleRate) {
            auto resampledAudio = resamplingManager_->upsampleForHost(...);
            renderCache->addResampledChunk(...);
        }
        // 3. completeChunkRender
        renderCache->completeChunkRender(..., CompletionResult::Succeeded);
    } else {
        renderCache->completeChunkRender(..., CompletionResult::TerminalFailure);
    }
    schedulerCv_.notify_one();
};
```

---

## 三、头文件完整差异对照

### 3.1 类继承结构

| 方面 | VST3 References | 工作区 |
|------|---------------|--------|
| AudioProcessor 继承 | `juce::AudioProcessor + AudioProcessorARAExtension` | `juce::AudioProcessor + AudioProcessorARAExtension` |
| 总线配置 | BusesProperties (立体声) | BusesProperties (立体声) |
| 双精度处理 | `supportsDoublePrecisionProcessing() = true` | 同 |

### 3.2 成员变量差异

| 方面 | VST3 References | 工作区 |
|------|---------------|--------|
| GPU 检测 | `gpuDetectorInitialized_` 原子标志 | 无（直接在构造函数检测） |
| 推理服务 | `f0Service_`, `vocoderDomain_` | 同 |
| 调度器 | `schedulerMutex_`, `schedulerCv_` | 同 |
| 工作线程 | `chunkRenderWorkerThread_` | 同 |
| 性能探针 | `perfAudioDurationHistogram_` 等 | 同 |
| 延迟加载标志 | `servicesInitialized_` | 无 |
| ARA 支持标志 | `araSupported_` | 无 |

### 3.3 缺失的成员（工作区有，VST3 References 删除）

VST3 References 的 `PluginProcessor.h` 删除了以下工作区存在的成员：

- `bumpEditVersion()` - 编辑版本递增
- `recordCacheCheck(bool cacheHit)` - 缓存命中探针
- `MAX_TRACKS` 常量
- `activeTrackId_`
- `anyTrackSoloed_`
- `tracks_` 数组
- `tracksLock_`
- `setActiveTrack()` / `getActiveTrackId()`
- `setTrackMuted()` / `isTrackMuted()`
- `setTrackSolo()` / `isTrackSolo()`
- `setTrackVolume()` / `getTrackVolume()`
- `getTrackRMS()`
- `getClipGainById()`
- `splitClipAtSeconds()` / `mergeSplitClips()`
- `deleteClip()`
- `moveClipToTrack()`
- `resampleClipDrySignal()` / `copyClipToSnapshot()` / `copySnapshotToClip()`
- `getClipChunkStats()`
- `anyTrackSoloed_`

### 3.4 新增的成员（VST3 References 有，工作区无）

VST3 References 新增了工作区不存在的单 CLIP 兼容层：

- `CurrentClipState clip_` - 单 CLIP 数据
- `clipLock_` - 替代 `tracksLock_`
- `nextClipId_` 原子
- `araSupported_` 原子
- `deprecated` 桩函数（编译兼容用）
  - `setActiveTrack(int trackId)` - no-op
  - `getActiveTrackId()` - 返回 0
  - `getNumTracks()` - 返回 1
  - `setTrackMuted/isTrackMuted/setTrackSolo/isTrackSolo/setTrackVolume/getTrackVolume/getTrackRMS/getNumClips/getSelectedClip/setSelectedClip/getClipGain/setClipGain` 等

### 3.5 processBlock 差异

**VST3 References (当前版本):**
```cpp
void processBlock(AudioBuffer<float>& buffer, MidiBuffer& midi) {
    if (isBoundToARA()) {
        processBlockForARA(buffer, isRealtime(), getPlayHead());
        return;
    }
    // 非 ARA 模式清空输出
    buffer.clear();
}
```

**工作区 (当前版本):**
```cpp
void processBlock(AudioBuffer<float>& buffer, MidiBuffer& midi) {
    // 需要确认是否也走 ARA 路径
}
```

---

## 四、CMakeLists.txt 分析

工作区的 `CMakeLists.txt` (706 行) 是完整的构建配置：

### 4.1 插件定义

```cmake
juce_add_plugin(OpenTune
    COMPANY_NAME "DAYA"
    PLUGIN_MANUFACTURER_CODE DAYA
    PLUGIN_CODE De77
    BUNDLE_ID "com.daya.opentune"
    FORMATS VST3
    IS_ARA_EFFECT TRUE
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
)
```

### 4.2 核心依赖

| 组件 | 版本/来源 |
|------|---------|
| ONNX Runtime CPU | 1.24.4 (`onnxruntime-win-x64-1.24.4`) |
| ONNX Runtime DML | 1.24.4 (`onnxruntime-dml-1.24.4`) |
| DirectML | 1.15.4 (NuGet) |
| D3D12 Agility SDK | 1.619.1 (NuGet) |
| ARA SDK | 2.2.0 (本地) |
| JUCE | master 分支 |

### 4.3 DLL 延迟加载

VST3 特有的延迟加载配置：
```cmake
# Windows: 延迟加载 onnxruntime.dll
target_link_options(OpenTune_VST3 PRIVATE "/DELAYLOAD:onnxruntime.dll")
```

### 4.4 POST_BUILD 自动复制

```cmake
# 运行时 DLL 复制到 VST3 bundle
onnxruntime.dll
DirectML.dll
D3D12/D3D12Core.dll
D3D12/D3D12SDKLayers.dll
models/rmvpe.onnx
models/hifigan.onnx
```

### 4.5 编译定义

```cmake
# VST3 专用
JUCE_WEB_BROWSER=0, JUCE_USE_CURL=0, JUCE_VST3_CAN_REPLACE_VST2=0
JUCE_REPORT_APP_USAGE=0, JUCE_STRICT_REFCOUNTEDPOINTER=1, JUCE_ASIO=1
JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1

# OpenTune 专用
OPENTUNE_VERSION
OPENTUNE_D3D12_AGILITY_SDK_VERSION
ORT_API_MANUAL_INIT
JUCE_USE_FLAC=1, JUCE_USE_OGGVORBIS=1, JUCE_USE_MP3AUDIOFORMAT=1
```

---

## 五、死代码清理记录

根据 `docs/changelog/2026-04-04-dead-code-cleanup.md`，已清理的函数：

| 文件 | 删除函数 | 原因 |
|------|---------|------|
| PluginProcessor | `bumpEditVersion()` | 未被调用 |
| PluginProcessor | `recordCacheCheck(bool)` | 未被调用 |
| TimelineComponent | `getViewportScroll()`, `getZoomLevel()` | 未被调用 |
| TimeConverter | `getZoomLevel()` | 未被调用 |
| VocoderInferenceService | `getVocoderHopSize()`, `isInitialized()` | 未被调用 |
| VocoderDomain | `getVocoderHopSize()`, `isInitialized()`, `isRunning()` | 未被调用 |
| VocoderRenderScheduler | `isRunning()` | 未被调用 |
| F0InferenceService | `isInitialized()` | 未被调用 |

---

## 六、后续合并建议

### 6.1 核心原则

1. **数据模型二选一**：要么保留工作区的多轨道架构（推荐，功能更完整），要么用 VST3 References 的单 CLIP 架构
2. **chunkRenderWorkerLoop 必须一致**：要么全部用工作区的完整实现，要么 VST3 References 先修复骨架实现
3. **API 层必须匹配**：头文件声明和 cpp 实现必须一致，不能出现引用已删除的符号

### 6.2 推荐方案

**方案 A（推荐）：以工作区为主，集成 VST3 References 的增量改进**

1. 保留工作区的 `chunkRenderWorkerLoop` 完整实现（多轨道）
2. 集成 VST3 References 的 ARA2 相关改进
3. 集成 VST3 References 的 CMakeLists.txt 配置
4. 集成 VST3 References 的死代码清理

**方案 B：以 VST3 References 为主，同步工作区缺失部分**

1. 确认 VST3 References 的单 CLIP 模型是目标架构
2. 重写 `chunkRenderWorkerLoop` 使其符合单 CLIP 语义
3. 删除所有多轨道相关的 deprecated stub

### 6.3 关键同步项

| 项目 | 状态 | 操作 |
|------|------|------|
| chunkRenderWorkerLoop 实现 | VST3 References 骨架断裂 | 用工作区完整实现替换，或重写 |
| PluginProcessor.h 头文件 | 两版本完全不同 | 需要决定主版本 |
| ARA DocumentController | VST3 References 较新 | 同步到工作区 |
| CMakeLists.txt | VST3 References 完整 | 同步到工作区 |
| 死代码清理 | VST3 References 已清理 | 同步到工作区 |
| ADR-0003 ARA 播放控制 | VST3 References 有文档 | 同步到工作区 |
| f0-vocoder-runtime-boundaries 文档 | VST3 References 有文档 | 同步到工作区 |

---

## 七、架构关键设计文档

VST3 References 中已建立的设计文档应作为后续工作的准则：

1. **延迟加载策略**（PluginProcessor.cpp:526-550）
   - 构造函数不加载 ONNX 模型
   - prepareToPlay() 中初始化
   - 避免 VST3 扫描器超时

2. **双域运行时边界**（docs/architecture/f0-vocoder-runtime-boundaries.md）
   - F0Domain (CPU, 并发) vs VocoderDomain (DML, 串行)
   - 无共享 Session/Env/配置

3. **ARA2 播放控制**（docs/adr/ADR-0003-ara-playback-controller-interface.md）
   - 正确路径: `getDocumentController()->getHostPlaybackController()`
   - 错误路径: `getHostInstance()->getPlaybackController()`

4. **Result<T> 错误处理**（Utils/Error.h）
   - 统一的错误类型系统
   - 完整的 Result<T> 特化版本

5. **LockFreeQueue**（Utils/LockFreeQueue.h）
   - MPMC 无锁队列
   - 环形缓冲区 + CAS 操作
   - 64 字节对齐原子变量

---

*本文件为 VST3 References 与工作区结构性差异分析，供合并决策参考*
