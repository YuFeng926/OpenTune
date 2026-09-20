# OpenTune 代码架构与分布审查

审查日期：2026-09-20

审查基线：`ed9f190`（2026-09-20，`refactor: consolidate PianoRoll and processor boundaries`）

审查范围：2026-05-01 至基线提交；并复核 2026-09-20 工作区实施改动（PianoRoll/Editor/ARA）、CMake、测试与可追溯的 Git 历史。Content state 历史追踪覆盖 `6fa421f` 至当前基线。

工作方式：先静态审查，再按本报告方案实施最小重构与死代码清理；本轮 ARA 实施改动与 ContentState 三阶段重构已完成，当前工作区实现改动尚未提交

## 文档索引

- §1-§4：结论、事实基线、模块分布与线程边界
- §5：Git 演进；§5.5 为 ARA 演变，§5.6-§5.7 为 Content state 分叉与重构方案
- §6-§8：死代码、重复结构与并行路径
- §9-§10：时间域原则与测试安全网
- §11-§12：优先级与已实施/下一阶段重构方案
- §13-§15：反模式、验证限制与文件/提交索引

## 1. 结论摘要

项目的概念分层已经形成：宿主/Standalone 入口、Content owner、不可变 snapshot、Render service、进程级推理 runtime、实时播放快照都能在代码中找到对应实现。问题不在于“没有架构”，而在于架构边界没有成为编译边界和唯一真相，导致同一语义在多个域、多个格式和多个缓存路径中重复表达。

当前最重要的总体架构根因有三个；构建格式分流不再列为当前根因。本轮实施后，总体根因 1 为“部分改善”，总体根因 2 已完成 owner/schema、snapshot 和 F0 revision 收口；ARA 专项剩余风险见 §5.5：

1. **部分改善**：`PianoRollComponent` 和两个 Editor 壳层与 Processor 的交叉协调已经明显收口。PianoRoll 已不再直接依赖 Processor，时间映射和 Editor 纯同步决策已单点化；但 `PluginProcessor` 仍是格式分发、内容 owner、渲染、传输和推理的总编排点，仍是主要协调热点。
2. **本轮已完成主要收口**：Content state 已统一为 `ContentState`，分析字段保留在 `AnalysisState` 分组；Standalone、Capture、ARA 共用一个 owner schema，跨域只保留 `EditableContentSnapshot`。共同字段投影集中到 `makeContentSnapshot()`，反向重建集中到 `contentStateFromSnapshot()`，ARA source shape 仍由 `AudioModification` 适配层补充。F0 成功提交已统一为一次 curve/Ready/analysis revision/content revision 提交。DC 模型容器被 RenderWorker 直接读取的竞争仍是独立风险，不与本次 state 收口混为一项。
3. 绝对时间原则在 RT 读取和缓存边界上大体正确，但若干消费点跳过了域映射或重复做秒到样本/帧的量化。当前最高风险仍是 ARA 非恒等 `TimeGrid` 的 playback 到 prepared output 读取链；PianoRoll 播放头命中音符的 timeline/source 错配已在本轮修复，仍需宿主回归确认。

一句话判断：本轮已收口 PianoRoll/Editor 的部分交叉协调、时间链和 Content state/snapshot/F0 revision 真相。Processor 总编排、DC/RenderWorker 并发和 ARA 非恒等 TimeGrid playback 风险仍独立存在；不要继续增加 helper、兼容层或缓存层。

ARA 专项结论应按职责和生命周期判断：Processor 的 `processBlock`、Standalone 传输状态机、宿主 transport 观察仍属于 Processor；ARA 内容、CRS、F0 服务和宿主通知仍属于 `OpenTuneDocumentController`。`AudioSource` wrapper、`AudioModification.content`、CRS immutable snapshot 和 `PlaybackRegion` 的 host pointer 索引都是合法适配边界。真正需要收口的是 host-owned placement 属性的本地镜像、AudioModification clone 生命周期，以及 DC 模型容器被后台 render 线程直接读取的问题。

## 2. 审查口径与事实基线

### 2.1 数量与时间范围

| 项目 | 结果 | 口径 |
| --- | ---: | --- |
| 基线提交 | `ed9f190` | `git log -1` |
| 区间提交数 | 559 | `git rev-list --count --since='2026-05-01' HEAD` |
| 区间实际首个提交 | 2026-05-05 `0729978` | Git author date |
| 月度提交数 | 5月 92、6月 98、7月 89、8月 218、9月 61 | Git author date；相对原基线增加 1 |
| 基线源码文件 | 283 | `Source/` tracked files：179 `.h`、104 `.cpp` |
| 当前工作区源码文件 | 283 | 与 `ed9f190` 基线相同；本轮 ARA 实施改动已纳入当前提交 |
| CMake 已列出的 Source `.cpp` | 104 | 当前工作区 Source `.cpp` 全部被主 CMake 文本列出 |
| 测试源文件 | 6 | 新增 `StandaloneProcessorStateCodecTests.cpp` |
| 测试默认状态 | OFF | `CMakeLists.txt:1376` |

提交主题按 conventional prefix 统计时，`fix` 190、`feat` 70、`refactor` 65；其余为未统一前缀、docs、test、build、cleanup 等。该统计只用于描述演进压力，不等同于代码质量评分。

### 2.2 证据等级

- **事实**：可由当前文件、引用搜索或 Git commit 直接复现。
- **高置信度问题**：当前代码有明确孤儿符号、域错配或重复实现证据。
- **待验证风险**：需要运行时测试、宿主行为或训练侧资料才能最终定性。
- **产品决策项**：代码上看起来是并行路径，但是否删除取决于产品承诺，不应擅自清理。

原始审查阶段没有连接 DAW/ARA host；本轮 ContentState 实施已完成 VST3/Standalone 主 target 构建和 7 个 opt-in 测试，但仍未完成 DAW/ARA host 运行时视觉回归。

## 3. 当前代码分布

### 3.1 构建目标

当前 CMake 的实际关系如下：

```text
juce_add_plugin(OpenTune)                 # VST3 + ARA
  |
  +-- OpenTune                 VST3 SharedCode（单格式）
  |     |
  |     +-- Source/PluginProcessor.*
  |     +-- Source/ARA/**
  |     +-- Source/Plugin/Capture/**
  |     +-- 公共 UI / Content / Render / Runtime / Inference / DSP / Utils
  |
  +-- OpenTuneStandalone                  # Standalone
  |     |
  |     +-- OpenTuneStandalone             Standalone SharedCode（单格式）
  |     +-- SourceStore / Arrangement / Project / StandaloneContent / Arrangement UI
  |     +-- 公共 UI / Content / Render / Runtime / Inference / DSP / Utils
  |
  +-- OpenTune_VST3              VST3 wrapper + VST3 editor entry
  +-- OpenTuneStandalone_Standalone  Standalone wrapper + Standalone editor entry
  +-- OpenTuneResources          BinaryData：字体、钢琴采样、主题/光标图片
  +-- OpenTuneOnnxRuntimeImportLib  Windows 改名 ORT 导入库
  +-- 6 个可选测试 executable
```

依据：`CMakeLists.txt:364-424`、`:544-800`、`:807-864`、`:1376-1458`。

`870f615` 已解决的构建边界：

- ARA 与 Capture 只挂入 `OpenTune` VST3 SharedCode；Standalone 专属的 SourceStore、Arrangement、Project、StandaloneContent 与 Standalone UI 只挂入 `OpenTuneStandalone`。
- 两个 wrapper 的 Editor 入口已分别挂在最终 wrapper target，不再依赖共享源码中的运行时 `wrapperType` 分流。
- VST3 状态 payload 已抽为 `Plugin/Vst3ProcessorStateCodec.*`，Capture persistence 增加独立测试。

剩余的构建维护事项，不属于当前架构根因：

- `OPENTUNE_COMMON_SOURCES` 在两个 SharedCode target 各编译一份是预期行为：两个产品需要各自生成一份包含公共实现的二进制，不构成错误的格式分流。
- Standalone OTSS state codec 已移到 `OPENTUNE_STANDALONE_SOURCES`，不再让 VST3 SharedCode 编译无引用的 Standalone codec。
- ARA/Capture、Standalone 专属源与公共源由 CMake 手工维护，属于源归属清单的维护成本；它不是当前 `wrapperType` 双路径问题的残留。
- `PluginProcessor.cpp` 仍有大量 `#if JucePlugin_Build_*`，但现在每个 SharedCode target 只启用一个格式宏，分支是可靠的编译期裁剪，不应与旧的双宏错误等同。

### 3.2 模块树与职责

