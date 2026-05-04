---
phase: 16-hop-对齐切分与末块语义
plan: 01
subsystem: core
tags: [silent-gap, sample-boundary, hop-alignment, render-cache]

requires:
  - phase: 15-样本真相源与冻结边界
    provides: FrozenRenderBoundaries baseline and sample truth helpers
provides:
  - Sample-domain SilentGap contract bound to stored 44.1kHz buffers
  - Hop-aligned chunk boundary builder that selects split samples inside silent gaps
  - RenderCache pending jobs carrying exact sample spans alongside seconds projections
affects: [16-02, 16-03, render-worker]

tech-stack:
  added: []
  patterns:
    - "Silent gaps keep samples as truth and project seconds only on demand"
    - "Chunk segmentation and pending jobs share one sample-boundary contract"

key-files:
  created: []
  modified:
    - Source/Utils/SilentGapDetector.h
    - Source/Utils/SilentGapDetector.cpp
    - Source/Inference/RenderCache.h
    - Source/Inference/RenderCache.cpp
    - Source/PluginProcessor.cpp
    - Source/Standalone/PluginEditor.cpp

key-decisions:
  - "D16-01-01: SilentGap now stores start/end samples directly; seconds became projection helpers only"
  - "D16-01-02: RenderCache keeps seconds keys for now, but worker-facing pending jobs must carry exact sample spans"

patterns-established:
  - "Pattern: choose internal chunk splits from hop-aligned samples inside each silent gap"
  - "Pattern: skip silent gaps with no legal hop-aligned split instead of fabricating misaligned boundaries"

requirements-completed: [CHNK-01, CHNK-02]

duration: 35min
completed: 2026-04-13
---

# Phase 16 Plan 01: sample-domain 静息处与 hop 对齐切分 Summary

**静息处与 chunk segmentation 现在共享同一套 sample-domain truth：`SilentGap` 直接持有 44.1kHz sample span，`RenderCache::PendingJob` 也开始携带精确 sample 边界，seconds 只剩下 cache key 投影角色。**

## Performance

- **Duration:** 35 min
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments

- 把 `SilentGap` 从 seconds 存储改成 `[startSample, endSampleExclusive)` sample span
- 把静息处查询 API 全部改成“秒值输入 -> sample 比较”的投影模式
- 在 `PluginProcessor.cpp` 中建立 hop-aligned sample chunk boundary builder，并让无合法 hop 点的静息处直接跳过
- 让 `RenderCache::Chunk` / `PendingJob` 显式携带 sample boundary，worker 不再需要从 seconds round-trip 恢复切分点

## Task Commits

1. **Task 1: 把 SilentGap 真相坐标改成 sample span** - `d733ef8` (feat)
2. **Task 2: 用 hop-aligned sample 边界替换 seconds 中点切分** - `d2ec96c` (feat)

## Files Created/Modified

- `Source/Utils/SilentGapDetector.h` - 把静息处契约改成 sample-domain truth，并保留 seconds 投影 helper
- `Source/Utils/SilentGapDetector.cpp` - detector 与 gap 查询逻辑全部切到 sample 比较
- `Source/Inference/RenderCache.h` - 给 `Chunk` / `PendingJob` 增加 sample boundary 字段
- `Source/Inference/RenderCache.cpp` - `requestRenderPending()` / `getNextPendingJob()` 开始传播 sample span
- `Source/PluginProcessor.cpp` - 用静息处内部 hop-aligned sample 构建 chunk boundaries，并按 sample overlap 请求 render
- `Source/Standalone/PluginEditor.cpp` - 当前 silent-gap 消费者改成直接读取 sample span

## Decisions Made

- 不再创建 `SilentGapSamples` 之类的平行结构，而是直接把 `SilentGap` 本身改成 sample truth contract
- 在 Phase 16 先保留 `RenderCache` 的 seconds key，避免过早重写 cache 读取层；但 worker-facing pending job 已经必须带 sample boundary

## Deviations from Plan

- None - plan executed as written.

## Known Stubs

- None.

## Self-Check: PASSED

- Summary file exists and task commit hashes `d733ef8` / `d2ec96c` are present in git history
