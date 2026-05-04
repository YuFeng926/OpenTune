---
phase: 11-最小状态模型定稿
verified: 2026-04-14T22:45:00+08:00
status: passed
score: 4/4 must-haves verified
---

# Phase 11: 最小状态模型定稿 Verification Report

**Phase Goal:** 冻结 VST3/ARA clip 路径的最小状态模型，让 `clipId / renderRevision / mappingRevision / layoutEpoch / RenderCache / RenderTask` 的职责边界明确、可查、可测。

## Observable Truths

| # | Truth | Status | Evidence |
| --- | --- | --- | --- |
| 1 | `clipId` 是 VST3/ARA 路径的统一身份键 | ✓ PASS | `TRUTH_01_ClipIdentityIsSingleTruthAcrossAraBindingAndPlaybackRead` PASS |
| 2 | mapping-only 不替换 clip 身份 | ✓ PASS | `TRUTH_03_MappingOnlyChangePreservesClipId` and cut-time focus tests PASS |
| 3 | `track` 只保留为 processor/UI 内部容器，不再是对外真相源 | ✓ PASS | `SAFE_03_Vst3SemanticStateDoesNotRequireTrackZero` PASS |
| 4 | clip semantic state 字段与只读查询面已经固定 | ✓ PASS | `PHASE11_MinimalStateModelFieldsAreReadableByClipId` PASS |

## Behavioral Spot-Checks

| Behavior | Command | Result | Status |
| --- | --- | --- | --- |
| Phase 11 tests | `build\OpenTuneTests.exe` | Phase 11 section all PASS | ✓ PASS |
| 双目标构建 | `cmake --build build --target OpenTune_Standalone --config Release` + `cmake --build build --target OpenTune_VST3 --config Release` | 成功 | ✓ PASS |

## Notes

- 历史上暴露过的 REAPER cut-time focus 问题，现在已经由 `PHASE11_CutTimeFocus_ContinuousScrollUsesClipRelativeBaseline` 和 `PHASE11_CutTimeFocus_PageScrollUsesClipRelativeBaseline` 两个自动化守护覆盖并通过。
- 早期文档要求把 `layoutEpoch` 镜像进 `AudioSourceClipBinding`。当前更优结构没有回补这一点：`layoutEpoch` 继续留在 processor 的 clip semantic state，ARA binding 只保留 `clipId + source-side revisions`，从而避免 adapter 长成第二套可写状态模型。
- `kVst3TrackId` 仍作为 VST3 editor 的壳层选择常量存在，但当前语义读取/写入已经不再依赖它作为外部真相源；因此“完全删除符号”不是当前 phase 的必要条件。

## Gate Status

- PASS
