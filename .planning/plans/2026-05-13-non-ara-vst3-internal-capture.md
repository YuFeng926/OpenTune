# non-ARA VST3 内录功能规划

> Implementation note for future agents: execute this plan task-by-task. Do not
> implement a runtime fallback from ARA to non-ARA capture. The two plugin
> formats must remain separate build-time products.

## 背景

OpenTune 需要同时支持三类宿主能力边界：

| 格式 | 宿主能力 | OpenTune 正确行为 |
|---|---|---|
| Standalone | 自己拥有文件导入、时间轴、播放头 | 完整本地工程时间轴 |
| ARA / ARA2 VST3 | 宿主提供 ARA DocumentController、音频源、region、播放/编辑渲染角色 | 通过 ARA session 读取 item / region，并可向宿主请求定位 |
| non-ARA VST3 | 宿主只把实时音频流送进插件，插件不能读取 DAW item 文档 | 通过用户显式内录捕获轨道播放音频，播放头只做宿主到插件的单向镜像 |

用户目标是让 non-ARA VST3 的 Record 按钮变成类似 Auto-Tune / Waves Tune 的
Transfer 工作流：

1. 第一次按下 Record：进入录音状态。
2. DAW 播放时，插件从 `processBlock()` 收到的轨道音频被写入插件内部缓存。
3. 第二次按下 Record：结束录音状态。
4. 结束后，DAW 继续播放轨道音频时不再写入插件内部缓存。
5. 已录入的音频必须进入现有前处理、F0 提取、渲染缓存链路，而不是另起一套数据模型。
6. Record 按钮在录音和处理期间必须有与当前主题一致的响应反馈。

当前代码已经有 `Source/Plugin/Capture` 雏形，包含 `CaptureSession`、
`CaptureRingBuffer`、`CaptureSegment`、`CapturePersistence`。这份规划不是从零
设计，而是把这套雏形收束为正式 non-ARA VST3 架构，并补齐边界。

## 设计原则

### 1. non-ARA capture 是主流程，不是 ARA 失败兜底

`OPENTUNE_ENABLE_ARA=OFF` / `JucePlugin_Enable_ARA=0` 的 VST3 构建应拥有
内录 workflow。`OPENTUNE_ENABLE_ARA=ON` 的 ARA VST3 构建应继续使用 ARA
DocumentController。ARA 构建中如果 DocumentController 不存在，应报告 ARA 绑定
错误，而不是自动切到 capture。

这样能避免两个格式在同一个二进制里互相补洞，保留 `.planning/PROJECT.md`
里要求的“双格式独立编译，零交叉影响”。

### 2. 音频线程只做实时安全采集

non-ARA VST3 唯一可靠的音频来源是宿主回调到插件的实时 audio stream。因此：

- `processBlock()` 是采集入口。
- 音频线程只允许写入预分配 SPSC/ring buffer、读取原子 segment snapshot、做数值保护。
- 音频线程不得分配内存、等待锁、跑 F0、触发 ONNX、写文件或访问 GUI 状态。
- 录音结束后的前处理、materialization 创建、F0 提取和渲染请求在 message/background
  侧完成。

### 3. 录入缓存必须复用现有三 Store / F0 链路

停止录音后，捕获的 PCM 应走现有导入路径：

```text
captured PCM
  -> prepareImport(...)
  -> commitPreparedImportAsMaterialization(...)
  -> requestMaterializationRefresh(...)
  -> RMVPE F0 extraction
  -> PitchCurve / notes / correctedSegments
  -> RenderCache / vocoder render
```

不得为 non-ARA capture 创建 parallel pitch model、parallel render cache 或
专用 F0 管线。

### 4. non-ARA 播放头只做 host -> plugin 单向同步

non-ARA VST3 没有标准方式让插件可靠设置 DAW 播放头。因此正确行为是：

- 宿主播放头变化时，插件内部播放头跟随更新。
- 用户点击插件内部时间轴、piano roll 或 playhead，不应改变插件内部持久播放位置。
- 插件内部点击也不能尝试同步 DAW 播放头。
- 插件可以显示 host-controlled playhead，但 non-ARA 模式下不提供本地 seek 语义。

当前 `Source/Plugin/PluginEditor.cpp` 的 `playheadPositionChangeRequested()` 在
非 ARA 分支仍调用 `processorRef_.setPosition(timeSeconds)`，这与目标行为冲突，
必须在实现阶段修正。

## 用户可见行为

### Record 按钮状态

