# Phase 23: Snapshot 模型与 region 真相源收敛 - Research

**研究日期：** 2026-04-16
**阶段范围：** `Phase 23 - Snapshot 模型与 region 真相源收敛`。[VERIFIED: .planning/ROADMAP.md:233][VERIFIED: .planning/REQUIREMENTS.md:62]
**研究结论置信度：** HIGH；核心判断直接来自 live tree 源码、vendored JUCE ARA 源码与当前 phase 文档，而不是历史记忆。[VERIFIED: .planning/PROJECT.md:16][VERIFIED: JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARAModelObjects.h:693][VERIFIED: Source/ARA/OpenTuneDocumentController.h:150]

## Project Constraints (from AGENTS.md)

- 规划前必须把 `.planning/codebase/` 文档当作“记忆提示”，再用 live tree 重新核对路径与符号是否还存在；本研究已按该要求回读 `.planning/codebase/*.md` 并对关键符号做 live grep/read 复核。[VERIFIED: AGENTS.md:4][VERIFIED: .planning/codebase/STRUCTURE.md:34][VERIFIED: .planning/codebase/ARCHITECTURE.md:51][VERIFIED: .planning/codebase/TESTING.md:194]
- 研究与后续规划必须用中文，并且以结构性正确方案为第一优先，禁止止血、最小改动、防御性编程、兜底、并行旧新结构、兼容层。[VERIFIED: AGENTS.md:10][VERIFIED: AGENTS.md:12][VERIFIED: AGENTS.md:14]
- VST3 UI 与 Standalone UI 必须继续隔离；`Source/Plugin/PluginEditor.h/.cpp` 只属于 VST3，`Source/Standalone/PluginEditor.h/.cpp` 只属于 Standalone。[VERIFIED: AGENTS.md:17][VERIFIED: .planning/codebase/ARCHITECTURE.md:11]
- shared processor 继续由 `Source/PluginProcessor.h/.cpp` 统一负责，ARA 适配层问题不得通过扩 core 把复杂度继续扩散到共享层。[VERIFIED: AGENTS.md:18][VERIFIED: .planning/PROJECT.md:70][VERIFIED: .planning/PROJECT.md:76]
- `VST3 References/docs/adr/ADR-0003-ara-playback-controller-interface.md` 被 AGENTS 作为参考文件列出，但当前 live tree 根目录不存在 `VST3 References/` 目录，因此该 ADR 不能作为本 phase 的可读 live evidence；本研究改用 vendored JUCE ARA 源码与 example 校对宿主播放控制与 edit-batch 契约。[VERIFIED: AGENTS.md:24][VERIFIED: read `E:\\TRAE\\OPenTune0427` directory][VERIFIED: glob/read `E:\\TRAE\\OPenTune0427\VST3 References\docs\adr\ADR-0003-ara-playback-controller-interface.md` failed][VERIFIED: JUCE-master/examples/Plugins/ARAPluginDemo.h:2074]

## Phase Requirements

- `SNAP-01` 要求“宿主结束一次 model edit 批次后，读侧只能看到一份自洽的 published snapshot”；这直接要求 Phase 23 先把 publish gate 和 snapshot contract 定型。[VERIFIED: .planning/REQUIREMENTS.md:12][VERIFIED: .planning/ROADMAP.md:239]
- `SNAP-02` 要求“同一 `ARAAudioSource` 下多个 `ARAPlaybackRegion` 必须按 region 维度维护真相”；这直接否定当前 `AudioSource -> single range` 结构。[VERIFIED: .planning/REQUIREMENTS.md:13][VERIFIED: .planning/ROADMAP.md:240]
- `SNAP-03` 要求“audio thread 与 editor thread 只读 immutable snapshot”；这直接否定当前 renderer/editor 通过 getter 直读 controller mutable state 的做法。[VERIFIED: .planning/REQUIREMENTS.md:14][VERIFIED: .planning/ROADMAP.md:241]

## 结论摘要

