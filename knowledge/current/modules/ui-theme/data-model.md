---
module: ui-theme
type: data-model
generated: true
source_scan: true
---

# ui-theme Data Model

> 基于源码扫描生成。

## 1. ThemeTokens — 语义色 Token 集合

**文件**: `Source/Standalone/UI/ThemeTokens.h:17-80`

所有字段均为 `juce::Colour` 类型（除 `cornerRadius` 为 `float`）。

### Token 分类表

| 分类 | Token 名 | 语义 |
|------|---------|------|
| **品牌/主色** | `primaryPurple` | 主强调色 |
| | `accent` | 辅助强调色 |
| | `lightPurple` | 浅变体 |
| | `darkPurple` | 深变体 |
| **背景层级** | `backgroundDark` | 最深背景 |
| | `backgroundMedium` | 中层背景 |
| | `backgroundLight` | 浅层背景 |
| **渐变** | `gradientTop` | 渐变起点色 |
| | `gradientBottom` | 渐变终点色 |
| **UI 控件** | `panelBorder` | 面板/卡片边框 |
| | `buttonNormal` | 按钮默认态 |
| | `buttonHover` | 按钮悬停态 |
| | `buttonPressed` | 按钮按下态 |
| **3D 效果** | `bevelLight` | 倒角亮面 |
| | `bevelDark` | 倒角暗面 |
| | `glowColor` | 发光色 |
| **文字** | `textPrimary` | 主文本 |
| | `textSecondary` | 次文本 |
| | `textDisabled` | 禁用文本 |
| | `textHighlight` | 高亮文本 |
| **钢琴卷帘** | `rollBackground` | 卷帘区域背景 |
| | `laneC` | C 音轨道高亮 |
| | `laneOther` | 其他音轨道 |
| | `gridLine` | 网格线 |
| **音高曲线** | `originalF0` | 原始 F0 曲线色 |
| | `correctedF0` | 矫正 F0 曲线色 |
| | `shadowTrack` | 影子轨迹 |
| **音符块** | `noteBlock` | 音符填充 |
| | `noteBlockBorder` | 音符边框 |
| | `noteBlockSelected` | 选中态 |
| | `noteBlockHover` | 悬停态 |
| **播放控制** | `playhead` | 播放头 |
| | `timelineMarker` | 时间线标记 |
| | `beatMarker` | 拍子标记 |
| **工具状态** | `toolActive` | 工具激活色 |
| | `toolInactive` | 工具未激活色 |
| | `buttonInactive` | 按钮未激活色 |
| **状态指示** | `statusProcessing` | 处理中（黄） |
| | `statusReady` | 就绪（绿） |
| | `statusError` | 错误（红） |
| **波形** | `waveformFill` | 波形填充 |
| | `waveformOutline` | 波形轮廓 |
| **音阶** | `scaleHighlight` | 音阶高亮 |
| **旋钮** | `knobBody` | 旋钮主体 |
| | `knobIndicator` | 旋钮指示器 |
| **几何** | `cornerRadius` | 全局圆角（`float`，默认 8.0f） |

**Token 总数**: 39 个颜色 + 1 个 float = **40 个 token**。

---

## 2. ThemeStyle — 样式参数集合

**文件**: `Source/Standalone/UI/ThemeTokens.h:82-115`

