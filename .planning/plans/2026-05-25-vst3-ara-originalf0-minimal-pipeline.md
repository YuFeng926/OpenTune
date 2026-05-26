# VST3 ARA 自动 OriginalF0 最小正确架构方案

**日期：** 2026-05-25  
**状态：** Proposed  
**验证文档：** `.planning/plans/2026-05-25-vst3-ara-originalf0-minimal-pipeline-test-verification.md`

## 目标

把 VST3 ARA 自动 `OriginalF0` 链路收敛到最小正确架构，只保留真正必需的两件事：

- 在目标 `source window` 上完成一次符合 ARA 契约的宿主读音频
- 用这一次读取同时完成：
  - 生成一份可播放的 materialization 音频缓存
  - 生成 RMVPE 所需的分析输入并完成 `OriginalF0`

同时明确以下产品边界：

- VST3 ARA 自动链路 **不再进入 GAME reference-note 提取**
- 当 `OriginalF0` 已提交且 F0 曲线已经对用户可见后，**立即释放 RMVPE 推理模型与所有分析专用临时资源**
- 自动链路结束后，**只保留播放所需缓存**，不保留额外闲置推理资源

## 需求重述

对当前这个 VST3 ARA 自动流程，真实需求只有：

1. 在 ARA 协议框架下规范地拿到目标音频
2. birth 一次完整可播放的 materialization
3. 自动提取 `OriginalF0`
4. F0 曲线可见后，释放 RMVPE 与分析期临时资源
5. 后续播放只依赖 playback buffer / render cache，不依赖推理模型常驻

因此最小链路应为：

```text
一次 ARA 读取
-> 一次 playable materialization birth
-> 一次 OriginalF0 派生
-> F0 可见
-> 立即释放 RMVPE 与分析临时资源
```

任何超出这条链的行为，都不应默认挂在 VST3 ARA 自动路径里。

## 当前结构性问题

现状自动链路做了超出需求的事情：

1. `VST3AraSession` 先 hydration 出整段 `copiedAudio`
2. `ensureAraRegionMaterialization()` 再从中切 `windowBuffer`
3. `prepareImport()` 再把这份数据转成 44.1 kHz 的 store/materialization buffer
4. `requestMaterializationRefresh()` 再从 materialization 音频回读，重新构造 `monoAudio`
5. RMVPE 跑完后，VST3 ARA 自动链还会继续挂上 GAME note generation

这带来三个结构性问题：

- **owner 膨胀**：分析输入在 RMVPE 之前就已经升级成多份全长 PCM owner
- **表示重复**：宿主原采样率全长 PCM、44.1 kHz 可播放 PCM、44.1 kHz 分析单声道 PCM 同时存在
- **职责污染**：VST3 ARA 自动 OriginalF0 链路被附带了并非刚需的 GAME ref 派生

## 目标架构

### 高层流程

```text
ARA sample access lease
-> 按 chunk 读取 source window
-> 分支 A：累积一份 44.1 kHz playableBuffer
-> 分支 B：直接做 channel select/downmix + 44.1k->16k，累积 rmvpeInput16k
-> 用分支 A birth 一次 materialization
-> 用分支 B 跑 RMVPE
-> commit F0 / energy
-> F0 曲线可见
-> 立即释放 RMVPE 模型 + 分析临时资源
```

### 核心规则

对 VST3 ARA 自动 `OriginalF0` 路径，最终只允许两类持久结果：

- materialization 播放所需音频缓存 / 正常播放缓存
- 已提交的 `OriginalF0` / `energy`

除此之外，其余资源都必须视为临时资源，并在成功提交后可立即释放。

## 架构决策

### 1. ARA 自动 OriginalF0 改为专用编排链

不要再把当前 VST3 ARA auto-birth 主要建立在通用 `requestMaterializationRefresh()` 路径上，如果该路径默认假设：

- 分析一定要从已 birth 的 materialization 音频再回读
- `OriginalF0` 成功后还会继续挂 GAME 自动 note generation

应新增或重构一个 **ARA 专用自动 OriginalF0 编排入口**，它的输入应直接是：

- `audioSource`
- `sourceId`
- `sourceWindow`
- 当前有效的 ARA sample access lease / reader

它一次性负责：

- birth playable materialization
- 构造 RMVPE 输入
- commit `OriginalF0`

