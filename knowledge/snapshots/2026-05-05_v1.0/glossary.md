---
spec_version: 1.0.0
status: draft
doc_type: glossary
generated_by: synthesis-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# OpenTune 全局术语表

本表由 Synthesis Agent 汇总所有模块 `key_terms`，按领域分类。
末尾"⚠️ 待确认冲突"表整理自 `.knowledge-work/conflicts.json`。

## AI / 模型相关

| 术语 | 英文 / 代码标识 | 定义 | 所在模块 |
|---|---|---|---|
| F0 | Fundamental Frequency | 基频（人声基本振动频率，单位 Hz）；RMVPE 输出 100 fps | inference, pitch-correction |
| Mel 频谱 | `MelSpectrogramProcessor` / `computeLogMelSpectrogram` | 对数梅尔频谱，反射填充 + STFT + Hann 窗 + Slaney 归一化三角滤波器；声码器输入特征 | dsp |
| Vocoder / 声码器 | `VocoderInterface` / `OnnxVocoderBase` / `PCNSFHifiGANVocoder` / `DmlVocoder` | Neural Source-Filter HiFi-GAN，输入 F0 + Mel，合成 44.1 kHz PCM | inference |
| RMVPE | `RMVPEExtractor` | Robust Model for Vocal Pitch Estimation，F0 提取模型；16 kHz 单声道输入 | inference |
| PC-NSF-HiFiGAN | `PCNSFHifiGANVocoder` | Pitch-Controllable NSF HiFi-GAN 声码器实现（ONNX） | inference |
| ONNX Runtime | `onnxruntime_cxx_api` | 微软开源推理引擎 1.17.3，支持 CPU / CoreML / DirectML | inference |
| DirectML | DML / `DmlVocoder` / `DmlConfig` | Windows 下 GPU 推理后端；通过 DML Execution Provider 注入 | inference |
| CoreML | `createF0SessionOptions` | macOS 下 GPU/ANE 推理后端 | inference |
| OnnxVocoderBase | `OnnxVocoderBase` (v1.3 新基类) | 声码器 ONNX 共享基类：I/O 探测、形状解析、mel 转置、uv 导出 | inference |
| VocoderScratchBuffers | `VocoderScratchBuffers` | thread_local scratch buffer，避免每次推理重建输入/输出张量 | inference |
| Chunk / 渲染分块 | `PendingRenderJob` / `ChunkStats` | 由静音段切分的渲染单位；RenderCache 以 chunk 为状态机粒度 | inference, core-processor |
| Chroma 调性 | `ChromaKeyDetector` / `DetectedKey` | 从 PCM 经 STFT → 12 维 pitch class → 双 profile Pearson 相关（K-S + Temperley）预测 Key | dsp |

## 核心处理器 / 数据模型

| 术语 | 英文 / 代码标识 | 定义 | 所在模块 |
|---|---|---|---|
| OpenTuneAudioProcessor | `OpenTuneAudioProcessor` | JUCE AudioProcessor 派生，双格式共享内核 | core-processor |
| Source | `SourceStore` / `Source` / `SourceSnapshot` | 原始导入音频身份与 PCM（COW） | core-processor |
| Materialization | `MaterializationStore` / `MaterializationSnapshot` | 可编辑载荷 + 已渲染 PCM 缓存 | core-processor |
| Placement | `Placement` / `CommittedPlacement` / `ImportPlacement` | 时间轴上对 Source/Materialization 的摆放 | core-processor |
| Arrangement | `StandaloneArrangement` | Standalone 多轨时间轴（Track / Placement / PlaybackSnapshot） | core-processor |
| SourceWindow | `SourceWindow` | Source-absolute 时间窗（lineage 事实） | core-processor, utils |
| PlaybackSnapshot | `PlaybackSnapshot` / `PlaybackReadSource` / `PlaybackReadRequest` | 音频线程读取的不可变播放视图 | core-processor |
| PreparedImport | `PreparedImport` / `ImportPlacement` | 导入 Pipeline 的两阶段中间态：worker prepare + main commit | core-processor |
| FrozenRenderBoundaries | `FrozenRenderBoundaries` / `freezeRenderBoundaries` | 渲染边界冻结快照（静音段 → chunk 边界） | core-processor |
| OriginalF0State | `OriginalF0State` enum | NotRequested / Extracting / Ready / Failed | core-processor |
| retire / revive | `retirePlacement` / `reviveMaterialization` | Undo 支持的两态切换（不物理删除，等 ReclaimSweep 回收） | core-processor |
| Reclaim Sweep | `runReclaimSweepOnMessageThread` / `scheduleReclaimSweep` | 主线程垃圾回收：对无人引用的 Source/Materialization 真删除 | core-processor |
| AudioEditingScheme | `AudioEditingScheme::Scheme` / `ParameterTarget` / `AutoTuneDecision` | 编辑方案：Notes-primary / CorrectedF0-primary；参数面板显示哪类目标 | core-processor |
| MaterializationTimelineProjection | `MaterializationTimelineProjection` | Timeline-time ↔ Materialization-local-time 双向投影 | core-processor, utils |