| 状态 | 触发 | 行为 | UI 反馈 |
|---|---|---|---|
| Idle | 初始 / 没有 Capturing 或 Processing segment | 点击后 arm capture | 普通 Record 外观 |
| Capturing | 第一次点击 Record 后 | DAW 正在播放时写入 capture FIFO；DAW 停止或未前进时不追加有效音频 | Record 按钮保持 active/toggled，并使用主题一致的录音强调色 |
| Processing | 第二次点击 Record 后 | 停止写入，提交 captured PCM 到现有导入/F0/render 链路 | Record 按钮禁用或进入处理中视觉状态，避免重复点击 |
| Edited | F0 / render 准备完成 | host playhead 落入 segment 时显示对应 materialization，可播放修正结果 | Record 回到可再次 arm 状态 |
| Failed | F0/导入失败或空录音 | 丢弃不可用 segment，让用户可重新录音 | 回到 Idle，并记录清晰日志 |

### 播放 / 录音关系

- Record active 但 DAW 未播放时，不应凭空增长 capture duration。
- DAW 播放头前进且 Record active 时，写入插件收到的 dry input copy。
- 第二次点击 Record 后，后续 DAW 播放不再写入这个 segment。
- 已完成 segment 的时间范围由 host playback time 锚定。
- host playhead 进入 Edited segment 范围时，插件可以用 rendered audio 替换 dry
  path；离开范围时恢复 dry pass-through。

### non-ARA 时间轴交互

- PianoRoll 可以展示当前 host playhead 对应的 captured materialization。
- 用户不能通过点击 PianoRoll 时间尺改变 DAW 播放头。
- 用户点击内部时间轴后，插件不应把 `positionAtomic_` 固定到点击位置；下一次
  host transport tick 应继续作为唯一来源。
- 如果需要 UI 提示，应通过禁用 hover/cursor/drag seek 或短状态反馈表达，
  不要加入解释性大段文字。

## 当前代码基础

### 已有可复用模块

- `Source/Plugin/Capture/CaptureSession.h`
  - 已定义 `Idle / HasCapturing / HasProcessing` 聚合状态。
  - 已描述 Melodyne Transfer-style non-ARA capture workflow。
  - 已通过 `ProcessorBindings` 连接到 `prepareImport()`、
    `commitPreparedImportAsMaterialization()`、`requestMaterializationRefresh()`。
- `Source/Plugin/Capture/CaptureRingBuffer.h`
  - 已按 audio producer / message consumer 的 SPSC 模型设计。
- `Source/Plugin/PluginProcessor.cpp`
  - non-ARA process path 已能从 host `AudioPlayHead::PositionInfo` 镜像
    `timeInSeconds` / `isPlaying`。
  - capture session process 后直接 return，适合作为 non-ARA VST3 主路径。
- `Source/Plugin/PluginEditor.cpp`
  - non-ARA `recordRequested()` 已有 Idle -> arm、Capturing -> stop、
    Processing -> ignore 的基本交互。
  - timer 中已有 `transportBar_.setRecordIndicatorActive(isCapturingNow)`。
- `Source/Standalone/UI/TransportBarComponent.cpp`
  - 已有 `setRecordIndicatorActive(bool)`，目前只有 toggle/repaint。

### 明确缺口

1. `captureSession_` 的创建边界需要和 build target 对齐，ARA 构建不应把 capture
   当成 DocumentController 失败时的补偿路径。
2. non-ARA `playheadPositionChangeRequested()` 目前仍会本地 `setPosition()`，需要改为
   no-op 或 host-mirror-only 策略。
3. Record UI 只有 bool active，缺少 Processing/Failed 等响应式状态。
4. 需要确认 `stopCapture()` 后不会继续写入已停止 segment，且 host 停止/倒带/跳转时
   不产生错误时间锚。
5. 需要端到端验证 captured PCM 是否完整进入现有前处理、F0、RenderCache 链路。

## 实施任务

### Task 1：冻结 build-time 架构边界

文件范围：

- `Source/PluginProcessor.h`
- `Source/PluginProcessor.cpp`
- `Source/Plugin/PluginEditor.cpp`

要求：

- non-ARA VST3：编译并启用 `CaptureSession`。
- ARA VST3：使用 `VST3AraSession` / ARA DocumentController，不使用 capture fallback。
- 代码分支应优先由 `#if JucePlugin_Enable_ARA` 或 target-specific 构建定义隔离。
- 日志命名要区分 `VST3 Capture` 与 `VST3 ARA`，避免现场诊断混淆。

验收：

- `OPENTUNE_ENABLE_ARA=OFF` 的 VST3 中，Record 只走 capture。
- `OPENTUNE_ENABLE_ARA=ON` 的 VST3 中，Record 只走 ARA DocumentController。
- ARA DocumentController 为 null 时仍显示 ARA 错误，不进入 capture。