而不是走完通用 refresh 之后再链式追加其他派生。

### 2. 只 birth 一次完整可播放 materialization

对一次自动读音频动作，playback materialization 只能 birth 一次。

保留现有播放契约：

- durable playback source 仍然是 materialization 的 `audioBuffer`
- 运行时存储域仍然是 44.1 kHz
- 当前播放链路 `MaterializationStore::getPlaybackReadSource()` -> `readPlaybackAudio()` 不改产品语义

这份 `audioBuffer` 是自动流程结束后唯一需要长期保留的音频 owner。

### 3. 在 ARA 读取过程中直接构造分析输入

在 chunk 读取阶段就同步完成分析域准备：

- 按当前策略选分析通道
- 必要时 downmix 成 mono
- 同步进入 44.1k -> 16k 分析域
- 只累积一份 `rmvpeInput16k`

禁止继续保留当前这条结构：

```text
materialization.audioBuffer -> monoAudio -> RMVPE
```

也就是说，VST3 ARA 自动路径里不应再出现“先 birth，再从已 birth 音频回读一整遍用于分析”的结构。

### 4. VST3 ARA 自动路径不进入 GAME

`OriginalF0` 完成不再自动触发 GAME note generation。

产品边界明确为：

- VST3 ARA 自动路径：**无 GAME**
- Standalone：GAME ref 链可保留
- regular VST3 capture：暂不在本方案中调整，除非后续明确重构

这是结构边界，不是“按时长/按启发式跳过”的软策略。

### 5. F0 曲线可见后立即释放 RMVPE

这里必须从“空闲定时回收”升级为“成功后立即释放”。

硬契约如下：

- `OriginalF0` 已成功 commit
- UI 可见状态已经达到 `OriginalF0State::Ready`

此时立刻执行：

- 释放 RMVPE model/session 资源
- 释放分析专用 buffer/vector
- 释放不再服务播放的 hydration/中间临时资源

只保留：

- born materialization playback buffer
- materialization 内已经提交的 `F0/energy`
- 正常播放所需 render/playback cache

不允许保留“为了稍后可能再用一次 RMVPE，所以先常驻”的推理资源。

## 必需行为变化

### A. VST3 ARA auto-read / auto-birth 路径

把现有多段 full-buffer 传递改成单次编排：

1. chunk-by-chunk 读取 ARA `source window`
2. 构造 `playableBuffer44k`
3. 同时构造 `rmvpeInput16k`
4. 用 `playableBuffer44k` birth materialization
5. 用 `rmvpeInput16k` 跑 RMVPE
6. commit F0 / energy
7. `OriginalF0State::Ready`
8. 立即释放 RMVPE 与分析临时资源

### B. 拆分 `requestMaterializationRefresh()` 的职责

如果 `requestMaterializationRefresh()` 仍需保留，必须拆掉“F0 成功后继续 GAME”的默认假设。

建议形成三条边界清晰的路径：

- 通用 refresh：只负责刷新 pitch data / materialization 状态
- Standalone 可选 note-ref 路径：按产品策略进入 GAME
- ARA 自动 OriginalF0 路径：绝不串到 GAME

### C. 强化 RMVPE 生命周期管理

`F0InferenceService` 需要提供面向此路径的显式能力：

```text
OriginalF0 visible -> release RMVPE now
```

而不是仅依赖：

```text
OriginalF0 visible -> 等 idle timer 未来某个时刻回收
```

也就是说，这次方案要求把“立即释放”提升为产品级行为，而不是调优项。

### D. 闲置资源回收策略

VST3 ARA 自动 `OriginalF0` 完成后，只允许保留：

- playback `audioBuffer`
- 已提交的 `OriginalF0` / `energy`
- 播放必需的 render/playback cache

不得保留：

- ARA 自动路径上的 GAME model/session
- commit 之后仍常驻的 RMVPE model/session
- 全长 `monoAudio` 分析副本
- 只为 hydration / 中间搬运存在、完成后已无播放价值的整段 PCM

## 文件级改动入口

### 主要文件

- 修改：`Source/ARA/VST3AraSession.cpp`
- 修改：`Source/PluginProcessor.cpp`
- 修改：`Source/Services/ImportedClipF0Extraction.h`
- 修改：`Source/Inference/F0InferenceService.{h,cpp}`
- 如 birth/commit seam 需要清理：`Source/MaterializationStore.{h,cpp}`
- 测试：`Tests/TestMain.cpp`