- Phase 23 真正要改的是 `OpenTuneDocumentController` 的内部状态模型与发布契约，而不是 `OpenTuneAudioProcessor` core；processor 已经具备 `getPluginPlaybackReadSource()` 与 `readPlaybackAudio()` 两个足够的 clip 级读接口，当前错误主要来自 controller 提供给 renderer/editor 的 source-level mapping 与 mutable state API。[VERIFIED: Source/PluginProcessor.h:456][VERIFIED: Source/PluginProcessor.h:627][VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:151][VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:193][VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:209]
- 当前 live tree 的 controller 仍保留 `audioSourceStates_`、`audioSourceRetryCount_`、`audioSourceSyncStates_`、`audioSourceClipBindings_` 与 `currentPlaybackAudioSource_` 五套 mutable 状态，而目标结构 `sources_ + regions_ + preferredRegion_ + publishedSnapshot_` / `loadSnapshot()` 在生产源码中完全不存在。[VERIFIED: Source/ARA/OpenTuneDocumentController.h:150][VERIFIED: Source/ARA/OpenTuneDocumentController.h:159][VERIFIED: .planning/PROJECT.md:16][VERIFIED: .planning/PROJECT.md:66][VERIFIED: grep `loadSnapshot|PublishedSnapshot|preferredRegion|RegionSlot|SourceSlot` over `Source/*.h,*.cpp` returned zero matches]
- 当前 source-level 真相单位在结构上就是错的：`AudioSourceSyncState` 只在 `ARAAudioSource*` 维度保存一组 `playback/source` range，`AudioSourceClipBinding` 也只记录 `clipId + revisions + sourceStart/sourceEnd + playbackStart`，没有任何 region identity，因此同 source 多 region 会互相覆盖。[VERIFIED: Source/ARA/OpenTuneDocumentController.h:80][VERIFIED: Source/ARA/OpenTuneDocumentController.h:141][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:402]
- 当前 audio thread 与 editor timer 都还在直接拼装 mutable state：renderer 在 `processBlock()` 里逐 region 读取 `getAudioSourceState()`、`getPlaybackRangeForAudioSource()`、`getSourceRangeForAudioSource()`，editor 的 `syncImportedAraClipIfNeeded()` 则把 `getCurrentPlaybackAudioSource()`、binding、contentRevision、mappingRevision、state、range getter 串成一整条同步逻辑。[VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:145][VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:162][VERIFIED: Source/Plugin/PluginEditor.cpp:1010][VERIFIED: Source/Plugin/PluginEditor.cpp:1014][VERIFIED: Source/Plugin/PluginEditor.cpp:1019][VERIFIED: Source/Plugin/PluginEditor.cpp:1026][VERIFIED: Source/Plugin/PluginEditor.cpp:1034]
- Phase 23 应该先把“单一 mutable model + immutable published snapshot + region-keyed truth + batch-end publish gate”一次定型；sample access lifecycle、content dirty/rebuild、region/source removal、renderer/editor 直接 snapshot 消费、以及最终 grep-zero cleanup 都应留给后续 phase，不要在本 phase 里抢跑混做。[VERIFIED: .planning/PROJECT.md:16][VERIFIED: .planning/PROJECT.md:17][VERIFIED: .planning/PROJECT.md:18][VERIFIED: .planning/REQUIREMENTS.md:18][VERIFIED: .planning/REQUIREMENTS.md:25][VERIFIED: .planning/REQUIREMENTS.md:30][VERIFIED: .planning/ROADMAP.md:78][VERIFIED: .planning/ROADMAP.md:79][VERIFIED: .planning/ROADMAP.md:80][VERIFIED: .planning/ROADMAP.md:81]

## live tree 证据

### 1. 当前 controller 仍是多张 source 侧半状态表

- `OpenTuneDocumentController.h` 仍直接持有 `audioSourceStates_`、`audioSourceRetryCount_`、`audioSourceSyncStates_`、`audioSourceClipBindings_`、`nextAudioSourceContentRevision_`、`nextAudioSourceMappingRevision_` 与 `currentPlaybackAudioSource_`，说明 live tree 还没有进入单模型/单快照结构。[VERIFIED: Source/ARA/OpenTuneDocumentController.h:150][VERIFIED: Source/ARA/OpenTuneDocumentController.h:159]
- `getAudioSourceState()` 直接返回可变 `AudioSourceState*`，而 `AudioSourceState` 内部持有的是 `std::shared_ptr<juce::AudioBuffer<float>>` 可变 buffer；这意味着读侧 API 现在天然允许直接拿到 mutable carrier，而不是 immutable view。[VERIFIED: Source/ARA/OpenTuneDocumentController.h:37][VERIFIED: Source/ARA/AudioSourceState.h:9][VERIFIED: Source/ARA/AudioSourceState.h:12]
- `AudioSourceSyncState` 只保存一组 `playbackStartSeconds/playbackEndSeconds/sourceStartSeconds/sourceEndSeconds`，没有 region identity、没有 region 数组，也没有 preferred region 独立字段。[VERIFIED: Source/ARA/OpenTuneDocumentController.h:141]

