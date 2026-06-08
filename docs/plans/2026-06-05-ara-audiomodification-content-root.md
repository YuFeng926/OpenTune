# ARA AudioModification Content Root Implementation Plan

**Goal:** Make `AudioModification` the ARA content root and remove the store split that made content truth live outside the official ARA object model.

**Architecture:** `AudioModification` owns editable content state. `PlaybackRegion` owns placement only. `OpenTuneDocumentController` coordinates ARA graph lifecycle, persistence, and notifications without becoming a content store. Render/cache/worker code becomes a concrete derived service that consumes immutable content snapshots.

**Migration stance:** This is not a greenfield rewrite of rendering. Keep and migrate the existing working assets where they match the new model: `RenderCache`, `TimeStretchCache`, chunk scheduling, immutable playback snapshots, and the dry-audio plus rendered-overlay read path. Delete `MaterializationStore` as content truth and as a content-routing abstraction.

**Tech Stack:** C++17, JUCE 8, ARA SDK 2.2, CMake presets with Visual Studio/MSBuild, existing `OpenTuneTests` source-scan and compile-contract style.

---

## 测试策略

测试使用 source-scan 契约模式（compile-time contract tests）：
- 扫描源码文本，检查必需 token 存在、禁止 token 不存在
- 确保架构边界在编译时可验证
- 测试文件：`Tests/TestMain.cpp`

关键测试维度：
1. **AudioModification 内容所有权** - 必须有 ContentKey/content/snapshotContent，禁止 materializationId
2. **PlaybackRegion placement-only** - 只有 placement 字段，禁止 content 字段
3. **无 DC/Processor store split** - 禁止 DC 和 Processor 各自拥有 materializationStore_
4. **Renderer assigned-region role** - 使用 host assigned regions，禁止 preferred region
5. **ARA lock-free** - Source/ARA 目录禁止 mutex/lock_guard/thread（允许 atomic<bool> 生命周期信号）
6. **无 fallback routing** - 禁止 `if (dc) else processor` 双 backend

---

## 当前状态（2026-06-08 更新）

### 已完成里程碑

**commit `26de6d1` - ARA 编辑器读路径迁移**
- ✅ ODC 新增 17 个只读访问器 (`readAudioBuffer`, `readPitchCurve`, etc.)
- ✅ PluginContentAccessInline ARA 分支读 `AudioModification.content + CRS`
- ✅ asyncLeaseToken_ 租约模型（生命周期收口）
- ✅ Processor 析构 Phase 1-6
- ✅ F0InferenceService shutdown 去阻塞
- ✅ 双 contentRevision 合并

### 未完成的关键路径（阻塞最终目标）

🔴 **Critical - 数据安全/一致性风险**
1. **ARA 写路径不写 AudioModification.content** - 编辑器读从 DC，写到 processor store，数据不一致
2. **ARA 持久化仍用旧 store schema** - `getContentSnapshot()` 序列化 materializationStore（但 ARA birth 不写它），保存/恢复会丢失数据
3. **DC/Processor MaterializationStore 双 store 并存** - 内容 truth 不唯一，迁移路径被阻塞

🟠 **High - 架构正确性风险**
4. **Renderer render plan 非线程安全** - `renderItems_` 普通 vector，message thread 写、audio thread 读
5. **F0 async result 缺 revision guard** - 只用 ContentKey 匹配，旧 F0 可能覆盖新 birth
6. **TimeStretchCache 仍用 materializationId key** - 不符合 ContentKey + revision contract
7. **Non-ARA domain owners 只有壳** - Standalone/VST3 仍依赖旧 store

### Task 完成度矩阵

