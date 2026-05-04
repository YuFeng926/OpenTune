---
phase: 17-真实长度发布与缓存派生
plan: 01
subsystem: cache
tags: [render-cache, sample-boundary, resampled-cache, export, playback]

requires:
  - phase: 16-hop-对齐切分与末块语义
    provides: FrozenRenderBoundaries, only-last-chunk padding semantics, padded-tail F0 gating
provides:
  - Sample-authoritative RenderCache publish contract for base and resampled chunks
  - Export and playback-read consumers that project time from published sample spans
  - Existing test scaffolds updated to the real-length cache API
affects: [17-02, phase-gate, diagnostics, export, playback-read]

tech-stack:
  added: []
  patterns:
    - "RenderCache publish APIs accept real sample spans and project seconds internally"
    - "Resampled cache attaches to the same true sample span as its base chunk"

key-files:
  created: []
  modified:
    - Source/Inference/RenderCache.h
    - Source/Inference/RenderCache.cpp
    - Source/PluginProcessor.cpp
    - Tests/TestMain.cpp

key-decisions:
  - "D17-01-01: Published chunk time windows are projections from stored sample spans, not caller-supplied seconds"
  - "D17-01-02: addChunk/addResampledChunk now require explicit sample spans so padding cannot leak into persisted cache lengths"

patterns-established:
  - "Pattern: publishSampleCount drives persisted base chunk length; resampled cache inherits the same true span"
  - "Pattern: export/read consumers use sample spans first and only project seconds for display-layer needs"

requirements-completed: [TASK-05, SAMP-03]

duration: 25min
completed: 2026-04-13
---

# Phase 17 Plan 01: sample-authoritative cache publish Summary

**RenderCache 现在把真实 sample span 作为 base/resampled 发布契约，导出与播放读取只消费这组 sample truth 的时间投影，不再把外部 seconds 输入重新抬成持久化事实。**

## Performance

- **Duration:** 25 min
- **Started:** 2026-04-13T11:06:57Z
- **Completed:** 2026-04-13T11:32:01Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments

- 把 `RenderCache::addChunk()` / `addResampledChunk()` 改成显式接收 sample span，并在内部投影 `startSeconds/endSeconds`
- 给 `PublishedChunk` 补齐 sample-domain boundary，让导出与读取消费者不再从 seconds 回推边界
- 让 worker publish 路径直接传递 `FrozenRenderBoundaries.trueStartSample/trueEndSample`
- 把现有测试脚手架更新到新 publish API，避免旧 seconds 签名继续污染 contract

## Task Commits

1. **Task 1 + Task 2: sample-authoritative cache publish contract 与消费者接线** - `d10cdc8` (feat)

## Files Created/Modified

- `Source/Inference/RenderCache.h` - 把 published chunk contract 与 publish API 切到 sample-domain truth
- `Source/Inference/RenderCache.cpp` - 用 sample span 推导 publish/read 时间窗，并校验 base chunk 长度与真实 span 一致
- `Source/PluginProcessor.cpp` - worker 发布路径和导出覆盖逻辑改为消费真实 sample boundary
- `Tests/TestMain.cpp` - 现有 RenderCache 调用点切到新的 sample-authoritative API

## Decisions Made

- 不保留 seconds 为 publish API 的输入真相；seconds 继续存在，但只作为 sample span 的投影视图
- resampled cache 与 base chunk 共用同一真实边界，不允许额外携带自己的 seconds 真相

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] `RenderCache` publish API 改签名后，现有测试调用必须同一波更新**
- **Found during:** Task 1 / Task 2 交界处
- **Issue:** 仅修改 `RenderCache` 接口会直接打断 `Tests/TestMain.cpp` 与 worker 调用点的编译，导致 sample-authoritative contract 无法落地为可构建代码。
- **Fix:** 在同一波代码提交里同步更新 `Tests/TestMain.cpp` 与 `Source/PluginProcessor.cpp` 的旧 seconds 调用点，统一切到 sample span 签名。
- **Files modified:** `Source/Inference/RenderCache.h`, `Source/Inference/RenderCache.cpp`, `Source/PluginProcessor.cpp`, `Tests/TestMain.cpp`
- **Verification:** 静态 `rg` audit 命中了 sample-authoritative symbol；`OpenTuneTests` 构建过程已重新编译并链接新的 shared code objects，直到触发与本计划无关的既有 baseline linker gate。
- **Committed in:** `d10cdc8`

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** 只把 compile-coupled 的调用点同步到同一波，没有扩展 Phase 17 范围。

## Issues Encountered

- `OpenTuneTests` 的构建已经重新编译本计划涉及的四个文件，并成功链接 `OpenTune_SharedCode.lib`；但最终 `OpenTuneTests.exe` 仍被当前 baseline 中的既有 linker 问题阻塞，表现为 `_purecall` 与 `operator new/delete` 的全局未解析符号。这不是本计划改动引入的局部语义错误，但它会阻塞 17-02 的自动化测试执行。

## Known Stubs

- None.

## Next Phase Readiness

- Phase 17 的缓存发布契约代码已到位，17-02 可以直接在此基础上补 regression tests 与 verification docs。
- 在继续 17-02 之前，需要先解决当前 baseline 的 `OpenTuneTests.exe` linker gate，否则无法诚实地执行 L1/L2/L6 自动化验证。

## Self-Check: PASSED

- Summary file exists at `.planning/phases/17-真实长度发布与缓存派生/17-01-SUMMARY.md`
- Commit `d10cdc8` is present in git history