### 2. source-level mapping 是当前最直接的结构残留

- `didUpdatePlaybackRegionProperties()` 与 `didAddPlaybackRegionToAudioModification()` 都先把 `currentPlaybackAudioSource_` 设成当前 region 对应的 source，再调用 `updatePlaybackRangeFromRegion(audioSource, playbackRegion)` 覆盖写入 `audioSourceSyncStates_[audioSource]`；这意味着映射真相仍是“每个 source 只保留最后一次覆盖进来的 region range”。[VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:31][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:55][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:70][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:88][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:402]
- `updatePlaybackRangeFromRegion()` 明确把单个 `playbackRegion` 的 `start/end in playback time` 与 `start/end in audio modification time` 写进 source-keyed `AudioSourceSyncState`，没有任何 region 容器或聚合逻辑。[VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:408][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:418]
- JUCE 官方 model object 已提供 `ARAAudioModification::getPlaybackRegions()` 用于枚举一个 modification 下的多个 region，但当前 `Source/ARA/*.h/.cpp` 唯一的 `getPlaybackRegions()` 使用点在 `OpenTunePlaybackRenderer.cpp` 的 renderer 读循环，controller 自己完全没有按 modification/region 维护 state。[CITED: JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARAModelObjects.h:950][VERIFIED: grep `getPlaybackRegions\(` over `Source/ARA/*.h,*.cpp` only found `Source/ARA/OpenTunePlaybackRenderer.cpp:95`]

### 3. 读侧仍通过多 getter 拼装真相

- `OpenTunePlaybackRenderer::processBlock()` 先遍历 host 给出的 `getPlaybackRegions()`，再针对每个 region 依次读取 `getAudioSourceState()`、`getAudioSourceClipBinding()`、`getPlaybackRangeForAudioSource()`、`getSourceRangeForAudioSource()`，说明 audio thread 现在看到的是多次 getter 拼接出来的 mixed state，而不是单次 snapshot 读取。[VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:95][VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:145][VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:151][VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:162]
- `OpenTuneAudioProcessorEditor::syncImportedAraClipIfNeeded()` 每次 timer tick 都依赖 `getCurrentPlaybackAudioSource()` → `getAudioSourceClipBinding()` → `getAudioSourceContentRevision()` / `getAudioSourceMappingRevision()` → `getAudioSourceState()` → `getPlaybackRangeForAudioSource()` / `getSourceRangeForAudioSource()` 的多 getter 链来判断是否同步 clip，这正是“多 getter 拼装真相”的 live 证据。[VERIFIED: Source/Plugin/PluginEditor.cpp:330][VERIFIED: Source/Plugin/PluginEditor.cpp:426][VERIFIED: Source/Plugin/PluginEditor.cpp:1010][VERIFIED: Source/Plugin/PluginEditor.cpp:1014][VERIFIED: Source/Plugin/PluginEditor.cpp:1019][VERIFIED: Source/Plugin/PluginEditor.cpp:1026][VERIFIED: Source/Plugin/PluginEditor.cpp:1034]
- `recordRequested()` 也依赖同一组 getter 读取“当前 ARA region”，并且首选对象依然是 `currentPlaybackAudioSource_` 而不是 region-level preferred view；这说明当前 editor 读约定还没有进入 region snapshot 时代。[VERIFIED: Source/Plugin/PluginEditor.cpp:833][VERIFIED: Source/Plugin/PluginEditor.cpp:849][VERIFIED: Source/Plugin/PluginEditor.cpp:864][VERIFIED: Source/Plugin/PluginEditor.cpp:876]

### 4. 线程边界当前没有被 state contract 明确守住

