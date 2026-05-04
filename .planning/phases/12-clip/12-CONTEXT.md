# Phase 12: Clip 真相源与映射收敛 - Context

**Gathered:** 2026-04-13
**Status:** Ready for planning

<domain>
## Phase Boundary

Phase 12 只交付 `TRUTH-02` 与 `REV-01`:
- ARA/VST3 播放链路先统一解析到同一 `clipId`，再通过 `clipId -> RenderCache` 读取；
- `mapping-only` 变化只更新映射语义，不进入 render queue。

本阶段不并入 Phase 13 的旧队列 revision/epoch 收敛，也不把 Phase 11 的 REAPER cut-time focus bug 扩写成 clip 真相源架构重做。Standalone 行为保持不变仍是硬约束。

</domain>

<decisions>
## Implementation Decisions

### 播放真相源与读取边界
- **D12-01:** `clipId -> PlaybackReadSource -> readPlaybackAudio()` 继续作为 ARA/VST3 唯一播放读取链路；禁止回退到 adapter 私有 cache、`AudioSourceState` 旁路或 `track 0` 约定。
- **D12-02:** `OpenTunePlaybackRenderer` 继续只负责宿主时间映射与 block 遍历，具体读取统一委托共享 processor 读 API。
- **D12-03:** `AudioSourceState` 只承载宿主音频缓冲与基础元数据，不重新承担 `RenderCache` 或第二读取事实源职责。

### mapping-only 语义与记账边界
- **D12-04:** `mapping-only` 的唯一共享层语义是“保持同一 `clipId`，更新映射并 bump `mappingRevision`”；不得触发 `enqueuePluginClipPartialRender()`。
- **D12-05:** 宿主 mapping 变化可以由 ARA adapter 检测，但最终语义记账必须落回 processor 的 clip semantic state；禁止把 adapter revision 当作最终真相源。
- **D12-06:** `setPluginClipStartSeconds(...)` 这类入口在 Phase 12 必须升级为 clip semantic write path，而不是继续只做裸 `startSeconds` 改写。

### track / 容器残留边界
- **D12-07:** plugin 当前 `track 0 + selectedClipIndex` 只允许继续作为“当前编辑对象选择机制”；任何播放、binding、render 语义都不得再把它当真相源。

### Phase boundary guard
- **D12-08:** 旧 `RenderCache / pending / revision` 队列核心保持不变；Phase 12 只负责把 `mapping-only` 从入队链路中彻底隔离，不提前并入 Phase 13 的 `renderRevision/layoutEpoch` 队列契约。
- **D12-09:** Phase 11 的 cut-time focus bug 继续单独跟踪，只要求 Phase 12 方案不要再引入第二套时间基线。

### the agent's Discretion
- `mapping-only` 共享层 API 的具体命名、参数结构与内部 helper 拆分。
- adapter revision 到 processor `mappingRevision` 的同步方式与日志细节。
- 测试落点是在现有 `Tests/TestMain.cpp` 扩展，还是顺带清理 Phase 11 characterization 覆盖结构。

</decisions>

<specifics>
## Specific Ideas

- 当前 live code 已经做到：ARA 播放通过 `binding.clipId -> getPluginPlaybackReadSource(...) -> readPlaybackAudio()` 读取，不再直接持有 adapter render cache。
- 当前 live code 也已经有 `mapping-only` 不入 render queue 的 editor 分支，但 `mappingRevision/renderRevision/layoutEpoch` 还没有真正成为共享层的写入语义。
- 本阶段的结构重点不是“再发明一个 clip-centric 读取入口”，而是把已经成型的 clip-centric 读取链路做成唯一真相源，并把 mapping 变化的语义记到账本补齐。

</specifics>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Milestone contracts
- `.planning/PROJECT.md` — v1.2 的 clip 真相源、变化分类与 Standalone 守护约束。
- `.planning/REQUIREMENTS.md` — `TRUTH-02` 与 `REV-01` 的正式 requirement。
- `.planning/ROADMAP.md` — Phase 12 目标、依赖与成功标准。
- `.planning/STATE.md` — 当前里程碑状态与 Phase 11 cut-time focus blocker。