- **Task 0** (Tests): ❌ 未完成 - 测试契约过期，仍要求旧架构存在
- **Task 1** (Content State Types): ⚠️ 大部分完成 - 类型已创建，但 ContentKey 用 hash、snapshot 缺字段
- **Task 2** (AudioModification Ownership): ⚠️ 结构完成，权威性未完成 - content 字段存在，但写路径不写它
- **Task 3** (ContentRenderService): ⚠️ 部分完成 - CRS 存在但 MaterializationStore 仍是 content owner
- **Task 4** (Processor Services Bridge): ⚠️ F0 ARA path 完成，其他未完成
- **Task 5** (ARA Birth): ⚠️ birth read path 完成，restore/persistence 未完成
- **Task 6** (Non-ARA Domain): ❌ 只有壳，未真正接管
- **Task 7** (Renderer Read Path): ✅ 基本完成，render plan 需改 immutable snapshot
- **Task 8** (Editor Content Access): ⚠️ 只读 ARA 分支完成，写路径/dual backend 未删除
- **Task 9** (Kill List Review): ❌ 未通过 - materializationId/store split/dual backend 大量残留

### ARA2 规范符合性审查结果

✅ **符合 ARA2 官方模型**
- AudioSource 职责正确（只负责 sample access）
- PlaybackRegion placement-only（不复制内容）
- Renderer 使用 assigned regions（不依赖 global preferred region）
- 多 region overlap 混合符合官方语义

⚠️ **偏离/妥协**
- ContentKey 用 hash 代替 persistent ID（碰撞风险、不透明）
- DC 持有 retired content pool（接近 content owner，偏离 coordinator 职责）
- CRS 持有 dry audio buffer（边界模糊：derived vs source-audio owner）

❌ **违反规划原则的残留**
- MaterializationStore 仍是事实 content owner
- DC/Processor store split 仍存在
- ARA 写路径仍写 processor store
- ARA persistence 仍是旧 materialization archive
- Kill list (Task 9) 远未通过

### 重复建设问题（2026-06-08 Oracle 审查）

🔴 **Critical - 不复用旧轮子，重新造轮子**

当前迁移存在严重的重复建设反模式：MaterializationStore 已有成熟稳定的基础设施，但 ContentRenderService 重新实现了一套几乎相同的代码。

**数据结构重复：**
- `PlaybackReadSource`：MaterializationStore.h:64-92 vs ContentRenderService.h:48-67（**字段完全相同**，只差 key 类型）
- `PendingRenderJob`：MaterializationStore.h:121-133 vs ContentRenderService.h:72-86（**字段完全相同**，只差 key 类型）

**基础设施重复：**
- playback source cache（atomic snapshot publisher）：Store L317-320 vs CRS L178-179
- RenderCache 管理（getOrCreate、remove、lookup）：Store getRenderCache() vs CRS L127-129
- SoundTouchStretcher pool：Store Entry.stretcher vs CRS stretchers_ vector
- 渲染工作器（thread、queue、cv、pause/resume）：Store 已删除 vs CRS L189-199
- TimeStretchCache：当前委托 CRS，但 key 仍是 materializationId

**新实现的 bug：**
- CRS `PendingRenderJob.targetRevision` 存在（L83），但 `enqueueRender()` 不复制到 `PendingRenderEntry`（ContentRenderService.cpp:111-128），revision 校验缺失
- TimeStretchCache key 仍用 `uint64_t materializationId`，不同 domain 的 objectId 可能碰撞

**AudioModificationContentState 缺失播放必需字段：**
- `audioBuffer`：播放、波形、render 输入必需，但当前只在 CRS playback source 中，导致 CRS 变成实际 source owner
- `renderRevision`：stale render 判断必需，当前只有 `contentRevision`

**Oracle 推荐方案 A（最小改动）：**
> MaterializationStore 原地改造成 ContentKey keyed 的统一 ContentStoreCore，删除 CRS 重复实现，复用所有成熟基础设施（cache、worker、snapshot publisher、stretcher pool）。只改 key 类型（materializationId → ContentKey），其他基础设施全部复用。

**架构原则：**
- 复用旧轮子的机械部件，不复用旧语义污染
- ContentPayloadState 从 MaterializationSnapshot 抽取权威字段（notes/pitch/timeGrid/audioBuffer），不包含 derived runtime（renderCache/stretcher）
- CRS 删除或退化为薄 facade，不再 own map/cache/worker
- 接口形式可以变（key、命名），但底层实现必须复用

