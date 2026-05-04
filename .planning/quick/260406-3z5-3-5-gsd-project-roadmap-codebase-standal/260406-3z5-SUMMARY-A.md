---
phase: quick-260406-3z5
plan: 01
task: 1
executor: A
status: complete
commit: e356443
files:
  - Source/PluginProcessor.h
  - Source/PluginProcessor.cpp
---

# Task 1 (Executor-A / Processor 域) Summary

## 目标
消除 VST3 并行 Clip 状态缓存，统一为 `track0 + selectedClipIndex` 单一权威源。

## 实施结果
- 删除 `CurrentClipState` 死定义与 `currentClipIdForPlugin_` 并行状态成员。
- `getCurrentClipIdForPlugin()` 改为在读锁下从 `tracks_[0].clips + selectedClipIndex` 实时推导。
- 清理 `currentClipIdForPlugin_` 的 `.store/.load` 同步分支。
- 在导入、插入、删除、移动等变更路径中统一维护 `selectedClipIndex`，避免激活 Clip 漂移。
- 对外接口签名保持不变，仅收敛内部状态语义。

## 验证
- `grep "struct CurrentClipState|currentClipIdForPlugin_"`（PluginProcessor.h/cpp）无匹配。
- `grep "currentClipIdForPlugin_\.store|currentClipIdForPlugin_\.load"`（PluginProcessor.cpp）无匹配。
- `cmake --build build-vst3 --config Debug --target OpenTune_VST3` 通过。

## 约束符合性
- 仅修改 `Source/PluginProcessor.h`、`Source/PluginProcessor.cpp`。
- 未改动 `Source/Standalone/**`。
