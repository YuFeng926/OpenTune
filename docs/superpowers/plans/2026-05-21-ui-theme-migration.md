# UI 主题重绘迁移计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 OpenTune-bugfix518 仓库的三套 UI 主题重绘（BlueBreeze/Aurora/DarkBlueGrey）迁移到本工作区

**Architecture:** 两阶段执行。Phase 1 直接覆盖 8 个核心主题文件（零风险），Phase 2 手动适配 8 个消费者文件（只增加新主题绘制分支，保留现有 TimeTool/Layout/AutoTune 等非主题逻辑）

**Tech Stack:** C++17, JUCE, header-only theme system

**源仓库:** `E:\TRAE\OpenTune-bugfix518`
**目标仓库:** `E:\TRAE\OpenTune0519New`

---

## Phase 1: 覆盖 8 个核心主题文件

直接从 bugfix518 复制到本工作区对应路径。

### Task 1.1: 覆盖原始色值文件 (3 files)

**Files:** Copy from bugfix518 → 本工作区
- `Source/Standalone/UI/BlueBreezeTheme.h` (58→96 lines, 完全重写 ~50 新令牌)
- `Source/Standalone/UI/DarkBlueGreyTheme.h` (微小调整)
- `Source/Standalone/UI/AuroraTheme.h` (58→79 lines, 新增 Glass/Piano 色值)

- [ ] **Step 1: Copy BlueBreezeTheme.h**
```bash
Copy-Item "E:\TRAE\OpenTune-bugfix518\Source\Standalone\UI\BlueBreezeTheme.h" -Destination "E:\TRAE\OpenTune0519New\Source\Standalone\UI\BlueBreezeTheme.h" -Force
```

- [ ] **Step 2: Copy DarkBlueGreyTheme.h**
```bash
Copy-Item "E:\TRAE\OpenTune-bugfix518\Source\Standalone\UI\DarkBlueGreyTheme.h" -Destination "E:\TRAE\OpenTune0519New\Source\Standalone\UI\DarkBlueGreyTheme.h" -Force
```

- [ ] **Step 3: Copy AuroraTheme.h**
```bash
Copy-Item "E:\TRAE\OpenTune-bugfix518\Source\Standalone\UI\AuroraTheme.h" -Destination "E:\TRAE\OpenTune0519New\Source\Standalone\UI\AuroraTheme.h" -Force
```

### Task 1.2: 覆盖 Token 聚合与缓存层 (2 files)

- `Source/Standalone/UI/ThemeTokens.h` (364→460 lines, 扩展结构体 + Token 重映射)
- `Source/Standalone/UI/UIColors.h` (406→1146 lines, +740 lines, 新增 15 绘制方法)

- [ ] **Step 4: Copy ThemeTokens.h**
```bash
Copy-Item "E:\TRAE\OpenTune-bugfix518\Source\Standalone\UI\ThemeTokens.h" -Destination "E:\TRAE\OpenTune0519New\Source\Standalone\UI\ThemeTokens.h" -Force
```

- [ ] **Step 5: Copy UIColors.h**
```bash
Copy-Item "E:\TRAE\OpenTune-bugfix518\Source\Standalone\UI\UIColors.h" -Destination "E:\TRAE\OpenTune0519New\Source\Standalone\UI\UIColors.h" -Force
```

### Task 1.3: 覆盖 LookAndFeel 层 (3 files)

- `Source/Standalone/UI/OpenTuneLookAndFeel.h` (1177→1172 lines, 引用新 BlueBreeze 令牌)
- `Source/Standalone/UI/AuroraLookAndFeel.h` (56→63 lines)
- `Source/Standalone/UI/AuroraLookAndFeel.cpp` (403→343 lines, 改用 UIColors 方法)

- [ ] **Step 6: Copy OpenTuneLookAndFeel.h**
```bash
Copy-Item "E:\TRAE\OpenTune-bugfix518\Source\Standalone\UI\OpenTuneLookAndFeel.h" -Destination "E:\TRAE\OpenTune0519New\Source\Standalone\UI\OpenTuneLookAndFeel.h" -Force
```

- [ ] **Step 7: Copy AuroraLookAndFeel.h**
```bash
Copy-Item "E:\TRAE\OpenTune-bugfix518\Source\Standalone\UI\AuroraLookAndFeel.h" -Destination "E:\TRAE\OpenTune0519New\Source\Standalone\UI\AuroraLookAndFeel.h" -Force
```