| 目录 | 主要职责 | 入口/核心类型 | 当前边界评价 |
| --- | --- | --- | --- |
| `Source/PluginProcessor.*` | JUCE AudioProcessor 外壳、状态、传输、三种内容域路由 | `OpenTuneAudioProcessor` | 巨石协调器，5401 行 `.cpp`、901 行 `.h`；本轮只抽出纯 patch/codec |
| `Source/SourceStore.*` | Standalone/工程源音频 identity、缓冲区和 retire 生命周期 | `SourceStore` | 与 domain content 的 source/window 语义并存 |
| `Source/StandaloneArrangement.*` | 多轨 placement、选择、轨道混音参数、RT playback snapshot | `StandaloneArrangement` | 结构清楚，但仍由 Processor 大量代理调用 |
| `Source/Content/` | ContentKey、统一 owner state、snapshot、Standalone/Capture owner | `DomainContentOwner`、`ContentState`、`EditableContentSnapshot` | owner schema 与共同 snapshot 投影已收口 |
| `Source/ARA/` | AudioSource、AudioModification、PlaybackRegion、DocumentController、ARA renderer | `OpenTuneDocumentController` | DC 是 document-scoped ARA owner；wrapper 只做必要适配，placement 由 SDK 对象投影，仍有 snapshot owner 与线程隔离待收口 |
| `Source/Plugin/Capture/` | 非 ARA VST3 capture、ring buffer、segment persistence | `CaptureSession` | 与 Standalone content 共用 owner schema 和 snapshot |
| `Source/Render/` | RenderJob、RenderWorker、CRS、Stage2、playback publisher | `ContentRenderService` | 具备单队列意图，依赖 Inference 形成环 |
| `Source/Runtime/` | 进程寿命的 F0/render runtime、control worker | `ProcessF0Runtime`、`ProcessRenderRuntime` | 生命周期安全较强，线程/锁层次复杂 |
| `Source/Inference/` | FCPE、GAME、vocoder、RenderCache、TimeStretchCache | `F0InferenceService`、`VocoderDomain` | 模型域与渲染域边界仍有交叉 |
| `Source/DSP/` | F0 key、mel、周期检测、移调、重采样、EQ | `AutoTune*`、`MelSpectrogram` | 个别公共域类型反向依赖 Content/Render |
| `Source/Standalone/UI/` | PianoRoll、Arrangement、主题、Transport、Preferences 组件 | `PianoRollComponent`、`ArrangementViewComponent` | Arrangement 已隔离；PianoRoll/共享 UI 仍在两个公共 target 编译，但 PianoRoll 已与 Processor 解耦 |
| `Source/Editor/` | 跨格式 editor factory、共享/Standalone preference 页、对话框 | `SharedPreferencePages` | 共享偏好页直接依赖 Standalone Theme/UI |
| `Source/Utils/` | 数据模型、编辑 action、项目持久化、偏好、映射和工具函数 | `Note`、`PitchCurve`、`TimeGrid` 等 | 已不是叶子层，存在反向依赖环 |
| `Tests/` | 7 个 opt-in 测试 | CMake optional targets | 新增 VST3/Standalone codec、Capture persistence 与 Content snapshot/F0 revision；仍缺时间域契约 |
| `tools/` | 离线 ONNX/F0/mel 验证脚本 | 不参与构建 | 构建与运行时之外的验证工具 |

### 3.3 依赖方向问题

理想方向应接近：

```text
纯数据/单位类型
        ↓
Content owner / domain adapter
        ↓
Render / Inference / DSP services
        ↓
Processor orchestration
        ↓
Editor / UI
```

当前存在的反向或环状依赖包括：

- `Utils/AppPreferences.h` 依赖 `Inference/IF0Extractor.h` 和 `Standalone/UI/ThemeTokens.h`。
- `Utils/PitchCurve.h` 依赖 `Inference/ChunkRenderStrategy.h`。
- `Utils/PlaybackAudioReader.h` 依赖 `Render/ContentRenderService.h`、`Inference/TimeStretchCache.h` 和 `Inference/RenderCache.h`。
- `Utils/PlacementActions.h` 直接 include `PluginProcessor.h`；`Utils/ProjectSession.cpp` 也直接 include `PluginProcessor.h`，形成“工具层反向依赖顶层协调器”。
- `Content/AnalysisState.h` 依赖 `DSP/ReferenceFeatures.h`，而 `DSP/ReferenceAutoAlign.h` 又依赖 `Content/ContentKey.h`。
- `Inference/GameNoteGenerator.cpp` 依赖 `Render/RenderChunkPlanner.h`，而 `Render/ContentRenderService.h` 依赖 Inference。
- `Editor/Preferences/SharedPreferencePages.cpp` 直接依赖 `Standalone/UI/UIColors.h` 和 `Inference/ModelFactory.h`。

这不是单纯的 include 风格问题。它意味着删除一个旧类型时，编译器无法只在一个层次给出残留清单，任何重构都容易扩大到整个共享 target。

## 4. 运行时数据流与线程边界

### 4.1 三种内容路径

```text
Standalone import
  AsyncAudioLoader
    -> PluginProcessor::prepareImport / commitPreparedImportAsPlacement
    -> SourceStore + StandaloneContentRepository
    -> ContentRenderService::publishPlaybackSource
    -> F0ExtractionService
    -> Stage1 Render -> Stage2 TimeGrid -> playback snapshot

Regular VST3 (non-ARA)
  host audio blocks
    -> CaptureSession / CaptureSegment
    -> CaptureSegmentContent
    -> F0ExtractionService
    -> ContentRenderService / RenderWorker
    -> PlaybackReadSource

ARA2 VST3
  ARA DocumentController
    -> AudioSource + AudioModification + PlaybackRegion
    -> focused region / ContentKey
    -> CRS-derived PlaybackReadSource
    -> OpenTunePlaybackRenderer
```

跨域读模型是 `EditableContentSnapshot`。音频线程原则上只读取不可变快照、prepared PCM、RenderCache/TimeStretchCache 的整数样本切片；这部分方向是正确的。

ARA 的边界必须按生命周期区分：SDK model object 的操作、ARA mutation、placement projection、CRS/F0 状态和 `notifyModelUpdates()` 由 DC 消息线程处理；`processBlock`、Standalone transport state machine 和宿主 transport observation 不因 ARA 而迁入 DC。`PlaybackRegion` 只保存 host pointer 供 DC 做生命周期索引，projection 构建时直接从 SDK 对象读取 placement；renderer 和音频线程只消费不可变 RenderPlan/CRS 数据。

### 4.2 线程/所有权表

| 线程/生命周期 | 当前实现 | 评价 |
| --- | --- | --- |
| 音频实时线程 | `processBlock`、ARA renderer；读取 atomic shared_ptr 和 prepared buffer | 主要路径无锁、无动态分配，方向正确 |
| 消息线程 | 两个 Editor 约 30Hz Timer、Processor timer、状态恢复和 mutation commit | UI 轮询职责较多，两个 Editor 重复同步逻辑 |
| ARA DC 模型线程 | ARA 回调、`AudioModification.content` mutation、placement projection、CRS/F0 生命周期和宿主通知 | SDK model 访问边界正确；不得让音频线程/RenderWorker 直接解引用 ARA 对象 |
| Render worker | 每个 `ContentRenderService` 一个 `RenderWorker` | ContentKey 单队列意图明确 |
| ARA render worker 读路径 | `findAudioModificationByContentKey()`、`snapshotAudioModification()` 仍直接遍历/读取 DC 的 `audioModifications_` 与 content | 这是独立的数据竞争风险；本轮未借 clone 修复，不应通过加锁或兼容路径掩盖 |
| Vocoder worker | `VocoderRenderScheduler` / `VocoderDomain` 串行 DML Run | 与进程级 reconfigure worker 叠加，生命周期复杂 |
| F0 worker | `F0ExtractionService` detached worker，状态放入 shared state | owner 生命周期安全，但 detached 语义使关闭/观测难以测试 |
| Reference worker | `ReferenceAnalysisService` 另一套相似 detached queue | 与 F0 service 重复实现去重、关闭、completion gate 逻辑 |
| Process runtime | `ProcessF0Runtime`、`ProcessRenderRuntime` heap singleton | 规避 DLL unload 风险有效，但“进程级 + owner 级”两层生命周期需持续维护 |
| Standalone project/export | 单线程 `ThreadPool`、独立 export `std::thread`、`future` 列表 | 同一 Editor 内存在三套后台任务管理机制 |

## 5. Git 演进回顾

### 5.1 阶段时间线

| 阶段 | 关键提交 | 结构变化 |
| --- | --- | --- |
| 5/5-5/22 | `0d5117c`、`51301df`、`62489a9`、`c0685c6` | 双格式/ARA 基础、Capture、TimeGrid、Stage2 引擎连续更换 |
| 5/23-5/30 | `a4e2f66`、`38572ea`、`b8c59c6`、`061aa31` | `.otproj`、Reference、旧 DSP/VAD 删除、timeline cache 开始建立 |
| 6/1-6/15 | `5a4418d`、`fbf8483`、`6fa421f`、`6f2438e`、`664aa53` | ARA 所有权从 MaterializationStore/Session 迁移到 ContentKey/owner |
| 6/16-6/30 | `919d659`、`f4c56f2`、`6c6b1c8`、`632ca3a` | mutation sink、Stage2 runtime、tile/VBlank/surface 多轮切换 |
| 7/1-7/28 | `9dc381f`、`55f7585`、`40bf5fb`、`fcd0910`、`66b3c08`、`de74b86` | TileCanvas 次日删除；缓存删除后又恢复 retained；playhead ownership 出现 revert 后重做 |
| 7/31-8/19 | `25181ef`、`a7db361`、`eee7b41` | OpenDyne、进程级推理脱钩、EQ 与主题集中增加 |
| 8/20-8/31 | `863ddb9`、`4b3f19f`、`7f8a826`、`24984bd`、`78231ff` | 周期移调重写、FCPE 替换 RMVPE、mel/vocoder 训练对齐 |
| 9/1-9/18 | `c4a125b`、`123d03c`、`808e9b8`、`3909948`、`59381a1`、`33e1af3` | save/open 收口、seek/ARA read/runtime init 加固、物理 UI zoom |
| 9/19 | `870f615` | Standalone/VST3 单格式 target 隔离；VST3 状态 codec 与 Capture persistence 测试 |

### 5.2 反复翻转的结构

- **Content ownership**：MaterializationStore → extracted services → ContentKey → mutation sink → TimeGrid ownership → transport ownership。`66b3c08` 回退 `927656b` 后，`de74b86` 再次以不同形式 hard-cut。
- **PianoRoll/timeline cache**：pre-rendered band → async tile → VBlank band → surface → composite tile → retained surface/zoom preview。`40bf5fb` 删除自管理像素缓存，次日 `fcd0910` 又引入 retained surface。
- **组件边界**：`9dc381f` 抽取 TileCanvas，`55f7585` 次日删除；说明抽象发生在渲染合同尚未稳定时。
- **Stage2/移调**：RubberBand、SoundTouch、hybrid、cycle shifter 在短时间内反复替换，随后周期检测又连续修补。
- **F0 模型**：RMVPE → DML 尝试 → 整提交回退 → FCPE runtime switch → 默认 FCPE → 删除 RMVPE。当前逻辑主路径已是 FCPE，但命名和单值接口仍有残留。
- **测试资产**：`TestMain.cpp` 和架构/视觉测试多次建立后删除。`ae82cf2`、`a5f5a92`、`439095d` 删除了大量已通过测试；当前工作区已有 7 个 opt-in 测试，本轮删除的生产孤儿 API 已同步清理，仍需避免按“本次已通过”批量删除测试。

