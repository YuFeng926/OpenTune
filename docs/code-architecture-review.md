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
| Content owner | 持有 `ContentState`，执行域内 mutation、生命周期和持久化适配 |
| `EditableContentSnapshot` | 跨线程和跨模块的只读内容合同 |
| `ContentSnapshotProjection` | 集中完成 `ContentState` 与 snapshot 的双向纯数据投影 |
| `ContentRenderService` / `RenderWorker` | 按 `ContentKey` 管理 render job、RenderCache、TimeStretchCache 和 playback source |
| `ProcessF0Runtime` / `ProcessRenderRuntime` | 进程级 F0 和 render 执行资源 |
| `OpenTuneDocumentController` | ARA document-scoped content、宿主通知、ARA mutation、CRS 和 ARA F0 生命周期 |
| Editor / PianoRoll | UI 状态、交互、视图投影和格式特有接线；PianoRoll 不直接依赖 Processor |

`PluginProcessor` 仍是最大的协调点。它保留格式边界、RT 音频处理和跨模块调度，不应再承担新的数据模型或通用 Router 职责。

### 1.3 内容状态

`ContentState` 是三个 content owner 的唯一 owner schema：

- `sourceWindow`、`audioBuffer`、`sampleRate`、`audioRevision`
- `AnalysisState analysis`：pitch curve、Original F0、DetectedKey、silent gaps、reference features 和分析生命周期
- notes、TimeGrid、pitch shift、volume envelope、note topology
- 分项 revision 和单一 `contentRevision`

ARA 的 `audioBuffer` 保持为空，PCM 由 `AudioSource` 提供。Standalone 的 retire/revive 状态属于 Standalone owner，不放进共享 `ContentState`。Render cache、worker、stretcher 和 playback publisher 也不属于 `ContentState`。

owner snapshot 统一通过 `makeContentSnapshot()` 生成；split、clone 等需要重建 owner state 的路径使用 `contentStateFromSnapshot()`。ARA 只在共同投影后补充缓存的 AudioSource shape 元数据。

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
  -> 非 identity TimeGrid 时进入 Stage2
  -> playback snapshot / 音频读取
```

Arrangement 只保存 placement、track 和 timeline 几何。placement 通过 `ContentKey` 找到 content snapshot，不复制内容字段。

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

F0 成功提交使用一次性 `applyOriginalF0()`：同时写入 curve、Ready、`f0Lifecycle`、`analysisRevision`、`pitchRevision` 和 `contentRevision`。render gate 使用 `PitchCurve::hasUsableOriginalF0()`，不单独相信状态枚举。Capture persistence restore 对 Ready 且有曲线的数据复用同一提交路径。

### 2.3 ARA2

```text
ARA host
  -> OpenTuneDocumentController
  -> AudioSource / AudioModification / PlaybackRegion
  -> ContentKey 和 document-scoped ContentState
  -> CRS / PlaybackReadSource
  -> OpenTunePlaybackRenderer
