# ARA AudioModification Content Root Implementation Plan

**Goal:** Make `AudioModification` the ARA content root and remove the store split that made content truth live outside the official ARA object model.

**Architecture:** `AudioModification` owns editable content state. `PlaybackRegion` owns placement only. `OpenTuneDocumentController` coordinates ARA graph lifecycle, persistence, and notifications without becoming a content store. Render/cache/worker code becomes a concrete derived service that consumes immutable content snapshots.

**Migration stance:** This is not a greenfield rewrite of rendering. Keep and migrate the existing working assets where they match the new model: `RenderCache`, `TimeStretchCache`, chunk scheduling, immutable playback snapshots, and the dry-audio plus rendered-overlay read path. Delete `MaterializationStore` as content truth and as a content-routing abstraction.

**Tech Stack:** C++17, JUCE 8, ARA SDK 2.2, CMake presets with Visual Studio/MSBuild, existing `OpenTuneTests` source-scan and compile-contract style.

---

## Required Verification

Follow `docs/plans/2026-06-05-ara-audiomodification-content-root-test-verification.md`.

The verification document is the contract. Do not start implementation tasks
until the L0-L4 tests are written and fail against the current code for the
expected reasons.

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

- Do not implement ARA ContentReader exposure unless a later requirement asks
  for host-visible content. It is valid to leave content unavailable to the host
  when this is an explicit design choice.
- Do not add a compatibility layer that keeps old store-backed ARA editing alive.
- Do not introduce global preferred-region rendering.
- Do not keep `MaterializationStore` as a content owner under a new name.
- Do not make `OpenTuneDocumentController` the content owner. It owns document
  coordination, not editable content truth.
- Do not rewrite `RenderCache`, `TimeStretchCache`, or chunk rendering from
  zero when existing behavior can be extracted into the new service.
- Do not treat asset reuse as permission to keep store-backed content routing,
  `materializationId` identity, or DC/processor store fallback paths.

---

## Object Model Contract

### AudioSource

`AudioSource` owns source identity and host sample access. It does not own
editable notes, pitch correction, time-grid edits, pitch-shift settings, or
render cache.

### AudioModification

`AudioModification` owns the editable content state for a source window.

Required owned state:

- Source provenance: source persistent ID and source window.
- Analysis state: original F0 readiness, pitch curve, detected key, silent gaps,
  and reference features.
- Editable state: notes, corrected pitch segments, time grid, pitch shift
  settings.
- Revisions: notes, pitch, time grid, pitch shift, and aggregate content
  revision.
- Lifecycle state: unloaded, reading source audio, analyzing, ready, failed, and
  retired.

The ARA content identity is the `AudioModification` object or its persistent ID,
not a materialization ID.

### Retired Content

`retired` is a lifecycle state of the same content identity, not a second store.
It replaces the current `retireMaterialization()` / `reviveMaterialization()`
semantics used by split, merge, delete, refresh, and undo.

Rules:

- Active content is owned by the domain content owner:
  `AudioModification`, standalone clip content, or regular VST3 capture content.
- Retiring content moves its authoritative state into a `RetiredContentRecord`
  keyed by the same `ContentKey`.
- Retired records are invisible to renderer/editor active lookup.
- Undo/revive moves the record back to its domain owner and bumps content
  revision.
- Reclaim releases retired records that are no longer referenced by placements,
  undo actions, or pending revive operations.
- Derived caches are not retired content. `ContentRenderService` clears or
  rebuilds them from the revived content snapshot.

Do not make `OpenTuneDocumentController` a content pool. For ARA, the controller
coordinates host lifecycle and can ask the content domain to retire or release
records, but it does not become the owner of editable content truth.

### PlaybackRegion

`PlaybackRegion` references an `AudioModification` and owns placement only:
timeline start, region offset, duration, and host placement identity. It must not
own or duplicate editable content.

### OpenTuneDocumentController

`OpenTuneDocumentController` owns ARA graph coordination:

- Register and look up `AudioSource`, `AudioModification`, and `PlaybackRegion`
  objects.
