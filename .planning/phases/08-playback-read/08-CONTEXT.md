# Phase 8: 统一 Playback Read 核心 - Context

**Gathered:** 2026-04-07
**Status:** Ready for planning

<domain>
## Phase Boundary

Phase 8 只交付 READ-01/READ-02：同一 clip 同一时刻，ARA 与常规播放必须通过同一读路径并输出一致语义；统一读策略固定为 `resampled -> rendered -> dry -> blank/silence`。本阶段不扩展到生命周期映射治理（Phase 9）和状态可视化（Phase 10）。

</domain>

<decisions>
## Implementation Decisions

### 读路径归属与调用边界
- **D-01:** 统一 read API 归属 `PluginProcessor` 共享层（不放在 ARA 专有层）。
- **D-02:** `OpenTunePlaybackRenderer` 只负责区域遍历与时间映射，具体音频读取委托统一 read API。
- **D-03:** 调用边界严格限制为 Processor/ARA renderer；编辑器仅消费状态快照，不允许直接读取 RenderCache 音频数据。

### 回退策略与语义一致
- **D-04:** 锁定严格四级读优先级：`resampled cache -> rendered -> dry -> blank/silence`，禁止路径分叉策略。
- **D-05:** dry/blank 采用 "dry 优先" 解释：当渲染结果不可用但原始音频有效时走 dry；仅在无有效源数据、越界或不可读时进入 blank/silence。
- **D-06:** 混音语义统一，不允许 ARA/常规双语义并存。

### 迁移顺序与验收
- **D-07:** 实施顺序为“先常规播放接统一读核，再接 ARA 路径”。
- **D-08:** Phase 8 验收采用末端总验收（非阶段内逐步 A/B gate）。

### the agent's Discretion
- 统一 read API 的具体函数命名、参数结构与内部拆分（例如单函数或小型 helper 组合）由实现阶段决定。
- 日志字段与日志等级细节由实现阶段决定，但需支持回退链路定位。

</decisions>

<specifics>
## Specific Ideas

- 用户强调：Phase 8 不允许通过“并行结构/双语义并存”达成过渡，必须直接收敛到单一路径。
- 用户确认：不在混音末端额外加入限幅/压缩等处理，以原始统一语义输出为准。

</specifics>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Milestone contracts
- `.planning/ROADMAP.md` — Phase 8 目标、范围边界与成功标准（READ-01/READ-02）。
- `.planning/REQUIREMENTS.md` — READ-01/READ-02 正式需求与跨阶段追踪关系。
- `.planning/PROJECT.md` — 里程碑硬约束（不改动 Standalone 行为，优先复用/迁移 Standalone 核心逻辑）。

### Phase 7 baseline decisions (must carry forward)
- `.planning/phases/07-控制与时间基线收敛/07-01-SUMMARY.md` — 宿主回读快照事实源与 request->readback 传输闭环。
- `.planning/phases/07-控制与时间基线收敛/07-02-SUMMARY.md` — playback/source range 单点时间映射与跨采样率读取换算基线。
- `.planning/phases/07-控制与时间基线收敛/07-VERIFICATION.md` — Phase 7 行为验证口径（为 Phase 8 验收提供前置判据）。

### Code truth sources for unified read path
- `Source/PluginProcessor.cpp` — 常规播放路径当前 RenderCache 读取与 ARA 分流入口（`processBlockForARA` 相关）。
- `Source/ARA/OpenTunePlaybackRenderer.cpp` — ARA 回放路径当前读取逻辑与 range 映射接线点。
- `Source/ARA/OpenTuneDocumentController.cpp` — playback/source range 提供方与 ARA 控制器桥接。
- `Source/Inference/RenderCache.cpp` — `readAtTimeForRate`、chunk 状态与回退判据来源。

### Architecture and merge constraints
- `.planning/codebase/ARCHITECTURE.md` — 线程模型、共享核心与层次边界。
- `.planning/codebase/VST3Merge.md` — Standalone/VST3 条件编译边界与历史结构差异。
- `VST3 References/docs/adr/ADR-0003-ara-playback-controller-interface.md` — ARA 播放控制调用边界参考。

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `PluginProcessor::processBlock` 现有常规播放 RenderCache 读取链路可作为统一读核的起点。
- `OpenTunePlaybackRenderer` 已具备 playback/source range 映射能力，可复用为 ARA 侧调用统一读核的前置层。
- `RenderCache::readAtTimeForRate` 已提供按时间和采样率读取能力，适合作为四级回退前两级核心实现基础。

### Established Patterns
- 条件编译边界已固定：`JucePlugin_Build_Standalone` 与 `JucePlugin_Enable_ARA` 必须保持清晰隔离。
- Phase 7 已建立“宿主回读为事实源”的传输状态模式，Phase 8 不能反向引入本地并行状态。
- 测试基线基于 `Tests/TestMain.cpp` + CTest，可延续用于回归读路径一致性。

### Integration Points
- 常规播放接入点：`Source/PluginProcessor.cpp` 中 clip 渲染读取与混音循环。
- ARA 接入点：`Source/ARA/OpenTunePlaybackRenderer.cpp` 的 block 读取主循环。
- 回退判据接入点：`Source/Inference/RenderCache.cpp` + clip 原始音频有效性判断逻辑。

</code_context>

<deferred>
## Deferred Ideas

- “ARA 与常规路径维持双语义并存”被明确拒绝，作为后续不采纳思路记录（原因：违反 READ-01 与单一路径原则）。

</deferred>

---

*Phase: 08-playback-read*
*Context gathered: 2026-04-07*