### 5.3 热点与返工信号

按本审查区间的路径提交数，热点为：

| 文件/路径 | 区间提交数 | 说明 |
| --- | ---: | --- |
| `Source/Standalone/UI/PianoRollComponent.cpp` | 186 | 渲染、交互、播放头、缓存、工具状态交汇 |
| `Source/PluginProcessor.cpp` | 115 | 所有 domain 路由、状态、传输和渲染协调 |
| `CMakeLists.txt` | 113 | 源文件/target/依赖/打包边界持续变动 |
| `Source/Standalone/UI/PianoRoll*` | 257 | PianoRoll 与相关工具/renderer 的总热点 |
| `Source/Content`、`ARA`、`Render`、`Runtime` | 128 | 所有权、渲染和生命周期反复调整 |

热点不是单独的“应该拆文件”信号，而是职责没有稳定不变量的信号。仅把巨文件机械拆成更多 helper，不会解决根因。

### 5.4 `870f615` 后的状态更新

| 原审查项 | 当前状态 | 说明 |
| --- | --- | --- |
| Standalone/VST3 构建边界 | **已解决** | 两个单格式 SharedCode target；ARA/Capture 与 Standalone 专属源已分挂；公共源双编译是预期行为 |
| Standalone/VST3 状态 codec | **已改善** | VST3 与 Standalone codec 均已从 Processor 的二进制字段解释中抽出；runtime/deferred restore 和 Capture session 提交仍由 Processor 编排 |
| Capture/Standalone persistence 测试 | **已改善** | 7 个测试全部通过；新增 Standalone OTSS v3/v2、trailing bytes、非法版本以及 Content snapshot/F0 revision 测试 |
| `PluginProcessor` 协调职责 | **部分改善** | `ContentPatchGeometry` 和 Standalone codec 已抽出；DomainKind 分发、mutation 后 render、transport、推理与 UI session state 仍由 Processor 编排 |
| `PianoRollComponent` 协调职责 | **部分改善** | `PianoRollTimeMap` 已统一映射，播放头 timeline/source 错配已修复，PianoRoll 已移除 Processor 直接依赖；组件仍保留 UI state、camera、interaction 和 retained rendering |
| 两个 Editor 同构同步 | **部分改善** | `EditorUiZoomDecision`、`ContentRevisionPulse` 已抽为纯决策，格式特有绑定和 heartbeat 仍留在各自 Editor |
| ARA placement wrapper | **已修正** | `PlaybackRegion` 不再镜像 start/duration、TimeStretch、fade、颜色或 modification ID；wrapper 只保留 host pointer，`makeProjection()` 在 DC 消息线程直接读取 SDK 当前值 |
| ARA AudioModification clone | **已修正** | `doCreateAudioModification()` 显式复制项目 content；PitchCurve 深拷贝、TimeGrid 共享不可变快照；persistent ID/ContentKey 仍由后续 `didUpdateAudioModificationProperties()` 单点绑定 |
| ARA content/CRS owner | **边界确认** | `AudioModification.content` 属于 DC 的 document-scoped 项目内容，AudioSource wrapper 承担 reader lease/source generation，CRS snapshot 为跨线程必要边界；这些不是应删除的“平行模型” |
| Content state / snapshot 真相 | **已收口** | Standalone/Capture/ARA 共用 `ContentState`；共同字段由 `makeContentSnapshot()` 投影，反向重建由 `contentStateFromSnapshot()` 完成；F0 成功提交已统一为一次 revision 提交 |
| ARA DC 与 RenderWorker 隔离 | **未处理，独立风险** | RenderWorker 仍可能直接遍历 DC 的 `audioModifications_`/content；本轮不通过加锁、registry 或兼容路径修复，后续应单独收口 immutable snapshot 交接 |
| 本轮确认的孤儿 API/迁移路径 | **已清理** | 删除 `commitPreparedImportAsContent`、`ensureSourceById`、`isInferenceReady`、ToolHandler 旧映射函数和不可达 Editor 注入块 |

实施后原先的编译分流根因仍保持已解决，且 Standalone codec 不再进入 VST3 target。ARA placement 镜像与 AudioModification clone 缺陷已修正，但尚无专属 ARA 单测或 DAW 宿主回归。Content snapshot parity 和 owner F0 revision 已有 7 个本地测试覆盖；剩余高价值风险是 ARA 非恒等 TimeGrid 的宿主级回归、DC 模型与 RenderWorker 的线程隔离、PianoRoll 多 placement 的视觉回归以及 deferred restore 的更深集成覆盖；它们是后续验证/边界收口问题，不应倒推删除合法的 ARA adapter、DC owner 或 CRS snapshot。

### 5.5 ARA 迁移演变与根因收窄

ARA 的当前结构不是一次性错误设计，而是多轮迁移留下的边界残余。主线与 ref-branch 存在重复 hash（例如 `462510f/0d5117c`、`fbf8483/6da8be2`），不应重复计为两次独立引入。

| 时间 | 提交 | 当时目的 | 当前判断 |
|---|---|---|---|
| 5/5 | `462510f` | 建立 ARA；Session 提供宿主读取、异步 hydration 和 RT snapshot | Session 同时承担选择/绑定/materialization，形成早期第二编排点 |
| 5/18 | `c55e3ed` | 分离 ARA/Capture，加入 OTAB 绑定持久化 | content 当时仍在 Processor MaterializationStore，ARA 主要保存绑定 |
| 5/26-6/2 | `58130d4`、`4c91535`、`87daed9` | 处理异步 birth、revision 防陈旧和宿主加载时序 | Session 继续吸收状态与选择策略，第二模型层变重 |
| 6/3 | `fbf8483`、`aac602b` | 删除旧 Session，改用 ARA 对齐的 AudioSource/AudioModification/PlaybackRegion wrapper，并把 RT snapshot 移到 CRS | wrapper 适配与 CRS 边界是合理的；镜像字段残余仍未消除 |
| 6/5 | `36454de` | 将 content store 从 Processor 移入 DC，解决多 Processor 绑定同一 ARA Document 的覆盖 | DC 成为正确的 document-scoped owner |
| 6/7-6/8 | `6fa421f`、`2fc7b9d`、`b066a24`、`b7456e9` | 建立 ContentKey/CRS/content，并迁移 Editor 读写 | DTO、snapshot、CRS 逐步并存；写入口开始集中到 DC |
| 6/15 | `d3825d7` | 删除 MaterializationStore，让 AudioModification 持有持久 content，改用 ARA2 archive | 修正了 content owner 错位；persistent ID↔ContentKey 历史映射仍需后续生命周期收口 |
| 6/16-6/18 | `919d659`、`5c1dc94`、`caeeb52` | 统一 mutation/render 状态机，修复 DC 通知线程约束和 DC 访问问题 | CRS immutable snapshot 成为必要边界，但 DC 容器的后台读取仍需独立处理 |
| 7/15-7/18 | `0eb2698`、`de74b86` | 收紧 TimeGrid ownership，区分持久内容与 playback-ready 状态 | wrapper 生命周期和 content 生命周期继续分裂，但职责区分本身是合理的 |
| 9/6-9/8 | `c99d522`、`f268921` | 修复 source 内容变化后的 F0 陈旧和 archive restore 前 wrapper 注册时序 | 证明 wrapper 同步/注册必须满足生命周期合同 |
| 2026-09-20 | 工作区本轮修复 | 删除 host placement 镜像，补齐官方 clone hook | 当前根因已收窄为 clone/lifecycle/thread contract，不再是“存在 wrapper”本身 |

因此，ARA 根因应准确表述为：

> ARA wrapper 最初用于 host callback、HostAudioReader lease、source generation 和 DC 内部对象索引，属于合理适配；AudioModification.content 与 CRS snapshot 也分别承担 document 内容 owner 和跨线程不可变读取边界。真正错误的是 PlaybackRegion 曾保存 host-owned placement 属性副本、clone 未接入官方创建钩子、ContentKey 历史映射脱离 active host object 生命周期，以及 RenderWorker 直接读取 DC 模型容器。

clone 的时序必须按 SDK 生命周期理解：

1. SDK 调用 `doCreateAudioModification(audioSource, hostRef, optionalModificationToClone)`；hook 先创建新的 SDK `ARAAudioModification`，再按新 host pointer 注册内部 DTO。此时新对象还没有 SDK 写入的 persistent ID，因此 hook 不绑定 `ContentKey`。
2. 若有 clone 源且源 DTO 有 content，hook 复制 modification-scoped 内容；已完成且有有效 OriginalF0 的分析结果保留，`PitchCurve` 深拷贝，`TimeGridSnapshot` 等不可变对象共享；未完成的分析态清空，不复制无主的 `Extracting` 状态。源 `originalF0InputStamp` 在内容复制后带入 DTO，作为后续同源判断的输入标记；它不在 hook 中触发分析。
3. hook 把 DTO 放入 DC 容器并返回 SDK 对象。这个阶段不读取 PCM、不创建 CRS、不调用 `notifyModelUpdates()`，也不向宿主发通知。
4. SDK 在 hook 返回后写入新 persistent ID，并回调 `didUpdateAudioModificationProperties()`。DC 在这个回调中按 host pointer 更新 identity，再建立 persistent ID → `ContentKey` 绑定，并按当前 AudioSource attach source。此后才刷新 projection/CRS 和发布 model change；ContentKey 不能提前从 clone 源 ID 推导。

