# OpenTune 架构现状

本文只记录当前代码的架构、主要功能逻辑和未解决问题。历史迁移、审查过程和已完成重构不在本文展开。

## 1. 架构现状

### 1.1 构建边界

```text
OpenTune VST3 + ARA
  ├─ PluginProcessor
  ├─ ARA / Capture
  ├─ Content / Render / Runtime / Inference / DSP / Utils
  └─ VST3 wrapper

OpenTuneStandalone
  ├─ PluginProcessor
  ├─ StandaloneContent / SourceStore / Arrangement / Project
  ├─ Content / Render / Runtime / Inference / DSP / Utils
  └─ Standalone wrapper
```

VST3 和 Standalone 使用各自的 SharedCode target。公共源码分别编译到两个产品，ARA/Capture 只进入 VST3 target，Standalone 专属源码只进入 Standalone target。格式差异由编译目标和现有 adapter 表达，不在运行时保留第二套格式路径。

### 1.2 模块职责

| 模块 | 当前职责 |
| --- | --- |
| `PluginProcessor` | JUCE 生命周期、音频处理、格式装配、transport、内容域分发和 mutation 后的 render 调度 |
| `ContentState` | Standalone、Capture、ARA 共用的 owner 数据状态；包含 source window、PCM 引用、分析状态、编辑状态和 revision |
| Content owner | 持有 `ContentState`，执行域内 mutation 和生命周期；持久化由域外 adapter 读写其 content |
| `EditableContentSnapshot` | 跨线程和跨模块的只读内容合同 |
| `ContentSnapshotProjection` | 集中完成 `ContentState` 与 snapshot 的双向纯数据投影 |
| `ContentRenderService` / `RenderWorker` | 按 `ContentKey` 管理 render job、RenderCache 和 playback source；Standalone/Capture 的非恒等 TimeGrid 由 Stage2 使用 `TimeStretchCache` |
| `ProcessF0Runtime` / `ProcessRenderRuntime` | 进程级 F0 和 render 执行资源 |
| `OpenTuneDocumentController` | ARA document-scoped content、宿主通知、ARA mutation、CRS 和 ARA F0 生命周期 |
| Editor / PianoRoll | UI 状态、交互、视图投影和格式特有接线；PianoRoll 不直接依赖 Processor |

`PluginProcessor` 仍是最大的协调点。它保留格式边界、RT 音频处理和跨模块调度，不应再承担新的数据模型或通用 Router 职责。

### 1.3 内容状态

`ContentState` 是三个 content owner 的唯一 owner schema：

- `sourceWindow`、`audioBuffer`、`sampleRate`、`audioRevision`
- `AnalysisState analysis`：pitch curve、Original F0、DetectedKey、silent gaps、reference features、`analysisRevision` 和分析状态
- notes、TimeGrid、pitch shift、volume envelope、note topology
- 分项 revision 和单一 `contentRevision`

ARA 的 `audioBuffer` 保持为空，PCM 由 `AudioSource` 提供。Standalone 与 Capture 的 retire/revive 状态属于各自 owner，不放进共享 `ContentState`。Render cache、worker、stretcher 和 playback publisher 也不属于 `ContentState`。

owner snapshot 统一通过 `makeContentSnapshot()` 生成；split、clone 等需要重建 owner state 的路径使用 `contentStateFromSnapshot()`。空 `timeGrid` 在投影时使用 `TimeGridSnapshot::bootstrapIdentity()`。ARA 只在共同投影后补充缓存的 AudioSource shape 元数据。

### 1.4 线程与所有权

```text
消息线程
  ├─ owner mutation、持久化、ARA SDK 对象、宿主通知
  └─ render/F0 job 调度

音频线程
  ├─ processBlock / ARA renderer
  └─ 只读取 atomic snapshot、prepared PCM 和已发布 cache/source

Render worker
  └─ 消费 render job 和不可变输入，不应直接解引用 ARA DC 对象

F0 / Reference worker
  └─ 异步分析，通过 owner revision 和 completion gate 回写
```