- `OpenTuneDocumentController.h` 中上述多张 `std::map` 没有任何 `mutex`、`SpinLock` 或等价同步成员，而 `OpenTunePlaybackRenderer::processBlock()` 会在音频回调里持续读取这些 getter；这说明 controller 当前没有用结构手段把 mutable side 与 audio thread read side 隔离开。[VERIFIED: Source/ARA/OpenTuneDocumentController.h:150][VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:89][VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:145]
- `didUpdateAudioSourceProperties()` 仍会立即调用 `tryReadAudioData()`，而 `tryReadAudioData()` 会在 sample access 未启用时用 `juce::Timer::callAfterDelay` 做最多 10 次 retry；这不是 Phase 23 应该继续扩写的逻辑，而是 Phase 24 要替换掉的错误生命周期残留。[VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:105][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:129][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:143][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:172][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:181]
- JUCE ARA listener接口明确提供 `doUpdateAudioSourceContent()`、`willEnableAudioSourceSamplesAccess()`、`didEnableAudioSourceSamplesAccess()` 与 `willRemovePlaybackRegionFromAudioModification()`，而当前 `Source/ARA/OpenTuneDocumentController.h/.cpp` 对这些符号是零实现；这说明 lifecycle 收敛还没有开始，必须与 Phase 23 的结构重写严格分开。[CITED: JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARAModelObjects.h:715][CITED: JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARAModelObjects.h:730][CITED: JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARAModelObjects.h:914][VERIFIED: grep `didEndEditing|doUpdateAudioSourceContent|willEnableAudioSourceSamplesAccess|didEnableAudioSourceSamplesAccess|willRemovePlaybackRegionFromAudioModification` over `Source/ARA/*.h,*.cpp` returned zero matches]

### 5. 与文档/记忆的偏差

- `ROADMAP.md`、`PROJECT.md` 与 `REQUIREMENTS.md` 对 Phase 23 的方向性要求与 live tree 问题是对齐的；本研究没有发现这三份 phase 文档与当前源码的目标冲突。[VERIFIED: .planning/PROJECT.md:16][VERIFIED: .planning/REQUIREMENTS.md:12][VERIFIED: .planning/ROADMAP.md:233][VERIFIED: Source/ARA/OpenTuneDocumentController.h:150]
- 偏差主要来自历史参考资产：`AGENTS.md` 指向的 `VST3 References/.../ADR-0003...` 在当前 live tree 不存在，而 `.planning/codebase/CONCERNS.md` 仍提到 `AudioSourceState::renderCache`，但 live tree 的 `AudioSourceState` 只剩 `name + audioBuffer + sampleRate + channelCount + sampleCount`；因此后续 planner 必须继续以 live tree 为准，而不能把记忆文档当作结构事实源。[VERIFIED: AGENTS.md:24][VERIFIED: read `E:\\TRAE\\OPenTune0427` directory][VERIFIED: .planning/codebase/CONCERNS.md:47][VERIFIED: Source/ARA/AudioSourceState.h:9]

## 结构建议

### 1. 单一 mutable model

- `OpenTuneDocumentController` 应按项目既定目标收敛为 `sources_ + regions_ + preferredRegion_ + publishedSnapshot_`，其中 source slot 只保留 source metadata、content revision、binding 与 copied audio，region slot 只保留 region-level mapping 与 mapping revision，不再让 source slot 挂一份“当前播放区间”。[VERIFIED: .planning/PROJECT.md:16][VERIFIED: .planning/PROJECT.md:66][VERIFIED: .planning/ROADMAP.md:75]
- `preferredRegion_` 才是未来 editor/import 侧的唯一“当前 region”真相源；当前 `currentPlaybackAudioSource_` 只能表达“最后一个触发 callback 的 source”，表达不了“同 source 下 preferred region 切换”。[VERIFIED: .planning/PROJECT.md:66][VERIFIED: Source/ARA/OpenTuneDocumentController.h:159][VERIFIED: Source/Plugin/PluginEditor.cpp:1010]
- region truth 的身份单位必须是 playback region，而不是 `ARAAudioSource*`；否则 `SNAP-02` 无法成立，`CONS-04` 里的 `appliedRegion` 也没有可落地的 identity 基础。[VERIFIED: .planning/REQUIREMENTS.md:13][VERIFIED: .planning/ROADMAP.md:255][VERIFIED: .planning/ROADMAP.md:262]

### 2. immutable published snapshot