### 可能涉及的辅助文件

- `Source/ARA/OpenTuneDocumentController.cpp`
- `Source/Utils/AppLogger.{h,cpp}`

## 实施任务拆分

### 任务 1：先用测试锁定新契约

新增 architecture / processor / memory 方向测试，至少覆盖：

- VST3 ARA 自动 `OriginalF0` 路径不会进入 GAME
- VST3 ARA 自动 `OriginalF0` 不再从 `materialization.audioBuffer` 回建分析输入
- F0 可见后，RMVPE 资源会立即释放

### 任务 2：引入 ARA 自动 OriginalF0 专用编排入口

新增或重构 processor-level helper，输入：

- ARA source reader / lease context
- `sourceId`
- `sourceWindow`
- 显示/元数据上下文

输出：

- 已 birth 的 `materializationId`
- 已 commit 的 `OriginalF0`

该入口不再绕回旧的通用后处理链。

### 任务 3：同一轮 chunk 读取同时构造播放域与分析域

在 ARA chunk 读取中：

- 累积一份 `playableBuffer44k`
- 同步构造一份 `rmvpeInput16k`

确保不再出现 birth 完以后再整段回读生成 `monoAudio` 的旧结构。

### 任务 4：只用 playable buffer birth 一次 materialization

用上一步生成的 `playableBuffer44k` 直接 birth materialization。

除非播放确有必要，否则不再为同一次动作建立额外全长 PCM owner。

### 任务 5：运行 RMVPE 并提交 OriginalF0 / energy

直接使用 `rmvpeInput16k` 跑 RMVPE，并提交：

- `PitchCurve.originalF0`
- `PitchCurve.originalEnergy`
- `OriginalF0State::Ready`

此提交路径与 GAME note generation 解耦。

### 任务 6：成功提交后立即释放资源

在成功 commit 且 F0 曲线可见后立刻：

- 显式释放 RMVPE 推理资源
- 清空分析临时 buffer
- 确保 ARA 自动链不再残留隐藏的推理常驻内存

### 任务 7：彻底移除 VST3 ARA 自动路径中的 GAME

重构当前 completion 行为，使 VST3 ARA 自动用例不再触达：

- `ensureNoteGeneratorReady()`
- `noteGeneratorPool_`
- hidden GAME reference-note generation

如果 Standalone 仍需 GAME，则保留在 Standalone 专属或显式 non-ARA 路径中。

## 日志与诊断要求

需要补齐明确日志，便于后续内存与时序分析：

```text
ARA OriginalF0 pipeline: start sourceId=... window=[...]
ARA OriginalF0 pipeline: playableBuffer44k built samples=...
ARA OriginalF0 pipeline: rmvpeInput16k built samples=...
ARA OriginalF0 pipeline: materialization born materializationId=...
ARA OriginalF0 pipeline: F0 committed frames=...
ARA OriginalF0 pipeline: OriginalF0 visible, releasing RMVPE resources now
ARA OriginalF0 pipeline: complete
```

并增加一个显式负向告警：

```text
InvariantViolation: VST3 ARA automatic OriginalF0 path must not enqueue GAME reference-note generation
```

## 非目标

- 本次不改 Standalone 导入 UX
- 本次不重构 regular VST3 capture 的 F0/GAME 策略
- 本次不改变 playback ownership 对 materialization `audioBuffer` 的依赖
- 本次不引入 host-specific 分支
- 本次不长期维护新旧两套 ARA 自动链并行兼容

## 预期结果

方案落地后，VST3 ARA 自动 `OriginalF0` 会成为一条真正单一职责、低内存占用的链路：

- 一次 ARA 读取
- 一次 playable materialization birth
- 一次 `OriginalF0` 派生
- F0 可见后立即释放 RMVPE 与分析临时资源
- VST3 ARA 自动路径不再承担隐藏 GAME 内存成本

## 审阅确认项

实施前建议确认以下产品判断仍然成立：

1. VST3 ARA 自动路径永远不生成 hidden GAME reference notes。
2. 播放可以等待自动 `OriginalF0` 完成后再开始具备意义。
3. 自动读音频后，唯一需要长期保留的音频 owner 就是 born materialization playback buffer。
4. “F0 曲线可见后立即释放 RMVPE”优先级高于模型热复用。