当前设计优先使用不可变 snapshot 和 atomic `shared_ptr`，实时路径不新增锁和动态状态迁移。

## 2. 功能逻辑

### 2.1 Standalone 导入与播放

```text
文件导入
  -> SourceStore 保存源音频和 identity
  -> StandaloneContentRepository 创建 content owner
  -> ContentState 保存 source window、PCM、编辑和分析状态
  -> ContentRenderService 发布 PlaybackReadSource
  -> F0 分析
  -> Stage1 render
  -> ARA 固定 identity TimeGrid，不进入 Stage2
  -> playback snapshot / 音频读取
```

Arrangement 只保存 placement 的几何与混音/引用参数、track 状态和 timeline 几何。placement 通过 `ContentKey` 找到 content snapshot，不复制内容字段。

### 2.2 Regular VST3 Capture

```text
宿主音频 block
  -> CaptureSession / ring buffer
  -> CaptureSegment
  -> CaptureSegmentContent
  -> F0ExtractionService
  -> commitSegmentF0Result
  -> ContentRenderService / RenderWorker
  -> PlaybackReadSource
```

F0 成功提交使用一次性 `applyOriginalF0()`：同时写入 curve、Ready、`analysisRevision`、`pitchRevision` 和 `contentRevision`。render gate 以 `hasUsableOriginalF0()` 为准，不单独相信状态枚举；Capture 直接检查 `PitchCurve`，其他 render 路径检查 snapshot helper。Capture persistence restore 对 Ready 且有曲线的数据复用同一提交路径。

### 2.3 ARA2

```text
ARA host
  -> OpenTuneDocumentController
  -> AudioSource / AudioModification / PlaybackRegion
  -> ContentKey 和 document-scoped ContentState
  -> CRS / PlaybackReadSource
  -> OpenTunePlaybackRenderer
```

ARA SDK 对象和宿主 placement 只在 DC 消息线程访问。`PlaybackRegion` 只保存 host pointer；projection 只保存宿主 placement 属性和 `ContentKey`，不复制 `SourceWindow` 或其他 content state。可渲染 placement 要求 playback duration 与 modification duration 相等。渲染和编辑视图需要 source window 时，从对应 CRS `PlaybackReadSource.contentSnapshot` 读取。

ARA 只接受 identity TimeGrid。`applyTimeGrid()` 对非恒等 TimeGrid 直接返回失败；归档恢复遇到非恒等 TimeGrid 时返回 `nullopt`，不替换为 identity。

AudioModification clone 分为四步：

1. SDK 调用 clone hook，创建新 SDK modification 并按新 host pointer 注册 DTO。
2. clone hook 通过 `makeContentSnapshot()` / `contentStateFromSnapshot()` 复制可持久化 content；已完成的 F0 重建为独立可变 `PitchCurve`，不复制未完成的异步分析状态。
3. hook 返回，不读取 PCM、不启动分析、不通知宿主。
4. SDK 写入新 persistent ID 后回调 property update；DC 在此绑定 ContentKey、attach source、刷新 projection/CRS 并通知宿主。

### 2.4 时间和渲染数据

项目使用多个明确的时间域：

- timeline seconds：宿主或 Standalone 时间轴
- content/source seconds：Note、PitchCurve 和 SourceWindow 使用的内容时间
- output seconds：TimeGrid 变换后的播放输出时间
- sample index：明确 sample rate 下的整数样本
- F0 frame：由 hop size 和 F0 sample rate 定义的帧索引
- pixel：UI 显示坐标

绝对时间是上游事实来源。不同时间域比较前必须经过显式映射；sample/frame/hop 只作为离散访问、上下文覆盖或模型输入坐标，不得反向修改绝对时间、内容边界或持久化数据。UI pixel 不得反推新的绝对时间；音频线程只消费已准备好的整数 sample range。