## 音高修正 / Notes

| 术语 | 英文 / 代码标识 | 定义 | 所在模块 |
|---|---|---|---|
| PitchCurve | `PitchCurve` / `PitchCurveSnapshot` | COW 不可变 F0 曲线快照（`shared_ptr<const Snapshot>` + atomic_load/store） | utils（物理）/ pitch-correction（逻辑） |
| CorrectedSegment | `CorrectedSegment` | 五阶段修正片段（斜率旋转补偿 / 颤音 / 斜率旋转 / 音高偏移 / mixRetune） | pitch-correction |
| Note | `Note` / `NoteSequence` | 音符（开始帧、长度、音高、振幅、vibrato 参数等） | utils（物理）/ pitch-correction（逻辑） |
| NoteGenerator | `NoteGenerator` | F0 → Note 分段 FSM（voiced / gap-bridge / tail-extend / transition threshold） | utils（物理）/ pitch-correction |
| LineAnchor | `LineAnchor` / `DrawingState::LineAnchor` | 直线锚点绘制模式 | ui-piano-roll |
| SimdPerceptualPitchEstimator | `SimdPerceptualPitchEstimator` | PIP 感知音高估算（Tukey 15% 边缘 SSA + VNC 150ms 移动平均 + Energy 权重） | utils（物理）/ pitch-correction |
| Hermite smoothstep | `w = t²(3 - 2t)` | 修正过渡段左右各 10 帧的平滑核 | pitch-correction |
| ScaleSnapConfig | `ScaleSnapConfig` | 音阶量化（Major / Minor / HarmonicMinor / Dorian / Mixolydian / Pentatonic*） | pitch-correction |
| AsyncCorrectionRequest | `AsyncCorrectionRequest` | Worker 单槽版本号取消；携带 materializationIdSnapshot + epochSnapshot | pitch-correction, ui-piano-roll |
| PianoRollCorrectionWorker | `PianoRollCorrectionWorker` | 后台修正线程（单槽 pending + 版本号 → VersionMismatch 取消） | pitch-correction, ui-piano-roll |
| editedMaterializationEpoch | `editedMaterializationEpoch_` | 每次 `setEditedMaterialization` 自增；防止跨素材串轨 | ui-piano-roll |

## DSP / 音频数学

| 术语 | 英文 / 代码标识 | 定义 | 所在模块 |
|---|---|---|---|
| r8brain 重采样 | `r8b::CDSPResampler24` / `ResamplingManager` | 24-bit polyphase oneshot 重采样；导入至 44.1 kHz | dsp |
| LR4 分频 | `juce::dsp::LinkwitzRileyFilter<float>` / `CrossoverMixer` | 4 阶 Linkwitz-Riley 14 kHz 分频；LPF(vocoder) + HPF(dry) magnitude-flat 合成 | dsp |
| STFT Hann | `juce::dsp::FFT` + `juce::dsp::WindowingFunction<float>` | 短时傅里叶 + Hann 窗（Mel / Chroma 共用） | dsp |
| Slaney 归一化 | — | Mel 三角滤波器 Slaney 归一化风格 | dsp |
| Key Profile | K-S / Temperley / Pearson | 调性识别模板（Krumhansl-Schmuckler + Temperley），Pearson 相关集成 | dsp |

## 推理调度

