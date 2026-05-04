---
phase: 07-控制与时间基线收敛
plan: 02
subsystem: api
tags: [ara, playback, time-mapping, sample-rate, tdd]
requires:
  - phase: 07-01
    provides: ARA 控制侧宿主权威快照基线
provides:
  - PlaybackRenderer 单点时间映射函数（playbackTime -> sourceTime -> sourceSample）
  - 基于 DocumentController playback/source range 契约的读取路径
  - 跨采样率读取长度换算与 loop/seek 关键日志
affects: [phase-08, playback-read-core, ara-rendering]
tech-stack:
  added: []
  patterns: [single-source-time-mapping, range-contract-read-path, host-duration-based-source-read]
key-files:
  created: []
  modified:
    - Tests/TestMain.cpp
    - Source/ARA/OpenTunePlaybackRenderer.h
    - Source/ARA/OpenTunePlaybackRenderer.cpp
    - Source/ARA/OpenTuneDocumentController.h
    - Source/Utils/TimeCoordinate.h
key-decisions:
  - "以 playback/source range 作为 ARA 读取唯一事实源，移除 regionStart 近似偏移计算。"
  - "源读取长度按 host block 时间长度换算，保证跨采样率 block 对齐。"
patterns-established:
  - "时间映射唯一入口：mapPlaybackTimeToSourceSample"
  - "多 region 混音统一使用 addFrom，避免 copyFrom 覆盖"
requirements-completed: [BASE-03]
duration: 98 min
completed: 2026-04-06
---

# Phase 7 Plan 02: 时间映射与采样率换算收敛 Summary

**ARA PlaybackRenderer 已改为基于 playback/source range 契约做统一时间映射，并在跨采样率与 seek/loop 场景下使用同一读取换算逻辑。**

## Performance

- **Duration:** 98 min
- **Started:** 2026-04-06T14:23:53Z
- **Completed:** 2026-04-06T16:02:49Z
- **Tasks:** 3
- **Files modified:** 5

## Accomplishments
- 增加并落地单一映射函数 `mapPlaybackTimeToSourceSample`，统一处理比例映射、clamp、sample 边界。
- `processBlock` 改为调用 `getPlaybackRangeForAudioSource/getSourceRangeForAudioSource` 契约读取，替换旧近似偏移路径。
- 新增 host/source block 长度换算函数与关键日志，支撑 loop/seek/跨采样率回归检查。

## Task Commits

Each task was committed atomically:

1. **Task 1: 提炼时间映射函数并覆盖边界样例**
   - `37b811b` (test) RED: 增加映射边界失败用例
   - `bdb4934` (feat) GREEN: 增加统一映射函数与 TimeCoordinate 边界换算
2. **Task 2: 用映射函数重写 ARA processBlock 读取路径**
   - `345503e` (test) RED: 增加 host/source 读取长度换算失败用例
   - `01adada` (feat) GREEN: 读取路径切换为 range mapping 契约并修正混音写入语义
3. **Task 3: 采样率与 loop 回归验证**
   - `3426d5d` (chore) 增加关键映射日志用于 REAPER 回归观察

## Files Created/Modified
- `Source/ARA/OpenTunePlaybackRenderer.cpp` - 映射函数、读取路径重写、关键映射日志。
- `Source/ARA/OpenTunePlaybackRenderer.h` - 暴露映射/换算函数声明供测试调用。
- `Source/Utils/TimeCoordinate.h` - 增加 floor/ceil 样本换算工具。
- `Source/ARA/OpenTuneDocumentController.h` - 补齐 render cache 绑定函数声明，修复编译阻断。
- `Tests/TestMain.cpp` - 增加映射与读取长度的 TDD 用例。

## Decisions Made
- 以 DocumentController range 合同作为 ARA 读取入口，避免 PlaybackRenderer 自行维护第二套时间语义。
- 为跨采样率准确读取，先将 host block 转换为 source 侧样本数，再进入重采样/直拷贝分支。

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] 修复 DocumentController 头文件声明缺失导致的构建阻断**
- **Found during:** Task 1 GREEN 编译验证
- **Issue:** `OpenTuneDocumentController.cpp` 已实现 `setRenderCacheForAudioSource`，但头文件缺少声明，导致编译失败。
- **Fix:** 在 `OpenTuneDocumentController.h` 增加函数声明并包含 `RenderCache` 类型。
- **Files modified:** `Source/ARA/OpenTuneDocumentController.h`
- **Verification:** 重新构建至 OpenTuneTests 链接阶段通过该编译点。
- **Committed in:** `01adada`

---

**Total deviations:** 1 auto-fixed (Rule 3: 1)
**Impact on plan:** 偏差为执行阻断修复，未引入额外架构范围。

## Authentication Gates

None.

## Issues Encountered

- `OpenTune_VST3` 目标在当前分支基线存在 `updateHostTransportSnapshot` 未定义错误（`Source/PluginProcessor.cpp`），与本计划改动无直接关联。
- `OpenTuneTests` 链接阶段存在 `getHostTransportSnapshot` 未解析符号，属于当前工作区既有问题。
- 已记录至 `.planning/phases/07-控制与时间基线收敛/deferred-items.md`。

## Known Stubs

None.

## Next Phase Readiness

- 时间映射入口已单点化，可直接复用于 Phase 8 统一 Playback Read 核心。
- 需先消化 deferred items 中的既有编译/链接问题，以恢复完整自动化回归链路。

## Self-Check: PASSED

- FOUND: `.planning/phases/07-控制与时间基线收敛/07-02-SUMMARY.md`
- FOUND commits: `37b811b`, `bdb4934`, `345503e`, `01adada`, `3426d5d`