因此，F0 内容复制、stamp 搬运、host 通知和 ContentKey 绑定是四个不同阶段。clone hook 只完成第 1-3 步；第 4 步的 SDK property callback 才是 identity binding 和现有 DC 通知入口。

### 5.6 Content state 分叉与根因收窄

Content state 的问题不是某个 owner 写错了一个字段，而是一次次为真实边界补字段时，没有回写到共同的数据合同。关键时间点如下；重复 hash 的 ref-branch 对不重复计数。

| 时间 | 提交 | 当时目的 | 形成的当前状态 |
|---|---|---|---|
| 6/7 | `6fa421f` | 用 `ContentKey`、CRS 和 owner API 取代 `MaterializationStore`，首次建立 `AnalysisState`、`EditableContentState`、`AudioModificationContentState` 和 `EditableContentSnapshot` | 起点仍是一套共享 editable；ARA 聚合、Capture owner、Standalone owner 共用同一组编辑字段，snapshot 是跨 worker 的只读读模型 |
| 6/8 | `f65864a` | 为 Standalone 建立 clip identity、payload、retire/revive 和音频 revision | Standalone 从共享 editable 分家为 `ContentPayloadState`，因为它需要一个包含 PCM、分析、编辑和生命周期的完整 payload；snapshot 同步增加 PCM 字段 |
| 6/10 | `d8d111a` | 完成 Capture 持久化和 Standalone 集成 | Capture 为恢复 PCM/F0/key 扩容 `EditableContentState`；该类型从“仅可编辑”变为混合 owner state，而 `pitchCurve` 仍散落在 `CaptureSegmentContent::pitchCurve_` |
| 6/12 | `6f2438e` | 迁移全部读写到 owner-backed `ContentKey` API，删除旧 provider | `EditableContentSnapshot` 脱离 ARA 聚合并自包含全部跨域字段；共同 snapshot 合同和各 owner 的手写投影同时存在 |
| 6/15 | `d3825d7` | 删除 `MaterializationStore`，让 ARA `AudioModification` 持有 document-scoped content，并遵守 ARA2 不由 modification 持有源 PCM | 这是当前分叉的定形点：ARA 拆为 `AnalysisState + ARAEditableContentState`，Standalone 为 payload，Capture 仍为扩容 editable |
| 6/16 | `919d659` | 缓存 `AudioSource` shape，补齐 ARA archive/CRS 所需源元数据 | `EditableContentSnapshot` 加入 ARA source shape 字段；当前这些字段只有 ARA snapshot 写入，生产读取未形成稳定合同，属于后续清理候选，不是统一 state 的理由 |
| 7/15 | `0eb2698` | 收紧 ARA TimeGrid ownership 和生命周期 | `ContentLifecycle` 留在 Standalone payload，ARA 改用 `optional<AudioModificationContentState>` 表达存在性；owner 生命周期与内容字段进一步混合 |
| 6/26-8/19 | `7f5b437`、`f5a3a1e`、`25181ef`、`6d79e9e`、`6f9b511`、`2457fb6`、`8f9e0da` | 依次演进 correction/F0、OpenDyne envelope、note topology、DetectedKey 和 F0 可用性 | 每次字段或语义修正都要同步 3-4 个结构体；`hasUsableOriginalF0()` 最终把真实数据判定集中到了 snapshot，但 owner revision 规则仍未集中 |

根因可以收窄为两点：

1. **owner state schema 没有唯一来源**。PCM 是否存在、分析字段、编辑字段和各 revision 被三种 owner state 以不同布局表达；ARA 的“无 PCM”是合法差异，但不要求另一套字段命名和搬运规则。
2. **snapshot 是唯一跨域读合同，却没有唯一构造入口**。`StandaloneClipContent::snapshotContent()`、`CaptureSegmentContent::snapshotContent()`、`AudioModification::snapshotContent()` 以及 `snapshotAudioModification()` 各自复制字段，`payloadFromSnapshot()` 又反向复制一次。

这些历史分叉解释了原先的漂移：Standalone 的 TimeGrid 不推进 `contentRevision`，Capture/ARA 会推进；ARA 曾同时存在 `editable.contentRevision` 和外层 `contentRevision`；`applyOriginalF0()`、`applyDetectedKey()`、`applyReferenceFeatures()` 的判等和 revision 规则也不一致。这些不是三个域的产品差异，而是共同 schema 缺失后的实现差异；本轮已删除旧 state 和重复 ARA revision，并统一 F0 成功提交，TimeGrid/DetectedKey/reference features 的既有语义暂未扩大重构。

### 5.7 Content state 的最小重构方案

目标是让“内容是什么”和“这个域何时通知/退休/恢复”分开。方案不新增第四套大结构：把现有 `ContentPayloadState` 收敛为唯一 owner data state，命名为 `ContentState`；Standalone/Capture/ARA 都持有它，ARA 的 `audioBuffer` 恒为 null。`ContentLifecycle`、birth/retire/revive 不放进这个跨域 data state。`EditableContentSnapshot` 仍是跨线程只读投影，不与 owner state 合并。

统一 state 的字段分组只有四组：

1. `SourceWindow`、`audioBuffer`、`audioSampleRate`、`audioRevision`：PCM 三元绑定；ARA 只保留 source window，PCM 通过 `AudioSource` 读取，不能把 host PCM 写进 modification。
2. `AnalysisState analysis`：`pitchCurve`、OriginalF0、DetectedKey、silent gaps、reference features 及现有分析生命周期字段；Capture 不再把 `pitchCurve` 放在 owner 旁边。
3. notes、TimeGrid、PitchShift、volume envelope、note topology 和各分项 revision：这是编辑真相；render/cache/worker ownership 不进入这里。
4. 单一 `contentRevision`：内容根 revision；ARA 删除 `editable.contentRevision`，archive reader 对旧字段保留读取容差但不再生成第二账本。

集中两个纯数据入口：

- `makeContentSnapshot(const ContentState&)`：在 Content 层一次完成 owner state → `EditableContentSnapshot` 的字段投影。Standalone、Capture、`AudioModification::snapshotContent()` 和 DC 的 `snapshotAudioModification()` 全部复用；ARA 只在投影后补 `cachedSourceShape_` 的 source 元数据。
- `contentStateFromSnapshot(const EditableContentSnapshot&)`：与上述投影并排，替代 `PluginProcessor.cpp` 的 `payloadFromSnapshot()`，供 split/clone 等确实需要重建 owner state 的路径使用。

owner 的 `apply*` 可以保留现有名字，但只做域编排加一行 state mutation；mutation 和 revision 规则在 state 层定义。最小统一规则为：notes、volume envelope、pitch、TimeGrid、pitch shift、audio buffer 的变更各推进对应分项 revision；任何 owner-visible 内容变更推进单一 `contentRevision`；相同值的状态提交不推进；`applyOriginalF0` 一次完成 curve、Ready、`analysisRevision` 的提交；reference features 的现有“不推进 revision”语义暂保留；`hasUsableOriginalF0()` 是渲染数据可用性的唯一谓词，枚举只服务生命周期展示和持久化。

执行顺序：

1. **类型收敛（已完成）**：以现有 `ContentPayloadState` 为基形迁移为 `ContentState`，加入 `AnalysisState analysis`，删除 `EditableContentState`、`ARAEditableContentState`、`AudioModificationContentState`；Capture 将 `pitchCurve_` 并入 state；ARA 的 `makeBorn()` 逻辑移入 `attachSource()`，Standalone 的 retire/revive 状态留在 owner/retired record。archive/OTSS 字段布局、ContentKey、owner 生命周期和 CRS 保持不变。
2. **投影收敛（已完成）**：实现 `makeContentSnapshot()` 与 `contentStateFromSnapshot()`，删除手写共同字段投影和 `payloadFromSnapshot()`；parity 测试覆盖 sourceWindow、分析字段、envelope/revision，ARA source shape 由 `AudioModification` 投影后补充。
3. **行为收敛（部分完成）**：三个 owner 的 `applyOriginalF0()` 已统一为一次 curve/Ready/analysis revision/content revision 提交，Capture render gate 使用真实 F0 数据。TimeGrid 是否推进总 revision、DetectedKey 判等保持原语义，未在本轮扩大为独立重构；ARA 内层 revision 已删除。

不要在这一阶段删除 `DomainContentOwner`、把三域 switch 换成通用 Router、引入 COW/锁/CloneManager，或改动 ARA/Standalone/Capture 持久化容器。它们都扩大了变更面，不能解决 state schema 和投影入口不唯一的根因。

## 6. 当前仍存在的死代码与冗余

本轮已清理与重构直接相关的确定性残留：PianoRollToolHandler 的旧时间映射入口、Plugin Editor 不可达的惰性注入块、Processor 公共头文件中不必要的 `ProcessF0Runtime` include、重复的 AUTO Ref patch 过滤 lambda、Standalone codec 的错误 target 归属，以及本轮新增的冗余 `item.isValid()` 守卫。以下列表只保留尚未处理的独立候选，不把已删除符号重复列为待办。

### 6.1 高置信度死 API

以下符号在当前生产源码中仅有声明/定义，引用搜索没有实际消费者。应在确认没有外部 ABI 约束后删除；不要保留兼容别名。