- [ ] **Step 8: Copy AuroraLookAndFeel.cpp**
```bash
Copy-Item "E:\TRAE\OpenTune-bugfix518\Source\Standalone\UI\AuroraLookAndFeel.cpp" -Destination "E:\TRAE\OpenTune0519New\Source\Standalone\UI\AuroraLookAndFeel.cpp" -Force
```

---

## Phase 2: 适配 8 个消费者文件

在每个消费者文件中，保留所有现有非主题逻辑（TimeTool、Layout、AutoTune 管线等），仅增加新主题 API 调用的分支。参考 bugfix518 同名文件的主题绘制代码，但只提取主题相关部分。

### Task 2.1: 适配 PianoRollRenderer.cpp

**File:** `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp`

需加入的主题适配：
- `drawRuler`: 增加 `isAurora`/`isBlueBreeze` 分支 → 背景用 `fillAuroraTimelineBackground`/`fillMistedTimelineField`，颜色用 `pianoRollGrid`/主题感知 alpha
- `drawGrid`: 增加 Aurora/BlueBreeze 网格颜色分支
- `drawNotes`: 增加三主题 note 渲染（Aurora 玻璃效果、BlueBreeze 柔和效果）
- `drawF0Curve`: 增加主题感知线宽/光晕
- `drawPianoKeys`: 增加 BlueBreeze 键床渲染

保留不变：`drawTimeGridHandles`、`sourceTimeToScreenX`、`chunkBoundaries` 等现有逻辑

### Task 2.2: 适配 PianoRollComponent.cpp

**File:** `Source/Standalone/UI/PianoRollComponent.cpp`

需加入的主题适配：
- `paint`: 增加 Aurora/BlueBreeze 背景分支（`fillAuroraTimelineBackground`/`fillMistedTimelineField`）
- F0 曲线 alpha 使用 `isAuroraTheme()` 判断
- playhead 颜色用 `UIColors::playhead`
- 预览曲线颜色用 `UIColors::correctedF0`

保留不变：TimeTool 相关所有代码（`isTimeView()`、`drawTimeGridHandles`、`mouseDoubleClick`、`commitTimeGrid` 等）、Layout 系统、AutoTune 管线

### Task 2.3: 适配 TrackPanelComponent.h/.cpp

**File:** `Source/Standalone/UI/TrackPanelComponent.h` + `.cpp`

需加入的主题适配：
- Knob 绘制：增加 `drawAuroraKnob`/`drawBlueBreezePianoKnob` 分支
- 轨道卡片：增加 `fillBlueBreezeTrackCard` 分支
- 按钮：增加 `drawAuroraButtonChrome` 分支
- 轨道底色：增加 `isAuroraTheme()` → `auroraTrackAccent`/`drawAuroraGlow` 分支

保留不变：电平表逻辑、Mute/Solo、音量控制

### Task 2.4: 适配 ArrangementViewComponent.cpp

**File:** `Source/Standalone/UI/ArrangementViewComponent.cpp`

需加入的主题适配：
- 时间轴背景：增加 `fillAuroraTimelineBackground`/`fillMistedTimelineField` 分支

保留不变：Clip 拖拽、波形渲染、编排逻辑

### Task 2.5: 适配 TransportBarComponent.cpp

**File:** `Source/Standalone/UI/TransportBarComponent.cpp`

需加入的主题适配：
- 按钮：增加 `drawAuroraButtonChrome` 分支 + 光晕
- 托盘：增加 `fillBlueBreezeTray` 分支
- 显示井：增加 `fillBlueBreezeDisplayWell` 分支

保留不变：BPM、播放/停止、Tap、调式逻辑

### Task 2.6: 适配 TopBarComponent.cpp

**File:** `Source/Standalone/UI/TopBarComponent.cpp`

需加入的主题适配：
- Aurora 主题：`fillAuroraGlass` + `drawAuroraGlassFrame`

保留不变：菜单布局

### Task 2.7: 适配 ParameterPanel.cpp

**File:** `Source/Standalone/UI/ParameterPanel.cpp`

需加入的主题适配：
- Knob：增加 `drawAuroraKnob`/`drawBlueBreezePianoKnob` 分支
- 按钮：增加 `drawAuroraButtonChrome` 分支

保留不变：参数绑定逻辑

### Task 2.8: 验证编译

- [ ] 执行 CMake configure
- [ ] 执行 MSBuild 编译
- [ ] 确认无编译错误
