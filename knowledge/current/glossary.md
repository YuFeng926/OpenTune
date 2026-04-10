---
spec_version: 1.0.0
status: draft
doc_type: glossary
generated_by: synthesis-agent
generated_at: 2026-04-10
last_updated: 2026-04-10
---

# OpenTune — 术语表

## AI / 模型相关

| 术语 | 英文标识 | 定义 | 所在模块 |
|------|---------|------|---------|
| RMVPE | `RMVPEExtractor` | Robust Model for Vocal Pitch Estimation，基频 (F0) 提取深度学习模型。输入 16kHz 单声道音频，输出 100fps 的 F0 频率数组 | inference |
| NSF-HiFiGAN | `PCNSFHifiGANVocoder` | Neural Source-Filter HiFi-GAN，音高可控神经声码器。从 Mel 频谱 + F0 合成 44100Hz 音频，保持原始音色改变音高 | inference |
| ONNX Runtime | — | 微软开源推理引擎 (v1.17.3)，支持 CPU / CoreML (macOS) / DirectML (Windows) 后端 | inference |
| F0 | — | 基频 (Fundamental Frequency)，人声的基本振动频率 (Hz)。100fps = 每帧 10ms | inference, pitch-correction |
| Mel 频谱 | `MelSpectrogramProcessor` | 对数梅尔频谱，128 mels × N 帧，FFT=2048, hop=512, 40-16000Hz | dsp |
| 声码器 | Vocoder | 语音合成器，将频谱特征 + F0 转换为时域音频波形 | inference |
| Preflight Check | `PreflightResult` | RMVPE 推理前的三阶段资源验证（时长/内存/模型文件），避免运行时 OOM | inference |

## 音高修正

| 术语 | 英文标识 | 定义 | 所在模块 |
|------|---------|------|---------|
| PitchCurve | `PitchCurve` | 音高曲线管理器，通过 COW (Copy-on-Write) 模式发布不可变快照。连接编辑层与渲染层的核心数据枢纽 | ⚠️ pitch-correction (逻辑) / utils (物理文件) |
| PitchCurveSnapshot | `PitchCurveSnapshot` | 不可变音高曲线快照，包含 originalF0、originalEnergy、correctedSegments。通过 `atomic_store/load` 实现 lock-free 读取 | ⚠️ pitch-correction (逻辑) / utils (物理文件) |
| CorrectedSegment | `CorrectedSegment` | 修正段，包含帧范围 + 修正后 F0 数据 + 来源类型 (NoteBased/HandDraw/LineAnchor) + 修正参数 | pitch-correction |
| Retune Speed | `retuneSpeed` | 修正强度 (0.0~1.0)。0=保留全部原始细节仅偏移，1=完全拉平到目标音高 ("auto-tune" 效果) | pitch-correction |
| Slope Rotation | — | 斜率旋转补偿，五阶段修正算法的第一阶段。补偿自然音高漂移（如上升语调） | pitch-correction |
| PIP | `estimatePIP` | Perceptual Intentional Pitch，感知有意音高。VNC (振颤中性中心) + SSA (稳态分析) + 能量加权 | ⚠️ pitch-correction (逻辑) / utils (物理文件) |
| VNC | — | Vibrato-Neutral Center，150ms 居中移动平均，消除颤音影响的音高估算 | pitch-correction |
| NoteGenerator | `NoteGenerator` | 从 F0 曲线自动分割生成音符序列的算法。依据偏差阈值 (80 cents) 分段，静默桥接 (10ms) | ⚠️ pitch-correction (逻辑) / utils (物理文件) |
| Scale Snap | `ScaleSnapConfig` | 音阶对齐配置，将音符量化到指定调性的音阶音上 | dsp, pitch-correction |

## 渲染管线

| 术语 | 英文标识 | 定义 | 所在模块 |
|------|---------|------|---------|
| RenderCache | `RenderCache` | AI 渲染结果缓存。`SpinLock` 保护，双版本号协议 (`desired`/`published`)，1.5GB 全局上限，LRU 驱逐 | inference |
| Chunk | — | 渲染单元。音频按 silentGaps 拆分为自然 Chunk，每个 Chunk 独立渲染 | core-processor, inference |
| ChunkRenderTask | — | Chunk 渲染任务，包含时间范围和关联的 clip/track 信息。压入 deque 前端（最新编辑优先） | core-processor |
| Revision Protocol | `desiredRevision` / `publishedRevision` | 缓存版本协议。编辑时 bump desired，渲染完成时 set published，两者相等时缓存有效 | inference |
| Dry Signal | `drySignalBuffer_` | 原始音频信号（已重采样到设备采样率），渲染缓存未命中时的回退音源 | core-processor |

## 数据结构

