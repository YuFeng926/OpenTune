---
phase: 01-双格式构建基础
verified: 2026-04-14T22:30:00+08:00
status: passed
score: 4/4 must-haves verified
---

# Phase 1: 双格式构建基础 Verification Report

**Phase Goal:** 在同一代码库里建立 Standalone + VST3 的双格式构建基础，并把 ARA 扩展接入插件定义层。

## Current Reading

- 当前 live tree 不再沿用早期 `-DFORMATS=` 切换式设计。
- 当前更优结构是：`juce_add_plugin(OpenTune ...)` 固定声明 `FORMATS Standalone VST3`，然后用 target-specific build 与条件化 source / post-build 复制维持双格式隔离。
- 这比早期“反复切换 configure 参数”更稳定，也更符合现在的统一工作区设计。

## Observable Truths

| # | Truth | Status | Evidence |
| --- | --- | --- | --- |
| 1 | `OpenTune` 固定声明双格式目标 | ✓ PASS | `CMakeLists.txt:256` contains `FORMATS Standalone VST3` |
| 2 | ARA 扩展在插件定义层启用 | ✓ PASS | `CMakeLists.txt:259` contains `IS_ARA_EFFECT TRUE`; `CMakeLists.txt:54` contains `juce_set_ara_sdk_path(...)` |
| 3 | Standalone / VST3 的运行时复制规则都存在 | ✓ PASS | `CMakeLists.txt:564` 起为 `OpenTune_Standalone POST_BUILD`；`CMakeLists.txt:658` 起为 `OpenTune_VST3 POST_BUILD` |
| 4 | 当前单 build 树内两个目标都能成功生成 | ✓ PASS | `cmake --build build --target OpenTune_Standalone --config Release` 与 `cmake --build build --target OpenTune_VST3 --config Release` 均成功 |

## Behavioral Spot-Checks

| Behavior | Command | Result | Status |
| --- | --- | --- | --- |
| 双格式插件定义存在 | `rg -n "FORMATS Standalone VST3|IS_ARA_EFFECT TRUE" CMakeLists.txt` | 命中 | ✓ PASS |
| ARA SDK 接线存在 | `rg -n "juce_set_ara_sdk_path" CMakeLists.txt` | 命中 | ✓ PASS |
| Standalone 目标构建 | `cmake --build build --target OpenTune_Standalone --config Release` | 成功 | ✓ PASS |
| VST3 目标构建 | `cmake --build build --target OpenTune_VST3 --config Release` | 成功 | ✓ PASS |

## Notes

- 早期 Phase 1 计划里关于 `-DFORMATS="Standalone"` / `-DFORMATS="VST3"` 的验收口径，已经被当前更直接的固定双格式图替代，不再作为阻断项。
- `docs/UserGuide.html` 已恢复到主树，Standalone 的文档复制链重新成立。

## Gate Status

- PASS