- 读侧 contract 必须只剩 `loadSnapshot()`；发布后的 snapshot 需要把 source copied audio 与 region mapping 投影成 region-level published views，避免 renderer/editor 再回头拼 `state + binding + range + currentSource`。[VERIFIED: .planning/PROJECT.md:16][VERIFIED: .planning/PROJECT.md:66][VERIFIED: Source/Plugin/PluginEditor.cpp:1014][VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:145]
- 为满足 `SNAP-01`，snapshot rebuild / publish gate 应放在 ARA edit batch 结束点，而不是散落在每个 getter 或每个读侧调用点里；JUCE 暴露了 `ARADocumentListener::didEndEditing()`，官方 example 也在 `didEndEditing()` 里统一 `rebuildRegionSequenceViews()`，这与当前 phase 的 publish 目标一致。[VERIFIED: .planning/REQUIREMENTS.md:12][CITED: JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARAModelObjects.h:201][CITED: JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARADocumentController.cpp:370][CITED: JUCE-master/examples/Plugins/ARAPluginDemo.h:2074]
- snapshot 发布只解决“读什么、何时对外可见”；它不要求本 phase 立即重做 sample access copy worker，因此 Phase 23 应先把 publish primitive 定型，再让 Phase 24 把 content dirty 与 sample access 生命周期接进同一 publish gate。[VERIFIED: .planning/REQUIREMENTS.md:18][VERIFIED: .planning/REQUIREMENTS.md:20][VERIFIED: .planning/ROADMAP.md:79]

### 3. audio thread / editor read-side contract

- audio thread 的目标 contract 是“每个 block 只拿一次 immutable snapshot，并且整个 block 都不再接触 controller mutable model”；虽然 renderer 直接改读 snapshot 被 roadmap 放在 Phase 25，但 Phase 23 必须先把这个 contract 作为 controller 对外唯一正确接口立起来。[VERIFIED: .planning/REQUIREMENTS.md:14][VERIFIED: .planning/ROADMAP.md:255][VERIFIED: .planning/ROADMAP.md:260]
- editor 的目标 contract 是“timer 只根据 snapshot / epoch / preferred region truth 判断是否同步”；虽然 `appliedRegion` 落地属于 Phase 25，但 Phase 23 的 snapshot 设计现在就必须保留明确 region identity，否则后面没有地方承接 `appliedRegion`。[VERIFIED: .planning/PROJECT.md:47][VERIFIED: .planning/ROADMAP.md:256][VERIFIED: .planning/ROADMAP.md:262]

### 4. shared processor 边界

- 本 phase 不应向 `OpenTuneAudioProcessor` 增加新的 ARA 专属 playback/read/render API；当前 processor 已经能从 `clipId` 解析 `PlaybackReadSource`，并用统一 `readPlaybackAudio()` 完成 resampled/rendered/dry/blank 四级读取，controller snapshot 只需要继续产出稳定 `clipId` 与 region mapping 即可。[VERIFIED: .planning/PROJECT.md:70][VERIFIED: .planning/PROJECT.md:76][VERIFIED: Source/PluginProcessor.h:456][VERIFIED: Source/PluginProcessor.h:627][VERIFIED: Source/PluginProcessor.cpp:1485][VERIFIED: Source/PluginProcessor.cpp:4085]
- 这也意味着 Phase 23 的错误根源在 adapter state contract，而不是 core playback engine 缺接口；planner 不应把问题错误转译成“再给 processor 增加一套 region reader”。[VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:193][VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:209][VERIFIED: .planning/PROJECT.md:53]

## Phase 边界

### Phase 23 只做什么

- 只做 controller 内部状态模型重写：把现有 source-keyed 半状态表压成单一 mutable model，并定义 immutable published snapshot / `loadSnapshot()` 读约定。[VERIFIED: .planning/PROJECT.md:16][VERIFIED: .planning/ROADMAP.md:78]
- 只做 region-level truth 定稿：让一个 source 下多个 region 可以同时在 mutable model 与 published snapshot 中并存，而不是继续覆写单个 source range。[VERIFIED: .planning/REQUIREMENTS.md:13][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:402]
- 只做 publish gate 定型：把 edit-batch 结束后的统一发布位置确定下来，并让后续 phase 复用这一发布机制，而不是先在各 getter 或 timer 中补丁式拼状态。[VERIFIED: .planning/REQUIREMENTS.md:12][CITED: JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARADocumentController.cpp:370]

### Phase 24 再做什么

