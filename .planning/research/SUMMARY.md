# Milestone v1.1 Research Synthesis — Unified Playback Read

**Scope:** v1.1 playback-read 统一化（VST3/ARA）  
**Date:** 2026-04-06  
**Overall Confidence:** MEDIUM-HIGH

## Executive Summary

v1.1 不是“加新功能”，而是把现有 OpenTune 的播放读取语义收敛成**单一权威链路**：普通播放与 ARA 播放必须走同一 playback-read core，确保“编辑后听到的就是当前结果”。研究结论一致指向：不需要引入新三方库，不需要升级 JUCE/ARA SDK，不需要并行兼容层；核心是把 RenderCache、ARA 生命周期、UI 状态消费关系重新对齐。

推荐实现路线是“先控制面、再读路径、后体验优化”：先统一 transport authority 与时间映射，再完成 ARA/非 ARA 共用读取服务与 clip 生命周期真相源，最后做局部重渲染与可视化状态收口。这样可以先消灭“读错内容/不同步”这类致命问题，再追求性能与平滑体验。

最大风险集中在线程边界与双路径重入：ARA playback controller 线程误用、`processBlockForARA` 与 fallback 并行执行、AudioSource 映射并发读写、以及生命周期悬空回调。缓解策略是：线程白名单+断言、单入口互斥分流、映射容器并发改造、异步回调 token/cancel 机制，以及基于 clip+时间窗的 revision 合并防风暴。

## Recommended Scope

### Stack additions/changes

- **保持现有栈不变（Must）**：C++17、JUCE（当前 vendored master）、ARA SDK 2.2.0、`ResamplingManager(r8brain)`。
- **新增的是内部能力，不是外部依赖**：
  1. `PlaybackReadService`（ARA/普通播放共用读路径）
  2. `ClipRegistry`（AudioSource↔clipId↔renderCache 权威映射）
  3. `RenderInvalidationCoalescer`（revision storm 控制与分段失效）
  4. `RenderWorkSnapshot`（UI overlay 统一只读状态）
- **明确不做**：新重采样库、新缓存数据库层、新事件总线、双渲染管线、Standalone/VST3 兼容层、JUCE/ARA 版本升级。

### Table-stakes vs differentiators

**Table Stakes（v1.1 必须完成）**
1. Edit→Playback 一致性（禁止读过期缓存）
2. DAW transport 单一事实源（ARA controller authority）
3. 采样率与时间轴一致（seek/loop/start 边界不漂移）
4. Start/Stop/Seek 稳定（无旧 chunk 串音、无长静音）
5. 渲染可用性状态可见（Idle/Dirty/Rendering/Ready/Error）

**Differentiators（在正确性稳定后推进）**
- Region-aware 局部重渲染（优先做，哪怕受限版）
- Glitch-minimized 新渲染切换
- 一致性诊断信息（版本/控制调用最小可观测）

**Defer（v2+）**
- VST3 多轨/多 clip 编排扩展
- Standalone 架构或 UI 重构
- 新 ONNX 模型/算法升级
- 跨 DAW 全矩阵优化（v1.1 先打穿 REAPER）

## Architecture Direction

### Integration architecture essentials

1. **单一读路径原则**：`PluginProcessor::processBlock()` 与 `OpenTunePlaybackRenderer::processBlock()` 均委托 `PlaybackReadService::readClipSegment()`，策略顺序固定为 `resampled cache -> rendered chunk -> dry -> blank/silence`。
2. **生命周期真相源上移**：`ClipRegistry` 成为权威映射；Editor 不再持有业务真相，只消费状态。
3. **ARA 桥接职责清晰**：`OpenTuneDocumentController` 仅负责 source/region revision 与生命周期事件上报；重任务异步分发，避免回调线程阻塞。
4. **RenderCache 语义保留并增强**：沿用 `Pending/Running/Blank + desired/published revision`，新增事务窗口合并失效（按 clip+时间区间）以防 storm。
5. **UI 双编辑器隔离不变**：Standalone 与 VST3 editor 继续条件编译隔离，但统一读取 `RenderWorkSnapshot` 展示 overlay。

## Watch Outs

### Top watch-outs

1. **ARA controller 线程违规（HIGH）**：禁止在 render/audio 线程调用 request*；必须加线程白名单与断言。
2. **ARA/fallback 双路径重入（HIGH）**：以 `processBlockForARA()` 返回值做互斥分流，确保同 block 只跑一条路径。
3. **AudioSource map 并发读写（HIGH）**：`audioSourceStates_` 等容器必须并发策略统一（RW lock 或快照容器），否则有 data race/崩溃风险。
4. **异步悬空回调 UAF（HIGH）**：所有延时/重试路径引入 cancellation token + weak guard；对象销毁时集中取消。
5. **时间映射与混音语义错误（HIGH）**：统一 region↔source 映射公式；多 region 全分支使用 add/mix，避免 resample 分支覆盖前序内容。

### Research flags (needs extra validation)

- **需重点研究/验证阶段**：
  - 生命周期与线程安全收敛（并发容器、回调取消、线程边界）
  - DAW 集成验证（REAPER 下 transport 回读闭环、seek/loop 边界）
- **可按标准模式直接推进阶段**：
  - PlaybackReadService 提取与 ARA renderer 委托改造
  - UI overlay 改为 snapshot 只读消费

## Suggested Sequencing

### Phase ordering hints

1. **Phase 1 — 控制与时间基线（Transport + Timeline）**
   - 先统一“谁驱动谁”，建立 host 请求-确认闭环与统一映射公式。
   - 交付：transport authority、range 映射函数、误差预算。

2. **Phase 2 — 统一 playback-read 核心（Correctness First）**
   - 提取 `PlaybackReadService` 并让普通播放先接入，再切 ARA renderer 到同一读服务。
   - 交付：同 clip 同时刻同输出语义，消除 ARA/非 ARA 分叉。

3. **Phase 3 — 生命周期真相源上移（ClipRegistry）**
   - 把 `AudioSource↔clipId↔renderCache` 从 Editor 迁到 Processor/ARA 核心；短期双写验证后移除旧权威逻辑。
   - 交付：编辑器重开/删除 source 后映射稳定，不悬空。

4. **Phase 4 — Invalidation 合并与稳定性硬化**
   - 引入 `RenderInvalidationCoalescer`，将高频编辑合并成 chunk 级最小失效。
   - 交付：revision storm 明显收敛，仍保持最终一致。

5. **Phase 5 — 可视化与体验增强**
   - 接入 `RenderWorkSnapshot` 到两套 UI；在正确性稳定后再做 region-aware 重渲染与平滑切换。
   - 交付：状态可见、等待可解释、体验增强不破坏核心一致性。

> 顺序原则：**先控制面，再正确性闭环，再性能体验**；**先消灭错误播放，再优化等待时间**。