| 优先级 | 位置 | 符号/结构 | 证据与根因 |
| --- | --- | --- | --- |
| P1 | `Source/ARA/OpenTuneDocumentController.h/.cpp` | `readNotes`、`readNotesRevision`、`readTimeGrid`、`readTimeGridRevision`、`readContentRevision`、`readContentDuration`、`hasContent` | 只读访问 API 集群无生产调用；很可能是删除旧测试后未同步清理 |
| P1 | `Source/ARA/OpenTuneDocumentController.h/.cpp` | `requestSetCycleRange` | 当前全仓无调用者；保留的是对 ARA playback controller 的转发定义，测试删除后生产 API 孤儿 |
| P2 | `Source/PluginProcessor.h:782` | `setReferenceAnalysisNotificationDispatcherForTests` | `OPENTUNE_TEST_BUILD` 测试钩子在当前 7 个测试中无引用 |
| P2 | `Source/Standalone/UI/PianoRollComponent.h:196,217,219,246,247,400,497` | 多个访问器和 `getFrameRangeForTimeSpan` | 当前只剩定义/声明；对应调用曾在旧日志、旧选择 lambda 或 Oracle 清理提交中删除 |
| P2 | `Source/Standalone/UI/ArrangementViewComponent.h:123,132` | `fitToContent`、`hasUserManuallyZoomed` | 当前无调用，字段被直接使用 |
| P2 | `Source/Standalone/UI/ParameterPanel.h:90,104,121` | `isOpenDyneMode`、`setNoteSplit`、`ToolIconButton::getToolId` | `1b7b799` 已声明删除 dead note split API，但 setter 残留 |
| P2 | `Source/Standalone/UI/TrackPanelComponent.h:331,551,563,577` | `TransparentLabel`、`getActiveTrack`、`setTrackClipping`、`getVerticalScrollOffset` | 仅定义，没有调用者 |
| P2 | `Source/Standalone/UI/TransportBarComponent.h:44,204,237` | `clearAccentColour`、`getLayoutProfile`、`isWorkspaceView` | 引入后无调用者 |
| P2 | `Source/Utils/TimeGrid.h:147-156` | `TimeGrid` 薄包装器 | 当前所有业务均直接使用 `shared_ptr<const TimeGridSnapshot>`；包装器本身无外部实例使用，疑似遗留壳 |
| P2 | `ContentKey.h:13` | `DomainKind::StandaloneArrangement` | 当前引用搜索无使用点；需要确认是否为计划中的 domain，若不是应删除枚举值 |

`readPitchShift` 不在上述死 API 集群中：`Source/Plugin/PluginEditor.cpp:1389` 仍有生产调用，不能随同其它只读访问器删除；后续可改走统一 snapshot，但那是独立迁移。

### 6.2 明确的退化代码与旧词汇

| 位置 | 问题 | 判断 |
| --- | --- | --- |
| `SilentGapDetector.h:128-130`、`.cpp:230-235` | `detectAllGapsAdaptive` 的 `maxSearchDistanceSec` 明确未使用，函数只是转发 `detectAllGaps` | 纯别名和死参数；删除参数并把调用点改到真实函数 |
| `ImportedClipF0Extraction.h:109` | `FCPE ? "FCPE" : "FCPE"` | `78231ff` 删除 RMVPE 后的机械残留 |
| `PluginProcessor.cpp:959` | 同样的退化三元 | 同上 |
| `Standalone/PluginEditor.h:340`、`Plugin/PluginEditor.h:196` 等 | `rmvpeOverlayLatched_` 等名称仍沿用已删除模型 | 不是运行时死逻辑，但会误导维护者；改为 `f0OverlayLatched_` 一类真实语义 |
| `Runtime/ProcessRenderRuntime.cpp:29,335` | `kSilentGapBoundaryFadeSamples=0`，完整淡化代码永不执行 | 临时关闭已长期存在；恢复设计或删除整条死路径，不能同时保留“10ms 淡化”注释 |

### 6.3 不应误删的已清理对象

以下对象已经由历史提交清理，当前只在注释或 Git 历史中出现，不应在新重构中复活：

`MaterializationStore`、`RMVPEExtractor`、`CpuFeatures`、`LockFreeQueue.h`、`ThemedFileChooserContent`、`EqSpectrumAnimation`、`PianoRollCorrectionWorker`、`F0VisualLOD`、`TimeGridPatchBuilder`、`CompositeUndoAction`、`WindowsDllSearchPath`、旧 timeline cache/overlay 类型、`hostPositionRevision`。

## 7. 重复实现与未收口的同类结构

### 7.1 Content state 与 snapshot 重复

历史上曾存在以下字段的多份表达；当前 owner schema 和共同字段投影已收口：

- `notes`、`timeGrid`、`pitchShiftSettings`、各类 revision：历史上的 `EditableContentState`、`ContentPayloadState`、`ARAEditableContentState`，现统一位于 `ContentState`。
- `pitchCurve`、`originalF0State`、`detectedKey`、`silentGaps`、`referenceFeatures`：现由 `ContentState.analysis` 持有，并由 `makeContentSnapshot()` 投影到 `EditableContentSnapshot`。
- `StandaloneClipContent::snapshotContent()`、`CaptureSegmentContent::snapshotContent()`、`AudioModification::snapshotContent()` 和 ARA DC 渲染路径现复用共同投影；ARA source shape 仍由 `AudioModification` 适配层补充。
- split/clone 等反向重建路径直接使用 `contentStateFromSnapshot()`，旧 `payloadFromSnapshot()` 已删除。

历史具体漂移证据包括：Capture snapshot 不设置 `sourceWindow`；ARA 渲染 snapshot（`snapshotAudioModification()`）缺少 `volumeEnvelope`、`outputGainRevision` 以及 source shape 字段；ARA `AudioModification::snapshotContent()` 与 DC 渲染 snapshot 的字段集合也不相同。本轮共同字段已集中投影，ARA source shape 由适配层补充；Capture 不伪造不存在的 source lineage，只有 owner state 已明确提供时才投影 `sourceWindow`。

这不是所有字段都应该合并成一个大结构。第一性原理是：

1. 每个 domain 只保留一个 owner-truth。
2. 跨线程/跨模块只发布一个只读 snapshot 形状。
3. snapshot 构造必须是集中、可审计的投影，而不是各 owner/渲染入口各自复制字段。
4. render/cache/worker/lifecycle 不应混入编辑状态。

`ContentPayloadState`、`EditableContentState` 与 ARA 的 `AudioModificationContentState` 是已删除的历史分叉，不是三个产品概念。ARA 不拥有 PCM 是字段值/访问边界差异，不要求另一套 state schema：统一 owner state 中 `audioBuffer` 恒为 null，PCM 仍只来自 AudioSource。当前唯一 owner state 是 `ContentState`，分析字段收进 `AnalysisState`，Capture 的 pitch curve 归回 state，并只保留一层 `contentRevision`。`ContentLifecycle`、ARA birth/clone、Capture session 和 Standalone retire/revive 仍留在各自 owner 编排，不塞进跨域状态合同。

`EditableContentSnapshot` 不删除、不改成可变 state：它是跨线程唯一只读读合同，继续保留 `hasUsableOriginalF0()` 和 `forEachEffectiveF0Span()`。统一 state 解决 owner truth；集中 `makeContentSnapshot(const ContentState&)` 解决投影 truth，ARA 只在投影后补 cached AudioSource shape。`contentStateFromSnapshot()` 是唯一反向投影，供 split/clone 复用。

### 7.2 时间映射已收口

本轮新增 `PianoRollTimeMap`，统一了 `PianoRollComponent`、`PianoRollToolHandler` 和 `PianoRollRenderer` 的 source → TimeGrid output → timeline projection → pixel，以及反向链。它只持有 projection、非空 `TimeGridSnapshot` 引用和 `ViewMapper` 值，不做 sample/frame 量化，也不产生新的绝对时间真相。

播放头命中路径同时修复：`PlayHeadState` 的 timeline 秒会按每个 render item 的 projection/TimeGrid 逆映射到 source 后再和 Note 比较；高亮 damage 使用同一 placement 映射。播放头竖线、camera follow 和 output-domain TimeGrid handle 路径保持原语义。

仍缺一个独立的时间域单元测试，当前由双 target 编译、静态残留扫描和宿主级回归待补共同覆盖。

### 7.3 两个 Editor 的同步决策已收口

`Standalone/PluginEditor.cpp` 与 `Plugin/PluginEditor.cpp` 仍各自保留 timer、主题、时间签名、scale、快捷键、语言和格式特有 workflow，但重复的无 UI 决策已移入 `Utils/EditorUiSync.h`：

- `EditorUiZoomDecision`：统一 zoom 变化判断与 min-size 计算。
- `ContentRevisionPulse`：统一 notes/timeGrid/pitch revision 比较和下一基线。
- `ParameterPanelSync.h`：继续复用既有纯参数面板决策。

没有新增共享 Editor 基类、状态 coordinator 或组件引用 helper；组件 setter、ARA/Capture selection、Standalone Arrangement/Project workflow 仍留在各自壳层。

### 7.4 持久化迁移重复

EQ v5/v6/v7 的 `paletteSlot` 和旧 scalar filter 迁移分别存在于：

- `Utils/ProjectPersistence.cpp:579-651`
- `ARA/OpenTuneDocumentController.cpp:460-474`
- `Plugin/Capture/CapturePersistence.cpp:351-374`

格式容器不同是合理的，但字段迁移规则属于 Note/Eq 数据语义，不应在三处各自解释。建议提取纯数据 codec/migration 函数，容器层只负责读写属性；同时保留格式版本判断在各自边界。

### 7.5 音分偏差计算重复

`1200 * log2(corrected / original)` 出现在：

- `Inference/ChunkRenderStrategy.h:36`
- `Utils/PitchCurve.h:58`
- `Standalone/UI/PianoRoll/PianoRollRenderer.cpp:934`
- 另有 `LegacyNoteGenerator.cpp:192` 的 segment transition 计算

数学函数可以统一为纯 `centsBetweenFrequencies`，阈值和业务策略必须保留在调用方。不能把 50/75 音分等不同策略误合并成一个全局常量。

### 7.6 拍/小节计算三套定义

| 位置 | 当前计算 |
| --- | --- |
| `Utils/SnapUtils.h:10-16` | Beat=`(60/bpm)/4`；Bar=`(60/bpm)*4`，忽略 numerator/denominator |
| `Standalone/UI/TimelineLayerComposer.cpp:89-90,173-174` | `beatSeconds=(60/bpm)*4/denom`，小节由 numerator 决定 |
| `Standalone/UI/TransportBarComponent.cpp:1599-1604` | 与标尺相同的拍号语义 |

