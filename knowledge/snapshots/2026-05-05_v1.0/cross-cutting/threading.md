---
spec_version: 1.0.0
status: draft
doc_type: cross-cutting/threading
generated_by: synthesis-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# 线程模型（Threading）

OpenTune 作为实时音频应用，线程约束远比通用桌面应用严格。本文档定义三大线程角色、同步原语映射、跨线程通信模式与 VBlank 协同。

（注：由于 C++ 音频桌面应用的 "security" 概念有限——仅涉及 Windows DLL 搜索路径限制与 InterProcessLock，已并入 caching.md / utils 模块文档——本文件替代传统 security.md 作为第三份横切文档。）

## 三大线程角色

### 1. Audio Thread（音频线程）

**身份**：由 JUCE `AudioProcessor::processBlock` 运行。典型 128–1024 采样 / 回调，2–20 ms 预算。

**硬约束**：
- **禁止分配**（new / malloc / std::vector::push_back 触发 reallocate）；
- **禁止加锁**（`std::mutex::lock` / `CriticalSection::enter`）— 仅允许 `ScopedTryLock`；
- **禁止系统调用**（文件 I/O / 日志 I/O / 消息框）；
- **禁止阻塞**（Sleep / condition_variable::wait）；
- **禁止抛异常**（所有函数 noexcept）；
- **禁用 denormals**（`juce::ScopedNoDenormals`）。

**允许的操作**：
- `std::atomic_load<shared_ptr<const Snapshot>>`（COW 快照读取）；
- `ScopedTryLock`：失败直接回退干信号；
- `LockFreeQueue<T>::tryPop`（消费命令队列）；
- 纯内存 memcpy / SIMD kernel；
- Counter 累加（供 UI 线程读）。

**典型工作**：
```
processBlock:
  ScopedNoDenormals nodenorms;
  auto snapshot = std::atomic_load(&playbackSnapshot);   // lock-free
  for each placement in snapshot:
      readPlaybackAudio(cache, range) → PCM  // 非阻塞读 RenderCache
      CrossoverMixer::process(vocoder_pcm, dry_pcm) → mixed
      accumulate into output buffer
```

### 2. Message Thread（消息线程 / UI 主线程）

**身份**：JUCE `MessageManager`，所有 Component 的 paint / mouseDown / resized 回调、Timer 回调、AsyncUpdater 都在此线程。

**约束**：
- UI 组件构造/析构/重绘只能在此线程；
- 长任务（> 几 ms）必须放后台 worker，避免卡 UI；
- 可以加锁，但不要阻塞太久。

**典型工作**：
- 30 Hz / 10 Hz Timer 同步播放位置、模型状态、渲染进度；
- Listener 回调（PianoRollComponent::Listener, FrameScheduler 等）；
- UndoManager push/undo/redo；
- AppPreferences 读写（InterProcessLock + XML 落盘）；
- ReclaimSweep（`runReclaimSweepOnMessageThread`）。

### 3. Worker Threads（工作线程）

多个专用 worker，每类只做一件事：

| Worker | 来源 | 职责 |
|---|---|---|
| **F0 Extraction Worker** | `F0ExtractionService` | 按 LockFreeQueue 取任务 → 调 `F0InferenceService` → 写回 Materialization |
| **Vocoder Render Worker** | `VocoderRenderScheduler` / `chunkRenderWorkerLoop` | 按 chunk 优先级队列取任务 → `OnnxVocoderBase::synthesize` → 写 RenderCache |
| **AsyncAudioLoader Worker** | `AsyncAudioLoader` | 后台解码音频文件 + r8brain 重采样到 44.1 kHz |
| **PianoRoll Correction Worker** | `PianoRollCorrectionWorker` | 单槽 + 版本号取消；执行五阶段修正算法 |
| **Hydration Worker**（ARA） | `VST3AraSession::hydrationWorkerThread` | 按 `kHydrationChunkSamples` 从 host 拉 PCM → 注入 SourceStore |
| **ImportedClipF0Extraction**（内联） | `ImportedClipF0Extraction` | 单次辅助（不是常驻 worker） |