**风险：**
- 不采纳复用方案：两套基础设施持续分叉，每次 bug fix 修两遍，数据一致性风险
- 采纳复用方案：必须守住 ARA2 边界（AudioModification 是 content root，不能让 Store 继续作为全局 owner）

### 下一步优先级（P0 必须先修）

**Phase 0（前置）：停止重复建设，复用成熟基础设施**
1. **MaterializationStore key-generalize** - 把 `std::map<uint64_t, Entry>` 改成 `std::map<ContentKey, Entry>`，API 从 `getSnapshot(uint64_t)` 改成 `getSnapshot(ContentKey)`
2. **统一 PlaybackReadSource / PendingRenderJob** - 删除 CRS 重复定义，改用 key-generalized Store 版本
3. **删除 CRS 重复基础设施** - 删除 CRS playbackSources_/renderCaches_/stretchers_/worker，改为 Store core 复用
4. **TimeStretchCache 改 ContentKey** - key 从 `materializationId` 改成 `ContentKey + pitchRevision + timeGridRevision`

**Phase 1（数据安全/一致性）：**
5. **修复 ARA 写路径** - 所有 ARA editor writes 必须落到 `AudioModification.content`，删除 ARA processor-store writes
6. **修复 ARA persistence** - 保存/恢复 `AudioModificationContentState`，不保存旧 materialization store
7. **删除 ARA DC materializationStore_** - DC 只协调 ARA graph，不拥有 content store

---

## Non-Goals

- Do not implement ARA ContentReader exposure unless a later requirement asks for host-visible content. It is valid to leave content unavailable to the host when this is an explicit design choice.
- Do not add a compatibility layer that keeps old store-backed ARA editing alive.
- Do not introduce global preferred-region rendering.
- Do not keep `MaterializationStore` as a content owner under a new name.
- Do not make `OpenTuneDocumentController` the content owner. It owns document coordination, not editable content truth.
- Do not rewrite `RenderCache`, `TimeStretchCache`, or chunk rendering from zero when existing behavior can be extracted into the new service.
- Do not treat asset reuse as permission to keep store-backed content routing, `materializationId` identity, or DC/processor store fallback paths.

---

## Object Model Contract (ARA2 Official)

### AudioSource
`AudioSource` owns source identity and host sample access. It does not own editable notes, pitch correction, time-grid edits, pitch-shift settings, or render cache.

### AudioModification
`AudioModification` owns the editable content state for a source window.

Required owned state:
- Source provenance: source persistent ID and source window.
- Analysis state: original F0 readiness, pitch curve, detected key, silent gaps, and reference features.
- Editable state: notes, corrected pitch segments, time grid, pitch shift settings.
- Revisions: notes, pitch, time grid, pitch shift, and aggregate content revision.
- Lifecycle state: unloaded, reading source audio, analyzing, ready, failed, and retired.

The ARA content identity is the `AudioModification` object or its persistent ID, not a materialization ID.

### Retired Content
`retired` is a lifecycle state of the same content identity, not a second store. It replaces the current `retireMaterialization()` / `reviveMaterialization()` semantics used by split, merge, delete, refresh, and undo.

Rules:
- Active content is owned by the domain content owner: `AudioModification`, standalone clip content, or regular VST3 capture content.
- Retiring content moves its authoritative state into a `RetiredContentRecord` keyed by the same `ContentKey`.
- Retired records are invisible to renderer/editor active lookup.
- Undo/revive moves the record back to its domain owner and bumps content revision.
- Reclaim releases retired records that are no longer referenced by placements, undo actions, or pending revive operations.
- Derived caches are not retired content. `ContentRenderService` clears or rebuilds them from the revived content snapshot.

Do not make `OpenTuneDocumentController` a content pool. For ARA, the controller coordinates host lifecycle and can ask the content domain to retire or release records, but it does not become the owner of editable content truth.