4/4 时差异被掩盖，非 4/4 时吸附、标尺和显示互相矛盾。应定义一组无状态 BeatMath 函数并把拍号作为显式输入；这是语义收口，不是新增复杂层。

### 7.7 MIDI/Hz 双 API

`Utils/PitchUtils.h:31-42` 是纯 MIDI/Hz 转换；`Standalone/UI/ViewMapper.h:39-47` 同名函数额外嵌入 `±0.5` 的 lane center 偏移。当前调用结果大体自洽，但共名 API 很容易把显示坐标规则带入音高量化。建议将 UI 函数更名为 `freqToLaneCenter`/`laneCenterToFreq`，并把 `0.5` 变成命名常量；音符、吸附和模型一律走 `PitchUtils`。

## 8. 并行结构与不生效路径

### 8.1 单格式 target 已建立

`870f615` 已删除“同一 SharedCode 同时启用 Standalone/VST3 宏”的错误结构，格式专属入口和 domain 源现在挂到明确 target。`OPENTUNE_COMMON_SOURCES` 在两个 SharedCode target 各编译一份是两个产品生成独立二进制的正常实现。后续不应恢复运行时 `wrapperType` 分流；新格式差异应进入对应专属源或现有格式 adapter。手工源清单可以另加 CMake 归属检查，但不应作为当前架构重构的理由。

### 8.2 identity TimeGrid 的两种表示

owner snapshot 中 identity `TimeGrid` 通常是非空对象；发布到 `PlaybackReadSource` 时，`publishStandalonePlaybackSource` 和 ARA 发布逻辑又把 identity 转成 `nullptr`：

- non-null identity：内容真相/编辑/持久化表示。
- null：播放读取路径的 identity sentinel。

这解释了多次“无音频/无 grid”崩溃和后续 null guard。identity 是一种合法映射，不应同时以“对象”和“缺失”表示。建议统一为非空 identity snapshot，或定义明确的 `TimeMap` value semantics；不要继续扩散空指针守卫。

### 8.3 F0 状态枚举与实际数据并行

`EditableContentSnapshot::hasUsableOriginalF0()` 已经定义了真实数据判定：曲线存在、F0 非空、hop/sample rate 有效。但多个 UI、ARA、Capture 与 processor 的 render gate 路径仍直接检查 `originalF0State == Ready`。这形成“枚举说 Ready”和“数据是否可用”两条真相轴。应统一阶段状态与数据可用性契约：状态用于生命周期展示，render gate 使用数据谓词或一个唯一命名的 `hasUsableOriginalF0`。

### 8.4 隐藏主题与持久化/环境变量后门

Overdose、BlueBreeze、DarkBlueGrey 的 UI 入口被隐藏，但代码、偏好 token 和环境变量 `OPENTUNE_THEME` 仍可激活。若这些主题是暂时保留的产品资产，这是产品决策项；若已不再支持，就应整条删除 theme token、迁移和分支，而不是保留“入口隐藏、状态可恢复”的并行产品路径。

### 8.5 重复后台 worker 机制

F0 和 Reference 分析各自实现 detached worker、队列去重、shutdown、completion dispatch；Render/Vocoder 又有另一套 worker 和 generation。它们的业务不同，不应强行做通用线程框架，但 owner 关闭、generation、completion gate 的不变量应写成同一套测试合同。否则每次生命周期修复都可能只覆盖其中一条路径。

### 8.6 ref-branch 未合入实现

`standalone..ref-branch` 有 63 个未合入提交，其中包含 `84e0589` 的 MDXNet 替代推理实现；当前分支没有 MDXNet 引用，后续又走了 GAME/FCPE 路线。这不是当前生产代码死路径，但属于仓库级并行实现。应明确该分支是归档实验还是仍有产品价值；若是归档，应在分支/文档层标注，避免继续从未合入实现反向设计。

## 9. 第一性原理：时间、采样、帧与显示坐标

### 9.1 唯一域定义

项目内应始终区分以下域：

| 域 | 含义 | 真相来源 |
| --- | --- | --- |
| timeline seconds | 宿主/Standalone 时间轴上的绝对秒 | host PositionInfo、placement start、ARA region placement |
| content output seconds | TimeGrid 输出域秒 | `TimeGridSnapshot::tauForward/tauInverse` |
| source seconds | 内容源/F0/Note/curve 的本地源秒 | `SourceWindow` 和 Note/PitchCurve 合同 |
| sample index | 某个明确 sample rate 下的绝对样本位置 | audio buffer、host absolute sample、canonical 44.1kHz |
| F0 frame | hop/sample rate 定义的帧索引 | `F0Timeline` 和模型预处理合同 |
| pixel | 纯显示投影 | `ViewMapper`、camera、renderScale |

绝对时间仍是上游事实来源。禁止用 pixel 反推新的绝对时间，也禁止把一个域的秒数不加映射直接拿到另一个域比较。

### 9.2 高风险域错配

#### T1：ARA playback → prepared output 可能跳过 TimeGrid（高风险候选，待回归测试先行）

位置：`ARA/OpenTunePlaybackRenderer.cpp:20-35,282-288`；`ARA/OpenTuneDocumentController.cpp:1696-1767`；`Inference/TimeStretchCache.cpp:197-249`。

当前链路：

```text
playback seconds
  -> mapPlaybackTimeToContentTime()
  -> contentOffset = modificationTime - sourceStart
  -> secondsToSamples(contentOffset, hostSampleRate)
  -> sliceForOutputRange()  // 按 prepared output sample 读取
```

从静态合同看，`contentOffset` 是 source/content 语义，而 `sliceForOutputRange` 的索引是已经经过 TimeGrid 的 output 语义。若该调用链确实直接使用非恒等网格的 prepared output，则位置和读取长度都会错；恒等 TimeGrid 时问题会被掩盖。当前源码没有一个明确的中间值证明这两个域已经被转换，因此将其列为高风险候选，而不是已运行确认的 bug。正确方向应是：

```text
playback absolute seconds
  -> region 内线性映射到 source/content 时间
  -> TimeGrid tauForward(source) 得到 output seconds
  -> 明确的 output seconds -> target sample round
  -> 按 output 域范围读取
```

这不是“再多加一个换算”而是补齐缺失的域转换。修复前应添加非恒等 TimeGrid 的 ARA playback regression test，锁定 source/output 位置、长度和包络映射。

#### T2：播放头命中音符的 timeline/source 错配（已修复，待宿主回归）

原位置：`Standalone/UI/PianoRollComponent.cpp:1509`、`:3521-3536`。原条件把 timeline 绝对秒直接和 source-domain 的 `Note::startTime/endTime` 比较，在 active placement 非零起点或非恒等 TimeGrid 时域不一致。

当前已由 `PianoRollTimeMap::timelineToSource()` 修复。`drawPlayheadNoteHighlight` 和 VBlank `collectHitNotes` 都按每个 render item 的 projection/TimeGrid 先逆映射到 source，damage bounds 也复用对应 placement 映射。仍需 DAW/ARA 和非恒等 TimeGrid 的宿主级回归确认视觉行为。

#### T3：ARA playback 映射中的跨域相减（中风险，需确认宿主 trim 合同）

`mapPlaybackTimeToContentTime` 中 `modificationTime - region.contentWindow.sourceStartSeconds` 混合了 modification/output 和 source 起点。当前整段、未 trim 场景可能数值相同，但函数没有把域关系写成不变量。应以 region 的 modification 起点和 content-local offset 明确构造，不要依赖两个绝对量恰好相等。

### 9.3 秒到样本/帧的重复量化

| 位置 | 当前行为 | 判断 |
| --- | --- | --- |
| `PluginProcessor.cpp:2686-2706` | 同一 split 点的 audio/silent gaps 使用 `splitSample`，notes/pitch curve 使用 `splitOffsetSeconds` | sample 锚与 seconds 锚并存，未强制互锁；应统一由唯一 sample 或唯一 seconds 锚派生两类切分 |
| `PluginProcessor.cpp:1966-1974,2009` | placement start/duration/clipIn 各自转样本，随后又 samples→seconds 计算 fade | 可产生一采样接缝或淡入误差；placement 边界应一次量化 |
| `ARA/OpenTuneDocumentController.cpp:2402-2403` | render range 直接 `static_cast<int>(seconds * sampleRate)` | 隐式 trunc；范围起点/终点应显式采用 floor/ceil 合同 |
| `TimeGrid.cpp:28-30` | 最小 source 间距先 round 到 100fps frame，再比较 15 帧 | 这是额外的 100fps 量化，不是纯绝对时间 150ms；还与其他 5ms 去重 epsilon 并存 |
| `ResamplingManager.cpp:56-57` | trunc；`TimeStretchCache.cpp:63-65,146-149` 用 round；RenderCache 用 `sampleRateProject` | 同类长度投影策略碎片化；应统一 point 用 round、区间 start/end 用 floor/ceil |
| `ARA/OpenTunePlaybackRenderer.cpp:193` | 10ms crossfade 用 trunc 后再钳制 128..2048 | 低采样率下实际不是 10ms；应使用 round，若保留下限则命名为最小样本策略 |
| `ProcessRenderRuntime.cpp:819-821` | `ceil(end * frameRate) + 1` | `ceil` 已覆盖端点，额外 +1 可能把下一帧纳入 blank/correction 判定 |

正确的现有范式应保持：`TimeCoordinate::sampleRateProject` 用 round 做点投影；区间覆盖用 floor 起点、ceil 终点；实时读取只消费已经 prepared 的整数样本切片，不在音频线程重新做秒换算。

### 9.4 F0/训练时间轴的待验证契约