每个 Worker 的约束：
- 可以加锁、可以分配；
- 但不能持有锁跨 I/O（避免阻塞 audio thread 读）；
- 支持 token / 版本号取消；
- Processor 析构前必须 `join`（见下方生命周期）。

## 同步原语映射表

| 原语 | 头文件 / 类 | 读者 | 写者 | 适用场景 |
|---|---|---|---|---|
| **COW Snapshot** | `std::atomic<std::shared_ptr<const T>>` + `std::atomic_load/store` | Audio / UI / Worker | Message / Worker | `PitchCurve`, `MaterializationSnapshot`, `PlaybackSnapshot`, `PublishedSnapshot`（ARA） |
| **ScopedTryLock** | `juce::ScopedTryLock` | Audio（失败即退让） | — | 音频线程尝试访问可锁资源 |
| **ReadWriteLock** | `juce::ReadWriteLock` | Message / Worker | Message / Worker | `SourceStore`, `StandaloneArrangement`（非 audio 路径） |
| **SpinLock** | `juce::SpinLock` | Message | Message | `StandaloneArrangement::PlaybackSnapshot`（短临界区） |
| **std::atomic** | `<atomic>` | 任意 | 任意 | 版本号（`renderRevision`, `epoch`）、counter、状态位 |
| **LockFreeQueue** | `Utils::LockFreeQueue<T>`（Vyukov 序号规则） | 单/多 消费者 | 单/多 生产者 | `F0ExtractionService` 任务队列、`PianoKeyAudition` 64 槽 SPSC |
| **InterProcessLock** | `juce::InterProcessLock` | — | Message | AppPreferences XML 写入，多实例互斥 |
| **shared_mutex** | `std::shared_mutex`（如用） | Worker | Worker | `MaterializationStore` 多读单写 |
| **Mutex / CriticalSection** | `std::mutex` / `juce::CriticalSection` | Message / Worker | Message / Worker | 短临界区保护内部状态（VST3AraSession 状态机） |
| **condition_variable** | `std::condition_variable` | Worker idle | Worker wake | chunk render worker 睡眠/唤醒 |
| **Token 取消** | 自定义 `uint64_t token` | Worker 检查 | UI 推进 | `F0ExtractionService` 请求取消 |
| **版本号取消** | `int64_t version` | Worker 比对 | UI 覆盖 | `AsyncCorrectionRequest`；pending slot 覆盖即 `VersionMismatch` |

## 跨线程通信模式

### 模式 A：Command Queue（UI → Worker）

```
UI Thread                   Worker Thread
─────────                   ─────────────
submit(Task)                loop:
  token = ++nextToken;        task = queue.pop();  // block
  queue.push({token, data});  if (task.token < cancelFence) skip;
  return token;               else execute;
cancel(token):                writeback(result);
  cancelFence = token;
```

用于：`F0ExtractionService`, `VocoderRenderScheduler`, `AsyncAudioLoader`。

### 模式 B：Publish-Subscribe（Worker → UI / Audio）

Worker 生成新 Snapshot → `std::atomic_store(&slot, newSnapshot)` → 下一次读者自然看到。

读者：
- Audio Thread：`std::atomic_load(&slot)`（lock-free）；
- UI Thread：Timer 轮询比较 revision，触发 repaint。

用于：`PlaybackSnapshot`, `PitchCurve`, `MaterializationSnapshot`。

### 模式 C：Version Fence（Worker 单槽）

```
UI Thread                             Worker Thread
─────────                             ─────────────
pending = {version, data};            while (alive):
  atomic_store(pendingSlot, pending);    current = atomic_exchange(pendingSlot, nullptr);
                                         if (current == nullptr) wait();
                                         else {
                                           if (current.version < latestVersion) skip;
                                           else execute; writeback;
                                         }
```

