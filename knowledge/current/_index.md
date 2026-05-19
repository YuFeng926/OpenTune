---
spec_version: 1.0.0
generated_by: orchestrator-agent
last_updated: 2026-05-05
---

# Spec 文档索引

## 最新版本：2026-05-05_v1.0

> 跨日期规则：因 2026-05-05 ≠ 上次运行日期 2026-04-10，版本号重置为 v1.0；旧 2026-04-10 版本作为快照归档。

## 版本历史

| 版本 | 日期 | 模块数 | 文档数 | 说明 |
|------|------|--------|--------|------|
| 2026-05-05_v1.0 | 2026-05-05 | 9 | 43 | 全量重建：新增 ara-vst3 模块；同步 v1.3+ ARA/VST3 双格式、Materialization Pipeline、UndoManager、ChromaKeyDetector、CrossoverMixer、OnnxVocoderBase、Editor/Preferences 等结构变更 |
| 2026-04-10_v1.0 | 2026-04-10 | 8 | 38 | 初次生成 |

## 当前版本模块状态

| 模块 | overview | api | data-model | business | 状态 |
|------|----------|-----|------------|----------|------|
| core-processor | ✅ | ✅ | ✅ | ✅ | draft |
| ara-vst3 | ✅ | ✅ | ✅ | ✅ | draft |
| inference | ✅ | ✅ | ✅ | ✅ | draft |
| dsp | ✅ | ✅ | ✅ | ✅ | draft |
| pitch-correction | ✅ | ✅ | ✅ | ✅ | draft |
| ui-piano-roll | ✅ | ✅ | ✅ | ✅ | draft |
| ui-main | ✅ | ✅ | ✅ | ✅ | draft |
| ui-theme | ✅ | ✅ | ✅ | ✅ | draft |
| utils | ✅ | ✅ | ✅ | ✅ | draft |

状态：✅ 已生成 | 🔄 生成中 | ⚠️ 有警告 | ❌ 失败 | `-` 不适用

## 全局文档

| 文档 | 状态 |
|------|------|
| constitution.md | ✅ |
| architecture.md | ✅ |
| glossary.md | ✅ |
| cross-cutting/caching.md | ✅ |
| cross-cutting/error-handling.md | ✅ |
| cross-cutting/threading.md | ✅ |
| cross-cutting/undo-affected-range-invariant.md | ✅ |
| cross-cutting/coordinate-system-source-time-display-output.md | ✅ (vocal-time-stretch v7) |
| cross-cutting/two-stage-render-pipeline.md | ✅ (vocal-time-stretch v7) |

## 本次运行摘要

- 运行时间：2026-05-05
- 运行 ID：2026-05-05_v1.0
- 新增/更新文档：43 份（9 模块 × 4 文档 + 7 全局）
- 模块结果：9 success / 0 partial / 0 failed
- 术语冲突：16 条（见 `.knowledge-work/conflicts.json`）
- HyperWiki 模式：跳过（MCP 不可用）
- 主路径：源码扫描（Glob + Grep + Read）

## v1.3+ 关键架构变更（本次同步）

1. **双格式发布**：Standalone + VST3/ARA，通过 `EditorFactory` seam 共享 `PluginProcessor` 内核
2. **Materialization Pipeline**：`MaterializationStore` + `SourceStore` + `StandaloneArrangement` 三件套，编辑/渲染/播放分离
3. **统一 UndoManager**：替代旧 `UndoAction` + `PianoRollUndoSupport`
4. **DSP 重构**：`ChromaKeyDetector` 替代 `ScaleInference`；新增 `CrossoverMixer`（LR4 14 kHz 分频）；新增 `OnnxVocoderBase` 共享基类
5. **Editor/Preferences 子系统**：`SharedPreferencePages` / `StandalonePreferencePages` / `TabbedPreferencesDialog` + `AppPreferences` 双层配置
6. **PianoRoll 视觉失效拆分**：`PianoRollVisualInvalidation` 独立模块
7. **三主题统一**：`OpenTuneLookAndFeel` header-only 集成 BlueBreeze/DarkBlueGrey；Aurora 保留独立 `.cpp`
8. **RenderCache 容量**：256 MB（旧文档误记 1.5 GB，已修正）

## vocal-time-stretch v7 关键架构新增（2026-05-12）

1. **TimeGrid 数据模型**：`Source/Utils/TimeGrid.{h,cpp}`，COW `TimeGridSnapshot` 与
   `PitchCurve` 平级存于 `MaterializationStore`；piecewise-linear `τ` / `τ⁻¹`；
   端点 ClipStart/ClipEnd 永远 locked
2. **Rubber Band R3 拉伸引擎**：`Source/Inference/RubberBandStretcher.{h,cpp}` 包装
   `ThirdParty/rubberband-4.0.0` single-file build，OptionProcessOffline + EngineFiner
   + FormantPreserved；每编辑 reset → setKeyFrameMap → study → process 全重建
3. **双阶段渲染**：Stage 1 `RenderCache`(=PitchCache, chunk-wise) +
   Stage 2 `TimeStretchCache`(clip-wide per-matId)，共享 256MB LRU；详见
   `cross-cutting/two-stage-render-pipeline.md`
4. **检测管线**：`Source/DSP/{OnsetDetector,PhonemeClassifier,WordSegmenter}` +
   `Source/Inference/SileroVadExtractor`(Phase H scaffolding) 在 F0 提取后自动播种
   TimeGrid handles
5. **Time tool UI**：`ToolId::TimeTool=5`，参数面板第 6 按钮 + 'T' 快捷；与 Note 工具
   家族**互斥**(setCurrentTool 内 §8.4 切换清理)；Phase H 增 Shift 多选 + Alt 取消吸附
   + 组拖动同 Δ
6. **τ 投影 cross-cutting 不变量**：`cross-cutting/coordinate-system-source-time-display-output.md`
   规定数据层永远存源时间、显示层 τ 投影、输入层 τ⁻¹ 反投影
7. **Undo affected-range 不变量延伸**：`TimeGridEditAction` 与 `PianoRollEditAction`
   严格平级；构造时显式接受 affected source frame range，禁止从 snapshot diff 反推