```

ARA SDK 对象和宿主 placement 只在 DC 消息线程访问。`PlaybackRegion` 保存必要的 host pointer；projection 构建时直接读取 SDK 当前 placement，不在 wrapper 中保存 placement 副本。

AudioModification clone 分为四步：

1. SDK 调用 clone hook，创建新 SDK modification 并按新 host pointer 注册 DTO。
2. clone hook 复制可持久化 content；已完成的 F0 深拷贝 PitchCurve，不复制未完成的异步分析状态。
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

绝对时间是上游事实来源。不同时间域比较前必须经过显式映射；UI pixel 不得反推新的绝对时间；音频线程只消费已准备好的整数 sample range。

PianoRoll 的 source、output、timeline、pixel 映射集中在 `PianoRollTimeMap`。播放头命中音符时，先按 placement 和 TimeGrid 将 timeline 映射到 source，再与 Note 时间比较。

### 2.5 持久化和测试

Standalone、Capture、ARA 保留各自的 archive/container 格式，但内容字段都恢复到对应 owner 的 `ContentState`。旧归档中的 ARA 双 revision 字段只作为迁移输入，不生成第二套运行时账本。

当前启用测试默认关闭；打开 `OPENTUNE_BUILD_TESTS` 后有 7 个测试目标，覆盖状态 codec、Capture persistence、Content snapshot 和 F0 revision。最近一次 VS/CMake/Ninja 构建中，VST3/Standalone target 和 7/7 CTest 均通过。

## 3. 剩余问题

### P0：正确性和线程边界

1. **ARA 非恒等 TimeGrid playback 链仍缺宿主回归。** 需要确认 playback seconds 到 source/content、再到 output seconds 和 prepared sample 的完整映射。恒等 TimeGrid 不能覆盖这个问题。
2. **ARA DC 与 RenderWorker 的交接仍有竞争风险。** 当前 render 路径可能通过 `findAudioModificationByContentKey()` 和 `snapshotAudioModification()` 直接读取 DC 的 modification 容器和 content。应交接 immutable snapshot/revision，不应靠加锁或兼容路径掩盖边界。
3. **ARA placement、clone 和多 placement PianoRoll 仍缺 DAW/ARA 宿主回归。** 代码已完成静态和编译验证，但 host placement 更新、clone 独立性、通知时序和视觉行为尚未由宿主确认。

### P1：时间域和状态合同

1. **秒到 sample/frame 的量化规则分散。** split、placement fade、ARA render range、TimeGrid 和 RenderCache 使用了不同的 trunc/round/floor/ceil 组合，可能产生边界偏差。需要逐项确定 point 与 interval 的取整合同，不能凭经验改绝对时间公式。
2. **split 仍同时使用 sample 锚和 seconds 锚。** 音频、silent gaps 与 notes、pitch curve 分别使用两类锚，需保证它们由同一个切点派生。
3. **F0 frame 与模型窗中心的合同未锁定。** FCPE padding、F0Timeline 和 mel/F0 插值可能存在半 hop 偏移，需要训练侧定义和回归样本确认。
4. **部分 mutation revision 规则仍未完全统一。** TimeGrid、DetectedKey、Reference features 保留历史语义，后续修改必须分别确认是否推进总 revision，不能继续隐式扩展。
5. **identity TimeGrid 有两种表示。** owner snapshot 使用非空 identity object，PlaybackReadSource 又用 `nullptr` 表示 identity sentinel。该映射目前合法但容易诱发空指针守卫和误判，需要收敛语义。

### P2：维护成本

1. `PluginProcessor` 仍承载较多 domain、render、transport 和 inference 编排，继续拆分前应先确定新的 owner 和不变量，不能只按文件大小抽 helper。
2. F0、Reference、Render/Vocoder 分别拥有 detached worker、去重、shutdown 和 completion 机制。业务不能强行合并，但关闭、generation 和 completion gate 应建立共同测试合同。
3. 三种持久化容器仍各自解释 EQ/Note migration；格式边界应保留，纯数据迁移规则可以共享。
4. Beat 计算、MIDI/Hz 与 lane-center 转换、cents math 等仍有重复或同名 API，后续应在纯数学边界单点化，不能把 UI 坐标规则混入音高真相。
5. 测试仍默认关闭，缺少时间域、ARA renderer/model contract、split geometry、pending seek 和宿主级回归测试。

### 不属于当前架构问题的事项

- `OPENTUNE_COMMON_SOURCES` 在两个 SharedCode target 各编译一次是两个产品生成独立二进制的正常结果。
- ARA wrapper、document-scoped `AudioModification.content` 和 CRS snapshot 是不同生命周期的合法边界，不应因“存在多个对象”而重新合并。
- 不应恢复运行时 `wrapperType` 双格式分流，也不应新增 ContentDomainRouter、锁、COW 或 CloneManager 来掩盖尚未解决的 owner/时间域问题。