### PlaybackRegion
`PlaybackRegion` references an `AudioModification` and owns placement only: timeline start, region offset, duration, and host placement identity. It must not own or duplicate editable content.

### OpenTuneDocumentController
`OpenTuneDocumentController` owns ARA graph coordination:
- Register and look up `AudioSource`, `AudioModification`, and `PlaybackRegion` objects.
- Build projection data for editor selection and renderer assignment.
- Store and restore ARA archive data by `AudioModification` persistent ID.
- Trigger `AudioModification::notifyContentChanged(..., true)` when content revision changes.

It must not own a `MaterializationStore`, processor-local content store, or any equivalent content truth container.

### OpenTunePlaybackRenderer
The renderer consumes the playback regions assigned to that renderer by the host. It renders every overlapping assigned region and mixes overlaps. It must not render from a global preferred playback region or from editor focus state.

---

## Shared Content/Render Service Contract

Create a neutral content snapshot/service boundary outside the ARA object layer:
- `ContentKey`: domain-neutral identity for a content root.
- `DomainContentOwner`: minimal owner interface implemented by ARA, standalone, and regular VST3 content roots.
- `EditableContentState`: authoritative editable content fields.
- `EditableContentSnapshot`: immutable read model for render/cache/worker code.
- `ContentCommands`: edit operations that mutate the owning domain object and bump revisions.
- `ContentRenderService`: consumes `EditableContentSnapshot` and publishes derived render artifacts.

ARA uses `AudioModification` as the owner of `EditableContentState`.

Standalone and regular VST3 must use their own domain owners before final migration is accepted. They reach render/cache/worker code through the same service interfaces. They must not reuse a generic `MaterializationStore` as the shared content owner.

### ContentRenderService（修订后定义）

⚠️ **当前状态（2026-06-08）：CRS 重复实现了 MaterializationStore 的基础设施，违反复用原则。**

**应有架构（方案 A - 最小改动）：**
- MaterializationStore 原地改造成 `ContentStoreCore`（key 从 `materializationId` 改 `ContentKey`）
- CRS 删除重复实现，退化为薄 facade 或直接删除
- 复用 Store 已有的：playback snapshot publisher、RenderCache 管理、Stretcher pool、worker 基础设施

**如果保留 CRS（作为薄 facade）：**
```cpp
class ContentRenderService {
public:
    explicit ContentRenderService(ContentStoreCore& core);
    bool getPlaybackReadSource(ContentKey key, PlaybackReadSource& out) const;
    void enqueueRender(PendingRenderJob job);
    TimeStretchCache& getTimeStretchCache();
private:
    ContentStoreCore& core_;  // 不 own，只引用
};
```

**CRS 应拥有的（derived state only）：**
- ❌ ~~playbackSources_ map~~ → 复用 Store playback snapshot cache
- ❌ ~~renderCaches_ map~~ → 复用 Store per-entry RenderCache
- ❌ ~~stretchers_ vector~~ → 复用 Store per-entry Stretcher
- ❌ ~~worker thread/queue/cv~~ → 复用 Store render worker
- ✅ TimeStretchCache（但 key 必须改 ContentKey + revisions）

**CRS 不能拥有的：**
- notes、pitch curves、time grids、pitch-shift settings
- detected key、source provenance、content lifecycle truth
- audioBuffer（source audio payload 属于 content，不属于 derived cache）

**禁止的操作：**
- 重新实现 MaterializationStore 已有的稳定基础设施
- 在 render/cache miss 时创建第二个 content truth
- Fallback 到 processor-local 或 DC-local store lookup

### Migration Rules
- Tests first. Contract tests must fail before implementation.
- Every migration checkpoint must remove an old content path when the new path becomes active.
- No fallback content routing. A failed content lookup is a bug to fix at the owner/projection boundary, not a reason to try another store.
- Render/cache/worker code may cache derived artifacts, but cannot mutate authoritative content directly.
- ARA code remains lock-free: no mutex, critical section, blocking wait, or hidden store lock in `Source/ARA`.

---