| 术语 | 英文 / 代码标识 | 定义 | 所在模块 |
|---|---|---|---|
| F0InferenceService | `F0InferenceService` | RMVPE 单次推理封装（重采样 + 高通 + 噪声门 + 单次 ONNX 推理 + 倍频修正 + 空隙填补） | inference |
| F0ExtractionService | `F0ExtractionService` | 多线程 F0 提取任务队列（请求去重 + token 取消） | core-processor |
| VocoderDomain | `VocoderDomain` | Service + Scheduler + Cache 的聚合门面 | inference |
| VocoderInferenceService | `VocoderInferenceService` | 声码器推理线程包装 | inference |
| VocoderRenderScheduler | `VocoderRenderScheduler` | Chunk 级任务队列调度 + 版本协议 + 预读 | inference |
| RenderCache | `RenderCache` | LRU 驱逐的 chunk PCM 缓存（上限 256 MB） | inference |
| RenderCache::ChunkStats | `RenderCache::ChunkStats` | 每 chunk 状态机元数据（Ready / Rendering / Dirty / etc.） | inference |
| renderRevision | `renderRevision` / `desiredRevision` / `hydratedContentRevision` | 单调递增版本号；比较决定是否需重渲染 | core-processor, ara-vst3, inference |
| kRenderSampleRate | `TimeCoordinate::kRenderSampleRate = 44100` | 内部单一采样率真值 | utils |
| kHydrationChunkSamples | `kHydrationChunkSamples` | ARA hydration 每次从 host 读取的 chunk 大小 | ara-vst3 |
| Lease Generation | `leaseGeneration` / `retiringReaderLease` | Source 读者租约代数；support snapshot 迁移 | ara-vst3 |

## ARA / VST3 集成

| 术语 | 英文 / 代码标识 | 定义 | 所在模块 |
|---|---|---|---|
| ARA | Audio Random Access 2 | DAW 与插件间共享音频/时间线访问协议 | ara-vst3 |
| PlaybackRegion | `ARAPlaybackRegion` | ARA 片段（host-managed） | ara-vst3 |
| AudioSource | `ARAAudioSource` | ARA 源音频句柄 | ara-vst3 |
| AudioModification | `ARAAudioModification` | ARA 音频修改对象（⚠️ 当前未建模到 session） | ara-vst3 |
| MusicalContext | `ARAMusicalContext` | ARA 音乐上下文（BPM / 节拍） | ara-vst3 |
| DocumentController | `OpenTuneDocumentController` | ARA 入口，JUCE `ARADocumentControllerSpecialisation` 派生 | ara-vst3 |
| PlaybackRenderer | `OpenTunePlaybackRenderer` | ARA 实时渲染回调 | ara-vst3 |
| VST3AraSession | `VST3AraSession` | ARA session 状态机（SourceSlot / RegionSlot / PublishedSnapshot / BindingState） | ara-vst3 |
| preferred region | `preferredRegion` | 当前用户聚焦的 ARA region（用于驱动 PianoRoll） | ara-vst3 |
| auto-birth | `auto-birth materialization` / `ensureAraRegionMaterialization` | ARA region 首次出现时自动为其创建 materialization | ara-vst3 |
| BindingState | `Unbound` / `HydratingSource` / `BoundNeedsRender` / `Renderable` | Region ↔ Materialization 绑定状态机 | ara-vst3 |
| isometric projection invariant | — | Timeline ↔ source 时间映射的等距投影不变式（容差 0.001s） | ara-vst3 |

## UI 架构