`TimeCoordinate` 中，`secondsToSamples` 是 point 的向零截断，`secondsToSamplesNearest` 用于模型/窗口中心的最近 sample，`secondsToSamplesFloor` 与 `secondsToSamplesCeil` 成对表示 interval 覆盖，`sampleRateProject` 用于跨采样率的离散边界投影。投影结果不回写秒域真相。

PianoRoll 的 source、output、timeline、pixel 映射集中在 `PianoRollTimeMap`。播放头命中音符时，先按 placement 和 TimeGrid 将 timeline 映射到 source，再与 Note 时间比较。

### 2.5 持久化和测试

Standalone、Capture、ARA 保留各自的 archive/container 格式，但内容字段都恢复到对应 owner 的 `ContentState`。`contentRevision` 和 `audioRevision` 不从归档恢复，新内容身份从 1 开始；ARA 另外保存并恢复 `notesRevision`、`pitchRevision`、`timeGridRevision` 及分析相关 revision，Standalone 也恢复 `referenceFeatures.analysisRevision`。不额外生成运行时 revision 账本。跨采样率 buffer 长度由源 sample 数和源/目标采样率一次投影得到；不得从投影后的 buffer 长度反算源内容秒数。

当前启用测试默认关闭；打开 `OPENTUNE_BUILD_TESTS` 后有 7 个测试目标：`AutoTunePitchShifterTests`、`PitchParameterContractTests`、`PitchLaneVisualPolicyTests`、`Vst3ProcessorStateCodecTests`、`StandaloneProcessorStateCodecTests`、`CapturePersistenceTests` 和 `ContentSnapshotProjectionTests`。最近一次 VS/CMake/Ninja 构建中，VST3/Standalone target 和 7/7 CTest 均通过。

## 3. 已完成的边界收敛

当前代码已具备以下边界：

1. `Stage2TimeStretchRebuilder` 不再构造缺少 snapshot 的临时 `PlaybackReadSource`；canonical Stage1 读取只接收 job 固定的 source PCM 和 `RenderCache`。
2. Stage2 提交前同时校验已发布 snapshot 的 `contentRevision`、`timeGridRevision`、`audioRevision` 和 audio buffer identity；`TimeStretchCache` 仍以 `(contentRevision, timeGridRevision)` 为唯一版本键。
3. 局部 Stage1 编辑会为未受影响的 pending/running chunk 继承新的 `contentRevision`，避免 prepared overlay 跳过该 chunk 后回退到 dry。
4. Standalone、Capture 的非恒等 TimeGrid 经过 canonical-settled gate 进入 Stage2；ARA 只接受 identity TimeGrid，对非恒等 TimeGrid 显式拒绝，且不进入 Stage2。
5. `PluginProcessor` 的 ARA/Capture 分支在最终输出后发布频谱；Standalone placement 的 PianoRoll projection、ARA 非零 source window projection 和 TrimLeft output/source 映射已按各自坐标合同修正。
6. 删除了未消费的 ARA content accessor、cycle-range wrapper、播放采样率 getter 和 renderer CRS setter。worker 不再携带冗余的 `startSeconds`，只使用 sample range。
7. 删除未消费的 `mappingRevision`、`pitchShiftRevision`、`outputGainRevision` 和双账本 analysis lifecycle；VST3 旧状态的字节布局 skip 常量仍保留，仅用于读取已发布的历史布局。
8. `PianoRollComponent` 和 `PianoRollToolHandler` 的 scratch curve mutation 直接返回新的 `shared_ptr<const PitchCurveSnapshot>`；业务层不再通过旧 `getSnapshot()` 或 `clone()` 回读编辑结果。
9. Standalone、Capture 的 Stage1 completion 直接携带当次 snapshot/audio 进入 Stage2 settle gate；ARA 不建立 Stage2 completion chain。
10. `PlaybackRegionProjection` 只保留宿主 placement 属性和 `ContentKey`；ARA renderer 从发布的 `PlaybackReadSource.contentSnapshot` 取得 `SourceWindow` 和 TimeGrid。
11. `ContentSnapshotProjection` 对空 `timeGrid` 使用 bootstrap identity，snapshot 与 `ContentState` 的投影不再要求调用方预先填充 identity grid。
12. 采样率投影、TimeGrid spacing、F0 区间投影和 placement fade 已分别收敛到命名明确的离散坐标或绝对 seconds 路径；这些派生坐标不回写内容时间。