### Task 2：完善 CaptureSession 状态机

文件范围：

- `Source/Plugin/Capture/CaptureSession.h`
- `Source/Plugin/Capture/CaptureSession.cpp`
- `Source/Plugin/Capture/CaptureSegment.h`
- `Source/Plugin/Capture/CaptureRingBuffer.h`

要求：

- `armNewCapture()` 只在没有 Capturing/Processing segment 时成功。
- `processBlock()` 只有在 Capturing 且 host transport 实际前进时写入 dry copy。
- `stopCapture()` 后立即切断采集写入路径。
- 空录音、全静音、render 失败都必须释放 Processing 状态，让用户能再次录音。
- host 倒带、循环、跳转时不要把非连续时间拼成一个连续 take；最小可接受策略是
  检测时间不连续并自动结束/分段，具体选择需在实现前用代码现状确认。

验收：

- Record on -> 播放 5 秒 -> Record off 后，capture duration 约等于 host 播放推进时长。
- Record off 后继续播放 5 秒，segment duration 不再增长。
- Processing 失败不会卡死 Record 按钮。

### Task 3：把 captured PCM 接入现有前处理和 F0 链路

文件范围：

- `Source/PluginProcessor.cpp`
- `Source/Plugin/Capture/CaptureSession.cpp`
- `Source/Audio/AsyncAudioLoader.h` 及现有导入 helper，如需复用

要求：

- 停止录音后提交 `std::shared_ptr<juce::AudioBuffer<float>>` 和 host sample rate。
- 复用 `prepareImport()` 做声道、采样率、silent gap、source window 等标准化。
- 复用 `commitPreparedImportAsMaterialization()` 创建 materialization。
- 复用 `requestMaterializationRefresh()` 触发 RMVPE F0。
- 不创建 capture 专用 SourceStore/MaterializationStore 平行分支。

验收：

- non-ARA capture 完成后，PianoRoll 能看到原始 F0 / note generation 所依赖的数据。
- 已录音频能进入现有 render cache，后续 pitch edit 可以触发同一 vocoder 渲染链路。
- 保存/恢复后，不把 vocoder output 当成持久真相；恢复应重新触发 render。

### Task 4：补齐 Record 按钮响应式反馈

文件范围：

- `Source/Standalone/UI/TransportBarComponent.h`
- `Source/Standalone/UI/TransportBarComponent.cpp`
- `Source/Plugin/PluginEditor.cpp`
- 主题相关 LookAndFeel / ThemeTokens，如需要

要求：

- 将单一 `setRecordIndicatorActive(bool)` 扩展为能表达至少三态：
  `Idle / Capturing / Processing`。
- Capturing：Record 按钮保持 toggled/active，使用当前主题下可读的录音强调色。
- Processing：按钮禁用或显示处理状态，避免用户重复 stop/arm；样式要与主题一致。
- Failed/Idle：回到普通外观。
- 不在 UI 里增加大段功能说明；tooltip 或简短状态字段可以沿用现有风格。

验收：

- 在 BlueBreeze / DarkBlueGrey / Aurora 主题下，Record active 状态都清晰可见。
- Processing 状态下连续点击不会产生重复 segment。
- 状态变化由 capture session 状态驱动，而不是 UI 自己猜。

### Task 5：实现 non-ARA host-only 播放头镜像

文件范围：

- `Source/Plugin/PluginEditor.cpp`
- `Source/Standalone/UI/PianoRollComponent.cpp`
- `Source/Standalone/UI/PianoRollComponent.h`
- 可能涉及 transport/playhead listener glue

要求：

- non-ARA VST3 的播放头来源只允许是 host `AudioPlayHead::PositionInfo`。
- `playheadPositionChangeRequested()` 在 non-ARA VST3 中不得调用
  `processorRef_.setPosition(timeSeconds)`。
- 如果 PianoRoll/Transport 仍发出 local seek 请求，non-ARA editor 应忽略或禁用这些入口。
- ARA VST3 保留 `DocumentController::requestSetPlaybackPosition()` 语义。
- Standalone 保留本地 seek 语义。

验收：

- non-ARA VST3 中拖动/点击插件时间轴，DAW 播放头不变。
- non-ARA VST3 中拖动/点击插件时间轴，插件内部播放头不应停在点击位置。
- DAW 播放头变化时，插件显示播放头跟随。

### Task 6：完善 captured materialization 投影

文件范围：

- `Source/Plugin/PluginEditor.cpp`
- `Source/Plugin/Capture/CaptureSession.cpp`

要求：