| 术语 | 英文 / 代码标识 | 定义 | 所在模块 |
|---|---|---|---|
| PluginEditor | `OpenTuneAudioProcessorEditor`（Standalone / VST3 两套） | Mediator 根组件 | ui-main, ara-vst3 |
| PianoRollComponent | `PianoRollComponent` | 钢琴卷帘主组件（40+ setter / Listener / invalidateVisual） | ui-piano-roll |
| PianoRollToolHandler | `PianoRollToolHandler` | 工具分派（5 工具）+ 快捷键契约 | ui-piano-roll |
| PianoRollRenderer | `PianoRollRenderer` / `RenderContext` | 钢琴卷帘绘制入口 | ui-piano-roll |
| PianoRollVisualInvalidation | `PianoRollVisualInvalidation` / `makeVisualFlushDecision` | 纯函数视觉失效决策器（v1.3 拆出） | ui-piano-roll |
| InteractionState | `InteractionState` / `NoteDragState` / `NoteResizeState` / `DrawingState` | 交互聚合态 | ui-piano-roll |
| PianoKeyAudition | `PianoKeyAudition` | 钢琴键试听：消息线程写 64 槽 SPSC + 音频线程读；8 Voice 池 | ui-piano-roll, utils |
| PianoRollEditAction | `PianoRollEditAction` (UndoAction 子类) | 钢琴卷帘 Undo 记录（before/after notes + segments 快照） | utils（物理）/ ui-piano-roll（逻辑） |
| WaveformMipmap | `WaveformMipmap` | 6 级 LOD 波形缓存（int8 压缩） | ui-main |
| FrameScheduler | `FrameScheduler` | 节流绘制调度器（聚合脏区，VBlank 同步） | ui-main |
| TimeConverter | `TimeConverter` | UI 像素 ↔ 时间转换 | ui-main |
| AppPreferences | `AppPreferencesState` / `SharedPreferencesState` / `StandalonePreferencesState` | 双层偏好（Shared + Standalone）+ InterProcessLock + XML | utils |
| ParameterPanelSync | `ParameterPanelSync` / `ParameterPanelSyncContext` / `Decision` | 纯函数：根据选区/工具决定 ParameterPanel 显示哪组参数 | ui-main, utils |
| RenderingPriority | `RenderingPriority` enum (GpuFirst / CpuFirst) | 推理后端偏好，通过 `resetInferenceBackend(forceCpu)` 生效 | ui-main |
| VisualInvalidation::Reason/Priority/Request | — | 视觉失效的原因/优先级/请求三件套 | ui-piano-roll |

## 主题 / LookAndFeel

| 术语 | 英文 / 代码标识 | 定义 | 所在模块 |
|---|---|---|---|
| ThemeTokens | `ThemeTokens` | 44 Colour + cornerRadius 语义 Token | ui-theme |
| ThemeStyle | `ThemeStyle` | 23 字段风格参数 | ui-theme |
| ThemeId | `ThemeId` enum (3 值：BlueBreeze / DarkBlueGrey / Aurora) | 主题标识 | ui-theme |
| OpenTuneLookAndFeel | `OpenTuneLookAndFeel` | header-only，内含 drawBlueBreezeXxx + drawDarkBlueGreyXxx 私有分派 | ui-theme |
| AuroraLookAndFeel | `AuroraLookAndFeel` | 独立 .cpp，含 drawNeonGlow 等霓虹效果 | ui-theme |
| UIColors | `UIColors` | 全局静态颜色字段缓存（44 色） | ui-theme |

## 基础设施

| 术语 | 英文 / 代码标识 | 定义 | 所在模块 |
|---|---|---|---|
| AppLogger | `AppLogger` | 结构化日志入口 | utils |
| UndoManager | `UndoManager` (v1.3 新) | 单一线性栈 + UndoAction 抽象基类 | utils |
| UndoAction | `UndoAction` | Undo 抽象基类（PlacementActions / PianoRollEditAction 均子类） | utils |
| LockFreeQueue | `LockFreeQueue<T>` | Vyukov 序号规则 MPSC/SPSC 无锁队列 | utils |
| Error / Result | `Error` / `Result<T>` / `ErrorCode` | 不抛异常的错误模型 | utils |
| CpuFeatures | `CpuFeatures` | ISA 检测（SSE/AVX/NEON） | utils |
| CpuBudgetManager | `CpuBudgetManager` | ONNX 线程预算（totalBudget / onnxIntra / onnxInter / onnxSequential / allowSpinning） | utils |
| AccelerationDetector | `AccelerationDetector` | GPU 枚举 + 集成显卡判定 + DML 适配器打分 | utils |
| ModelPathResolver | `ModelPathResolver` | 三级模型路径搜索（bundle / installed / user-override） | utils |
| TimeCoordinate | `TimeCoordinate` | 秒 ↔ 采样 ↔ 帧转换（kRenderSampleRate = 44100 唯一真值） | utils |
| LocalizationManager | `LocalizationManager` / `ScopedLanguageBinding` | 本地化（默认中文，v1.3 起） | utils |
| SilentGapDetector | `SilentGapDetector` / `SilentGap` | 静音段检测（驱动 chunk 边界） | utils |
| KeyShortcutConfig | `KeyShortcutConfig` | 快捷键绑定表 | utils |
| PresetManager | `PresetManager` | 预设读写 | utils |
| ScopedTryLock | `juce::ScopedTryLock` | 音频线程锁竞争退让：失败回退干信号 | core-processor, utils |
| ScopedNoDenormals | `juce::ScopedNoDenormals` | `processBlock` 禁用 denormals | core-processor |
| Windows DLL 三层防护 | `D3D12AgilityBootstrap.cpp` / `WindowsDllSearchPath.cpp` / `OnnxRuntimeDelayLoadHook.cpp` | D3D12 Agility SDK + SearchPath 限制 + Delay-Load Hook 三重防护 | utils |

