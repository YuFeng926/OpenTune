# Phase 9: 生命周期绑定与失效收敛 - Context

**Gathered:** 2026-04-07
**Status:** Ready for planning

<domain>
## Phase Boundary

Phase 9 只交付 READ-03 与 STAB-01：
- 建立 AudioSource -> clipId -> renderCache 的核心权威生命周期绑定；
- 在导入 / replace / 删除 / 高频编辑下，仅失效受影响 chunk，抑制 revision storm。

本阶段不扩展到状态可视化与一致性诊断展示（Phase 10 的 STAB-02 / STAB-03），也不改变 Standalone 行为语义（SAFE-01 继续作为硬约束）。

</domain>

<decisions>
## Implementation Decisions

### 生命周期真相源
- **D9-01:** 绑定真相源上移到 `OpenTuneDocumentController + OpenTuneAudioProcessor` 共享核心，UI 不再持有权威绑定状态。
- **D9-02:** `PluginEditor` 仅消费绑定快照与 revision，不负责维护 `AudioSource -> clipId` 关系。

### replace / mapping 语义
- **D9-03:** content 变化时优先对已绑定 clip 执行 in-place replace：保留原 `clipId`。
- **D9-04:** replace 必须复用原 `renderCache`，仅做受影响范围失效；禁止重建 cache。
- **D9-05:** mapping 变化（如 playbackStart 变更）只更新时间映射与 clip start，不触发重建。

### 删除与回收语义
- **D9-06:** `willDestroyAudioSource` 走单向收敛：解绑映射、清理 pending/running 关联状态、删除对应 clip（若存在），确保无悬空引用。

### 失效收敛
- **D9-07:** 失效范围以“音频 diff 区间 与 编辑区间”并集为准，仅标记重叠 chunk 为 pending。
- **D9-08:** 沿用 RenderCache `desiredRevision` 去抖模型，不引入并行旧新链路或兼容层。

### The Agent's Discretion
- 核心映射索引的数据结构与并发控制实现。
- 失效区间合并与边界取整策略。
- 诊断日志字段、节流与输出级别细节。

</decisions>

<specifics>
## Specific Ideas

- 用户已明确确认：**replace 必须保留原 clipId 并复用原 renderCache，仅做范围失效，不允许重建 cache**。
- 该确认直接作为 Phase 9 实现硬约束，优先级高于“最小改动”或“实现简化”。
- 结构目标是一次性收敛到单真相源，不接受 UI/Core 并行维护的过渡态。

</specifics>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Milestone contracts
- `.planning/ROADMAP.md` — Phase 9 目标、依赖与成功标准（READ-03 / STAB-01）。
- `.planning/REQUIREMENTS.md` — READ-03 与 STAB-01 正式需求定义。
- `.planning/PROJECT.md` — SAFE-01 硬约束（Standalone 行为不变）。

### Phase 8 baseline（must carry forward）
- `.planning/phases/08-playback-read/08-CONTEXT.md` — 统一读核边界与调用约束。
- `.planning/phases/08-playback-read/08-04-SUMMARY.md` — ARA/常规共用读 API 与四级回退完成态。
- `.planning/phases/08-playback-read/08-VERIFICATION.md` — Phase 8 验收与剩余阻断（人工验证）。

### Code truth sources for lifecycle binding
- `Source/ARA/OpenTuneDocumentController.h` / `Source/ARA/OpenTuneDocumentController.cpp` — AudioSource 状态、revision、生命周期回调入口。
- `Source/ARA/AudioSourceState.h` — AudioSource 侧 `audioBuffer/sampleRate/renderCache` 持有结构。
- `Source/Plugin/PluginEditor.cpp` — 当前 ARA 导入/同步逻辑（现存 UI 绑定语义，需要下沉）。
- `Source/PluginProcessor.cpp` — `commitPreparedImportClip`、`replaceClipAudioById`、`deleteClipById`、`enqueuePartialRenderById`。
- `Source/Inference/RenderCache.h` / `Source/Inference/RenderCache.cpp` — revision 状态机与 pending 收敛机制。

### Architecture and merge constraints
- `.planning/codebase/ARCHITECTURE.md` — 线程模型与共享层边界。
- `.planning/codebase/VST3Merge.md` — Standalone/VST3 条件编译隔离约束。

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `OpenTuneDocumentController` 已具备 `audioSourceStates_` 与 content/mapping revision，可直接承载生命周期真相源。
- `RenderCache::requestRenderPending/getNextPendingJob/completeChunkRender` 已具备 revision 去抖，不需要新增并行调度体系。
- `enqueuePartialRenderById` 已支持局部区间触发，可作为范围失效执行器。

### Structural Gaps to Close
- 现有 ARA->clip 绑定仍在 `PluginEditor` 层（`AraImportedClipBinding`），不满足 READ-03 的核心层真相源要求。
- `replaceClipAudioById` 当前会重建 `renderCache`，与 STAB-01 目标冲突，需改为复用 cache + 局部失效。
- 删除路径尚未将 AudioSource 生命周期与 clip 清理做强一致收敛。

### Integration Points
- 导入：`recordRequested` / `syncImportedAraClipIfNeeded`（现 UI 编排）需迁移为核心编排。
- 替换：`replaceClipAudioById` 需要从“重建 cache”切到“保留 cache + 失效区间”。
- 删除：`willDestroyAudioSource` 触发 processor 侧 clip 清理与映射回收。

</code_context>

<acceptance>
## Acceptance Targets (Phase 9)

- **READ-03:** 导入、replace、删除后，AudioSource 命中当前有效 clip 映射；不存在悬空引用与陈旧绑定。
- **STAB-01:** 高频编辑与 replace 下仅受影响 chunk 进入 pending；无全量 cache 失效风暴。
- **SAFE-01 (guard):** Standalone 分支行为与交互语义保持不变。

</acceptance>

<deferred>
## Deferred Ideas

- 重建 cache 的 replace 路径（已拒绝：违背 STAB-01）。
- UI 持有权威绑定状态（已拒绝：违背单真相源）。

</deferred>

---

*Phase: 09-lifecycle-binding*
*Context gathered: 2026-04-07*