- Build projection data for editor selection and renderer assignment.
- Store and restore ARA archive data by `AudioModification` persistent ID.
- Trigger `AudioModification::notifyContentChanged(..., true)` when content
  revision changes.

It must not own a `MaterializationStore`, processor-local content store, or any
equivalent content truth container.

### OpenTunePlaybackRenderer

The renderer consumes the playback regions assigned to that renderer by the
host. It renders every overlapping assigned region and mixes overlaps. It must
not render from a global preferred playback region or from editor focus state.

---

## Shared Content/Render Service Contract

Create a neutral content snapshot/service boundary outside the ARA object layer:

- `ContentKey`: domain-neutral identity for a content root.
- `DomainContentOwner`: minimal owner interface implemented by ARA, standalone,
  and regular VST3 content roots.
- `EditableContentState`: authoritative editable content fields.
- `EditableContentSnapshot`: immutable read model for render/cache/worker code.
- `ContentCommands`: edit operations that mutate the owning domain object and
  bump revisions.
- `ContentRenderService`: consumes `EditableContentSnapshot` and publishes
  derived render artifacts.

ARA uses `AudioModification` as the owner of `EditableContentState`.

Standalone and regular VST3 must use their own domain owners, such as
arrangement clip content or capture segment content, before final migration is
accepted. They reach render/cache/worker code through the same service
interfaces. They must not reuse a generic `MaterializationStore` as the shared
content owner.

### ContentRenderService

`ContentRenderService` is the concrete replacement for the render/cache/worker
part of `MaterializationStore`, not a new content owner.

It owns derived state only:

- `RenderCache` instances keyed by a neutral content key.
- `TimeStretchCache` entries keyed by content key and relevant revisions.
- Chunk render queue and worker lifecycle.
- Chunk stats and render progress read models.
- Immutable playback read snapshots for the audio thread.

It does not own notes, pitch curves, time grids, pitch-shift settings, detected
key, source provenance, or content lifecycle truth.

Required inputs:

- `ContentKey`: a domain-neutral identity. In ARA it is derived from the
  `AudioModification` persistent ID. In standalone and regular VST3 it comes
  from their own domain content owner. It is not `materializationId`.
- `EditableContentSnapshot`: immutable notes, pitch/time-grid/pitch-shift state,
  source window, analysis state, and revisions.
- Source-audio access lease or immutable source-audio buffer owned by the domain
  owner.
- Processor service callbacks for heavy DSP work.

Required outputs:

- `PlaybackReadSource` or its renamed equivalent for lock-free audio reads.
- Render progress/chunk stats for editor overlays.
- Derived audio artifacts published through immutable snapshots.

Allowed writes:

- Publish, replace, or invalidate derived render/cache artifacts.
- Mark chunk state as pending, running, blank, succeeded, stale, or failed.

Forbidden writes:

- Mutating `AudioModification.content` or any other domain content root.
- Creating a second content truth when a render/cache miss occurs.
- Falling back to processor-local or DC-local store lookup.

### Existing Asset Migration Map

Move these assets into `ContentRenderService` with their current behavior
preserved first:

- `RenderCache` chunk state machine: pending/running/completion states, revision
  rejection, blank chunks, chunk stats, global memory accounting, and immutable
  published snapshots.
- `RenderCache::overlayPublishedAudioForRate(...)`: rendered-overlay playback
  path.
- `TimeStretchCache`: atomic reader snapshot, build generation, invalidation,
  global memory accounting, and output-range slicing.
- `MaterializationStore::PlaybackReadSource`: lock-free read model, renamed if
  needed so it no longer implies materialization ownership.
- `OpenTuneAudioProcessor::readPlaybackAudio(...)` and
  `Source/Utils/PlaybackAudioReader.h`: dry-source read plus rendered-overlay
  behavior.
- Chunk scheduling and worker wakeup/drain mechanics currently hosted by
  `MaterializationStore`.

Correct these while migrating because they are ownership or revision defects,
not reusable contracts:

- Stage 2 cache identity must include the relevant content key and pitch/time
  revisions. A rebuild must not store `pitchRev=0` as a universal cache key.