| 术语 | 英文标识 | 定义 | 所在模块 |
|------|---------|------|---------|
| Note | `Note` | 音符数据：startTime/endTime (秒)、pitch (Hz)、pitchOffset (半音)、retuneSpeed、vibrato 参数 | utils |
| NoteSequence | `NoteSequence` | 有序不重叠的音符容器，支持 insert/erase/range-replace | utils |
| LineAnchor | `LineAnchor` | 线条锚点：用户在 LineAnchor 工具中放置的控制点，系统在 log2 空间插值生成 F0 | utils |
| ClipSnapshot | `ClipSnapshot` | Clip 完整状态快照，用于 Undo/Redo 恢复 | utils |
| Result\<T\> | `Result<T>` | Rust-style 结果类型，基于 `std::variant<T, Error>` 实现。项目约定不抛异常 | utils |
| ErrorCode | `ErrorCode` | 结构化错误码枚举 (7 类 ~20 种)：模型/初始化/音频/F0/Mel/参数/未知 | utils |
| LockFreeQueue | `LockFreeQueue<T>` | MPMC 有界无锁环形队列，`alignas(64)` 缓存行对齐 | utils |

## UI 相关

| 术语 | 英文标识 | 定义 | 所在模块 |
|------|---------|------|---------|
| PianoRoll | `PianoRollComponent` | 钢琴卷帘编辑器，用户可视化和编辑 F0 曲线与音符的核心交互组件 | ui-piano-roll |
| ToolHandler | `PianoRollToolHandler` | 工具交互分发器，按当前工具模式 (AutoTune/Select/DrawNote/LineAnchor/HandDraw) 处理鼠标/键盘事件 | ui-piano-roll |
| RenderContext | — | 每次 paint 构建的不可变参数快照，确保渲染一致性 | ui-piano-roll |
| FrameScheduler | `FrameScheduler` | 帧调度器单例，合并 repaint 请求、优先级排序，节流至 ~60fps | ui-main |
| WaveformMipmap | `WaveformMipmap` | 6 级波形 LOD 缓存 (32~32768 samples/peak)，int8 压缩存储，增量构建 | ui-main |
| PlayheadOverlay | `PlayheadOverlayComponent` | VBlank 独立刷新的播放头覆盖层，全 `std::atomic` 属性确保线程安全 | ui-main |
| ThemeTokens | `ThemeTokens` | 语义化主题 Token 结构体，三套主题实例：BlueBreeze / DarkBlueGrey / Aurora | ui-theme |
| UIColors | `UIColors` | 全局 `static inline` 色值缓存，主题切换时重写，零运行时查找开销 | ui-theme |
| Mediator | — | PluginEditor 作为中心协调者的设计模式，所有子组件通过 Listener 通知事件 | ui-main |

## 信号处理

| 术语 | 英文标识 | 定义 | 所在模块 |
|------|---------|------|---------|
| ResamplingManager | `ResamplingManager` | r8brain 封装，24-bit 精度。语义化接口：downsampleForInference (→16kHz)、upsampleForHost (→设备率) | dsp |
| ScaleInference | `ScaleInference` | Krumhansl-Schmuckler 调式检测算法，12 大调 + 12 小调模板匹配，3 秒投票防抖 | dsp |
| SimdAccelerator | `SimdAccelerator` | 运行时 SIMD 函数指针分派 (Scalar/AVX2/AVX-512/NEON/Accelerate)，提供 dotProduct、vectorLog 等 | utils |
| SilentGapDetector | `SilentGapDetector` | 音频静息段检测器，用于确定 Chunk 渲染边界 | utils |

## 线程 / 并发

| 术语 | 英文标识 | 定义 | 所在模块 |
|------|---------|------|---------|
| COW | Copy-on-Write | PitchCurve 的不可变快照模式。每次修改创建新快照并 `atomic_store`，读者通过 `atomic_load` 获取 | pitch-correction |
| Two-Phase Import | — | 两阶段导入：prepare (后台线程 CPU 密集) + commit (消息线程写锁，轻量挂载) | core-processor |
| ScopedTryLock | `juce::SpinLock::ScopedTryLock` | 非阻塞尝试加锁。音频线程访问 RenderCache 时使用，失败即回退 | inference, core-processor |
| tracksLock_ | `juce::ReadWriteLock` | 轨道数据保护锁。Audio thread 持 ReadLock，UI thread 持 WriteLock | core-processor |

## 框架 / 工具

| 术语 | 英文标识 | 定义 | 所在模块 |
|------|---------|------|---------|
| UndoAction | `UndoAction` | Command 模式可逆操作基类，12 种具体子类 + CompoundUndoAction 原子组合 | utils |
| AppLogger | `AppLogger` | 全静态分级日志 (Debug/Info/Warning/Error)，文件记录 + PerfTimer RAII 计时 | utils |
| AccelerationDetector | `AccelerationDetector` | 硬件加速后端自动检测 (DirectML / CoreML)，构造时一次性检测 | utils |
| ModelPathResolver | `ModelPathResolver` | ONNX Runtime 及模型文件路径解析器，处理 Windows DLL 搜索路径 | utils |
