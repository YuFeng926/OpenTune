---
phase: 20-统一失效入口
reviewed: 2026-04-14T17:24:35.9712314Z
depth: deep
files_reviewed: 13
files_reviewed_list:
  - Source/Standalone/UI/PianoRollComponent.h
  - Source/Standalone/UI/PianoRollComponent.cpp
  - Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h
  - Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp
  - Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h
  - Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp
  - Tests/TestMain.cpp
  - .planning/phases/20-统一失效入口/20-TEST-VERIFICATION.md
  - .planning/phases/20-统一失效入口/20-VERIFICATION.md
  - Source/Standalone/PluginEditor.cpp
  - Source/Plugin/PluginEditor.cpp
  - Source/Plugin/PluginEditor.h
  - Source/Standalone/UI/FrameScheduler.h
findings:
  critical: 0
  warning: 3
  info: 0
  total: 3
status: issues_found
---

# Phase 20: Code Review Report

**Reviewed:** 2026-04-14T17:24:35.9712314Z
**Depth:** deep
**Files Reviewed:** 13
**Status:** issues_found

## Summary

本次审查覆盖了 Phase 20 从 `e645a2c` 到 `59d7a047e3e1283ddb363db88cf313971e381423` 的统一失效入口实现、最终落盘文件，以及为定位行为回归额外读取的 editor heartbeat / scheduler 集成点。没有发现新的安全问题，但发现 1 个明确的交互行为回归，以及 2 个会削弱 Phase 20 gate 可信度的验证问题：暂停态编辑反馈现在被 editor heartbeat 限速到 30Hz/10Hz；测试程序不会以失败退出；verification 文档还 overclaim 了未真正执行的静态审计与 wiring 覆盖。

## Warnings

### WR-01: 非播放交互 repaint 被退化为 editor heartbeat 节奏

**File:** `Source/Standalone/UI/PianoRollComponent.cpp:1201`
**Issue:** `invalidateVisual()` 现在只把请求 merge 进 `pendingVisualInvalidation_`，并不会自己安排 flush；真正 flush 只发生在 `Source/Standalone/UI/PianoRollComponent.cpp:1374`、`Source/Standalone/UI/PianoRollComponent.cpp:1386`、`Source/Standalone/UI/PianoRollComponent.cpp:1431` 这些 timer / heartbeat / VBlank 入口。与此同时，用户输入路径已经全部改成只调用 `invalidateVisual()`：例如 `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp:28`、`Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp:171`、`Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp:209`，以及暂停态时间线点击通过 `Source/Standalone/UI/PianoRollComponent.cpp:233` 和 `Source/Standalone/UI/PianoRollComponent.cpp:1314` 只登记 playhead invalidation。结果是暂停态拖拽、框选、时间线点击、缩放等基础编辑反馈，不再走旧的“同帧/近同帧 interactive scheduler bridge”，而是最多只能按 editor heartbeat flush：Standalone idle 为 30Hz、推理中降到 10Hz（`Source/Standalone/PluginEditor.cpp:29`, `Source/Standalone/PluginEditor.cpp:288`, `Source/Standalone/PluginEditor.cpp:828`），VST3 为 30Hz（`Source/Plugin/PluginEditor.h:149`, `Source/Plugin/PluginEditor.cpp:294`, `Source/Plugin/PluginEditor.cpp:330`）。这会把交互可见反馈拉长到约 33ms，Standalone 推理中甚至约 100ms，是明确的行为回归。
**Fix:** 保持 `invalidateVisual(...)` 作为唯一入口，但让它在 message-thread interactive 场景下自己安排一次单次 flush（例如 component-local async flush 或统一的 interactive scheduler bridge），不要把暂停态编辑反馈完全依赖 editor heartbeat。Phase 21 可以继续清理多时钟，但 Phase 20 不应先把基础编辑响应退化到 30Hz/10Hz。

### WR-02: Phase 20 的 L2/L6 gate 目前不会因为测试失败而 fail

**File:** `Tests/TestMain.cpp:102`
**Issue:** `logFail()` 只打印日志，不累计失败状态；`main()` 从 `Tests/TestMain.cpp:4191` 开始执行完整测试后固定 `return 0`。这意味着 `OpenTuneTests.exe` 即使已经打印了 `[FAIL]`，进程退出码仍然是成功。当前 Phase 20 verification 又把 `EXITCODE=0` 直接记录成 blocker none（`.planning/phases/20-统一失效入口/20-TEST-VERIFICATION.md:60`, `.planning/phases/20-统一失效入口/20-TEST-VERIFICATION.md:78`, `.planning/phases/20-统一失效入口/20-TEST-VERIFICATION.md:112`），所以 L2/L6 在自动化意义上并不是 blocking gate，回归可以静默漏过。
**Fix:** 给测试 harness 增加全局 failure counter 或汇总返回值；`logFail()` 标记失败，`main()` 根据失败总数返回非零。然后把 verification gate 明确绑定到进程退出码，而不是只摘录日志中的 `[PASS]` 行和 `EXITCODE=0`。

### WR-03: Verification 文档夸大了已验证范围，且没有覆盖 component wiring / cadence 回归

**File:** `.planning/phases/20-统一失效入口/20-TEST-VERIFICATION.md:13`
**Issue:** Phase 20 新增自动化测试只覆盖了 reducer 纯逻辑：`runPhase20UnifiedInvalidationTests()` 从 `Tests/TestMain.cpp:3770` 开始，只验证 `PianoRollVisualInvalidationState` / `makeVisualFlushDecision(...)`，没有实例化 `PianoRollComponent`、`PianoRollToolHandler` 或 editor heartbeat/VBlank 路径，因此完全测不到实际 wiring 是否把交互 repaint 拖慢。与此同时，记录的 L4 命令并没有搜索 `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp`（`.planning/phases/20-统一失效入口/20-TEST-VERIFICATION.md:13`），但执行结果却声称命中了该文件（`.planning/phases/20-统一失效入口/20-TEST-VERIFICATION.md:91`），`20-VERIFICATION.md` 也进一步把 `.h / .cpp` 都写成已验证（`.planning/phases/20-统一失效入口/20-VERIFICATION.md:43`）。所以当前 verification 证据既没有覆盖真实 component-level 行为，又在静态审计范围上存在自相矛盾，不能支撑“Phase 20 wiring 已被充分验证”的结论。
**Fix:** 先修正文档里的 L4 命令与结果，让实际搜索范围和落盘证据一致；再补一条 component integration regression：在桌面 host 中实例化 `PianoRollComponent`，模拟 paused 状态下的 timeline click / mouse drag / scroll/zoom，pump message loop 后断言 flush 不依赖 editor heartbeat 才能发生。否则 Phase 20 只能证明 reducer contract 正确，不能证明最终 wiring 正确。

## Remediation Status

- 2026-04-15 current live tree 已关闭 WR-01 / WR-02 / WR-03：PianoRoll flush cadence 已切到 `onVisualVBlankCallback(...)`，`Tests/TestMain.cpp` 已把失败状态接到进程退出码，`.planning/phases/20-统一失效入口/20-TEST-VERIFICATION.md` 与 `.planning/phases/20-统一失效入口/20-VERIFICATION.md` 已按 fresh evidence 重写。

---

_Reviewed: 2026-04-14T17:24:35.9712314Z_
_Reviewer: the agent (gsd-code-reviewer)_
_Depth: deep_