- `TimeStretchCache::sliceForOutputRange(...)` must not return a stale entry
  that bypasses revision validation.
- Worker jobs must not hold `MaterializationStore*` or rely on DC/processor store
  routing to publish results.
- Render callbacks must be service-owned and drained before detach; they must
  not keep a stale owner pointer alive.
- Persistence/rebuild code must recreate derived caches from content snapshots,
  not restore caches as content truth.

Delete instead of migrate:

- `MaterializationStore` fields that own notes, pitch curves, time grids,
  detected key, pitch-shift settings, source provenance, or content lifecycle.
- `materializationId` as ARA content identity.
- DC-owned and processor-owned `materializationStore_` split.
- `MaterializationContentProvider` dual backends.
- `if (dc)` routing between DC and processor stores.

### Processor Service Bridge

The processor may keep CPU-heavy services: F0 extraction, vocoder synthesis,
stage render DSP, and worker execution helpers. It must not own editable content.

Service results flow back through commands:

- The content owner submits an immutable snapshot plus source-audio access to a
  processor service.
- The processor returns a typed result, such as F0 analysis, pitch curve,
  rendered chunk, or Stage 2 cache data.
- The domain content owner applies authoritative analysis/edit results through
  `ContentCommands` and bumps revisions.
- `ContentRenderService` publishes derived audio/cache results against the
  snapshot revision that produced them.

If a result is stale, discard it at the content-key/revision boundary. Do not
retry through another store.

### Render Execution Lease

`ContentRenderService` owns queues, worker lifecycle, cache state, and playback
snapshots. It does not permanently own a processor callback.

The processor bridge attaches a short-lived execution lease that contains the
DSP functions needed by queued jobs. Detach stops accepting new jobs, drains or
cancels queued jobs by content key/revision, and leaves published derived
snapshots in a valid state. Jobs carry `ContentKey` and revisions, not
`MaterializationStore*`, DC store pointers, or processor store pointers.

### Standalone And Regular VST3 Boundary

`MaterializationStore` is deleted as shared content truth for all domains.
Standalone and regular VST3 are not blocked on the ARA object model, but they
must get explicit domain owners before the old store is removed:

- Standalone: arrangement clip content owns editable content; arrangement
  placements own timeline placement only.
- Regular VST3: capture/import segment content owns editable content; playback
  placement/session state owns placement only.
- Both domains feed `EditableContentSnapshot` into the same
  `ContentRenderService`.

During migration, extracted render/cache assets may temporarily coexist in the
same patch series, but a checkpoint is not complete while old and new content
owners both serve runtime edits.

---

## Migration Rules

- Tests first. L0-L4 tests must fail before implementation.
- Every migration checkpoint must remove an old content path when the new path
  becomes active. A checkpoint is not complete while old and new content owners
  both serve runtime edits.
- Migrate reusable render/cache assets before replacing their behavior. A rewrite
  is allowed only for logic that is coupled to content ownership or proven
  incorrect by the verification contract.
- No fallback content routing. A failed content lookup is a bug to fix at the
  owner/projection boundary, not a reason to try another store.
- Render/cache/worker code may cache derived artifacts, but cannot mutate
  authoritative content directly.
- ARA code remains lock-free: no mutex, critical section, blocking wait, or
  hidden store lock in `Source/ARA`.

---

## Task 0: Write Failing Contract Tests

**Files:**

- Modify: `Tests/TestMain.cpp`
- Optional create: `Tests/TestAraAudioModificationContentRoot.cpp`

**Step 1: Add `araAudioModificationIsContentRoot`**

Prove real compile-level content API exists on `AudioModification`.

Expected before implementation: FAIL because `AudioModification` currently
contains IDs/window/revisions only and uses materialization ID as renderability.

**Step 2: Add `araSourceDoesNotOwnMaterializationContentStore`**

Scan `Source/ARA/AudioModification.*`, `OpenTuneDocumentController.*`,
`MaterializationContentProvider.*`, and `OpenTunePlaybackRenderer.*`.

Forbidden content-owner tokens:

