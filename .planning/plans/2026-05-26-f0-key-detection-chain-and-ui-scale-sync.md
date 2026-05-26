# F0 提取主链调式检测接回与 UI 调式同步修复方案

**日期:** 2026-05-26  
**状态:** Proposed  
**验证文档:** `.planning/plans/2026-05-26-f0-key-detection-chain-and-ui-scale-sync-test-verification.md`

## 目标

修复最近几次提交后出现的两个回归：

1. 现行 ARA `OriginalF0` 主链 `birthAraMaterializationWithOriginalF0(...)` 在 F0 完成后没有接入既有 `ChromaKeyDetector` 调式智能检测逻辑。
2. `detectedKey` 异步写回后，当前激活素材的 UI 不会稳定刷新，导致 TransportBar / PianoRoll 不能及时显示检测结果。

本轮要求继续保持 `materialization.detectedKey` 为唯一真值，不新增并行通知链，不让 processor/service 直接驱动 UI。

## 已确认根因

### 根因 1：ARA 新主链漏掉调式检测

- `requestMaterializationRefresh(...)` 完成回调中已有既有契约：
  - 如果当前 `detectedKey.confidence <= 0.0f`
  - 则读取 materialization 音频
  - 运行 `ChromaKeyDetector`
  - 再 `setMaterializationDetectedKeyById(...)`
- 但 `birthAraMaterializationWithOriginalF0(...)` 只提交了：
  - `PitchCurve`
  - `OriginalEnergy`
  - `OriginalF0State::Ready`
- 没有运行 `ChromaKeyDetector`，也没有写 `detectedKey`。

### 根因 2：当前激活对象的 UI 缺少 detectedKey pull-sync

- VST3 `PluginEditor` 已有统一 reader 入口 `syncMaterializationProjectionToPianoRoll()`，其中会读取 `detectedKey` 并推到：
  - `transportBar_.setScale(...)`
  - `pianoRoll_.setScale(...)`
- Standalone `PluginEditor` 也有现成的：
  - `resolveScaleForPlacementMaterialization(...)`
  - `applyResolvedScaleForPlacementMaterialization(...)`
- 但 Standalone `timerCallback()` 目前只轮询：
  - active materialization id
  - curve
  - buffer
  - notesRevision
- 没有对当前激活 placement/materialization 的 `detectedKey` 做 pull-sync，因此异步检测完成后不会自动刷新。

## 设计原则

- `detectedKey` 继续只存于 `MaterializationStore`。
- 调式检测逻辑继续属于 processor 内容链，而不是 editor/UI。
- UI 只做 pull + apply，不做额外真值缓存链。
- Standalone 复用已有 `applyResolvedScaleForPlacementMaterialization(...)`，不重复直写 `transportBar_` / `pianoRoll_`。
- 不新增 listener / callback / pending flag / detectedKeyRevision。

## 执行任务

### Task 1：补 ARA 主链的调式检测

**文件：**

- 修改 `Source/PluginProcessor.cpp`

**改法：**

在 `birthAraMaterializationWithOriginalF0(...)` 成功提交 `PitchCurve` 后、`OriginalF0State::Ready` 前，复用既有契约：

1. 读取当前 materialization 的 `existingKey`
2. 仅在 `existingKey.confidence <= 0.0f` 时运行 `ChromaKeyDetector`
3. 使用当前 born materialization 的可播放音频做检测
4. `setMaterializationDetectedKeyById(...)`

**约束：**

- 不覆盖用户手动指定的高置信度调式。
- 不新造第二套检测规则。
- 不把检测逻辑下沉到 editor。

### Task 2：补 Standalone 当前激活对象的调式 pull-sync

**文件：**

- 修改 `Source/Standalone/PluginEditor.cpp`

**改法：**

在现有 `timerCallback()` 的 active materialization 轮询块里：

1. 继续复用已算出的 `activeTrack` / `activePlacementIndex`
2. 调用 `resolveScaleForPlacementMaterialization(...)`
3. 若解析出的 `rootNote/scaleType` 与 `lastScaleRootNote_ / lastScaleType_` 不同，则调用 `applyResolvedScaleForPlacementMaterialization(...)`

**约束：**

- 不新增 `lastDetectedKey`
- 不新增通知链
- 不让 `TransportBarComponent` / `PianoRollComponent` 自己读 store

### Task 3：补 focused architecture/source guards

**文件：**

- 修改 `Tests/TestMain.cpp`

**新增守卫：**

1. `KeyDetection_AraBirthPathRunsChromaDetectionBeforeReady`
   - 守卫 `birthAraMaterializationWithOriginalF0(...)` 内存在：
     - `ChromaKeyDetector`
     - `setMaterializationDetectedKeyById`
     - 且发生在 `OriginalF0State::Ready` 之前
2. `ScaleSync_StandaloneTimerPullsDetectedKeyForActivePlacement`
   - 守卫 Standalone `timerCallback()` 会对当前激活 placement/materialization 做调式 pull-sync
   - 并复用 `applyResolvedScaleForPlacementMaterialization(...)`

## Kill List

本轮明确不做：

- 不新增 `detectedKeyRevision`
- 不给 `MaterializationStore::setDetectedKey()` 加 listener
- 不从 `F0ExtractionService` / `PluginProcessor` 直接操作 UI
- 不改 `TransportBarComponent` / `PianoRollComponent` 成为 store reader
- 不为 Plugin/VST3 再加第二套 scale 刷新链

## 完成标准

- ARA birth 主链在 F0 成功后会提交 `detectedKey`
- Standalone 当前激活 placement 在异步调式检测完成后可自动刷新 UI
- VST3 现有统一 sync 链保持不被破坏
- focused architecture 守卫通过
- 不引入新的并行状态/通知结构
