# Phase 13 Test Verification

## Gates

| Level | Command | Expected |
| --- | --- | --- |
| L1 | `cmake --build build --target OpenTuneTests --config Release` | 测试目标构建成功 |
| L2/L3/L6 | `build\OpenTuneTests.exe` | Phase 13 tests and existing queue regressions PASS |
| L4 | `rg -n "desiredRevision|publishedRevision|getNextPendingJob|completeChunkRender|enqueuePluginClipPartialRender" Source` | 旧队列核心与 clip wrapper 可定位 |

## Required Evidence

- `QUEUE_01_EnqueuePluginClipPartialRenderTargetsResolvedClipCache`
- `QUEUE_03_RenderCacheStaleCompletionRequeuesLatestRevision`
- `MappingOnlyChangeSkipsContentReRendering`

## L5 Applicability

- L5: Not applicable.
- Reason: 当前 phase 验证的是共享 queue 语义与 stale-publish 屏障，没有 UI/宿主旅程 gate。
