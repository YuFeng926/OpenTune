# Phase 11 Plan 02 Summary

## 改动文件

- `Source/PluginProcessor.h`
- `Source/PluginProcessor.cpp`
- `Source/Plugin/PluginEditor.h`
- `Source/Plugin/PluginEditor.cpp`
- `Source/ARA/OpenTunePlaybackRenderer.cpp`

## 关键实现

### 1. Processor 对 VST3/ARA 暴露 clip-centric 入口

新增并实现以下 API，全部回落到同一 `tracks_ / TrackState::AudioClip` 真相源：

- `getPluginClipSemanticState(...)`
- `getPluginPlaybackReadSource(...)`
- `setPluginClipStartSeconds(...)`
- `replacePluginClipAudio(...)`
- `enqueuePluginClipPartialRender(...)`

其中读取接口按 `clipId` 跨 track 搜索实际 clip，写接口先定位 clip 所在 track，再复用现有共享核心逻辑，不引入第二套状态容器。

### 2. VST3 editor 改为 clip semantic state 消费方

- 增加 `resolveCurrentClipSemanticState(...)`，让活跃 clip 解析先走 `clipId -> ClipSemanticState`
- `resolveCurrentClipId()` 基于 clip semantic state 返回唯一活跃 clip 身份
- `getRenderStatusSnapshot()` 改为 `clipId -> getPluginPlaybackReadSource(...) -> renderCache`
- heartbeat/诊断日志明确输出：
  - `clipId`
  - `renderRevision`
  - `mappingRevision`
  - `layoutEpoch`
  - `publishedRenderRevision`
- `recordRequested()` 在导入后改用 `setPluginClipStartSeconds(...)`
- `syncImportedAraClipIfNeeded()` 先取 `clipId + ClipSemanticState`；mapping-only 分支只允许：
  - `setPluginClipStartSeconds(...)`
  - binding 更新
  - 不调用 `replacePluginClipAudio(...)`
- `syncClipToPianoRoll()` 改为先解析 clip semantic state，再通过 `getPluginPlaybackReadSource(...)` 获取音频读取源
- `pitchCurveEdited()` / `trackTimeOffsetChanged()` 改用 clip-centric enqueue / startSeconds 写入口

### 3. ARA playback renderer 改为 clip-centric playback 读取

- 删除 `constexpr int kAraPluginTrackId = 0;`
- renderer 读取路径改为：`binding.clipId -> getPluginPlaybackReadSource(...) -> readPlaybackAudio()`
- `mappedSourceTime` 只保留为 clip 内读取起点输入
- `audioSource` / `playbackRegion` 继续只作为 ARA 映射输入来源，不再作为 processor 读取真相源

## 验证

### LSP

- 已先运行相关 `lsp_diagnostics`
- `PluginEditor.cpp`: 当前工具返回 `No diagnostics found`
- `PluginProcessor.cpp` / `OpenTunePlaybackRenderer.cpp`: 当前 clangd 环境缺少 JUCE 头搜索路径，报出 `'juce_audio_processors/juce_audio_processors.h' file not found`，不属于本次代码改动引入的问题

### 静态契约核对

- `PluginEditor.cpp` 包含：
  - `getPluginClipSemanticState(`
  - `setPluginClipStartSeconds(`
  - `mappingOnly=true`
  - `replacePluginClipAudio(`
  - `layoutEpoch`
- `OpenTunePlaybackRenderer.cpp` 包含：
  - `binding.clipId`
  - `getPluginPlaybackReadSource(`
- `OpenTunePlaybackRenderer.cpp` 已不再包含：
  - `constexpr int kAraPluginTrackId`

### 构建

- 仓库未使用根目录 `build/`，实际使用已存在的 `build_vst3/`
- 已构建：
  - `OpenTuneTests`
  - `OpenTune_Standalone`
  - `OpenTune_VST3`
- 结果：三者均构建成功

## 风险 / 备注

- 本次严格停在 Phase 11，只做入口语义收敛；未进入 Phase 13 的旧队列重构
- `getCurrentClipIdForPlugin()` 仍通过 plugin 容器的当前选择解析活跃 clip；但 VST3/ARA 后续消费面已经改为先以 `clipId` 进入 clip semantic state / playback source
