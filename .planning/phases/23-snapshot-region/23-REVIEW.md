---
phase: 23-snapshot-region
reviewed: 2026-04-16T04:17:41Z
depth: standard
files_reviewed: 5
files_reviewed_list:
  - Source/ARA/OpenTuneDocumentController.h
  - Source/ARA/OpenTuneDocumentController.cpp
  - Source/ARA/OpenTunePlaybackRenderer.cpp
  - Source/Plugin/PluginEditor.cpp
  - Tests/TestMain.cpp
findings:
  critical: 0
  warning: 2
  info: 0
  total: 2
status: issues_found
---

# Phase 23: Code Review Report

**Reviewed:** 2026-04-16T04:17:41Z
**Depth:** standard
**Files Reviewed:** 5
**Status:** issues_found

## Summary

本次审查对照了 `.planning/phases/23-snapshot-region/23-01-PLAN.md`、`.planning/phases/23-snapshot-region/23-02-PLAN.md`、`.planning/phases/23-snapshot-region/23-03-PLAN.md`、对应 SUMMARY、`.planning/REQUIREMENTS.md` 与实际源码改动。结论是：`SNAP-03` 的“读侧改成 `loadSnapshot()`”表面上已经落地，且本批改动里没有看到 `appliedRegion`、epoch consumer、`will/didEnableAudioSourceSamplesAccess()` 这类显式 Phase 24/25/26 逻辑混入；但 `SNAP-02` 还没有真正闭环，因为 region-level view 仍复用了 source-level 的 clip binding，导致“同 source 多 region”时 renderer/editor 仍可能消费到错误 clip truth。另一个问题是 Phase 23 新增测试大多是源码字符串审计或手工拼接 `PublishedSnapshot`，并没有执行真实 reducer / publish / consumer 路径，因此不能证明 SNAP-01/02/03 已经真正落地。

## Warnings

### WR-01: 同一 audio source 的多个 playback region 仍共享一份 source-level clip binding，`SNAP-02` 未真正闭环

**File:** `Source/ARA/OpenTuneDocumentController.cpp:112`
**Issue:** `PublishedRegionView` 在构建时把 `SourceSlot.binding` 直接复制到每个 region view，上游绑定仍然按 `ARAAudioSource` 存在 `SourceSlot` 里（`Source/ARA/OpenTuneDocumentController.cpp:407`）。但 renderer 已经按 region 逐个消费这个 view（`Source/ARA/OpenTunePlaybackRenderer.cpp:163` / `Source/ARA/OpenTunePlaybackRenderer.cpp:167`），而 editor 导入的 clip 仍然是 region slice，不是整段 source（`Source/Plugin/PluginEditor.cpp:183`）。结果是：当同一 `ARAAudioSource` 下存在两个不同 source range 的 `ARAPlaybackRegion` 时，两个 region 会拿到同一个 `clipId` / applied-state，renderer 会把“当前绑定 region 的 clip 内容”错误地套到另一个 region 的 mapping 上，播放与同步都会偏到错误音频。这说明 Phase 23 只是把 mapping 做成了 region-level，但 clip truth 仍是 source-level，`SNAP-02` 在行为上没有真正成立。
**Fix:** 把 binding / applied-state 从 `SourceSlot` 下沉到 `RegionSlot`（或等价的 region-keyed binding 结构），让 `registerAudioSourceClipBinding()` / `updateAudioSourceBindingRevisions()` 按 `ARAPlaybackRegion` 或 `RegionIdentity` 更新，而不是按 `ARAAudioSource` 更新。`PublishedRegionView.binding` 必须来自 region 自己的 applied-state，renderer/editor 才能在同 source 多 region 场景下读到自洽 truth。例如：
```cpp
struct RegionSlot
{
    RegionIdentity identity;
    AudioSourceClipBinding binding;
    double playbackStartSeconds{0.0};
    double playbackEndSeconds{0.0};
    double sourceStartSeconds{0.0};
    double sourceEndSeconds{0.0};
    uint64_t mappingRevision{0};
};
```

### WR-02: Phase 23 新增测试没有执行真实 controller / renderer / editor 行为，无法证明 SNAP-01/02/03 已落地

**File:** `Tests/TestMain.cpp:2789`
**Issue:** `SNAP_01_ControllerPublishesSingleSnapshotAtEditBatchEnd` 通过读取 `OpenTuneDocumentController.cpp` 源码字符串并查找 `publishSnapshot();` / `markSnapshotDirty();` 来判断行为（`Tests/TestMain.cpp:2815`），并没有调用真实的 reducer 或 publish gate。`SNAP_02` 与 `SNAP_03_PreferredRegionIdentityChangesWithoutContentRewrite` 也只是手工 new 出 `PublishedSnapshot` / `PublishedRegionView` 再做值比较（`Tests/TestMain.cpp:2877`, `Tests/TestMain.cpp:2985`），完全绕过了 `buildPublishedSnapshot()`、`registerAudioSourceClipBinding()`、`syncImportedAraClipIfNeeded()` 和 renderer block read path。换句话说，这些测试即使全部 PASS，也抓不到 WR-01 这种“真实代码路径里 region view 仍复用 source-level binding”的错误，因此当前 phase 的完成证据是不充分的。
**Fix:** 增加可执行测试，直接驱动真实 reducer / publish / consumer 路径，而不是审计源码文本或拼假 snapshot。最小可行方案是给 `OpenTuneDocumentController` 抽一个 test probe / pure reducer helper，然后覆盖：
```cpp
// 1. 同 source 两个 region，各自绑定不同 applied-state
// 2. publish 后 snapshot 中两个 PublishedRegionView 的 binding 必须不同
// 3. editor sync 读取 preferred region 时，必须命中该 region 自己的 binding
// 4. renderer 在 host 命中 region B 时，不能复用 region A 的 clip truth
```
至少要把“同 source 多 region + region slice clip + preferred region 切换”做成真实执行用例，否则 Phase 23 的 GREEN 结果仍然无法证明 SNAP-01/02/03 真正成立。

---

_Reviewed: 2026-04-16T04:17:41Z_
_Reviewer: the agent (gsd-code-reviewer)_
_Depth: standard_
