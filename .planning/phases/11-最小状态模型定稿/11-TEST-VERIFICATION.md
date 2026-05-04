# Phase 11 Test Verification

## Scope

Phase 11 只验证最小状态模型与 clip-centric 读取边界，不要求把后续 phase 计划中的 queue/runtime 语义提前做完。

## Gates

| Level | Command | Expected |
| --- | --- | --- |
| L1 | `cmake --build build --target OpenTuneTests --config Release` | 测试目标构建成功 |
| L1 | `cmake --build build --target OpenTune_Standalone --config Release` | Standalone 构建成功 |
| L1 | `cmake --build build --target OpenTune_VST3 --config Release` | VST3 构建成功 |
| L2/L3/L6 | `build\OpenTuneTests.exe` | Phase 11 section全部 PASS |
| L4 | `rg -n "ClipSemanticState|getPluginClipSemanticState|getPluginPlaybackReadSource|mappingRevision|renderRevision|layoutEpoch" Source` | clip semantic state 合同可定位 |

## Required Evidence

- `PHASE11_CutTimeFocus_ContinuousScrollUsesClipRelativeBaseline`
- `PHASE11_CutTimeFocus_PageScrollUsesClipRelativeBaseline`
- `TRUTH_01_ClipIdentityIsSingleTruthAcrossAraBindingAndPlaybackRead`
- `TRUTH_03_MappingOnlyChangePreservesClipId`
- `SAFE_03_Vst3SemanticStateDoesNotRequireTrackZero`
- `PHASE11_MinimalStateModelFieldsAreReadableByClipId`

## L5 Applicability

- L5: Not applicable.
- Reason: 旧的 cut-time focus 人工缺陷现在已经有直接自动化守护；Phase 11 的当前 closure 以 deterministic regression 为准。