- `MaterializationStore`
- `materializationStore_`
- `getMaterializationStore()`
- `createMaterialization(`
- `materializationId`
- `DcContentAccess`
- `ProcessorContentAccess`

Expected before implementation: FAIL.

**Step 3: Add `araHasSingleContentRootNoDcProcessorStoreSplit`**

Scan `Source/PluginProcessor.*`, `Source/Plugin/PluginEditor.*`, and
`Source/Standalone/UI/PianoRollComponent.*`.

Forbidden content-routing tokens:

- `if (auto* dc = getDocumentController())`
- `dc->getMaterializationStore()`
- `return materializationStore_`
- `makeDocumentControllerAccess`
- `makeProcessorAccess`

Expected before implementation: FAIL.

**Step 4: Add `contentRenderServiceMigratesExistingAssets`**

Prove the new render service is an extraction of existing render/cache assets,
not an empty abstraction:

- It must reference `RenderCache`.
- It must reference `TimeStretchCache`.
- It must preserve a playback read source equivalent to the current
  `PlaybackReadSource`.
- It must preserve dry-source playback plus rendered-overlay behavior.
- It must own worker queue and chunk progress state outside
  `MaterializationStore`.

Expected before implementation: FAIL because those assets are still owned or
coordinated through `MaterializationStore` and processor/store callbacks.

**Step 5: Add `processorServicesDoNotWriteProcessorLocalContent`**

Prove F0, analysis, chunk render, and Stage 2 services return typed results to
the content owner or `ContentRenderService`. They must not write authoritative
content into a processor-local store.

Expected before implementation: FAIL.

**Step 6: Strengthen `playbackRendererFollowsAssignedRegionRole`**

Keep the assigned playback-region contract and prove overlap mixing remains.

Expected before implementation: PASS for assigned-region direction, FAIL only if
the test also forbids materialization store usage before the content root exists.

**Step 7: Run tests and record expected failure**

Run:

```bat
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe"
```

Expected: FAIL for the new content-root contracts.

---

## Task 1: Add the Content State Type

**Files:**

- Create: `Source/Content/EditableContentState.h`
- Create: `Source/Content/AnalysisState.h`
- Create: `Source/Content/ContentKey.h`
- Modify: `CMakeLists.txt`

**Step 1: Define `ContentKey`**

The key identifies a domain content root without importing the old
materialization model.

Required properties:

- `DomainKind`: `ARAAudioModification`, `StandaloneClip`, or
  `RegularVST3Capture`.
- Persistent object ID owned by that domain.
- Optional source-window discriminator if the domain owner supports multiple
  windows per source object.

Forbidden properties:

- `materializationId`
- DC store pointer
- processor store pointer
- render-cache pointer

**Step 2: Define `DomainContentOwner`**

Keep this interface small. It exists so ARA, standalone, and regular VST3 can
share commands/render services without sharing a store.

Required operations:

- `ContentKey contentKey() const`
- `EditableContentSnapshot snapshotContent() const`
- apply content commands and bump revisions
- `retireContent(ContentKey)`
- `reviveContent(ContentKey)`
- `releaseRetiredContent(ContentKey)`

It must not expose store pointers, renderer caches, or host placement state.

**Step 3: Define `AnalysisState`**

Analysis fields are owned by the content root but are not edit-command truth:

- `OriginalF0State originalF0State`
- `std::shared_ptr<PitchCurve> pitchCurve`
- `DetectedKey detectedKey`
- `std::vector<SilentGap> silentGaps`
- reference feature state if already part of the render contract
- analysis lifecycle and failure state

**Step 4: Define `EditableContentState`**

The type contains editable fields only. It does not contain analysis results,
store IDs, placement, render queues, worker state, or cache ownership.

Fields:

- `std::vector<Note> notes`
- `std::vector<CorrectedSegment> correctedSegments`
- `std::shared_ptr<const TimeGridSnapshot> timeGrid`
- `PitchShiftSettings pitchShiftSettings`
- `uint64_t notesRevision`
- `uint64_t pitchRevision`
- `uint64_t timeGridRevision`
- `uint64_t pitchShiftRevision`
- `uint64_t contentRevision`