- `didUpdateAudioSourceProperties()` 里的 eager read、`tryReadAudioData()`、`audioSourceRetryCount_` 与 `Timer::callAfterDelay` retry 全部属于 sample access lifecycle 错误残留，必须留给 Phase 24 统一替换为 `doUpdateAudioSourceContent()` / `will/didEnableAudioSourceSamplesAccess()` 驱动的内容生命周期。[VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:105][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:129][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:172][CITED: JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARAModelObjects.h:715][CITED: JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARAModelObjects.h:730][VERIFIED: .planning/ROADMAP.md:244]
- region/source removal 的 stale 清理也应留给 Phase 24；当前 `willDestroyPlaybackRegion()` 仍是空实现，而 `willRemovePlaybackRegionFromAudioModification()` 完全不存在，本 phase 不要顺手把 removal 语义一并做完然后误报 milestone 完成。[VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:64][VERIFIED: grep `willRemovePlaybackRegionFromAudioModification` over `Source/ARA/*.h,*.cpp` returned zero matches][VERIFIED: .planning/REQUIREMENTS.md:21]

### Phase 25 再做什么

- renderer 每个 block 只读一次 snapshot、editor timer 只按 snapshot epoch 与 preferred region view 决定同步，这些都属于 Phase 25 的读侧消费收敛，不应在 Phase 23 中把 `OpenTunePlaybackRenderer.cpp` 与 `PluginEditor.cpp` 全量重写成最终形态。[VERIFIED: .planning/ROADMAP.md:255][VERIFIED: .planning/ROADMAP.md:260][VERIFIED: .planning/ROADMAP.md:261]
- `appliedRegion` 的行为闭环也属于 Phase 25；Phase 23 只需要保证 snapshot 具备明确 region identity，不能在本 phase 抢跑写 editor `appliedRegion` 判定逻辑。[VERIFIED: .planning/PROJECT.md:19][VERIFIED: .planning/ROADMAP.md:256][VERIFIED: .planning/ROADMAP.md:262]

### Phase 26 再做什么

- 旧 getter、retry、binding/sync 半状态的最终删除、grep-zero 守护与 brownfield lock-down 都属于 Phase 26；Phase 23 不要把时间花在全仓 cleanup，而要把预算集中到 controller 结构定型上。[VERIFIED: .planning/REQUIREMENTS.md:32][VERIFIED: .planning/ROADMAP.md:267]
- 如果 Phase 23 为了保持中间态可编译而保留了少量旧函数名，这些符号在 Phase 26 之前都只能是 snapshot projection 的外皮，不得再拥有独立状态；最终 grep-zero 仍以 Phase 26 为准。[VERIFIED: .planning/ROADMAP.md:75][VERIFIED: .planning/ROADMAP.md:272]

## 给 planner 的拆分建议

### Plan 23-01：冻结 controller contract 与 snapshot 类型边界

- 目标：在 `OpenTuneDocumentController.h` 中把 public/ private contract 改到 phase 目标结构，明确 `sources_ + regions_ + preferredRegion_ + publishedSnapshot_` 与 `loadSnapshot()`，同时让任何对外读接口都不再暴露 `AudioSourceState*` 这类 mutable carrier。[VERIFIED: .planning/PROJECT.md:16][VERIFIED: Source/ARA/OpenTuneDocumentController.h:37][VERIFIED: Source/ARA/OpenTuneDocumentController.h:150]
- 涉及文件：`Source/ARA/OpenTuneDocumentController.h` 为主；若类型定义与辅助 builder 放在 `.cpp`，则同步改 `Source/ARA/OpenTuneDocumentController.cpp`。[VERIFIED: .planning/PROJECT.md:70]
- 验证点：phase 结束后，header 中不再存在 `audioSourceStates_`、`audioSourceSyncStates_`、`currentPlaybackAudioSource_` 这类旧 truth carrier，并且源码中能 grep 到新的 snapshot contract 符号。[VERIFIED: Source/ARA/OpenTuneDocumentController.h:150][VERIFIED: Source/ARA/OpenTuneDocumentController.h:159]
- 主要风险：把 region mapping 继续挂回 source slot，或把 `preferredRegion_` 偷换成“最后一个 source 指针”，会直接把 `SNAP-02` 再次做成假收敛。[VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:55][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:88]
- 必须避免：不能为了保守编译而继续让 header 暴露 `AudioSourceState*` / source-level range getter 作为长期 contract，也不能把 `OpenTuneAudioProcessor` 扩成第二套 ARA state hub。[VERIFIED: .planning/PROJECT.md:53][VERIFIED: Source/ARA/OpenTuneDocumentController.h:37][VERIFIED: Source/PluginProcessor.h:585]

### Plan 23-02：重写 mutable model reducer 与 batch-end publish 路径