- `FCPEExtractor.cpp:148-150` 的 padding 使分析窗中心与 `F0Timeline::timeAtFrame(k)=k*hop/sampleRate` 可能存在半 hop 偏移。当前链路可能内部自洽，但必须在模型预处理合同中明确“F0 frame 表示窗起点还是中心”。
- `ProcessRenderRuntime.cpp:792,1043-1054` 用 mel 帧的 `i*512/44100` 时间去插值 F0。需和训练侧 `interp_f0` 的索引/中心约定对齐；仅有“训练对齐”注释不足以证明两端同轴。
- `F0Timeline`、`PitchCurve` 和 UI 多处自行 floor/ceil；应让帧范围 API 成为唯一边界入口，避免调用方再做一轮取整。

这些属于待验证风险，不应在没有训练数据/回归样本时直接改公式。

### 9.5 常量、容差和可变参照系

- `TimeCoordinate::kRenderSampleRate` 声明为唯一来源，但仍有多处裸 `44100`：`StandaloneArrangement.cpp:465`、`SoundTouchStretcher.cpp:24`、`PianoKeyAudition.h:38`、`PlaybackReadSource.h:22`、`OpenTuneDocumentController.h:45`、`OpenTunePlaybackRenderer.h:119`、`CaptureSession.h:239`、`ContentState.h:30`、`MelSpectrogram.h:21` 等。
- `DmlVocoder.cpp:180` 的 `numFrames * 512` 应与 `RenderChunkPlanner` 的 hop 常量建立同一来源。
- `nearlyEqualSeconds` 在 `PluginProcessor.cpp:411` 和 `Plugin/PluginEditor.cpp:47` 重复定义；pending seek 又在 `PianoRollComponent.cpp:3576-3577` 使用 double 精确相等。应使用一个命名容差 helper；这不是反算时间，只是确认宿主已观察请求。
- PPQ 数据轴 `PluginProcessor.h:166,495`、`.cpp:2131-2159` 以及 `PlayHeadState.h:176-177,229-230` 被写入但无读取方，是死数据轴。项目当前播放真相是绝对秒/playing/loop boolean，PPQ 不应作为未来功能的预留搬运。
- `TuningConfig.h:11-14` 是可变的全局 `float&`。音符存储 Hz 这一点正确，但 tuning 变更后 UI lane 和吸附使用新参照、旧音符仍是旧参照，且跨 UI/render worker 无同步。至少需要原子读写和明确语义：`Note.pitch` 是绝对 Hz，MIDI 只是即时显示/吸附投影。
- `ViewMapper` 的 `±0.5` lane center 与 `PitchUtils` 的纯 MIDI 域必须改成不同命名，避免显示偏移进入模型/吸附。
- 9/18 物理缩放后，PianoRoll 用物理原点增量累积，Arrangement 用逻辑原点重推：`PianoRollComponent.cpp:2435-2436` vs `ArrangementViewComponent.cpp:708-710`。应统一由逻辑原点单点重推物理值，避免长滚动接缝漂移。

## 10. 测试与架构安全网

当前测试层有三个结构性问题：

1. 测试默认关闭。当前已有 7 个 opt-in 测试，包含 VST3/Standalone state codec、Capture persistence、Content snapshot 和 owner F0 revision；ARA ownership、PlaybackRegion placement 直读、AudioModification clone、render invalidation、TimeGrid 非恒等 ARA playback、transport epoch 仍没有持续的当前测试目标。ARA placement/clone 已完成代码级修复，但未有专属自动化测试锚点。
2. `CMakeLists.txt:1387` 的注释声称字段漂移会编译失败，但 `Tests/PitchParameterContractTests.cpp` 使用自定义 stub，不 include 真实 `Source/Utils/Note.h` 或 `PitchControlConfig.h`，因此无法提供该保证。
3. 历史上“通过即删除”测试导致生产死 API 和架构回归无锚点。测试删除本身不是问题，删除测试后必须同步删除其唯一服务 API，或把不变量迁移到永久、最小的纯函数测试。

建议保留的最小安全网：

- 一个真实 include production types 的 pitch parameter contract test。
- 一个纯时间域测试：timeline → content → source 与反向链，含非零 placement start、非恒等 TimeGrid、trim（当前仍缺）。
- 一个 ARA renderer 测试：非恒等 TimeGrid 下播放样本起点/长度/包络域正确。
- 一个 ARA model contract 测试：host placement 属性变化后 projection 读取最新 SDK 值；clone 保留 notes/TimeGrid/有效 F0、深拷贝 PitchCurve、复用输入 stamp，并且不在 clone hook 中触发宿主通知。
- 一个 split geometry 测试：同一 split sample 同时约束 notes、pitch curve、silent gaps、两段 placement。
- 一个 Content owner snapshot parity 测试：Standalone/Capture/ARA 对同一语义字段生成一致 snapshot，允许 ARA 缺少 PCM。
- 一个 ContentState mutation/revision 测试：覆盖统一投影字段集合、单一 `contentRevision`、F0 单次提交、TimeGrid/DetectedKey 判等和 ARA 缺 PCM。
- 一个 pending seek 测试：宿主回传按 block 量化的时间时，暂停状态不会永久显示 pending 值。
- 一个 dead API 架构检查不应替代编译器；删除符号后让 CMake/build 暴露真正残留调用。

## 11. 优先级建议

### P0：先锁定正确性

1. 为 ARA 非恒等 TimeGrid 增加回归测试，修正 playback → source/output → sample 的域链。
2. **已实施**：修正 PianoRoll 播放头命中音符的 timeline/source 域错配；剩余 DAW/ARA 宿主回归。
3. 统一 split 的 sample/seconds 锚，并把 placement 播放路径的边界从多处秒到样本换算改为一次量化后派生；明确每个区间的 floor/ceil/round 合同。

### P1：删除旧路径并收口真相

1. **部分实施**：本轮删除已确认的 Processor/PianoRoll 迁移残留、退化映射入口和重复 patch lambda；6.1 其余独立候选仍需单独确认后处理，不增加兼容别名。
2. 按 §5.7 收敛唯一 `ContentState`、统一 `EditableContentSnapshot` 投影和 revision 更新规则；不增加新的兼容 state。
3. 收口 identity TimeGrid 表示，消灭“identity object / nullptr sentinel”并行语义。
4. 把 `nearlyEqualSeconds`、sample-rate projection、cents math、BeatMath、lane-center API 逐个单点化。
5. **已由 `870f615` 完成**：Standalone 与 VST3 单格式 target 已分离；不再把 target 分流列为待重构项。后续可选增加源归属检查，但不影响当前 domain/UI 重构优先级。

### P2：降低维护成本

1. **已实施**：把两个 Editor 的 zoom/revision 共享同步决策移出 UI 壳层，保留格式特有绑定。
2. 把三种持久化容器的 EQ/Note migration 变成同一套纯数据 codec。
3. 统一物理/逻辑像素原点计算。
4. 清理隐藏主题或正式写明它们是受支持的实验主题；删除无产品承诺的 ref-branch 并行设计依据。
5. 重新建立少量永久架构/域测试，不再按“本次已通过”批量删除。

## 12. 协调热点的精确重构方案

目标不是把三个大文件平均切小，而是让每种真相只有一个 owner、每种转换只有一个实现。`PluginProcessor` 保留 JUCE 生命周期、RT 音频处理、格式装配与 mutation 后的 render 调度；`PianoRollComponent` 保留相机、交互状态和 retained rendering；两个 Editor 保留组件绑定与格式特有工作流。其余只在纯数据或纯决策边界抽取。

### 12.1 切片一：PianoRoll 时间映射单点化（已实施）

新增一个无状态 `PianoRollTimeMap` 值对象，放在 `Source/Standalone/UI/PianoRoll/`。构造输入固定为：

- `ContentTimelineProjection` 值。
- 非空 `const TimeGridSnapshot&`。
- `ViewMapper` 值。

只提供四类显式转换：`sourceToTimeline`、`timelineToSource`、`sourceToX`、`xToSource`。它不保存 ContentKey、不读取 owner、不做 sample/frame 量化、不拥有锁。`xToSource` 只表示一次 UI 交互坐标，不得持久化或反写为新的绝对时间真相。

实施结果：

1. `PianoRollComponent`、`PianoRollToolHandler`、`PianoRollRenderer` 已改用 `PianoRollTimeMap`；旧公式 wrapper 已删除。
2. 播放头绘制和 damage 命中已先把 timeline seconds 映射到 source seconds，再与 `Note::startTime/endTime` 比较。
3. output-domain 的 TimeGrid handle 直连路径没有误用 source map。
4. 双 target 编译通过；永久时间域单元测试和 DAW/ARA 宿主回归仍是后续安全网。

本切片只集中现有映射，不改变任何绝对时间数值、换算公式或取整策略；若测试表明公式本身需改，必须另行审批。

### 12.2 切片二：PianoRoll 去除 Processor 依赖（已实施）

`PianoRollComponent` 当前直接使用 Processor 处理 undo、频谱、自动生成音符和 TimeTool seed。应删除 `setProcessor`、`processor_` 及 `PluginProcessor` 依赖，改用已有边界：

- 内容读：现有 `ReadContentSnapshotFn`。
- 内容写：现有 `ContentEditCommands`；`generateNotesOnly` 已在接口中，TimeTool seed 作为同一内容命令补入该接口。
- undo：注入非 owning `UndoManager&`。
- EQ 频谱：注入一个只读 `SpectrumReader` 回调。
- transport：继续只读现有 `PlayHeadState` 引用。

Editor 负责一次性接线；PianoRoll 不再知道 Processor。不要新建 `PianoRollModel`、`RenderController` 或第二个 `InteractionController`：当前组件是 UI 状态 owner，`PianoRollRenderer` 和 `PianoRollToolHandler` 已分别承担渲染与工具边界，再加一层只会复制状态。

实施结果：`PianoRollComponent.*` 已不再包含、前置声明或保存 `OpenTuneAudioProcessor`；undo、频谱、AUTO、TimeGrid seed 均通过注入能力或 `ContentEditCommands` 进入。两个 Editor 的构造接线和现有命令链已通过双 target 编译。

