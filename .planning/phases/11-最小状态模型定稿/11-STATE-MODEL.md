# Phase 11 State Model Freeze

Date: 2026-04-12
Phase: 11-最小状态模型定稿
Scope: 只冻结最小状态模型与读取边界；不改写旧 `RenderCache / pending / revision` 队列本质

## 目标

Phase 11 之后，VST3/ARA clip 渲染链路只允许围绕以下六个抽象展开：

1. `clipId`
2. `renderRevision`
3. `mappingRevision`
4. `layoutEpoch`
5. `RenderCache`
6. `RenderTask`

本文件冻结的是**语义边界**与**读取边界**，不是 Phase 13 的旧队列重构实现。

## 1. `clipId`

- 唯一身份键。
- ARA 播放、VST3 同步、共享播放读取、RenderCache 绑定都必须先解析到 `clipId`。
- 当前代码归属：
  - `Source/PluginProcessor.h` -> `TrackState::AudioClip::clipId`
  - `Source/ARA/OpenTuneDocumentController.h` -> `AudioSourceClipBinding::clipId`
- `track` 可以继续作为 processor 内部容器索引存在，但不是 VST3 clip 语义主角。

## 2. `renderRevision`

- clip 语义层字段，只表达“这个 clip 的渲染语义是否换代”。
- 当前 Phase 11 只冻结字段与只读查询面，不改写旧 `RenderCache` 的 chunk revision 机制。
- 当前代码归属：
  - `Source/PluginProcessor.h` -> `TrackState::AudioClip::renderRevision`
  - `Source/PluginProcessor.h` -> `ClipSemanticState::renderRevision`
- 读取边界：VST3/ARA 下游通过 processor 的 clip-centric 查询读取它，而不是从 ARA/source-side revision 侧推导。

## 3. `mappingRevision`

- clip 语义层字段，只表达“宿主时间/播放区域如何映射到同一个 clip 去读”。
- 它不进入旧 `RenderCache` revision dedup，不承担渲染去重键职责。
- 当前代码归属：
  - `Source/PluginProcessor.h` -> `TrackState::AudioClip::mappingRevision`
  - `Source/PluginProcessor.h` -> `ClipSemanticState::mappingRevision`
  - `Source/ARA/OpenTuneDocumentController.h` -> `AudioSourceSyncState::mappingRevision` / `AudioSourceClipBinding::appliedMappingRevision`
- 其中 ARA 层的 `mappingRevision` 仍是 source 输入层版本；processor clip 上的 `mappingRevision` 才是共享 clip 语义层字段。

## 4. `layoutEpoch`

- clip 语义层字段，表达 ARA 输入/布局快照是否换代。
- Phase 11 只冻结命名、归属与读取面；真正接入 stale publish 屏障留到 Phase 13。
- 当前代码归属：
  - `Source/PluginProcessor.h` -> `TrackState::AudioClip::layoutEpoch`
  - `Source/PluginProcessor.h` -> `ClipSemanticState::layoutEpoch`
  - `Source/ARA/OpenTuneDocumentController.h` -> `AudioSourceClipBinding::layoutEpoch`
- ARA binding 中的 `layoutEpoch` 只是 clip 语义层快照，不是第二套可写状态。

## 5. `RenderCache`

- 继续承担旧系统的渲染缓存、pending 索引、chunk 状态、desired/published revision 生命周期。
- 它不是 mapping 真相源，也不是 clip 身份真相源。
- 当前代码归属：
  - `Source/Inference/RenderCache.h`
  - `Source/PluginProcessor.cpp` worker 与读取路径
- Phase 11 边界：允许把 `RenderCache::StateSnapshot.maxPublishedRevision` 显式映射为 `ClipSemanticState::publishedRenderRevision`；不允许重写队列本质。

## 6. `RenderTask`

- 这是旧队列未来继续收敛时的工作单元语义，而不是 Phase 11 要新增的第二套调度器。
- 合法 contract 至少包含：
  - `clipId`
  - `renderRevision`
  - `layoutEpoch`
  - chunk/range 信息
- 不应让 `mappingRevision` 进入 render task dedup 主语。
- 当前现实承载仍是 `Source/PluginProcessor.cpp` 内部 worker job；Phase 11 只冻结 contract，不重写调度实现。

## 读取边界

- VST3/ARA 下游读取 clip 语义状态时，必须通过 `Source/PluginProcessor.h` 中的 clip-centric 查询接口获取：
  - `getClipSemanticStateById(...)`
  - `getPluginClipSemanticState(...)`
- `AudioSourceClipBinding` 只作为 “ARA source 如何连到 clip 语义状态” 的桥接缝，不承担第二套状态模型。
- `RenderCache` 只暴露 render 侧事实；mapping 与 layout 语义必须从 clip 语义层读取。

## Phase 11 禁止事项

- 不允许 `track 0` 作为 VST3 clip 语义主角。
- 不允许把 `audioSourceId` 或 `playbackRegionId` 提升为渲染链路真相源。
- 不允许在 Phase 11 引入第二套队列、兼容层或并行状态模型。
- 不允许双写 `RenderCache` 与另一套 clip/render 状态容器。
- 不允许把 `mappingRevision` 混入旧 render revision 去重语义。

## 与后续 Phase 的边界

- Phase 11：冻结六个抽象与读取边界。
- Phase 12：把 VST3/ARA 入口继续收敛到 `clipId -> RenderCache` 读取真相，并把 mapping-only 从 render queue 语义里剥离。
- Phase 13：保持旧 `RenderCache / pending / revision` 核心不变，只重构变化分类、入队规则与 `layoutEpoch` stale publish 屏障。