## 4. 剩余问题

### P1：宿主行为和回归覆盖

1. **ARA placement、clone 和多 placement PianoRoll 仍缺 DAW/ARA 宿主回归。** projection 不再携带 content state；仍需宿主确认 placement 更新、clone 独立性、通知时序和视觉行为。
2. **Capture 非恒等 TimeGrid 需要真实宿主回归。** 代码路径已接入单一 Stage2 settle gate，但录制、编辑、播放和宿主 block 边界的时序仍没有 VST3 宿主测试。

### P1：时间域和状态合同

1. **离散投影与绝对时间的边界合同仍需宿主和数据回归。** placement fade、RenderCache 跨率长度、TimeGrid spacing、F0 区间投影已分别使用明确的 seconds/point/interval 语义；剩余问题集中在 split、ARA source window 的 fractional sample 对齐和模型输入边界，不能凭经验改绝对时间公式。
2. **split 仍同时使用 sample 锚和 seconds 锚。** 音频、silent gaps、notes 与 pitch curve 必须继续由同一个绝对切点派生，并补充边界回归。
3. **F0 frame 与模型窗中心的合同未锁定。** FCPE padding、F0Timeline 和 mel/F0 插值需要训练侧定义和回归样本确认。
4. **旧 cache 拒绝和非恒等 TimeGrid 的 cache miss 已有纯逻辑测试。** ARA 非零 source window、局部渲染 revision、导出一致性以及 placement fractional sample 边界目前只有代码合同，尚无对应专门测试；相关 ARA/Capture 行为仍缺宿主级验证。当前启用测试目标为 7 个，CTest 全部通过。
5. **split 的 PitchCurve 仍缺少绝对 frame origin 合同。** 音频和 silent gaps 已按 canonical sample 切分，notes 保持 seconds；但非 F0 frame 边界切分时，当前 PitchCurve 没有保存绝对 frame origin，不能仅靠重复 floor/ceil 同时保证两段 F0 无重叠、无丢失且 frame 时间精确对应。需要先确定 frame origin 或重采样合同，再修改 split 持久化结构。

### P2：维护成本

1. `PluginProcessor` 仍承载较多 domain、render、transport 和 inference 编排，继续拆分前应先确定新的 owner 和不变量，不能只按文件大小抽 helper。
2. F0/Reference 使用 detached worker，RenderWorker 使用可 join 的常驻线程，Vocoder control worker 依附进程寿命单例；各自拥有去重、shutdown 和 completion 机制。业务不能强行合并，但关闭、generation 和 completion gate 应建立共同测试合同。
3. 三种持久化容器仍各自解释 EQ/Note migration；格式边界应保留，纯数据迁移规则可以共享。
4. Beat 计算、MIDI/Hz 与 lane-center 转换、cents math 等仍有重复或同名 API，后续应在纯数学边界单点化，不能把 UI 坐标规则混入音高真相。
5. 测试默认仍为关闭选项；本次验证显式打开 `OPENTUNE_BUILD_TESTS` 后，VST3/Standalone target 和 7/7 CTest 均通过。

### 不属于当前架构问题的事项

- `OPENTUNE_COMMON_SOURCES` 在两个 SharedCode target 各编译一次是两个产品生成独立二进制的正常结果。
- ARA wrapper、document-scoped `AudioModification.content` 和 CRS snapshot 是不同生命周期的合法边界，不应因“存在多个对象”而重新合并。
- 不应恢复运行时 `wrapperType` 双格式分流，也不应新增 ContentDomainRouter、锁、COW 或 CloneManager 来掩盖尚未解决的 owner/时间域问题。
