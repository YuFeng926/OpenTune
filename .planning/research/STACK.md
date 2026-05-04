# Milestone v1.1 Stack Research — Unified Playback Read 架构

## Context

本次里程碑是**增量架构重构**，不是技术栈换代：

- 已有能力（ONNX、RenderCache、Standalone 多轨 UI、VST3 ARA 基线）不再重研。
- 新增能力集中在 4 件事：
  1. ARA/普通播放共用一套 playback read core（含 RenderCache 时间读取、重采样、dry/blank 策略）
  2. AudioSource ↔ clipId/renderCache 生命周期安全绑定
  3. VST3 渲染 overlay 状态数据化
  4. revision storm 控制 + partial invalidation 语义

结论先行：**不需要新增第三方库**；需要的是在现有 C++17 + JUCE + ARA + RenderCache 体系内做接口收敛与语义固化。

---

## Required Stack Decisions

### 1) 继续使用现有核心栈（Must-have）

| 决策 | 版本/标准 | 集成点 | 结论 |
|---|---|---|---|
| 语言与并发原语 | C++17（现状） | `Source/PluginProcessor.*`, `Source/ARA/*`, `Source/Inference/RenderCache.*` | **保持不变**，用现有 `std::atomic`/JUCE 锁即可 |
| 框架 | JUCE（当前 vendored master） | `AudioProcessorARAExtension`, `ARADocumentControllerSpecialisation`, `ARAPlaybackRenderer` | **保持不变**，统一 playback core 挂到现有 renderer/processor 路径 |
| ARA SDK | 当前工程配置 2.2.0（可继续） | `Source/ARA/OpenTuneDocumentController.*` | **本里程碑不升级 SDK**，避免非目标风险 |
| 重采样 | 现有 `ResamplingManager`（r8brain backend） | `Source/DSP/ResamplingManager.*`, `RenderCache::addResampledChunk/readAtTimeForRate` | **保持单一重采样实现** |

### 2) 新增“内部栈能力”而非新依赖（Must-have）

> 这里的“新增”是模块/契约，不是第三方库。

| 能力 | 需要的栈变化 | 必须集成点 |
|---|---|---|
| 统一 playback read core | 提炼单一读取契约（建议 `PlaybackReadService`/`PlaybackReadCore`） | 被 `PluginProcessor::processBlock`（普通播放）和 `OpenTunePlaybackRenderer::processBlock`（ARA）共同调用 |
| 生命周期安全绑定 | 在 `AudioSourceState`/`OpenTuneDocumentController` 固化绑定元数据：`clipId + contentRevision + mappingRevision (+generation/token)` | `didUpdateAudioSourceProperties` / `didAddPlaybackRegion...` / `willDestroyAudioSource` / VST3 editor 导入绑定逻辑 |
| Overlay 数据化 | 统一以 `RenderCache::ChunkStats + clipId` 作为 UI 状态源；UI 只消费状态，不推导内部渲染细节 | `Source/Plugin/PluginEditor.cpp` 的 overlay 更新与 `getClipChunkStatsById()` |
| Revision storm 控制 | 基于现有 `desiredRevision/publishedRevision + Pending/Running/Blank` 状态机做**分段失效**，禁止全量 clear 触发风暴 | `RenderCache::requestRenderPending/completeChunkRender/markChunkAsBlank/getNextPendingJob` 与 `chunkRenderWorkerLoop` |

### 3) 必须明确的语义标准（Must-have）

1. **读优先策略顺序固定**：`resampled cache -> base rendered chunk -> dry -> blank/silence`。  
2. **blank 是一等状态**（不是失败）：当无可渲染 F0 时进入 `Blank`，并可被后续编辑触发转回 `Pending`。  
3. **partial invalidation 最小粒度 = 时间区间对应 chunk**，不允许“编辑一次清全 clip cache”。  
4. **revision 单调递增且按 clip 作用域隔离**，杜绝跨 clip 污染。

---

## Optional Tooling

> 可选项仅用于研发效率，不是里程碑交付前提。

1. **现有 CTest/OpenTuneTests 增补语义测试（推荐）**  
   - 覆盖：dry/blank policy、revision 过期重投递、AudioSource 销毁后绑定失效。  
   - 不引入新测试框架。

2. **日志标签规范化（推荐）**  
   - 统一 `PlaybackReadTrace / RenderTrace / ARA-BindingTrace` 前缀，便于 storm 定位。  
   - 继续使用现有 `AppLogger`，不引入新 observability 平台。

---

## Do Not Add

为防止架构漂移，本里程碑明确**不新增**：

1. **不新增重采样库**（如 libsamplerate/soxr）——已有 `ResamplingManager + r8brain` 足够。  
2. **不新增缓存/数据库层**（SQLite/Redis/自建索引服务）——RenderCache 已具备状态机与版本语义。  
3. **不新增事件总线/响应式框架**（Rx/消息中间层）——当前调用链可直接表达数据流。  
4. **不做 Standalone/VST3 兼容层封装**——坚持现有条件编译边界，避免并行结构。  
5. **不引入第二套 playback 渲染管线**——ARA 与普通播放只能共享同一 read core。  
6. **不进行 JUCE/ARA SDK 版本升级**（本里程碑）——该变更与目标无直接收益且风险高。

---

## Rationale

1. **范围匹配**：需求是“统一读取语义 + 生命周期绑定 + 状态可观测 + revision 控制”，都属于现有内核能力的结构化重组，不是生态能力缺失。  
2. **当前代码已具备关键基础**：
   - `RenderCache` 已有 `Pending/Running/Blank`、`desiredRevision/publishedRevision`、`readAtTimeForRate`、`addResampledChunk`。
   - ARA 控制器已有 `AudioSourceState`、content/mapping revision、播放区间更新与销毁回收。
   - VST3 编辑器已使用 `getClipChunkStatsById()` 驱动 overlay 完成判断。
3. **工程约束匹配**：项目明确禁止兼容层、止血式改动；因此最正确做法是收敛为单一 playback read core，而非叠加新库和新并行路径。

### Confidence

- **Stack 决策：HIGH**（以代码现状 + JUCE 官方 ARA 文档为依据）
- **不升级 ARA SDK 的建议：MEDIUM**（技术上可升级到 ARA SDK 2.3，但与本里程碑目标弱相关，建议后置为独立里程碑）

### Sources

- 项目上下文：
  - `.planning/PROJECT.md`
  - `.planning/ROADMAP.md`
  - `.planning/REQUIREMENTS.md`
  - `Source/Inference/RenderCache.h`
  - `Source/ARA/OpenTuneDocumentController.h`
  - `Source/ARA/OpenTuneDocumentController.cpp`
  - `Source/ARA/OpenTunePlaybackRenderer.h`
  - `Source/ARA/OpenTunePlaybackRenderer.cpp`
  - `Source/Plugin/PluginEditor.cpp`
- 官方文档：
  - JUCE `ARAPlaybackRenderer` 文档：https://docs.juce.com/master/classjuce_1_1ARAPlaybackRenderer.html
  - JUCE `ARADocumentControllerSpecialisation` 文档：https://docs.juce.com/master/classjuce_1_1ARADocumentControllerSpecialisation.html
  - JUCE `AudioProcessorARAExtension` 文档：https://docs.juce.com/master/classjuce_1_1AudioProcessorARAExtension.html
  - ARA SDK 仓库（发布信息与版本上下文）：https://github.com/Celemony/ARA_SDK