## 实现路线图（高层）

迁移分为 9 个主要阶段，当前完成度见"当前状态"章节。详细实现步骤已删除，以实际代码状态为准。

### Task 0: Contract Tests
编写 source-scan 契约测试，验证架构边界。

### Task 1: Content State Types
定义 ContentKey、DomainContentOwner、AnalysisState、EditableContentState、AudioModificationContentState、RetiredContentRecord、EditableContentSnapshot。

### Task 2: AudioModification Ownership
AudioModification 拥有 by-value content 字段，实现 snapshotContent()、applyXxx() 命令、retire/revive 支持。

### Task 3: Extract ContentRenderService
从 MaterializationStore 抽取 derived state（RenderCache、TimeStretchCache、worker、playback snapshots）到 ContentRenderService。

### Task 4: Processor Services Result Bridge
F0/analysis/render 服务返回 typed results，由 content owner 或 CRS 应用，不写 processor-local store。

### Task 5: ARA Birth And Restore
birth 填充 AudioModification.content，persistence 保存/恢复 AudioModificationContentState（不保存旧 store）。

### Task 6: Non-ARA Domain Ownership
StandaloneClipContent、CaptureSegmentContent 实现 DomainContentOwner，删除共享 MaterializationStore。

### Task 7: Renderer Read Path
Renderer 从 host assigned regions → AudioModification content snapshots → CRS playback sources，混合 overlaps。

### Task 8: Editor Content Access
Editor 读写 AudioModification.content（ARA）或 domain owner（non-ARA），删除 dual backend / materializationId API。

### Task 9: Kill List Review
最终审查：禁止 MaterializationStore content owner、materializationId、DC/processor store split、fallback routing、dual backend、重复建设。

**必须删除的重复建设（2026-06-08 新增）：**
- [ ] 删除 `ContentRenderService::PlaybackReadSource` 重复定义（ContentRenderService.h:48-67）
- [ ] 删除 `ContentRenderService::PendingRenderJob` 重复定义（ContentRenderService.h:72-86）
- [ ] 删除 CRS 重复 RenderCache 管理（ContentRenderService.h:127-129, .cpp:65-87）
- [ ] 删除 CRS 重复 playback source cache（ContentRenderService.h:178-183, .cpp:21-59）
- [ ] 删除 CRS 重复 SoundTouchStretcher pool（ContentRenderService.h:156-164, .cpp:263-280）
- [ ] 删除 Store ↔ CRS PlaybackReadSource adapter（MaterializationStore.cpp:294-314）
- [ ] 删除 processor oldSource → CRS source conversion（PluginProcessor.cpp:577-591）
- [ ] 删除 `MaterializationContentAccess/MaterializationContentCommands` 兼容层（MaterializationContentProvider.h:13-80）
- [ ] 删除 ARA birth 手写 content construction，改用统一 `CreateContentRequest`
- [ ] 禁止新增"同字段、同语义、只换名字"的 DTO

**必须删除的旧架构残留：**
- [ ] 禁止 `materializationId` identity（改用 ContentKey）
- [ ] 禁止 DC/Processor 各自拥有 `materializationStore_`
- [ ] 禁止 `if (dc) -> X else processor -> Y` dual backend 路由
- [ ] 禁止 ARA 写路径写 processor materializationStore_
- [ ] 禁止 fallback content routing（content lookup 失败应报错，不能尝试另一个 store）
- [ ] 禁止 `MaterializationStore` 作为 content owner（只能作为 key-generalized runtime core）
- [ ] 禁止 PlaybackRegion 持有 content 字段（只能有 placement）
- [ ] 禁止 Renderer 依赖 global preferred region（只能用 host assigned regions）
- [ ] 禁止 Source/ARA 目录包含 mutex/lock_guard/thread（允许 atomic<bool>）
- [ ] 禁止 TimeStretchCache key 用 `materializationId`（必须用 ContentKey + revisions）

---

## 参考文档