### Prior phase context to carry forward
- `.planning/phases/08-playback-read/08-CONTEXT.md` — 统一 read API 归属共享 processor，ARA renderer 只做映射。
- `.planning/phases/09-lifecycle-binding/09-CONTEXT.md` — `AudioSource -> clipId -> renderCache` 生命周期绑定与 mapping 不重建 cache。
- `.planning/phases/11-最小状态模型定稿/11-STATE-MODEL.md` — `clipId / renderRevision / mappingRevision / layoutEpoch / RenderCache / RenderTask` 六个最小抽象。
- `.planning/phases/11-最小状态模型定稿/11-02-SUMMARY.md` — Phase 11 已把读路径和 editor/ARA 读取入口收敛到 clip-centric 语义。

### Live code truth sources for Phase 12
- `Source/ARA/OpenTunePlaybackRenderer.cpp` — ARA 播放入口、宿主时间映射和统一读取接线点。
- `Source/ARA/OpenTuneDocumentController.h` / `Source/ARA/OpenTuneDocumentController.cpp` — `AudioSourceClipBinding`、adapter revision 与 lifecycle binding 真相源。
- `Source/ARA/AudioSourceState.h` — AudioSource 持有结构，确认无第二播放事实源。
- `Source/PluginProcessor.h` / `Source/PluginProcessor.cpp` — `ClipSemanticState`、`getPluginPlaybackReadSource(...)`、`readPlaybackAudio(...)`、clip semantic write path。
- `Source/Plugin/PluginEditor.cpp` — `syncImportedAraClipIfNeeded()` 的 mapping-only / content-change 分流与 VST3 clip 同步链路。
- `Source/Inference/RenderCache.h` / `Source/Inference/RenderCache.cpp` — 旧队列与 `desiredRevision` 机制，作为 Phase 12 不可越界重写的边界。

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `OpenTunePlaybackRenderer` 已经先从 binding 解析 `clipId`，再经 processor 获取 `PlaybackReadSource` 并统一走 `readPlaybackAudio()`；这是 Phase 12 的正确播放基线。
- `ClipSemanticState` 已经把 `mappingRevision / renderRevision / layoutEpoch / publishedRenderRevision` 暴露为按 `clipId` 可读的共享状态。
- `syncImportedAraClipIfNeeded()` 已经存在 mapping-only 分支，可作为“mapping-only 不入 render queue”的现成接缝。

### Structural Gaps to Close
- `mappingRevision / renderRevision / layoutEpoch` 目前主要还是“可读字段”，缺少与真实写路径对应的 bump 逻辑。
- `mappingChanged` 现在仍主要来自 ARA adapter revision，而不是共享 processor 的 clip semantic write path。
- plugin 当前活跃 clip 解析仍经 `track 0 + selectedClipIndex` 选择，虽不再是读取真相源，但仍可能误伤后续实现语言。

### Integration Points
- 播放读取：`binding.clipId -> getPluginPlaybackReadSource(...) -> readPlaybackAudio()`。
- mapping-only 写入口：`syncImportedAraClipIfNeeded()` 到 processor 的 start/mapping 更新接口。
- queue 边界：`enqueuePartialRenderById()` -> `RenderCache::requestRenderPending()`，本阶段只允许 content/render 变化走这条链。

</code_context>

<acceptance>
## Acceptance Targets (Phase 12)

- **TRUTH-02:** ARA/VST3 播放统一命中 `clipId -> RenderCache` 绑定；不存在 adapter 私有 cache、`AudioSourceState::renderCache` 旁路或 `track 0` 读取事实源。
- **REV-01:** `mapping-only` 变化只 bump `mappingRevision` 并更新映射，不触发 render queue invalidation。
- **SAFE-02 guard:** Standalone 分支行为与交互语义不因 Phase 12 改动而改变。

</acceptance>

<deferred>
## Deferred Ideas

- 在 Phase 12 里把 `renderRevision/layoutEpoch` 直接接进旧 `RenderCache` revision 队列（已拒绝：属于 Phase 13 范围）。
- 把 Phase 11 的 cut-time focus bug 扩写为 clip 真相源/播放链路重做（已拒绝：当前证据指向 VST3 卷帘时间基线，而不是读取真相源失配）。
- 继续让 adapter revision 兼任共享层最终版本语义（已拒绝：会形成第二真相源）。

</deferred>

---

*Phase: 12-clip*
*Context gathered: 2026-04-13*