| 分类 | 字段 | 类型 | 默认值 | 说明 |
|------|------|------|--------|------|
| **圆角** | `panelRadius` | float | 16.0f | 面板圆角 |
| | `controlRadius` | float | 10.0f | 控件圆角 |
| | `fieldRadius` | float | 10.0f | 输入框圆角 |
| | `knobRadius` | float | 999.0f | 旋钮圆角（圆形） |
| **线宽** | `strokeThin` | float | 1.0f | 细描边 |
| | `strokeThick` | float | 2.0f | 粗描边 |
| | `focusRingThickness` | float | 2.0f | 焦点环宽度 |
| **阴影** | `shadowAlpha` | float | 0.25f | 阴影透明度 |
| | `shadowRadius` | int | 10 | 阴影模糊半径 |
| | `shadowOffset` | Point\<int\> | {0, 4} | 阴影偏移 |
| **发光** | `glowAlpha` | float | 0.0f | 发光透明度 |
| | `glowRadius` | float | 0.0f | 发光半径 |
| **倒角** | `bevelWidth` | float | 2.0f | 倒角宽度 |
| | `bevelIntensity` | float | 0.3f | 倒角强度 |
| **动画** | `animationDurationMs` | float | 150.0f | 动画时长(ms) |
| | `hoverGlowIntensity` | float | 0.8f | 悬停发光强度 |
| **VU 表** | `vuLow` | Colour | — | VU 低区间色 |
| | `vuMid` | Colour | — | VU 中区间色 |
| | `vuHigh` | Colour | — | VU 高区间色 |
| | `vuClip` | Colour | — | VU 过载色 |
| **时间显示** | `timeActive` | Colour | — | 时间激活色 |
| | `timeInactive` | Colour | — | 时间非激活色 |
| **开关** | `timeSegmentStyle` | bool | true | 时间是否分段显示 |

---

## 3. 三套主题原始色值定义

### 3.1 BlueBreeze::Colors

**文件**: `BlueBreezeTheme.h:9-52` | 命名空间: `OpenTune::BlueBreeze`

| 色值组 | 常量名 | Hex 值 | 用途 |
|--------|--------|--------|------|
| Graph BG | `GraphBgDeep` | `0xFF7A8F9E` | 主工作区深色 |
| | `GraphBgMid` | `0xFF8CA2B0` | 中间色 |
| | `GraphBgLight` | `0xFFAABCC7` | 浅色 |
| Sidebar | `SidebarBg` | `0xFFC6D4DD` | 侧边栏 |
| | `PanelBorder` | `0xFFB0C0CC` | 面板边框 |
| Knob | `KnobBody` | `0xFF1A1A1A` | 钢琴漆黑 |
| | `KnobBodyLight` | `0xFF2A2A2A` | 亮部黑 |
| | `KnobHighlight` | `0x80FFFFFF` | 高光 |
| | `KnobEdge` | `0xFF404040` | 金属光泽 |
| | `KnobIndicator` | `0xFFE0E0E0` | 银白指针 |
| | `KnobTrack` | `0x40FFFFFF` | 轨道 |
| | `KnobShadow` | `0x40000000` | 阴影 |
| | `KnobGlow` | `0x30FFFFFF` | 悬停发光 |
| Interaction | `ActiveWhite` | `0xFFFFFFFF` | 激活态纯白 |
| | `HoverOverlay` | `0x1AFFFFFF` | Hover 白叠层 |
| | `ShadowColor` | `0xFF5A6A75` | 有色阴影 |
| Node/Accent | `NodeRed` | `0xFFE07A7A` | 柔红 |
| | `NodeYellow` | `0xFFF5D76E` | 柔黄 |
| | `NodePurple` | `0xFF9B59B6` | 柔紫 |
| | `AccentBlue` | `0xFF60A5FA` | 柔蓝 |
| Text | `TextDark` | `0xFF2C3E50` | 深色文本 |
| | `TextLight` | `0xFFF0F4F8` | 浅色文本 |
| | `TextDim` | `0xFF708090` | 暗淡文本 |
| Clip | `ClipGradientTop/Bottom` | `0xFFC8D4E0/0xFFA8B8C8` | 剪辑渐变 |
| | `ClipBorder` | `0xFF8A9CAD` | 剪辑边框 |
| | `ClipSelectedTop/Bottom` | `0xFFD0D8E0/0xFFB8C8D8` | 选中态渐变 |

### 3.2 DarkBlueGrey::Colors

**文件**: `DarkBlueGreyTheme.h:13-99` | 命名空间: `OpenTune::DarkBlueGrey`

完整 48 个色值常量。核心色系：