### 12.3 切片三：Processor 只抽纯算法与 codec（已实施）

不新增覆盖三域写路径的 `ContentDomainRouter`。15 处 `DomainKind` switch 包含 ARA、本地 CRS、Stage2 与 Capture 的不同后置动作；把它们塞入一个 Router 会形成新的超级协调器。当前 switch 保留在格式边界，先抽真正与 domain 无关的内容：

1. `ContentPatchGeometry` 已承载范围过滤、边界裁剪和旧 segment 左右保留；Processor 只负责 owner commit 与 render 调度，AUTO Ref 也复用同一 helper。
2. `StandaloneProcessorStateCodec` 已按 `Vst3ProcessorStateCodec` 模式抽出；Processor 只收集/应用状态，不解释 OTSS 二进制字段。
3. 已删除 `commitPreparedImportAsContent`、`ensureSourceById`、`isInferenceReady` 及本轮确认的旧 UI/映射残留，不保留别名。
4. 保持 `PluginPianoRollSessionState` 的 processor-lifetime owner：VST3 Editor 关闭重开仍需恢复镜头。仅为缩短文件而搬到 Editor 会改变生命周期，不能做。

验证结果：双 target 编译通过；7 个测试通过，其中新增 Standalone state 当前版本往返、旧版本读取、trailing bytes、非法版本以及 Content snapshot/F0 revision 测试。所有计算沿用当前绝对时间与 frame range 输入，没有新增反算或取整。

### 12.4 切片四：Editor 只共享纯同步决策（已实施）

复用 `Utils/ParameterPanelSync.h` 的形状，不创建 `UiSyncCoordinator`、共享 Editor 基类或持有组件引用的 helper：

- `EditorUiZoomDecision`：输入 processor truth、已应用值和基础尺寸，输出是否变化及 resize limits。
- `ContentRevisionPulse`：输入上次 ContentKey/revision 基线和当前 snapshot revision，输出 key reset、notes/timeGrid/pitch changed flags 与下一基线。

两个 Editor 仍各自执行 `setResizeLimits`、组件 setter、ARA/Capture selection、Standalone Arrangement/Project workflow。`syncSharedAppPreferences` 不整块抽取；只有在确认完全同语义后，才继续提取纯 diff，组件调用顺序留在各自壳层。

实施结果：两个 `applyUiZoomIfNeeded` 不再各自计算 zoom；revision 比较统一由 `ContentRevisionPulse` 完成；两种 Editor 的 heartbeat、格式特有绑定和 UI 结构保持不变。

### 12.5 切片五：ARA placement 与 clone 生命周期边界（已实施）

本切片保留既有 DC、wrapper、CRS 和 immutable projection 结构，只删除两处错误同步假设：

1. `PlaybackRegion` wrapper 不再保存 host-owned placement 属性。`makeProjection()` 在 DC 消息线程直接从 `juce::ARAPlaybackRegion` 读取 start/duration、TimeStretch、fade、effective color 和所属 AudioModification；renderer/音频线程仍只消费 RenderPlan 快照。
2. `OpenTuneDocumentController::doCreateAudioModification()` 接入 ARA 官方 clone 工厂。它按 host pointer 注册 DTO，并复制项目 content；PitchCurve 深拷贝，TimeGridSnapshot 共享不可变对象。persistent ID/ContentKey 仍只由随后 `didUpdateAudioModificationProperties()` 绑定。

clone hook 不读取 PCM、不启动 F0、不通知宿主，也不创建 CloneManager、pending registry 或兼容路径。未完成的 F0 分析不会复制无主的 `Extracting` 状态；已完成 OriginalF0 搭配源 `originalF0InputStamp` 复用，沿用现有 DC materialize CRS 路径。

本切片没有改变时间单位、取整策略、ARA 通知策略、archive 格式、Processor transport/processBlock 责任或后台任务架构。DC 模型容器被 RenderWorker 直接读取的竞争仍是独立的下一阶段 snapshot/revision owner 问题。

### 12.6 执行顺序与完成定义

已完成执行顺序：`PianoRollTimeMap` → `PianoRoll 去 Processor 依赖` → `ContentPatchGeometry / Standalone codec / 孤儿 API` → `Editor 纯同步决策` → `ARA placement 与 clone 生命周期边界` → `ContentState 类型收敛` → `单一 snapshot 投影` → `F0/revision 行为收敛`。TimeGrid/DetectedKey 规则和 DC/RenderWorker 并发仍是独立后续项。

完成定义：

- UI 的 source/output/timeline/pixel 映射只有一套实现：已完成。
- PianoRoll 不依赖 Processor，只依赖明确读写/undo/频谱/transport 合同：已完成。
- Processor 不再承载 Standalone 二进制字段解释和内容 patch 几何，但保留格式边界与 render 编排：已完成。
- Editor 不共享有状态协调器，只共享可直接单测的决策函数：已完成。
- ARA wrapper 只保留必要适配，host placement 与 clone 生命周期遵守 SDK/DC 合同：已完成；专属自动化测试和 DAW 宿主回归仍待补。
- Content state 只有一套 owner schema、一个 snapshot 投影入口和一套 F0/revision 合同：已完成；旧状态类型、重复共同字段投影和 `payloadFromSnapshot()` 已删除。
- 没有新增锁、worker、缓存、nullable fallback 或第二套状态；RT 路径未改动：已完成。

## 13. 不建议的处理方式

- 不要在旧路径外再加一条“新 API”，以兼容历史调用；确定无消费者的 API 应直接删除。
- 不要用更多 null guard 掩盖 identity TimeGrid 的双表示；先统一语义，再删除守卫。
- 不要把所有重复代码抽成一个拥有所有格式分支的超级 helper；抽象应停留在纯数据/纯数学边界。
- 不要把像素、UI zoom 或 host PPQ 反推为新的绝对时间真相。
- 不要在没有训练侧锚点的情况下擅自改变 F0 半 hop 或 mel/F0 插值公式。
- 不要只做文件拆分而保留相同的跨层依赖；先确定 owner、snapshot、render job 和时间域合同。

## 14. 审查限制与复核入口

本报告基于静态源码、引用搜索、CMake 文本和 Git 历史；本轮使用 VS/CMake/Ninja 完成 VST3/Standalone 主 target 构建，并运行 7 个 opt-in 测试；仍没有连接 DAW/ARA host、运行 GPU/ONNX 推理或验证训练仓库的 FCPE/mel 对齐。因此：

- “孤儿 API”是静态引用意义上的高置信度结论，删除前仍应确认没有外部插件 SDK/脚本 ABI 使用。
- ARA 非恒等 TimeGrid、F0 半 hop、mel/F0 配对属于需要运行时/数据回归确认的高价值风险，不应仅凭视觉结果判断。
- ARA placement projection 与 clone hook 已完成静态/编译验证，但 clone 内容独立性、host placement 更新传播和“不额外通知宿主”仍缺少专属自动化及 DAW 回归。
- Content state 历史、投影漂移和 F0 revision 漂移已由静态源码与 Git 历史确认；统一 `ContentState`、单一投影入口和 F0 一次提交已实现并通过本地构建/测试，但 TimeGrid/DetectedKey 规则、ARA 专属回归和宿主行为仍需继续验证。
- 主题隐藏、coulin9 权重、`ref-branch` 是否删除属于产品/发布决策，不是纯代码审查结论。

## 15. 文件与提交索引

重点源码：

- `CMakeLists.txt:364-424,544-864,1376-1452`
- `Source/PluginProcessor.h/.cpp`
- `Source/Standalone/UI/PianoRollComponent.h/.cpp`
- `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h/.cpp`
- `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp`
- `Source/Standalone/UI/PianoRoll/PianoRollTimeMap.h`
- `Source/Content/ContentPatchGeometry.h`
- `Source/Plugin/StandaloneProcessorStateCodec.h/.cpp`
- `Source/Utils/EditorUiSync.h`
- `Source/ARA/OpenTuneDocumentController.h/.cpp`
- `Source/ARA/OpenTunePlaybackRenderer.h/.cpp`
- `Source/Content/*`
- `Source/Content/ContentState.h`、`AnalysisState.h`、`ContentSnapshotProjection.h/.cpp`、`EditableContentSnapshot.h`
- `Source/Render/*`、`Source/Runtime/*`、`Source/Inference/*`
- `Source/Utils/TimeCoordinate.h`、`TimeGrid.*`、`ContentTimelineProjection.h`、`F0Timeline.h`
- `Tests/StandaloneProcessorStateCodecTests.cpp`

关键历史：

- `0d5117c`：双格式/ARA 基础
- `51301df`、`ecf83de`：Capture 和共享 target format guard 问题
- `6fa421f`、`6f2438e`、`664aa53`：ContentKey/ownership migration
- `f65864a`、`d8d111a`、`d3825d7`、`919d659`：Content state 分叉、ARA PCM 边界与 snapshot 扩展
- `6c6b1c8`、`40bf5fb`、`fcd0910`：PianoRoll cache 范式翻转
- `66b3c08`、`de74b86`：playhead ownership revert/rework
- `439095d`、`ae82cf2`、`a5f5a92`：测试批量删除，当前死 API 集群的主要来源
- `78231ff`、`a703271`：RMVPE/旧推理路径清理及残留
- `870f615`：Standalone/VST3 target 隔离、VST3 state codec、Capture persistence 测试
- `ed9f190`：PianoRoll/Processor 边界收敛；当前审查基线
- `2026-09-20` 工作区修复：ARA PlaybackRegion placement 直读与 AudioModification clone hook
- `9dced28`：`docs/` 被加入 `.gitignore`

### 文档存放说明

当前仓库 `.gitignore:22` 忽略整个 `docs/`。本文件已按用户要求写入版本控制，但不会出现在普通 `git status` 或未加 `-f` 的 Git diff 中；这属于既有仓库规则，本轮未修改 `.gitignore`，业务代码和测试改动仍需单独提交。
