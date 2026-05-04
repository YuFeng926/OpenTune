---
phase: 06-编译验证
plan: 04
subsystem: testing
tags: [comparison, artifacts, dependencies, vst3, standalone]
requires:
  - phase: 06-02
    provides: Standalone Debug/Release 产物
  - phase: 06-03
    provides: VST3 Debug/Release 产物
provides:
  - Standalone/VST3 产物大小对比基线
  - 依赖与模型一致性校验结论
affects: [06-VERIFICATION]
tech-stack:
  added: []
  patterns: [artifact-baseline, hash-consistency-check]
key-files:
  created:
    - .planning/phases/06-编译验证/06-04-BUILD-COMPARISON.md
    - .planning/phases/06-编译验证/06-04-SUMMARY.md
  modified: []
key-decisions:
  - "使用 Debug 工件进行哈希一致性比对，使用 Debug/Release 两套数据做体积基线。"
  - "将模型文件体积异常（占位文件）记录为验证观察项，留待后续实模接入阶段处理。"
patterns-established:
  - "跨格式发布前必须做 DLL/模型哈希一致性核验。"
requirements-completed: [TEST-03]
duration: 15min
completed: 2026-04-05
---

# Phase 6 Plan 04: 编译产物对比 Summary

**建立 Standalone 与 VST3 的二进制/依赖/模型对比基线并产出可复用部署结论。**

## Performance

- **Duration:** 15 min
- **Started:** 2026-04-05T04:20:01Z
- **Completed:** 2026-04-05T04:35:00Z
- **Tasks:** 5
- **Files modified:** 2

## Accomplishments
- 生成 `06-04-BUILD-COMPARISON.md`，覆盖主二进制、总大小、依赖和模型的对比表。
- 完成 DLL 与模型跨格式哈希一致性核验（全部一致）。
- 给出 Standalone 与 VST3 的部署结构差异与发布建议。

## Task Commits

本计划为验证与文档产出任务，未修改业务源码；本次以文档产物提交。

## Files Created/Modified
- `.planning/phases/06-编译验证/06-04-BUILD-COMPARISON.md` - 产物对比报告。
- `.planning/phases/06-编译验证/06-04-SUMMARY.md` - 本计划总结。

## Decisions Made
- 采用“大小 + 哈希”双维度验证，避免仅以文件存在性判断正确。
- 将 `models/*.onnx` 体积异常小定义为“已观测事实”，不在本计划内越界修改模型资产。

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] 计划中的 VST3 依赖目录假设与实际构建目录不一致**
- **Found during:** Task 2/4
- **Issue:** 计划假设 `Contents/Resources`，实际为 `Contents/x86_64-win`。
- **Fix:** 按真实目录执行依赖收集与哈希比对。
- **Files modified:** `.planning/phases/06-编译验证/06-04-BUILD-COMPARISON.md`
- **Verification:** DLL/模型文件完整且哈希一致。

---

**Total deviations:** 1 auto-fixed (1 validation-path bug)
**Impact on plan:** 不影响 TEST-03 目标，确保对比数据来自真实产物路径。

## Issues Encountered
- 无阻塞问题。

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 6 的 TEST-01/02/03 已有执行证据与总结文档。
- 可进入阶段级验证（生成 06-VERIFICATION.md 并判断 TEST-04 闭环状态）。

## Self-Check: PASSED
