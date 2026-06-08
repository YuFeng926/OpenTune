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

### 下一步优先级（P0 必须先修）

1. **修复 ARA 写路径** - 所有 ARA editor writes 必须落到 `AudioModification.content`，删除 ARA processor-store writes
2. **修复 ARA persistence** - 保存/恢复 `AudioModificationContentState`，不保存旧 materialization store
3. **删除 ARA DC materializationStore_** - DC 只协调 ARA graph，不拥有 content store

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

### ContentRenderService
`ContentRenderService` is the concrete replacement for the render/cache/worker part of `MaterializationStore`, not a new content owner.

It owns derived state only:
- `RenderCache` instances keyed by a neutral content key.
- `TimeStretchCache` entries keyed by content key and relevant revisions.
- Chunk render queue and worker lifecycle.
- Chunk stats and render progress read models.
- Immutable playback read snapshots for the audio thread.

It does not own notes, pitch curves, time grids, pitch-shift settings, detected key, source provenance, or content lifecycle truth.

Forbidden writes:
- Mutating `AudioModification.content` or any other domain content root.
- Creating a second content truth when a render/cache miss occurs.
- Falling back to processor-local or DC-local store lookup.

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
最终审查：禁止 MaterializationStore content owner、materializationId、DC/processor store split、fallback routing、dual backend。

---

## 参考文档

- ARA2 SDK Documentation: https://github.com/Celemony/ARA_SDK
- Oracle 架构审查报告（2026-06-08）：见 git commit `e0326f0`
- 读路径修复计划：`docs/plans/2026-06-08-ara-read-path-lifecycle-fix.md`