- **强调色基底**: `#60A5FA` (PrimaryBlue) / `#3B82F6` (Accent) — Tailwind Blue-400/500
- **背景三级**: `#1A212B` → `#222B36` → `#2B3643`
- **文本三级**: `#E6EDF5` → `#A8B4C3` → `#6F7C8C`
- **VU 表**: 青绿 → 柔蓝 → 暖黄 → 柔红

### 3.3 Aurora::Colors

**文件**: `AuroraTheme.h:9-43` | 命名空间: `OpenTune::Aurora`

完整 21 个色值常量。核心色系：

- **背景**: `#151B22` (BgDeep) / `#1A2332` (BgSurface)
- **霓虹色**: Cyan `#3B82F6` / Violet `#8B5CF6` / ElectricBlue `#0070FF` / Magenta `#EC4899` / NeonGreen `#22C55E` / NeonOrange `#F97316` / NeonRed `#EF4444` / NeonYellow `#EAB308`
- **文本**: 纯白 → 60% 白 → 40% 白

---

## 4. UIColors 静态字段（运行时缓存）

**文件**: `UIColors.h:18-93`

`UIColors` 维护与 `ThemeTokens` 同名同数量的 `static inline juce::Colour` 字段。初始值为 DarkBlueGrey 色系（硬编码）。调用 `applyTheme()` 后被覆盖。

关键设计：**UIColors 是全局可变状态**，所有 UI 组件通过 `UIColors::xxx` 直接访问当前主题色值，无需持有 ThemeTokens 引用。

---

## 5. ThemeStyle 各主题实例化值对比

| 参数 | BlueBreeze | DarkBlueGrey | Aurora |
|------|-----------|-------------|--------|
| panelRadius | 16.0 | 16.0 | 16.0 |
| controlRadius | 10.0 | 10.0 | 10.0 |
| fieldRadius | 10.0 | 10.0 | 10.0 |
| knobRadius | 999.0 | 999.0 | 10.0 |
| strokeThin | 1.0 | 1.2 | 1.0 |
| shadowAlpha | 0.25 | 0.25 | 0.25 |
| shadowRadius | 10 | 16 | 10 |
| glowAlpha | 0.0 | 0.0 | 0.6 |
| glowRadius | 0.0 | 0.0 | 12.0 |
| bevelWidth | 0.0 | 1.0 | 0.0 |
| bevelIntensity | 0.0 | 0.12 | 0.0 |
| animationDurationMs | 150 | 120 | 200 |
| hoverGlowIntensity | 0.4 | 0.3 | 1.0 |
| timeSegmentStyle | true | false | false |

**统一化趋势**: 三套主题的 panelRadius、controlRadius、fieldRadius、shadowAlpha 已统一为相同值。

---

## 6. 数据流关系图

```
BlueBreeze::Colors ─┐
DarkBlueGrey::Colors ┼──→ ThemeTokens (值对象，静态局部)
Aurora::Colors ──────┘         │
                               ├──→ Theme::getTokens() ──→ UIColors::applyTheme()
                               │                                  │
BlueBreeze::Style ────┐        │                      覆写 static inline 字段
DarkBlueGrey::Style ──┼→ ThemeStyle (值对象)                      │
Aurora::Style ────────┘        │                                  ▼
                               └──→ Theme::getStyle()    UIColors::xxx (全局访问点)
                                                                  │
                                                         ┌───────┘
                                                         ▼
                                          所有 UI 组件 / LookAndFeel
```

---

## 7. 待确认事项

- **UIColors 初始值与默认主题不一致**: `UIColors` 的硬编码初始值对应 DarkBlueGrey 色系，但 `Theme::activeThemeId_` 默认值为 `ThemeId::Aurora`。如果应用启动时未调用 `applyTheme()`，则 UIColors 色值与实际活跃主题不匹配。
- **ThemeTokens 中 `cornerRadius` 的定位**: 此字段混在颜色 token 中，但属于几何参数。ThemeStyle 中已有更完整的圆角参数。需确认 `cornerRadius` 与 `ThemeStyle::panelRadius` 的关系。