**Step 5: Define `AudioModificationContentState`**

The aggregate owned by ARA `AudioModification` contains:

- source provenance
- `AnalysisState analysis`
- `EditableContentState editable`
- lifecycle enum for empty/loading/analyzing/ready/failed/retired
- aggregate content revision

**Step 6: Define `RetiredContentRecord`**

The retired record contains the authoritative content state plus `ContentKey`.
It does not contain render caches, playback source snapshots, worker jobs, or
placement geometry.

**Step 7: Define `EditableContentSnapshot`**

The snapshot is immutable and is the only object render/cache/worker services
may consume.

It may include both editable and analysis data needed for rendering, but it must
not contain render/cache/worker ownership.

**Step 8: Register files in CMake**

Add `Source/Content/`, `Source/Render/`, and `Source/Services/` files as they are
created. Do not rely on IDE globbing or generated project drift.

**Step 9: Run build**

Run the L6 test build command. Expected: PASS if this task only adds unused
types.

---

## Task 2: Make AudioModification Own Content And Commands

**Files:**

- Modify: `Source/ARA/AudioModification.h`
- Modify: `Source/ARA/AudioModification.cpp`

**Step 1: Include `AudioModificationContentState`**

Add a by-value `AudioModificationContentState content;` member to
`AudioModification`.

**Step 2: Remove materialization identity**

Delete `materializationId`, `materializationRevision`, and any renderability
logic based on materialization IDs.

**Step 3: Add content helpers**

Required helpers:

- `bool hasContentState() const`
- `bool isRenderable() const`
- `EditableContentSnapshot snapshotContent() const`
- `void resetContentForSourceWindow(...)`
- edit helpers or command entry points that bump revisions

**Step 4: Add content command entry points**

Pitch, note, time-grid, detected-key, analysis, and pitch-shift changes must be
applied through the owning `AudioModification` or a command object bound to it.
Undo/redo actions must hold the command target, not a processor or store handle.

**Step 5: Add retire/revive hooks**

`AudioModification` participates in the shared domain-owner lifecycle:

- active content can move to `RetiredContentRecord`
- retired content can revive into an active `AudioModification` content state
- `willDestroyAudioModification()` retires or releases according to references,
  but does not leave content in DC-owned storage

**Step 6: Run tests**

Expected: `araAudioModificationIsContentRoot` PASS. Other migration tests still
FAIL until DC/editor/renderer paths stop using store routing.

---

## Task 3: Extract ContentRenderService From Existing Assets

**Files:**

- Create: `Source/Render/ContentRenderService.h`
- Create: `Source/Render/ContentRenderService.cpp`
- Modify: `Source/Inference/RenderCache.*`
- Modify: `Source/Inference/TimeStretchCache.*`
- Modify: `Source/Utils/PlaybackAudioReader.h`
- Modify: `Source/MaterializationStore.*`
- Modify: `Source/PluginProcessor.*`

This task happens before ARA birth, processor bridge, renderer, and editor
migration. The service must be real and backed by migrated assets before any
caller is switched to it.

**Step 1: Create CRS with existing asset ownership**

Move the following derived ownership into `ContentRenderService`:

- render worker thread and wakeup/drain state
- pending chunk queue
- `RenderCache` lookup and lifetime
- `TimeStretchCache` lookup and lifetime
- playback read source snapshot publication
- chunk stats

**Step 2: Add a compile-only delegation checkpoint**

During extraction, `MaterializationStore` may temporarily delegate render/cache
operations to `ContentRenderService` so the project can compile between edits.
It must not own a second copy of `RenderCache`, `TimeStretchCache`, worker queue,
or playback source cache. This checkpoint is not an accepted task completion.

**Step 3: Preserve playback behavior**

Keep the current read behavior:

- Try valid Stage 2 stretched audio first.
- Read dry source audio when Stage 2 is absent.
- Overlay rendered chunks through `RenderCache`.
- Keep audio-thread reads lock-free through immutable snapshots.

**Step 4: Fix cache revision contracts**