- ARA2 SDK Documentation: https://github.com/Celemony/ARA_SDK
- Oracle 架构审查报告（2026-06-08 完成度审查）：见 git commit `e0326f0`
- Oracle 架构审查报告（2026-06-08 重复建设审查）：见 commit `8484362`
- **Oracle 正确架构重构方案（2026-06-08）**：见当前会话 ora-1 完整设计
- 读路径修复计划：`docs/plans/2026-06-08-ara-read-path-lifecycle-fix.md`

## Oracle 审查关键结论

### 完成度审查（commit e0326f0）
1. **当前状态定性**：ARA 读路径迁移里程碑（部分完成），而非完整 AudioModification Content Root
2. **关键架构风险**：3 个 Critical + 4 个 High 风险阻塞最终目标
3. **Task 完成度**：0-9 任务大部分未完成或部分完成

### 重复建设审查（commit 8484362）
1. **重复建设反模式成立**：ContentRenderService 重新实现了 MaterializationStore 的成熟基础设施
2. **数据结构完全重复**：PlaybackReadSource、PendingRenderJob 只差 key 类型
3. **基础设施全部重复**：playback cache、RenderCache 管理、Stretcher pool、worker
4. **新实现的 bug**：CRS targetRevision 未传入 PendingRenderEntry

### 正确架构重构方案（ora-1 完整设计）

**核心判断：**
> 拆掉 MaterializationStore 的全局 store 语义，把它里面稳定的机械部件提取成 domain-neutral runtime services；然后让 ARA 的 AudioModification、Standalone 的 StandaloneClipContent、VST3 Capture 的 CaptureSegmentContent 分别 by-value 拥有内容状态。

**架构分层（三层）：**
1. **Content Ownership Layer** - AudioModification / StandaloneClipContent / CaptureSegmentContent by-value 拥有 ContentPayloadState
2. **Derived Runtime Services Layer** - PlaybackSourcePublisher、RenderCacheRegistry、RenderWorker、TimeStretchCache、StretcherPool、RenderChunkPlanner（从 MaterializationStore 提取）
3. **Domain Coordination Layer** - ODC / StandaloneArrangement / CaptureSession 协调 graph，不拥有 content

**MaterializationStore 拆解：**
- ✅ 复用机械部件：PlaybackReadSource、RenderJob、playback snapshot publisher、RenderCache 管理、Stretcher pool、chunk planner
- 🔄 改造：ContentPayloadState（抽取权威字段）、TimeStretchCache（改 ContentKey + revision）、get/set commands（改为 owner commands）
- ❌ 删除：materializationId、global map、DC/processor store split、MaterializationContentProvider、fallback routing

**迁移路线图（5 个 Phase）：**
- **Phase 0**：架构准备 - 提取机械部件，CRS 改 thin facade
- **Phase 1**：ARA 域迁移 - AudioModification 拥有 content，ODC 删除 store
- **Phase 2**：Standalone 域迁移 - StandaloneClipContent 拥有 content
- **Phase 3**：VST3 Capture 域迁移 - CaptureSegmentContent 拥有 content
- **Phase 4**：删除 MaterializationStore
- **Phase 5**：清理 ContentRenderService

**ContentPayloadState 完整定义：**
- 包含：ContentKey、persistentId、sourceWindow、audioBuffer、pitchCurve、notes、timeGrid、pitchShiftSettings、detectedKey、silentGaps、referenceFeatures、all revisions、lifecycle
- 不包含：renderCache、stretcher、worker queue、PlaybackReadSource（这些是 derived runtime）

**方案对比：**
- 方案 A（最小改动，Store 改 key）：ARA2 符合性中，store 语义残留，容易保留 compatibility layer
- **方案 B（正确架构，提取机械部件）**：ARA2 符合性高，分层清晰，可彻底删除旧架构，符合"最正确重构"

**架构原则：**
- 复用旧轮子的机械部件，不复用旧语义污染
- 架构迁移 ≠ 功能重写，能复用必须复用
- 一句话：**复用 MaterializationStore 的机械部件，删除 MaterializationStore 的架构语义**
