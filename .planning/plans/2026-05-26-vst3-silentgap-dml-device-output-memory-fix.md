# VST3 silentGap + DML device output 内存修复方案

**日期:** 2026-05-26  
**状态:** Proposed  
**验证文档:** `.planning/plans/2026-05-26-vst3-silentgap-dml-device-output-memory-fix-test-verification.md`

## 目标

本轮只修三件事：

1. 让 VST3 ARA birth 路径重新接回 `silentGaps` 机制，按静息处分割 chunk 提交到渲染队列，与 Standalone 保持一致。
2. 让 DML vocoder 的 output 改为 device output 绑定，不再长期持有按历史最大 shape 预分配的 CPU output tensor。
3. 复核现有队列去重和会话释放边界，在不破坏 chunk 间 ONNX session 复用的前提下，尽量把“每次 run 的临时输出持有”收短到 run 结束即释放。

## 明确不做

- 不新增“硬限制 chunk 最大 frame 数”。
- 不做固定秒数强切 chunk。
- 不改 `GPU First` / `CPU First` 两态产品语义，不新增第三种渲染优先级。
- 不为了立刻释放内存而把 vocoder ONNX session 改成“每个 chunk 重建一次”。
- 不在本轮顺手重构 Stage 2、RenderCache 全局上限、retired materialization 生命周期。

## 根因判断

### 1. VST3 整段大 job 的直接根因

- `prepareImport(...)` 会清空 `PreparedImport::silentGaps`。
- 常规 `requestMaterializationRefresh(...)` 路径会重新执行 `SilentGapDetector::detectAllGapsAdaptive(...)`，再 `setSilentGaps(...)`。
- `birthAraMaterializationWithOriginalF0(...)` 当前直接 `prepareImport -> commitPreparedImportAsMaterialization -> RMVPE`，中间没有补回 `silentGaps`。
- `MaterializationStore::enqueuePartialRender(...)` 在没有 chunk boundaries 时会退化成整段单 job。

因此 ARA birth materialization 在 VST3 里会把整段音频一次性交给 vocoder。

### 2. DML 高 RAM 的直接放大器

- 当前 `DmlVocoder` 通过 CPU tensor 绑定 output。
- `preallocatedOutput_` / `outputBuffer_` 会按见过的最大 `numFrames` 长期保留。
- 这不是 DML 必须要求的，而是当前实现选择。

本轮把 output 改绑到 device，由 ORT/DML 管理 device output，再在 run 结束后把结果拷回短生命周期 CPU `std::vector<float>` 返回给调用方。

## 执行任务

### Task 1: ARA birth 路径补回 silentGaps

**文件:**

- `Source/PluginProcessor.cpp`
- `Tests/TestMain.cpp`

**做法:**

1. 在 `birthAraMaterializationWithOriginalF0(...)` 中，`prepareImport(...)` 成功后、`commitPreparedImportAsMaterialization(...)` 之前，基于 `preparedImport.storedAudioBuffer` 执行 `SilentGapDetector::detectAllGapsAdaptive(...)`。
2. 将检测结果写回 `preparedImport.silentGaps`，让 commit 出去的 materialization 从出生开始就携带与 Standalone 一致的 chunk 边界。
3. 不额外新增第二套 ARA 专属 chunking 逻辑，不在 birth 之后再补一条异步 refresh 链。

**结果要求:**

- VST3 ARA birth 产物从第一次 render enqueue 起就走 `silentGaps` 分段，而不是整段单 job。
- Standalone 与 VST3 共用同一条 `MaterializationStore::buildChunkBoundariesFromSilentGaps(...)` 真值线。

### Task 2: DML output 改为 device output

**文件:**

- `Source/Inference/DmlVocoder.h`
- `Source/Inference/DmlVocoder.cpp`
- `Tests/TestMain.cpp`

**做法:**

1. 删掉 DML 路径里长期持有的 CPU output 预分配成员：
   - `cpuMemoryInfo_`
   - `preallocatedOutput_`
   - `outputBuffer_`
   - `preallocatedFrames_`
2. 初始化 session 后查询 output 对应的 `OrtMemoryInfo`。
3. `runSession(...)` 改为：
   - `BindInput(...)`
   - `BindOutput(..., outputMemoryInfo)` 绑定到 device
   - `Run(...)`
   - `SynchronizeOutputs()`
   - `GetOutputValues()` 拿回本次 run 的 output value
   - 将 output tensor 拷贝到本次返回值 `std::vector<float>`
4. 本次 run 结束即释放 output `Ort::Value` 容器和临时 CPU vector 的中间持有，不保留“历史最大 shape CPU output buffer”。

**结果要求:**

- DML 路径不再通过固定常驻 CPU tensor 接 output。
- 仍保持 DML session 常驻复用，避免 chunk 间重复创建 ONNX session。

### Task 3: 保持渲染优先级产品语义不漂移

**文件:**

- `Source/Utils/AppPreferences.h`
- `Source/Editor/Preferences/SharedPreferencePages.cpp`
- `Source/Standalone/PluginEditor.cpp`
- `Source/Plugin/PluginEditor.cpp`
- `Tests/TestMain.cpp`

**做法:**

1. 确认渲染优先级仍只有：
   - `GpuFirst` 默认
   - `CpuFirst`
2. 确认偏好页切换仍只通过 `resetInferenceBackend(forceCpu)` 切后端。
3. 不引入与 DML output 绑定策略相关的新 UI 选项。

**结果要求:**

- 用户侧语义保持不变：GPU 优先默认，CPU 优先为唯一替代项。

### Task 4: 复核队列去重与释放边界

**文件:**

- `Source/Inference/VocoderRenderScheduler.cpp`
- `Source/Inference/VocoderInferenceService.cpp`
- `Tests/TestMain.cpp`

**做法:**

1. 确认 `VocoderRenderScheduler::submit(Job job)` 对同 `chunkKey` 的 queued job 替换逻辑仍在。
2. 不修改现有 dedup 语义，只补守护测试，防止本轮实现误伤。
3. 释放边界定义为：
   - 每个 run 的 output value / CPU 返回 buffer 为短生命周期。
   - vocoder session 继续常驻复用。
   - 若后续确认 session 内部 scratch 无法通过现有 API 主动回收，再单独开轮次处理 idle/session 重建策略。

## 完成标准

- ARA birth materialization 在提交渲染任务前已经带上 `silentGaps`。
- `DmlVocoder` 改为 device output binding，不再长期保留最大 CPU output tensor。
- 设置页与偏好仍只有 `GPU First` / `CPU First` 两态。
- 现有 `chunkKey` 队列去重机制未被破坏。
- 不引入硬 frame 上限和固定时长强切 chunk。