- 目标：把当前 `didUpdatePlaybackRegionProperties()` / `didAddPlaybackRegionToAudioModification()` / `didUpdateAudioSourceProperties()` 等现有回调改成统一写入新 mutable model，并在 edit-batch 结束点一次性生成/替换 published snapshot。[VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:31][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:70][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:105][CITED: JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARADocumentController.cpp:370]
- 涉及文件：`Source/ARA/OpenTuneDocumentController.cpp` 为主；如果 publish API 改到 header，可同步动 `Source/ARA/OpenTuneDocumentController.h`。[VERIFIED: .planning/PROJECT.md:70]
- 验证点：同一 source 下两个 region 在 model 中都保留下来；切换 preferred region 时 published snapshot 的 region identity 会变化；未发生 edit-batch end 时读侧不会看到半更新状态。[VERIFIED: .planning/REQUIREMENTS.md:12][VERIFIED: .planning/REQUIREMENTS.md:13][VERIFIED: .planning/PROJECT.md:66]
- 主要风险：用“改旧 map 的 key/value 结构”代替真正的 reducer 重写，或在 publish 后继续原地改 snapshot 内容，都会把本 phase 变成伪快照。[VERIFIED: Source/ARA/OpenTuneDocumentController.h:141][VERIFIED: .planning/PROJECT.md:66]
- 必须避免：不要在这个 plan 里接管 `will/didEnableAudioSourceSamplesAccess()`、`doUpdateAudioSourceContent()`、`willRemovePlaybackRegionFromAudioModification()`；这些 callback 虽然必须存在于终局架构，但它们在本 milestone 的 phase 划分里属于 Phase 24。[VERIFIED: .planning/ROADMAP.md:244][VERIFIED: .planning/ROADMAP.md:247][VERIFIED: grep `doUpdateAudioSourceContent|willEnableAudioSourceSamplesAccess|didEnableAudioSourceSamplesAccess|willRemovePlaybackRegionFromAudioModification` over `Source/ARA/*.h,*.cpp` returned zero matches]

### Plan 23-03：补 Phase 23 结构回归守护，必要时做最小 compile-follow 改动

- 目标：把 Phase 23 需要的结构回归测试补进 `Tests/TestMain.cpp`，优先覆盖“同 source 多 region”“preferred region 切换”“published snapshot 一次发布”的 reducer / helper 层；如果 controller public API 调整导致 `OpenTunePlaybackRenderer.cpp` 或 `PluginEditor.cpp` 编译失败，只允许做跟随式签名改动，不允许在本 plan 抢跑 Phase 25 的最终读侧逻辑。[VERIFIED: Tests/TestMain.cpp:2306][VERIFIED: Tests/TestMain.cpp:2801][VERIFIED: .planning/ROADMAP.md:255]
- 涉及文件：`Tests/TestMain.cpp` 必改；`Source/ARA/OpenTunePlaybackRenderer.cpp`、`Source/Plugin/PluginEditor.cpp` 仅在 public API 迁移所迫时跟随编译调整。[VERIFIED: .planning/PROJECT.md:70][VERIFIED: Source/ARA/OpenTunePlaybackRenderer.cpp:145][VERIFIED: Source/Plugin/PluginEditor.cpp:1010]
- 验证点：当前 test harness 唯一入口仍是 `Tests/TestMain.cpp`，因此新测试应能通过 `OpenTuneTests` 被编译执行；同时要证明旧 source-level 覆写逻辑不会把两个 region 压成一个 truth。[VERIFIED: .planning/codebase/TESTING.md:8][VERIFIED: .planning/codebase/TESTING.md:17][VERIFIED: .planning/codebase/TESTING.md:18]
- 主要风险：现有测试几乎没有 controller/renderer/editor 覆盖，grep 只找到一个 `AudioSourceClipBinding` 结构体字面量，因此如果不先抽 reducer/helper，planner 会很容易把验证退回纯手工说明。[VERIFIED: grep `AudioSourceState|getAudioSourceState|getPlaybackRangeForAudioSource|getSourceRangeForAudioSource|getCurrentPlaybackAudioSource|registerAudioSourceClipBinding|updateAudioSourceBindingRevisions|clearAudioSourceClipBinding|AudioSourceClipBinding` over `Tests/*.cpp` only found `Tests/TestMain.cpp:2801`]
- 必须避免：不要在这个 plan 把 `kHeartbeatHz` timer 改成最终 epoch consumer，也不要把 retry/sample-access cleanup 一起塞进来；这两个方向分别属于 Phase 25 与 Phase 24。[VERIFIED: Source/Plugin/PluginEditor.h:157][VERIFIED: Source/Plugin/PluginEditor.cpp:294][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:181][VERIFIED: .planning/ROADMAP.md:244][VERIFIED: .planning/ROADMAP.md:255]