用于：`PianoRollCorrectionWorker`（单槽 + 版本号，最新优先；老请求被 pending 覆盖即静默取消，产出 `VersionMismatch` 被丢弃）。

### 模式 D：VBlank 同步（UI Repaint）

`FrameScheduler` 合并多个 invalidate 请求，在 VBlank 触发一次 repaint。避免每事件一次 paint。

```
Event Source → FrameScheduler::requestRepaint(rect, priority)
                    │
                    ▼
              pending_ 队列聚合 + 合并矩形
                    │
                    ▼
        VBlank Timer → flushPending() → Component::repaint(merged)
```

用于：
- 主界面：PlayheadOverlay（30 Hz 可见 / 10 Hz 隐藏）；
- 钢琴卷帘：`PianoRollVisualInvalidation::makeVisualFlushDecision` 纯函数合并多源失效。

## 跨格式的线程差异

| 维度 | Standalone | VST3 + ARA |
|---|---|---|
| processBlock 驱动 | JUCE Standalone AudioDeviceManager | 宿主 DAW 音频引擎 |
| 宿主 Transport 控制 | 无（内部 Transport） | `HostTransportSnapshot` + `setPlayingStateOnly` |
| 时间轴数据 | `StandaloneArrangement`（内部 ReadWriteLock + SpinLock） | `VST3AraSession::PublishedSnapshot`（COW）+ ARA callbacks |
| Hydration | `AsyncAudioLoader`（Worker） | `hydrationWorkerThread`（单线程） |
| 生命周期 | Editor 创建 Processor 先于 Editor | DocumentController 早于 Processor；VST3 Editor 独立 |

## 生命周期 / Join 顺序

析构顺序必须严格：

```
OpenTuneAudioProcessor::~OpenTuneAudioProcessor
  1. 停止 processBlock（host 调度已停）
  2. 向所有 worker 发关闭信号
  3. join F0ExtractionService
  4. join chunkRenderWorker
  5. join AsyncAudioLoader
  6. 析构 RenderCache / MaterializationStore / SourceStore
  7. 析构 inference sessions（ONNX Runtime）
  8. 析构 JUCE 基类
```

**⚠️ 已知风险**：`ara-vst3` 模块警告 DC 析构 vs hydration worker 的 processor 生命周期窗口需要交叉验证（见 `ara-vst3/output.json::warnings`）。

## v1.3 线程相关变更

- `CpuBudgetManager` 精简到 5 字段（`totalBudget` / `onnxIntra` / `onnxInter` / `onnxSequential` / `allowSpinning`），线程预算初始化时固定，不再随 playback 动态变化；
- 移除 `SimdAccelerator` 统一分派；各 kernel 直接使用 `CpuFeatures` 查询 + 目标 ISA 内部函数；
- `PianoKeyAudition` 采用 64 槽 SPSC + 8 Voice 池；noteOn 消息线程写，`mixIntoBuffer` 音频线程读；
- `UndoManager` 单一线性栈（Message Thread only）。

## 性能红线

| 操作 | 允许线程 | 备注 |
|---|---|---|
| ONNX Run | Worker | 绝不可能在 audio thread |
| 文件 I/O | Worker / Message | audio thread 禁 |
| repaint | Message | 其他线程用 `MessageManager::callAsync` 跳转 |
| 加锁 | Message / Worker | audio thread 仅 TryLock |
| 日志 | Message / Worker | audio thread 用 counter 累加 |
| 分配 | Message / Worker | audio thread 禁（可能导致 priority inversion） |

## ⚠️ 待确认

- `resetInferenceBackend` 文档声明为 UI-thread call 但内部 join worker，从 audio/ARA 线程调用有 hazard（见 core-processor warnings）；
- `mappingLogCounter` 进程级静态，多 VST3 实例共享（ara-vst3 warnings）；
- `didUpdateMusicalContextProperties` 未回写 processor BPM；
- 析构顺序与 hydration worker 生命周期窗口未交叉验证。
