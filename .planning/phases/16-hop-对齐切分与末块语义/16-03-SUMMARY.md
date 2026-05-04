---
phase: 16-hop-对齐切分与末块语义
plan: 03
subsystem: testing
tags: [tests, verification, traceability, hop-alignment]

requires:
  - phase: 16-02
    provides: Final worker padding semantics and padded-tail F0 gating
provides:
  - Phase 16 regression tests in OpenTuneTests
  - Phase 16 test-verification document with real commands and outcomes
  - Requirement traceability document for all five Phase 16 requirements
affects: [phase-gate, 17, regression-suite]

tech-stack:
  added: []
  patterns:
    - "Shared-core render phases verify through OpenTuneTests plus contract grep evidence"
    - "Verification docs record the real VS environment path and single-config test binary path"

key-files:
  created:
    - .planning/phases/16-hop-对齐切分与末块语义/16-TEST-VERIFICATION.md
    - .planning/phases/16-hop-对齐切分与末块语义/16-VERIFICATION.md
  modified:
    - Tests/TestMain.cpp

key-decisions:
  - "D16-03-01: Phase 16 follows test-driven-spec by creating test-verification before final test implementation claims"
  - "D16-03-02: Existing Phase 15 frozen-boundary tests now validate the sample-aware last-chunk contract instead of the old seconds-based middle-chunk behavior"

patterns-established:
  - "Pattern: hop-aligned segmentation tests inject explicit silent gaps through replaceClipAudioById to avoid insert-time recomputation"
  - "Pattern: verification docs bind each requirement to a concrete test name or grep command"

requirements-completed: [CHNK-01, CHNK-02, CHNK-03, CHNK-04, TASK-04]

duration: 40min
completed: 2026-04-13
---

# Phase 16 Plan 03: 自动化证据与 traceability Summary

**`OpenTuneTests` 现在直接覆盖 hop 对齐切分、无合法 hop 点时跳过静息处、only-last-chunk padding，以及 padded-tail F0 不再延伸四个核心场景；Phase 16 的五个 requirement 也都绑定到了真实命令和目标文件。**

## Performance

- **Duration:** 40 min
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments

- 在 `Tests/TestMain.cpp` 新增 `runPhase16HopAlignedChunkTests()` 并接入主测试入口
- 把 `RenderCache` 旧单测和 Phase 15 frozen-boundary 单测同步迁移到 sample-aware 新契约
- 用真实构建/测试结果写出 `16-TEST-VERIFICATION.md`
- 用 requirement 级 traceability 写出 `16-VERIFICATION.md`

## Task Commits

1. **Task 1: 为 hop 对齐切分与末块 padding 语义添加回归测试** - `71b71ca` (test)
2. **Task 2: 生成 Phase 16 的测试验证文档与 requirement traceability** - `127d40a` (docs)

## Files Created/Modified

- `Tests/TestMain.cpp` - 新增 Phase 16 回归测试，并把旧 RenderCache/Phase 15 测试更新到 sample-aware 契约
- `.planning/phases/16-hop-对齐切分与末块语义/16-TEST-VERIFICATION.md` - 记录真实执行的 L1/L2/L4/L6 命令和结果
- `.planning/phases/16-hop-对齐切分与末块语义/16-VERIFICATION.md` - 记录五个 Phase 16 requirement 的逐项证据

## Decisions Made

- 为满足 `test-driven-spec` 的硬约束，先创建 `16-TEST-VERIFICATION.md`，再开始实现和执行测试
- hop-aligned segmentation 测试不依赖 `insertClipSnapshot()` 的自动静息处计算，而是通过 `replaceClipAudioById(..., silentGaps)` 明确注入测试所需的 sample-domain gaps

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] 先创建 `16-TEST-VERIFICATION.md` 再进入测试实现**
- **Found during:** Task 1 start
- **Issue:** `test-driven-spec` 要求 test-verification 文档必须先于测试实现存在，而原 plan 把它放在 Task 2
- **Fix:** 先创建 `16-TEST-VERIFICATION.md` 骨架，再实现测试，最后回填真实结果
- **Files modified:** `.planning/phases/16-hop-对齐切分与末块语义/16-TEST-VERIFICATION.md`
- **Impact:** 只调整执行顺序，不降低任何测试或验证标准

**2. [Rule 3 - Blocking] 改为动态发现本机可用的 VS Developer shell**
- **Found during:** L1 build verification
- **Issue:** 预设的 VS 2022 路径不存在，初次构建命令无法进入开发者环境
- **Fix:** 改为通过 `vswhere.exe` 动态定位 `VsDevCmd.bat`，并把命令收敛到 `.planning/scripts/invoke-msvc-cmake.ps1`
- **Files modified:** `.planning/phases/16-hop-对齐切分与末块语义/16-TEST-VERIFICATION.md`
- **Impact:** 只修正环境命令，不改变产品代码和验证门槛

**3. [Rule 3 - Blocking] 修复未初始化 vocoder hop size 导致的测试失真**
- **Found during:** `OpenTuneTests` first run
- **Issue:** 未初始化的 `VocoderDomain::getVocoderHopSize()` 返回 0，导致 `enqueuePartialRender()` 的边界构建走空列表
- **Fix:** 仅当当前 hopSize > 0 时才覆盖默认 512；worker 侧同样应用该规则
- **Files modified:** `Source/PluginProcessor.cpp`
- **Impact:** 恢复 Phase 16 预期的 hop 对齐切分，不引入额外兼容层

## Known Stubs

- None.

## Self-Check: PASSED

- Summary file exists and task commit hashes `71b71ca` / `127d40a` are present in git history