Make cache hits and slices validate the content key plus relevant revisions.
Remove universal `pitchRev=0` Stage 2 publication. Stale chunk or Stage 2
results are discarded, not routed through another store.

**Step 5: Replace owner pointers and callback ownership**

Worker jobs target `ContentRenderService` and content keys. DSP execution comes
through the processor bridge execution lease. Jobs must not carry
`MaterializationStore*`, DC store pointers, processor store pointers, or a
long-lived raw callback.

**Step 6: Delete store-owned derived assets**

Remove store-owned copies after callers use `ContentRenderService`. A temporary
delegation shell cannot remain after this task.

**Step 7: Run tests**

Expected:

- `contentRenderServiceMigratesExistingAssets` PASS.
- TimeStretch/Stage2 revision tests PASS.
- Store-forbidden tests still FAIL only where birth/processor/editor/renderer
  callers have not yet moved.

---

## Task 4: Add The Processor Services Result Bridge

**Files:**

- Create or modify service interfaces under `Source/Services/`
- Modify: `Source/PluginProcessor.h`
- Modify: `Source/PluginProcessor.cpp`
- Modify: `Source/ARA/OpenTuneDocumentController.*`
- Modify: `Source/ARA/AudioModification.*`

**Step 1: Define service requests and results**

Requests contain immutable content snapshots, source-audio access, and target
revisions. Results contain typed payloads only:

- F0 analysis result.
- Pitch curve result.
- Rendered chunk result.
- Stage 2 stretched-audio result.
- Failure result with reason and source revision.

No result may contain a store pointer or ask the processor to choose a content
owner.

Stage 2 requests use `(ContentKey, pitchRevision, timeGridRevision)` and the
snapshot that produced the request. They must not use
`(materializationId, MaterializationStore*)`.

**Step 2: Route F0 and analysis completion to the content owner**

The processor service returns analysis data. The ARA
`AudioModification` applies authoritative analysis state through content
commands, bumps revisions, notifies JUCE ARA content change, and schedules render
cache updates.

**Step 3: Route render artifacts to `ContentRenderService`**

Rendered chunks and Stage 2 stretched audio are derived artifacts. They are
published by `ContentRenderService` only if the content key and source revisions
still match the request snapshot.

**Step 4: Remove processor-local content writes**

Delete service paths that write notes, pitch curves, time grids, detected key,
or pitch-shift settings into `OpenTuneAudioProcessor::materializationStore_`.

**Step 5: Run tests**

Expected: processor-bridge tests PASS. ARA editor/renderer store-forbidden tests
may still FAIL until their callers are moved.

---

## Task 5: Move ARA Birth And Restore Into AudioModification

**Files:**

- Modify: `Source/ARA/OpenTuneDocumentController.h`
- Modify: `Source/ARA/OpenTuneDocumentController.cpp`
- Modify: `Source/ARA/AudioModification.*`
- Modify: `Source/Render/ContentRenderService.*`

**Step 1: Replace materialization birth**

`birthMaterializationForModification()` must become an
`AudioModification` content initialization path:

- Read source samples through the ARA source reader lease.
- Populate the target `AudioModification.content` fields.
- Set lifecycle/analyze state.
- Bump content revision.
- Notify content changed through the JUCE ARA bridge.
- Schedule analysis/render services through the already extracted
  `ContentRenderService` and processor bridge.

**Step 2: Replace ARA archive payload**

Persist by `AudioModification` persistent ID and content payload. Do not persist
store IDs, materialization bindings, `RenderCache`, or `TimeStretchCache` as
authoritative content.

**Step 3: Delete DC content store ownership**

Remove DC-owned store fields and methods from `OpenTuneDocumentController`.

**Step 4: Run tests**

Expected:

- ARA birth/restore store-forbidden tests PASS.
- Renderer/editor tests may still FAIL until their content paths are migrated.

---

## Task 6: Split Non-ARA Domain Ownership From The Old Store

**Files:**

- Create or modify neutral content domain files under `Source/Content/`
- Modify: `Source/StandaloneArrangement.*`
- Modify: `Source/Plugin/Capture/*`
- Modify: `Source/PluginProcessor.*`

