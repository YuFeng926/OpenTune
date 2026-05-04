---
phase: 17-真实长度发布与缓存派生
plan: 02
subsystem: testing
tags: [tests, verification, windows-linking, render-cache, sample-boundary]

requires:
  - phase: 17-01
    provides: Sample-authoritative RenderCache publish contract and consumer wiring
provides:
  - Phase 17 regression coverage for true-length base/resampled cache publishing
  - Phase 17 verification docs with real L1/L2/L4/L6 evidence
  - Windows/MSVC test-target link semantics restored for `OpenTuneTests`
affects: [phase-gate, 18, regression-suite, windows-build]

tech-stack:
  added: []
  patterns:
    - "Windows test targets link against explicit MSVC + UCRT + UM runtime directories instead of ambient shell state"
    - "Chunk-boundary regressions verify the pure sample-boundary helper directly, not a racing background worker"

key-files:
  created:
    - .planning/phases/17-真实长度发布与缓存派生/17-VERIFICATION.md
  modified:
    - CMakeLists.txt
    - Source/PluginProcessor.h
    - Source/PluginProcessor.cpp
    - Tests/TestMain.cpp
    - .planning/phases/17-真实长度发布与缓存派生/17-TEST-VERIFICATION.md
    - .planning/phases/17-真实长度发布与缓存派生/deferred-items.md

key-decisions:
  - "D17-02-01: OpenTuneTests must not depend on transient VsDevCmd LIB state; link paths are derived from the configured toolchain instead"
  - "D17-02-02: Phase 16 boundary regressions must target deterministic pure sample-boundary logic instead of racing the live render worker"

patterns-established:
  - "Pattern: fix MSVC linker gates structurally by restoring explicit toolchain search paths, not by local shell hacks"
  - "Pattern: playback fallback assertions follow sample-truth layering: resampled -> rendered -> dry"

requirements-completed: [TASK-05, SAMP-03]

duration: 31min
completed: 2026-04-13
---

# Phase 17 Plan 02: verification closure and linker gate resolution Summary

**Phase 17 现在既有真实回归测试，也有完整的 verification traceability；`OpenTuneTests` 的 Windows linker 语义也被修正到稳定可复现状态，完整套件重新通过。**

## Performance

- **Duration:** 31 min
- **Started:** 2026-04-13T11:32:01Z
- **Completed:** 2026-04-13T12:03:37Z
- **Tasks:** 3
- **Files modified:** 6

## Accomplishments

- 创建并回填了 `17-TEST-VERIFICATION.md`，把 L1/L2/L4/L6 真实命令与结果全部落盘
- 新增并跑通了 Phase 17 regression tests，直接守住 `TASK-05` 与 `SAMP-03`
- 从第一性原理修复了 `OpenTuneTests.exe` 的 linker gate：不再依赖缺失的隐式 link dirs 或外部 shell `LIB` 状态
- 让 Phase 16 的边界回归测试摆脱 live worker 竞态，完整回归套件重新稳定通过

## Task Commits

1. **Task 1: 先创建 Phase 17 的 test-verification 骨架** - `1357b27` (docs)
2. **Task 2: 为真实长度发布与 sample->time 投影添加回归测试** - `e652ccf` (test)
3. **Task 3: 执行验证并写出 traceability 文档** - `7caa2e0` (fix), `1f41c48` (test)

## Files Created/Modified

- `CMakeLists.txt` - 为 Windows/MSVC 最终链接目标补齐显式 runtime link directories，恢复 `OpenTuneTests` 链接语义
- `Source/PluginProcessor.h` - 暴露纯 chunk-boundary helper 以供 deterministic regression 使用
- `Source/PluginProcessor.cpp` - 实现可复用的 chunk-boundary helper 静态入口
- `Tests/TestMain.cpp` - 新增 Phase 17 测试，并把 sample-boundary / playback fallback 旧回归修正到新契约
- `.planning/phases/17-真实长度发布与缓存派生/17-TEST-VERIFICATION.md` - 回填真实 L1/L2/L4/L6 结果
- `.planning/phases/17-真实长度发布与缓存派生/17-VERIFICATION.md` - requirement 级 traceability