## 验证关注点

- 当前自动化测试对 ARA controller / renderer / VST3 editor 几乎没有直接覆盖；现有 grep 只在 `Tests/TestMain.cpp:2801` 看到一个 `AudioSourceClipBinding` 字面量，因此 Phase 23 如果不新增针对 snapshot reducer 的测试，planner 将没有可信的自动化闭环。[VERIFIED: grep `AudioSourceState|getAudioSourceState|getPlaybackRangeForAudioSource|getSourceRangeForAudioSource|getCurrentPlaybackAudioSource|registerAudioSourceClipBinding|updateAudioSourceBindingRevisions|clearAudioSourceClipBinding|AudioSourceClipBinding` over `Tests/*.cpp` only found `Tests/TestMain.cpp:2801`]
- 当前仓库已有的可执行测试命令仍是 `cmake --build build --target OpenTuneTests` 与 `ctest --test-dir build --output-on-failure`；Phase 23 的新增守护应沿用这条现有验证链，而不是另起一套阶段性脚本。[VERIFIED: .planning/codebase/TESTING.md:17][VERIFIED: .planning/codebase/TESTING.md:18]
- 最关键的 phase-local 断言应该是“同一 source 的两个 region 同时存在时，published snapshot 里必须出现两个独立 region truth”，因为这正是当前 source-level 覆写模型最容易被误保留下来的地方。[VERIFIED: .planning/REQUIREMENTS.md:13][VERIFIED: Source/ARA/OpenTuneDocumentController.cpp:402]
- 第二个关键断言应该是“preferred region 切换即使不改 content，也会改变 published snapshot 中的 region identity/selection truth”；如果这里只靠 `mappingRevision` 数字比较，后续 Phase 25 的 `appliedRegion` 很容易再次失效。[VERIFIED: .planning/ROADMAP.md:262][VERIFIED: .planning/PROJECT.md:66]
- 第三个关键断言应该是“controller 对外不再暴露 mutable map carrier”；至少要守住 `AudioSourceState*` 和 `currentPlaybackAudioSource_` 不再是 read-side contract 的组成部分。[VERIFIED: Source/ARA/OpenTuneDocumentController.h:37][VERIFIED: Source/ARA/OpenTuneDocumentController.h:159]
- `.planning/config.json` 已把 `workflow.nyquist_validation` 设为 `false`，因此本研究不额外生成 Nyquist 风格的 Validation Architecture；Phase 23 直接依赖现有 `OpenTuneTests` + focused reducer guards 即可。[VERIFIED: .planning/config.json:40]

## 资料来源

- live tree phase 文档：`.planning/PROJECT.md`、`.planning/REQUIREMENTS.md`、`.planning/ROADMAP.md`、`.planning/STATE.md`。[VERIFIED: read corresponding files]
- live tree 生产源码：`Source/ARA/OpenTuneDocumentController.h/.cpp`、`Source/ARA/OpenTunePlaybackRenderer.h/.cpp`、`Source/Plugin/PluginEditor.h/.cpp`、`Source/PluginProcessor.h/.cpp`、`Source/ARA/AudioSourceState.h`。[VERIFIED: read corresponding files]
- live tree 测试与记忆：`Tests/TestMain.cpp`、`.planning/codebase/STRUCTURE.md`、`.planning/codebase/ARCHITECTURE.md`、`.planning/codebase/TESTING.md`、`.planning/codebase/CONCERNS.md`、`.planning/codebase/CONVENTIONS.md`、`.planning/codebase/INTEGRATIONS.md`、`.planning/codebase/STACK.md`。[VERIFIED: read corresponding files]
- vendored JUCE ARA 契约证据：`JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARAModelObjects.h`、`JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARADocumentController.cpp`、`JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARAPlugInInstanceRoles.h`、`JUCE-master/modules/juce_audio_formats/format/juce_ARAAudioReaders.h/.cpp`、`JUCE-master/examples/Plugins/ARAPluginDemo.h`。[VERIFIED: read corresponding files]