## 时间 / 帧坐标

| 术语 | 英文 / 代码标识 | 定义 | 所在模块 |
|---|---|---|---|
| 44.1 kHz | `kRenderSampleRate = 44100` | 内部单一采样率真值 | utils |
| 100 fps | `F0FrameRange` / F0Timeline hopSize=160@16k | F0 帧率（每帧 10 ms） | inference |
| F0 帧索引 | `F0Timeline` | hopSize × sampleRate → secondsPerFrame；提供 frame↔seconds 转换 | core-processor |
| 秒 / 采样 / 帧 | `secondsToSamples` / `samplesToSeconds` / `secondsToSamplesFloor` / `secondsToSamplesCeil` | 所有模块经 TimeCoordinate 统一换算 | utils |
| epoch | `editedMaterializationEpoch` / `epoch` | 素材编辑代数；防止 Worker 回写串轨 | ui-piano-roll, ara-vst3 |

## 控制 / 命令协议

| 术语 | 英文 / 代码标识 | 定义 | 所在模块 |
|---|---|---|---|
| DiagnosticControlCall | `DiagnosticControlCall` / `DiagnosticInfo` | 诊断命令通道 | core-processor |
| HostTransportSnapshot | `HostTransportSnapshot` | 宿主传输状态快照（BPM / play / position） | core-processor |
| RenderBadge / AutoRenderOverlay | `RenderBadgeComponent` / `AutoRenderOverlayComponent` / `evaluateRenderStatus` / `evaluateAutoRenderOverlay` | 渲染状态纯函数决策器 + UI 呈现 | ui-main |
| Import batch | `AsyncAudioLoader` / `prepareImport` / `commitPreparedImportAsPlacement` / `commitPreparedImportAsMaterialization` | 多文件导入两阶段 pipeline | core-processor |

---

## ⚠️ 待确认冲突

来自 `.knowledge-work/conflicts.json`（16 条，略述）：

| 术语 | 冲突类型 | 需要决断 |
|---|---|---|
| RenderCache 容量 | 文档漂移（v1.0=1.5 GB / 代码=256 MB） | 以源码为准 256 MB |
| PitchCurve / NoteGenerator / SimdPerceptualPitchEstimator | 物理位置（utils/）vs 逻辑域（pitch-correction） | 物理归 utils，逻辑归 pitch-correction（已采纳） |
| F0ExtractionService vs F0InferenceService | 任务层 vs 推理引擎层 | 任务调度层归 core-processor，推理引擎归 inference |
| Undo 迁移 | v1.2 12 子类 → v1.3 UndoManager + 抽象基类 | v1.3 已生效，旧类删除 |
| OnnxVocoderBase 新基类 | v1.3 新增 | 已采纳 |
| ChromaKeyDetector vs ScaleInference | v1.3 替换 | 已采纳 |
| TabbedPreferencesDialog vs OptionsDialogComponent | v1.3 替换 | 已采纳 |
| LookAndFeel 合并（BlueBreeze + DarkBlueGrey → OpenTuneLookAndFeel） | v1.3 合并 | 已采纳 |
| PianoRollUndoSupport → PianoRollEditAction | v1.3 搬迁到 utils/ | 已采纳 |
| Aurora::Colors::Cyan 命名歧义 | 名称 vs 实际色值不一致 | ⚠️ 待确认 |
| UIColors 默认色 vs currentThemeId_ | 初值不一致导致冷启动首帧色差 | ⚠️ 待确认 |
| SimdAccelerator / DmlRuntimeVerifier | v1.3 移除 | 已采纳 |
| 多轨模型（StandaloneArrangement vs ARA PlaybackRegion） | Dual-Format by-design | 共享内核，时间轴模型分离 |

完整冲突与决议见 `.knowledge-work/conflicts.json`。