## Decisions Made

- Windows 下的 `OpenTuneTests` 链接不能再依赖外部 developer shell 是否正确保留 `LIB`；链接语义必须由 CMake 配置本身显式给出
- 旧的 phase 16 regression 若本质上要验证 pure sample-boundary logic，就不应继续依赖后台 worker 的调度时序

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] `OpenTuneTests.exe` 在 Windows 上缺少 CRT/UCRT/UM 链接路径**
- **Found during:** Task 3 (L1 build)
- **Issue:** `OpenTuneTests.exe` 链接阶段出现 `_purecall`、`operator new/delete`、`atexit`、`__std_terminate`、`free` 等全局未解析符号，根因是 CMake toolchain metadata 中 `CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES` 为空，而构建链路又没有可用 `LIB` 环境留到 Ninja link step。
- **Fix:** 在 `CMakeLists.txt` 中从 `CMAKE_CXX_COMPILER` / `CMAKE_MT` 推导 MSVC + Windows SDK runtime library 目录，并显式注入所有最终链接目标。
- **Files modified:** `CMakeLists.txt`
- **Verification:** `cmake -S . -B build && cmake --build build --target OpenTuneTests --config Release` 通过
- **Committed in:** `7caa2e0`

**2. [Rule 1 - Bug] 旧 playback fallback 测试仍按 seconds-truth 断言 dry tail**
- **Found during:** Task 3 (L2 test run)
- **Issue:** 旧断言假设 resampled cache 的 host-rate长度定义真实时间窗，但 Phase 17 已把真实时间窗收敛到 render-rate sample truth；因此 true span 内应先回退到 rendered cache，再回退到 dry。
- **Fix:** 更新 `Four-level fallback: partial resampled read falls back to rendered before dry` 的断言，验证的是 fallback 层级与 mono cache 语义，而不是旧 seconds-based 数值假设。
- **Files modified:** `Tests/TestMain.cpp`
- **Verification:** `OpenTuneTests.exe` 重新通过该用例
- **Committed in:** `1f41c48`

**3. [Rule 3 - Blocking] Phase 16 hop-aligned regression 与 live render worker 存在竞态**
- **Found during:** Task 3 (L2 test run after fallback fix)
- **Issue:** `OpenTuneAudioProcessor` 构造时会立即启动 chunk render worker，旧测试在 `enqueuePartialRender()` 后直接拉 pending jobs，存在被后台 worker 抢先消费的竞态。
- **Fix:** 将 pure `buildChunkBoundariesFromSilentGaps(...)` 提升为 `OpenTuneAudioProcessor` 静态 helper，并把两个 phase 16 regression 改成“boundary helper + RenderCache requestRenderPending” 的确定性链路。
- **Files modified:** `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp`, `Tests/TestMain.cpp`
- **Verification:** `OpenTuneTests.exe` 重新通过全部 hop-aligned chunk regressions
- **Committed in:** `1f41c48`

---

**Total deviations:** 3 auto-fixed (1 bug, 2 blocking)
**Impact on plan:** 都属于穿过 Phase 17 verification gate 所必须完成的正确性修复，没有引入兼容层或并行结构。

## Issues Encountered

- Windows/MSVC toolchain detection没有给 Ninja link step 带出隐式运行时库目录，导致测试目标先前根本无法完成链接
- Phase 17 的 sample-truth 语义使一个旧 playback fallback 测试的期待值过时，需要改回正确分层语义

## Next Phase Readiness

- `TASK-05` 与 `SAMP-03` 已有直接测试与静态证据，Phase 17 已具备进入状态更新和 phase 完成的条件
- Phase 18 可以在当前 sample-authoritative cache/time projection 基线之上继续推进 UI 时间轴投影

## Self-Check: PASSED

- Summary file exists at `.planning/phases/17-真实长度发布与缓存派生/17-02-SUMMARY.md`
- Commits `1357b27`, `e652ccf`, `7caa2e0`, and `1f41c48` are present in git history
