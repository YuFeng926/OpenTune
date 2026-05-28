# Standalone Experimental Features Gate And TimeTool Anchor Seed

**日期:** 2026-05-27
**状态:** Verified / Main Contract Landed
**验证文档:** `.planning/plans/2026-05-27-standalone-experimental-features-gate-and-time-tool-anchor-seed-test-verification.md`

## 1. Contract Summary

本合同覆盖两个边界：

- Standalone 实验功能入口由单独 app-level preference 控制。
- TimeTool 首次进入 clip 时只播种 identity handles，且 seed 由 processor 单入口负责。

AUTO(REF) 主链由
`.planning/plans/2026-05-27-auto-ref-reference-driven-pitch-and-timing-alignment.md` 定义。本文件只定义 UI exposure gate
与 TimeTool seed 边界。

## 2. Experimental Features Gate

`experimentalFeaturesEnabled` 是唯一实验功能曝光开关。

该开关控制：

- TimeTool toolbar button 是否展示。
- PianoRoll tool menu 是否包含 TimeTool。
- TimeTool 快捷入口是否允许切入。
- Arrangement reference button 是否绘制。
- Arrangement reference hover / cursor / click / menu 是否可达。
- Shared preferences 中的实验能力提示。

该开关不控制 AUTO(REF) 算法行为，也不写入 project state。

## 3. AUTO Ref Mode Preference

AUTO Ref mode preference 只表达 reference alignment 是否启用以及使用正式产品路径。

允许的产品语义：

- `Off`
- `Game`

该 preference 不再兼任实验功能总开关，不再承担 UI exposure 语义。

## 4. TimeTool Seed Contract

首次从非 TimeTool 切换到 TimeTool 时：

- 如果当前 materialization 已有内部 handles，no-op。
- 如果当前 time grid 已有用户 stretch 编辑，no-op。
- 如果当前 grid 只是默认 identity endpoints，则 processor 准备 timing features。
- processor 根据 `ReferenceTimingFeatures.anchors` 生成 identity internal handles。
- 所有 seeded handles 满足 `source_seconds == output_seconds`。
- seed 不改变 clip duration、不改变 playback timing、不触发 reference alignment。

再次进入 TimeTool 时不得覆盖已有用户编辑。

## 5. Ownership

UI ownership:

- 展示/隐藏 TimeTool 与 reference UI。
- 触发用户选择 TimeTool。
- 在实验开关关闭后回退到稳定工具。

Processor ownership:

- `ensureTimeToolAnchorSeed(...)` 是 seed 的唯一入口。
- 读取 materialization snapshot。
- 确认是否需要 seed。
- 构建或读取 `ReferenceFeatureSet`。
- 将 timing anchors 编译为 identity `TimeGridSnapshot`。
- 通过 store 提交结果。

Store ownership:

- 持有 `TimeGridSnapshot` 真值。
- 保持已有用户编辑不被 seed 覆盖。

## 6. Files Owning This Contract

- `Source/Utils/AppPreferences.h`
- `Source/Utils/AppPreferences.cpp`
- `Source/Editor/Preferences/SharedPreferencePages.h`
- `Source/Editor/Preferences/SharedPreferencePages.cpp`
- `Source/Standalone/UI/ParameterPanel.h`
- `Source/Standalone/UI/ParameterPanel.cpp`
- `Source/Standalone/UI/PianoRollComponent.cpp`
- `Source/Standalone/UI/ArrangementViewComponent.h`
- `Source/Standalone/UI/ArrangementViewComponent.cpp`
- `Source/Standalone/PluginEditor.h`
- `Source/Standalone/PluginEditor.cpp`
- `Source/PluginProcessor.h`
- `Source/PluginProcessor.cpp`
- `Source/MaterializationStore.h`
- `Tests/TestAutoRefArchitecture.cpp`
- `Tests/TestAutoRefIntegration.cpp`

## 7. Final Kill List

本合同不得出现：

- UI 层自行创建 `TimeGridSnapshot`。
- toolbar 隐藏但 menu / shortcut / click path 仍可进入 TimeTool 的半门控。
- reference button 只隐藏 draw path 但 hover / cursor / click 仍可达。
- 关闭实验开关后 destructive clear 用户数据。
- TimeTool seed 触发 reference alignment。
- TimeTool seed 生成非 identity timing。
- seed 入口散落在 toolbar、menu、mouse handler 等多个 UI path。
- 独立于 `ReferenceFeatureSet` 的 seed-only feature path。

## 8. Completion Definition

本合同已满足：

- `experimentalFeaturesEnabled` 独立于 AUTO Ref mode。
- TimeTool 与 Arrangement reference UI 的 draw / menu / shortcut / hover / cursor / click 均受统一开关控制。
- `ensureTimeToolAnchorSeed(...)` 是 processor-owned 单入口。
- seed 只生成 identity handles。
- re-entry 不覆盖已有 stretch 编辑。
- focused verification 已完成，详见配套验证文档。