This task happens before editor migration so `PluginEditor` and
`PianoRollComponent` have real domain owners to target. Do not leave the editor
with an ARA-only owner plus processor/store fallback for non-ARA paths.

**Step 1: Define standalone content owner**

Create `StandaloneClipContent` or the closest local name that fits existing
style. It implements `DomainContentOwner`. Standalone placements reference this
content key and own placement only.

**Step 2: Define regular VST3 content owner**

Create `CaptureSegmentContent` or the closest local name that fits existing
style. Capture segments produce this content owner, not a materialization store
entry.

**Step 3: Use shared content/render services**

Standalone, regular VST3, and ARA all feed render/cache/worker services through
the same snapshot interface.

**Step 4: Preserve existing working behavior through migrated assets**

Standalone and regular VST3 should reuse the extracted `ContentRenderService`
assets rather than rebuilding cache/playback paths from scratch.

**Step 5: Delete remaining store truth**

Remove old content-store APIs once all domains use their own owners.

---

## Task 7: Replace Renderer Read Path

**Files:**

- Modify: `Source/ARA/OpenTunePlaybackRenderer.h`
- Modify: `Source/ARA/OpenTunePlaybackRenderer.cpp`
- Modify: `Source/ARA/OpenTuneDocumentController.*`
- Modify: `Source/Render/ContentRenderService.*`

**Step 1: Replace materialization read source**

Renderer projections resolve host-assigned playback regions to
`AudioModification` content snapshots and content keys, then request playback
read sources from `ContentRenderService`.

**Step 2: Preserve assigned-region overlap behavior**

Keep the existing role contract:

- Use host-assigned playback regions.
- Iterate all overlapping assigned regions.
- Mix overlaps into the output buffer.
- Return handled silence for empty/non-overlap blocks.

**Step 3: Keep render reads derived**

Renderer code reads derived audio/cache artifacts only. It must not mutate
`AudioModification.content`, trigger content fallback routing, or read editor
focus state.

**Step 4: Run tests**

Expected: renderer role tests PASS and ARA renderer store-forbidden tests PASS.

---

## Task 8: Replace Store-Backed Editor Content Access

**Files:**

- Modify or delete: `Source/ARA/MaterializationContentProvider.h`
- Modify or delete: `Source/ARA/MaterializationContentProvider.cpp`
- Modify: `Source/Plugin/PluginEditor.h`
- Modify: `Source/Plugin/PluginEditor.cpp`
- Modify: `Source/Standalone/UI/PianoRollComponent.*`

This task happens after ARA and non-ARA content owners exist. It must not solve
missing ownership by keeping a dual provider or an `if (dc)` route.

**Step 1: Replace materialization ID API**

Editing APIs must take a neutral content handle resolved by the active domain
owner. In ARA the handle resolves to `AudioModification`; in standalone and
regular VST3 it resolves to their domain content owner. They must not take
`materializationId` in the ARA path.

**Step 2: Delete dual backends**

Remove `DcContentAccess`, `ProcessorContentAccess`,
`makeDocumentControllerAccess`, and `makeProcessorAccess`.

**Step 3: Route commands to the content owner**

Pitch, notes, time grid, detected key, and pitch shift edits mutate the owning
content state and bump revisions.

**Step 4: Run tests**

Expected: `araHasSingleContentRootNoDcProcessorStoreSplit` PASS.

---

## Task 9: Final Kill List Review

Run the L0-L6 verification commands.

The migration is rejected if any of these remain:

- `MaterializationStore` as content owner.
- `materializationId` as ARA content identity.
- DC-owned or processor-owned content store split.
- `if (dc)` content routing.
- Store-backed `MaterializationContentProvider` dual backend.
- ARA renderer using editor focus or preferred-region state.
- Render/cache/worker code mutating authoritative content.
- A greenfield replacement for `RenderCache`/`TimeStretchCache` while equivalent
  working assets were available to migrate.

When all blocking levels pass, commit with a message that names the architectural
root:

```text
refactor(ara): make AudioModification the content root
```