- non-ARA 模式下，active materialization 由 host playhead 是否落入 Edited segment 决定。
- host playhead 离开所有 Edited segment 时，PianoRoll 不应错误显示上一段仍为 active。
- 进入 segment 时，projection 的 local time 要等于 `hostTime - segmentStart`。
- 编辑 notes/correctedSegments 时必须写回当前 materialization，而不是全局单例。

验收：

- 多次内录多个 segment 后，host 播放头进入不同 segment，PianoRoll 显示对应 materialization。
- host 播放头在空白区域时，不误编辑上一段 take。

### Task 7：持久化和恢复

文件范围：

- `Source/Plugin/Capture/CapturePersistence.h`
- `Source/Plugin/Capture/CapturePersistence.cpp`
- `Source/PluginProcessor.cpp` state save/load

要求：

- 保存 Edited/Processing segment 与 materialization id / 时间范围。
- 恢复时跳过 orphan materialization。
- 恢复后重新触发 `refreshMaterialization()`，让 F0/render cache 重新建立。
- 不保存实时 ring buffer 或未完成 Capturing 的裸状态；宿主关闭工程时 Capturing 应被丢弃或安全停止。

验收：

- non-ARA VST3 录入并完成 F0 后保存 DAW 工程，再打开工程，segment 与 F0/编辑状态可恢复。
- 恢复后 RenderCache 可以重新填充，播放时不依赖上一次进程内缓存。

### Task 8：验证矩阵

构建命令沿用当前 Windows Path 大小写冲突绕行：

```bat
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-legacy-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-legacy-vs18-clean --config Release --target OpenTune_VST3"
```

手工验收场景：

1. non-ARA VST3 插到含音频 item 的轨道。
2. DAW 播放前点击 Record，按钮进入 Capturing。
3. DAW 播放 5 秒，点击 Record 停止，按钮进入 Processing。
4. F0 完成后，PianoRoll 显示 captured take。
5. DAW 播放头进入 take 范围，插件播放头跟随，edited audio 可替换 dry path。
6. DAW 播放头离开 take 范围，插件不再显示错误 active segment。
7. Record off 后继续播放轨道音频，不增加 segment duration。
8. 点击插件内部时间轴，DAW 播放头不变，插件播放头继续由 host 控制。
9. 保存并重开 DAW 工程，capture segment 和 materialization 可恢复。
10. ARA VST3 在 Studio One / ARA host 中仍走 DocumentController，不触发 capture fallback。

回归检查：

- Standalone 构建不受 non-ARA capture 改动影响。
- ARA VST3 构建中 `JucePlugin_Enable_ARA=1`。
- non-ARA VST3 构建中 `JucePlugin_Enable_ARA=0`。
- `rg "Unable to access VST3 ARA DocumentController"` 仍只属于 ARA 分支错误。
- `rg "setPosition\\(timeSeconds\\)" Source/Plugin/PluginEditor.cpp` 不应显示 non-ARA local seek。

## 非目标

- 不在 non-ARA VST3 中读取 DAW item 文件或宿主 project document。
- 不在 non-ARA VST3 中实现插件到 DAW 的播放头定位。
- 不把 ARA DocumentController 失败自动降级为 capture。
- 不为 capture 建立独立 F0/render 数据模型。
- 不为了“看起来能用”在 audio thread 上做阻塞等待或后台任务启动。

## 风险

| 风险 | 影响 | 应对 |
|---|---|---|
| 宿主不在停止状态发送稳定 `timeInSeconds` | capture segment 锚点错误 | 以 host time 连续性检测决定是否写入，必要时分段或停止 |
| DAW 循环播放时 Record active | 多段音频被拼接成错位 take | 检测时间回跳，自动结束当前 segment 或新建 segment |
| input bus 不是预期声道数 | captured PCM 声道错误 | 继续以 `getMainBusNumInputChannels()` 作为 capture layout 真相，并限制到 mono/stereo |
| F0 提取失败 | Processing 卡住 | `isRenderFailed()` 必须释放状态并允许重新录音 |
| UI 状态只靠按钮 toggle | 状态漂移 | UI 每帧从 `CaptureSession::getGlobalState()` 派生 |

## 完成定义

non-ARA VST3 Record 是一个完整、可重复、可恢复的内部 Transfer workflow：

- 第一次点击开始采集宿主实时轨道音频。
- 第二次点击停止采集并提交现有前处理/F0/render 链路。
- 录音停止后不再写入插件内部缓存。
- UI 清楚表达 Capturing / Processing / Idle。
- 播放头只从宿主同步到插件，插件内部 seek 不反向控制宿主。
- ARA / ARA2 VST3 继续使用 ARA DocumentController，不被 capture 逻辑污染。
