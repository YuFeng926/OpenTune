# Phase 12 Test Verification

## Gates

| Level | Command | Expected |
| --- | --- | --- |
| L1 | `cmake --build build --target OpenTuneTests --config Release` | 测试目标构建成功 |
| L2/L3/L6 | `build\OpenTuneTests.exe` | Phase 12 tests plus linked clip/read regressions PASS |
| L4 | `rg -n "getPluginPlaybackReadSource\(|setPluginClipStartSeconds\(" Source/ARA/OpenTunePlaybackRenderer.cpp Source/PluginProcessor.cpp Source/Plugin/PluginEditor.cpp` | 关键接线存在 |

## Required Evidence

- `REV_01_SetPluginClipStartSecondsBumpsMappingRevisionWithoutRenderInvalidation`
- `ClipPlaybackReadSourceUsesCoreClipCache`
- `TRUTH_01_ClipIdentityIsSingleTruthAcrossAraBindingAndPlaybackRead`

## L5 Applicability

- L5: Not applicable.
- Reason: 当前 closure 以 clip-centric runtime wiring 和 deterministic unit/integration regressions 为准。
