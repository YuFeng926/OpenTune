---
phase: quick-260406-fj9-clipselectioncontext-standalone-auto-cli
plan: 01
subsystem: standalone-selection-context
completed_at: 2026-04-06
requirements:
  - PROC-05
  - PROC-06
key_files:
  modified:
    - Source/Standalone/PluginEditor.h
    - Source/Standalone/PluginEditor.cpp
    - Source/Standalone/UI/ArrangementViewComponent.h
    - Source/Standalone/UI/ArrangementViewComponent.cpp
    - Source/Standalone/UI/PianoRollComponent.h
    - Source/Standalone/UI/PianoRollComponent.cpp
    - Source/PluginProcessor.h
    - Source/PluginProcessor.cpp
    - Tests/TestMain.cpp
decisions:
  - 选择单一真相固定为 ClipSelectionContext{trackId, clipId}，clipIndex 仅在入口按 clipId 派生。
  - NoSelection 统一为显式无效态（selectedClipIndex=-1，trackId=-1/clipId=0）。
  - AUTO 前置条件双端收敛：上下文可解析 + OriginalF0State::Ready，否则立即拒绝。
---

# Phase quick-260406-fj9 Plan 01: ClipSelectionContext Standalone AUTO Summary

完成了 Standalone 选择链路的结构性收敛：先清理并行写路径，再统一 clipId 主导的上下文语义，并把 AUTO 启动严格绑定到有效上下文与 OriginalF0 Ready。

## Task 1 — 清理并行选择写路径，建立唯一入口

### 完成内容
- `ArrangementViewComponent::Listener::clipSelectionChanged` 事件语义从 `(trackId, clipIndex)` 升级为 `(trackId, clipId)`。
- Arrangement 侧移除多处直接写 processor 选择状态的并行路径，只上报选择事实。
- Editor 侧通过统一入口 `applyClipSelectionContext(trackId, clipId)` 完成：
  - clipId → clipIndex 派生
  - processor/trackPanel/pianoRoll 同步
  - clipId 无效时统一 NoSelection 清空

### 验证
- `cmake --build build --config Debug --target OpenTune_Standalone` 通过。

### 提交
- `1c0fe99` `refactor(quick-260406-fj9-clipselectioncontext-standalone-auto-cli-01): consolidate standalone clip selection entry`

## Task 2 — 统一 ClipSelectionContext 单一真相并显式 NoSelection

### RED
- 新增选择语义失败用例（空轨 NoSelection、clipId 派生、无效上下文安全返回）。

### GREEN
- `TrackState::selectedClipIndex` 默认值改为 `-1`。
- Processor 中空轨/无效选择路径统一为 `-1`，删除伪有效 `0` 回写语义。
- PianoRoll 强化上下文判定：
  - `hasActiveClipContext()` 要求 `trackId>=0 && clipId!=0`
  - AUTO 前先校验上下文可解析到 clipIndex，否则拒绝

### 验证
- `cmake --build build --config Debug --target OpenTuneTests; if ($?) { .\build\Debug\OpenTuneTests.exe }` 通过（新增语义用例通过）。

### 提交
- `a08da63` `test(quick-260406-fj9-clipselectioncontext-standalone-auto-cli-01): add failing selection context semantics tests`
- `1e8bdfc` `fix(quick-260406-fj9-clipselectioncontext-standalone-auto-cli-01): unify NoSelection with clip-context validation`

## Task 3 — AUTO 前置条件绑定统一上下文

### 完成内容
- 在 PianoRoll 端增加硬性前置条件：
  - clip context 可解析（`getClipIndexById >= 0`）
  - `OriginalF0State == Ready`
  - 任一不满足立即返回 false，不进入渲染流程
- 保持 `correctionInFlight_` + `clipContextGeneration_` 失效机制，`clearClipContext()` 立即释放 in-flight latch。
- 补充测试覆盖：
  - NoSelection/无效上下文拒绝 AUTO
  - 有效上下文 + Ready 可 enqueue
  - clear context 后 in-flight 安全释放

### 验证
- `cmake --build build --config Debug --target OpenTuneTests; if ($?) { .\build\Debug\OpenTuneTests.exe }` 通过。
- `cmake --build build --config Debug --target OpenTune_Standalone` 通过。

### 提交
- `458077b` `test(quick-260406-fj9-clipselectioncontext-standalone-auto-cli-01): lock AUTO preconditions to valid clip context`

## Deviations from Plan

无偏离；按“先清冗余再统一语义”顺序执行。

## Environment / Blocking Evidence

本次任务无阻塞；构建与测试均可执行并通过。

## Known Stubs

未发现影响本计划目标达成的占位 stub。
