---
module: ui-theme
type: data-model
generated: true
source_scan: true
updated: 2026-05-05
---

# ui-theme 数据模型

## 1. ThemeTokens 字段表

**声明文件**：`Source/Standalone/UI/ThemeTokens.h`
**规模**：43 `juce::Colour` + 1 `float` = 44 字段

### 1.1 Primary / Accent（4）

| 字段 | 类型 | BlueBreeze | DarkBlueGrey | Aurora | 用途 |
|------|------|------------|--------------|--------|------|
| primaryPurple | Colour | `0xFF60A5FA` AccentBlue | `0xFF60A5FA` | `0xFF8B5CF6` Violet | 命名遗留（保留紫色命名但各主题用蓝/紫） |
| accent | Colour | `0xFFFFFFFF` ActiveWhite | `0xFF3B82F6` Electric | `0xFF3B82F6` Cyan | 高亮强调色 |
| lightPurple | Colour | `0xFFD6E4ED` | `0xFF93C5FD` LightBlue | `0xFF0070FF` ElectricBlue | 浅色强调 |
| darkPurple | Colour | `0xFF5A6A75` | `0xFF2563EB` DarkBlue | `0xFF151B22` BgDeep | 深色强调 |

### 1.2 Background（3）

| 字段 | BlueBreeze | DarkBlueGrey | Aurora |
|------|-----------|--------------|--------|
| backgroundDark | `0xFFC6D4DD` SidebarBg | `0xFF1A212B` | `0xFF151B22` BgDeep |
| backgroundMedium | `0xFFD4E0E8` | `0xFF222B36` | `0xFF1A2332` BgSurface |
| backgroundLight | `0xFFE7EEF3` | `0xFF2B3643` | `0x1FFFFFFF` BgOverlay |

### 1.3 Gradient（2）

| 字段 | BlueBreeze | DarkBlueGrey | Aurora |
|------|-----------|--------------|--------|
| gradientTop | `0xFFAABCC7` GraphBgLight | `0xFF273241` | `0xFF151B22` BgDeep |
| gradientBottom | `0xFF7A8F9E` GraphBgDeep | `0xFF171D27` | `0xFF151B22` BgDeep |

### 1.4 UI Element（4）

| 字段 | BlueBreeze | DarkBlueGrey | Aurora |
|------|-----------|--------------|--------|
| panelBorder | `0xFFB0C0CC` | `0xFF3C4A5A` | `0x33FFFFFF` BorderLight |
| buttonNormal | `0x00000000` 透明 | `0xFF2B3643` | `0xFF1A2332` BgSurface |
| buttonHover | `0x1AFFFFFF` HoverOverlay | `0xFF334253` | `0x66FFFFFF` BorderGlow |
| buttonPressed | `0xFFFFFFFF` ActiveWhite | `0xFF1B2430` | `0xFF3B82F6` Cyan |

### 1.5 Bevel / Glow（3）

| 字段 | BlueBreeze | DarkBlueGrey | Aurora |
|------|-----------|--------------|--------|
| bevelLight | `0xFFFFFFFF` | `0xFF3E4D5E` | `0x00000000` 无 |
| bevelDark | `0xFF8FA2AF` | `0xFF0B111B` | `0x00000000` 无 |
| glowColor | `0xFF60A5FA` AccentBlue | `0xFF60A5FA` | `0xFF3B82F6` Cyan |

### 1.6 Text（4）

| 字段 | BlueBreeze | DarkBlueGrey | Aurora |
|------|-----------|--------------|--------|
| textPrimary | `0xFF2C3E50` TextDark | `0xFFE6EDF5` | `0xFFFFFFFF` |
| textSecondary | `0xFF708090` TextDim | `0xFFA8B4C3` | `0x99FFFFFF` 60% |
| textDisabled | `0xFFA1AFBA` | `0xFF6F7C8C` | `0x66FFFFFF` 40% |
| textHighlight | `0xFF60A5FA` AccentBlue | `0xFF60A5FA` | `0xFF3B82F6` Cyan |

### 1.7 Piano Roll（4）

| 字段 | BlueBreeze | DarkBlueGrey | Aurora |
|------|-----------|--------------|--------|
| rollBackground | `0xFFAABCC7` GraphBgLight | `0xFF141A22` | `0xFF151B22` BgDeep |
| laneC | `0x0FFFFFFF` | `0xFF1D2631` | `0x08FFFFFF` |
| laneOther | `0x08FFFFFF` | `0xFF171F29` | `0x03FFFFFF` |
| gridLine | `0x20FFFFFF` | `0xFF2B3643` | `0x33FFFFFF` BorderLight |

### 1.8 Pitch Curve（3）

| 字段 | BlueBreeze | DarkBlueGrey | Aurora |
|------|-----------|--------------|--------|
| originalF0 | `0xFFE07A7A` NodeRed | `0xFFE53935` Material Red | `0xFFFF0000` 正红 |
| correctedF0 | `0xFF60A5FA` AccentBlue | `0xFF60A5FA` | `0xFF00FFFF` 青 |
| shadowTrack | `0x3060A5FA` | `0x4060A5FA` | `0x40000000` |

### 1.9 Note Block（4）

| 字段 | BlueBreeze | DarkBlueGrey | Aurora |
|------|-----------|--------------|--------|
| noteBlock | `0xFFF5D76E` NodeYellow | `0xFF60A5FA` | `0xFF3B82F6` Cyan |
| noteBlockBorder | `0xFFE5C75E` | `0xFF3B82F6` | `0xFFFFFFFF` |
| noteBlockSelected | `0xFFFFE8A0` | `0xFF93C5FD` | `0xFF0070FF` ElectricBlue |
| noteBlockHover | `0xFFFBE090` | `0xFF93C5FD` | `0xFFEC4899` Magenta |

### 1.10 Playhead / Timeline（3）

| 字段 | BlueBreeze | DarkBlueGrey | Aurora |
|------|-----------|--------------|--------|
| playhead | `0xFFFFFFFF` | `0xFFE6EDF5` | `0xFF3B82F6` Cyan |
| timelineMarker | `0xFF60A5FA` | `0xFF60A5FA` | `0x99FFFFFF` |
| beatMarker | `0x303B4D5A` | `0xFF3C4A5A` | `0x66FFFFFF` |

### 1.11 Tool / Button（3）

| 字段 | BlueBreeze | DarkBlueGrey | Aurora |
|------|-----------|--------------|--------|
| toolActive | `0xFFFFFFFF` | `0xFF60A5FA` | `0xFF3B82F6` Cyan |
| toolInactive | `0xFF708090` TextDim | `0xFF3C4A5A` | `0x66FFFFFF` TextDim |
| buttonInactive | `0x18000000` | `0xFF2B3643` | `0xFF1A2332` BgSurface |

### 1.12 Status（3）

| 字段 | BlueBreeze | DarkBlueGrey | Aurora |
|------|-----------|--------------|--------|
| statusProcessing | `0xFFF39C12` | `0xFFEAB308` | `0xFFEAB308` Warning |
| statusReady | `0xFF2ECC71` | `0xFF34D399` | `0xFF22C55E` Success |
| statusError | `0xFFE07A7A` | `0xFFF87171` | `0xFFEF4444` Error |

### 1.13 Waveform / Scale / Knob（5）

| 字段 | BlueBreeze | DarkBlueGrey | Aurora |
|------|-----------|--------------|--------|
| waveformFill | `0x25FFFFFF` | `0x403B82F6` | `0x4000F0FF` |
| waveformOutline | `0x60FFFFFF` | `0xFF60A5FA` | `0x8000F0FF` |
| scaleHighlight | `0x60FBBF24` | `0x60FBBF24` | `0x20FFFFFF` |
| knobBody | `0xFF1A1A1A` 钢琴漆黑 | `0xFF0F141B` | `0xFF1A2332` BgSurface |
| knobIndicator | `0xFFE0E0E0` 银白 | `0xFFE6EDF5` | `0xFF3B82F6` Cyan |

### 1.14 其它

| 字段 | 类型 | 默认/值 | 备注 |
|------|------|---------|------|
| cornerRadius | float | 8.0f（默认）/ `xxxStyle.PanelRadius`（各主题实际 =16） | 与 `ThemeStyle.panelRadius` 语义重叠 |

## 2. ThemeStyle 字段表

**声明文件**：`Source/Standalone/UI/ThemeTokens.h`
**规模**：16 数值 + 6 Colour + 1 bool = 23 字段

| 字段 | 类型 | BlueBreeze | DarkBlueGrey | Aurora | 用途 |
|------|------|-----------|--------------|--------|------|
| panelRadius | float | 16 | 16 | 16 | 面板圆角（三主题已统一） |
| controlRadius | float | 10 | 10 | 10 | 按钮/ComboBox/滑块轨圆角 |
| fieldRadius | float | 10（=控件圆角） | 10 | 10 | 文本框 / 字段圆角 |
| knobRadius | float | 999 | 999 | 10（注：代码注释说复用 controlRadius） | 旋钮圆角 |
| strokeThin | float | 1.0 | 1.2 | 1.0 | 细描边 |
| strokeThick | float | 2.0 | 2.0 | 2.0 | 粗描边 |
| focusRingThickness | float | 2.0（硬编码） | 2.0 | 2.0（硬编码） | 聚焦环 |
| shadowAlpha | float | 0.25 | 0.25 | 0.25 | 阴影透明度 |
| shadowRadius | int | 10 | 16 | 10 | 阴影半径 |
| shadowOffset | Point\<int\> | {0,4} | {0,5} | {0,4} | 阴影偏移 |
| glowAlpha | float | 0.0 | 0.0 | 0.6（GlowAmount） | 发光透明度 |
| glowRadius | float | 0.0 | 0.0 | 12.0（硬编码） | 发光半径 |
| bevelWidth | float | 0.0（硬编码） | 1.0 | 0.0 | 浮雕宽度 |
| bevelIntensity | float | 0.0（硬编码） | 0.12 | 0.0 | 浮雕强度 |
| animationDurationMs | float | 150 | 120 | 200 | 默认动画时长 |
| hoverGlowIntensity | float | 0.4 HoverGlowAmount | 0.3 | 1.0 | 悬停发光强度 |
| vuLow | Colour | AccentBlue | `0xFF2DD4BF` | `0xFF3B82F6` Cyan | VU 低电平 |
| vuMid | Colour | NodeYellow | `0xFF60A5FA` | `0xFFEAB308` Warning | VU 中 |
| vuHigh | Colour | NodeRed | `0xFFFBBF24` | `0xFFEF4444` Error | VU 高 |
| vuClip | Colour | `0xFFFF4A4A` | `0xFFF87171` | `0xFFEC4899` Magenta | VU 过载 |
| timeActive | Colour | AccentBlue | `0xFFE6EDF5` | `0xFF3B82F6` Cyan | 时间显示激活色 |
| timeInactive | Colour | TextDim α20% | `0xFF7E8A99` | `0x66FFFFFF` TextDim | 时间显示非激活色 |
| timeSegmentStyle | bool | true | false | false | 是否使用 7 段数码管风格 |

## 3. 主题原始色常量清单

### 3.1 BlueBreeze::Colors（25 键）

| 键 | Hex | 用途 |
|----|-----|------|
| GraphBgDeep | 0xFF7A8F9E | 工作区渐变底 |
| GraphBgMid | 0xFF8CA2B0 | 工作区渐变中 |
| GraphBgLight | 0xFFAABCC7 | 工作区渐变顶 |
| SidebarBg | 0xFFC6D4DD | 侧栏/面板背景 |
| PanelBorder | 0xFFB0C0CC | 面板边框 |
| KnobBody | 0xFF1A1A1A | 钢琴漆旋钮底色 |
| KnobBodyLight | 0xFF2A2A2A | 旋钮亮部 |
| KnobHighlight | 0x80FFFFFF | 旋钮顶部高光 |
| KnobEdge | 0xFF404040 | 旋钮金属边缘 |
| KnobIndicator | 0xFFE0E0E0 | 旋钮银白指针 |
| KnobTrack | 0x40FFFFFF | 旋钮轨道 |
| KnobShadow | 0x40000000 | 旋钮投影 |
| KnobGlow | 0x30FFFFFF | 旋钮悬停发光 |
| ActiveWhite | 0xFFFFFFFF | 选中/激活纯白 |
| HoverOverlay | 0x1AFFFFFF | 悬停白色覆盖 |
| ShadowColor | 0xFF5A6A75 | 有色阴影 |
| NodeRed | 0xFFE07A7A | 糖果红（original F0） |
| NodeYellow | 0xFFF5D76E | 糖果黄（note block） |
| NodePurple | 0xFF9B59B6 | 糖果紫 |
| AccentBlue | 0xFF60A5FA | 糖果蓝（primary/accent） |
| TextDark | 0xFF2C3E50 | 浅底上的深文字 |
| TextLight | 0xFFF0F4F8 | 深底上的浅文字 |
| TextDim | 0xFF708090 | 次级文字 |
| ClipGradientTop | 0xFFC8D4E0 | Clip 渐变顶 |
| ClipGradientBottom | 0xFFA8B8C8 | Clip 渐变底 |
| ClipBorder | 0xFF8A9CAD | Clip 边框 |
| ClipSelectedTop | 0xFFD0D8E0 | 选中 Clip 渐变顶 |
| ClipSelectedBottom | 0xFFB8C8D8 | 选中 Clip 渐变底 |

### 3.2 DarkBlueGrey::Colors（48 键）

按分组列出：

- **主强调**（4）：PrimaryBlue=0xFF60A5FA / Accent=0xFF3B82F6 / LightBlue=0xFF93C5FD / DarkBlue=0xFF2563EB
- **背景层**（3）：BackgroundDark=0xFF1A212B / Medium=0xFF222B36 / Light=0xFF2B3643
- **渐变**（2）：GradientTop=0xFF273241 / GradientBottom=0xFF171D27
- **UI 元素**（4）：PanelBorder=0xFF3C4A5A / ButtonNormal=0xFF2B3643 / ButtonHover=0xFF334253 / ButtonPressed=0xFF1B2430
- **3D 效果**（3）：BevelLight=0xFF3E4D5E / BevelDark=0xFF0B111B / GlowColor=0xFF60A5FA
- **文字**（4）：TextPrimary=0xFFE6EDF5 / Secondary=0xFFA8B4C3 / Disabled=0xFF6F7C8C / Highlight=0xFF60A5FA
- **钢琴卷帘**（4）：RollBackground=0xFF141A22 / LaneC=0xFF1D2631 / LaneOther=0xFF171F29 / GridLine=0xFF2B3643
- **音高曲线**（3）：OriginalF0=0xFFE53935 / CorrectedF0=0xFF60A5FA / ShadowTrack=0x4060A5FA
- **音符块**（4）：NoteBlock=0xFF60A5FA / Border=0xFF3B82F6 / Selected=0xFF93C5FD / Hover=0xFF93C5FD
- **播放头 / 时间线**（3）：Playhead=0xFFE6EDF5 / TimelineMarker=0xFF60A5FA / BeatMarker=0xFF3C4A5A
- **工具**（3）：ToolActive=0xFF60A5FA / Inactive=0xFF3C4A5A / ButtonInactive=0xFF2B3643
- **状态**（3）：Processing=0xFFEAB308 / Ready=0xFF34D399 / Error=0xFFF87171
- **波形**（2）：WaveformFill=0x403B82F6 / Outline=0xFF60A5FA
- **音阶**（1）：ScaleHighlight=0x60FBBF24
- **旋钮**（2）：KnobBody=0xFF0F141B / Indicator=0xFFE6EDF5
- **VU 表**（4）：Low=0xFF2DD4BF / Mid=0xFF60A5FA / High=0xFFFBBF24 / Clip=0xFFF87171
- **时间显示**（2）：TimeActive=0xFFE6EDF5 / Inactive=0xFF7E8A99

### 3.3 Aurora::Colors（21 键）

- **背景**（3）：BgDeep=0xFF151B22 / BgSurface=0xFF1A2332 / BgOverlay=0x1FFFFFFF
- **边框**（2）：BorderLight=0x33FFFFFF / BorderGlow=0x66FFFFFF
- **霓虹强调**（8）：Cyan=0xFF3B82F6（实际是蓝）/ Violet=0xFF8B5CF6 / ElectricBlue=0xFF0070FF / Magenta=0xFFEC4899 / NeonGreen=0xFF22C55E / NeonOrange=0xFFF97316 / NeonRed=0xFFEF4444 / NeonYellow=0xFFEAB308
- **文字**（3）：TextPrimary=0xFFFFFFFF / Secondary=0x99FFFFFF / Dim=0x66FFFFFF
- **状态**（3）：Success=0xFF22C55E / Warning=0xFFEAB308 / Error=0xFFEF4444
- **旋钮**（2）：KnobBody=0xFF1A2332 / Indicator=0xFF3B82F6

## 4. UIColors 字段

**声明文件**：`Source/Standalone/UI/UIColors.h`
**规模**：44 个 `static inline juce::Colour` + 1 `float cornerRadius` + 2 状态字段 = 44+3

### 4.1 完整字段列表（按源代码顺序）

| 分组 | 字段 | 默认（初始化值） |
|------|------|------------------|
| Primary | primaryPurple | 0xFF6A9AB8 |
|  | accent | 0xFF6A9AB8 |
|  | lightPurple | 0xFF8AB8D0 |
|  | darkPurple | 0xFF4A7A98 |
| Background | backgroundDark | 0xFF222830 |
|  | backgroundMedium | 0xFF2F3640 |
|  | backgroundLight | 0xFF3E4652 |
| Gradient | gradientTop | 0xFF353C48 |
|  | gradientBottom | 0xFF222830 |
| UI Element | panelBorder | 0xFF4E5865 |
|  | buttonNormal | 0xFF3E4652 |
|  | buttonHover | 0xFF5D6D7E |
|  | buttonPressed | 0xFF2C3E50 |
| 3D | bevelLight | 0xFF4A4A60 |
|  | bevelDark | 0xFF080810 |
|  | glowColor | 0xFF00F5FF |
| Text | textPrimary | 0xFFECF0F1 |
|  | textSecondary | 0xFFBDC3C7 |
|  | textDisabled | 0xFF7F8C8D |
|  | textHighlight | 0xFF7FB3D5 |
| Piano Roll | rollBackground | 0xFF1E2329 |
|  | laneC | 0xFF2F3640 |
|  | laneOther | 0xFF252B33 |
|  | gridLine | 0xFF3E4652 |
| Pitch Curve | originalF0 | 0xFFFF6666 |
|  | correctedF0 | 0xFF6A9AB8 |
|  | shadowTrack | 0x406A9AB8 |
| Note Block | noteBlock | 0xFF7FB3D5 |
|  | noteBlockBorder | 0xFFA9CCE3 |
|  | noteBlockSelected | 0xFFFF7675 |
|  | noteBlockHover | 0xFFAED6F1 |
| Playhead/Timeline | playhead | 0xFFFFFFFF |
|  | timelineMarker | 0xFF7FB3D5 |
|  | beatMarker | 0xFF4E5865 |
| Tool | toolActive | 0xFF7FB3D5 |
|  | toolInactive | 0xFF4E5865 |
|  | buttonInactive | 0xFF4E5865 |
| Status | statusProcessing | 0xFFF39C12 |
|  | statusReady | 0xFF2ECC71 |
|  | statusError | 0xFFE74C3C |
| Waveform | waveformFill | 0x603E4652 |
|  | waveformOutline | 0xFF3E4652 |
| Scale | scaleHighlight | 0x60F39C12 |
| Knob | knobBody | 0xFF1B2026 |
|  | knobIndicator | 0xFF7FB3D5 |
| Geometry | cornerRadius (float) | 8.0f |
| 状态 | currentThemeId_ (ThemeId) | Aurora（默认） |
|  | currentThemeStyle_ (ThemeStyle) | Aurora 的 style 实例 |
| 常量 | navControlHeight (int) | 50 |
|  | navFontHeight (float) | 20.0 |
|  | navMonoFontHeight (float) | 20.0 |

> ⚠️ **初始值 vs. 默认主题不匹配**：字段初始值是介于 DarkBlueGrey/BlueBreeze 之间的一组手写柔和蓝灰值（见 primaryPurple=#6A9AB8），但 `currentThemeId_` 默认是 `Aurora`。应用启动时 `PluginEditor` 会立即调用 `applyTheme(appPreferences_.theme)` 覆盖，所以初始值只在极短时间窗内可见。

### 4.2 `ShadowLevel` 枚举

| 成员 | 值 | 语义 | DarkBlueGrey 下的阴影参数 |
|------|----|------|---------------------------|
| Ambient | 0 | 面板贴底 | alpha 0.28 / radius 14 / offset (0,3) |
| Float | 1 | 悬浮控件（TopBar/工具条） | alpha 0.34 / radius 20 / offset (0,6) |
| Pop | 2 | 弹窗 / 菜单 | alpha 0.45 / radius 26 / offset (0,10) |

## 5. 主题切换数据流

```
AppPreferences.shared.theme (ThemeId)
      │
      ▼ ctor / menu change
PluginEditor::applyThemeToEditor(ThemeId)
      │
      ├──► UIColors::applyTheme(ThemeId)
      │        ├──► currentThemeId_ = id
      │        ├──► currentThemeStyle_ = Theme::getStyle(id)
      │        └──► applyTheme(Theme::getTokens(id))
      │                 └──► 拷贝 43 Colour + cornerRadius 到 static inline 字段
      │
      ├──► setLookAndFeel(id==Aurora ? &auroraLAF : &openTuneLAF)
      │
      └──► 各子组件 applyTheme() 读取 UIColors::currentThemeId() 做内部分支
              └──► getLookAndFeel().setColour(ResizableWindow::backgroundColourId, backgroundDark)
              └──► sendLookAndFeelChange() 触发级联 repaint
```

## 6. 持久化

`ThemeId` 由 `ui-main` 的 `AppPreferences` 模块持久化（`shared.theme` 字段），本模块不直接触碰磁盘。

## 待确认

1. **cornerRadius 与 panelRadius 职责重叠**：`ThemeTokens.cornerRadius` 和 `ThemeStyle.panelRadius` 在所有主题中都被设为相同值（16），真实消费者分别是什么？是否应合并？
2. **Aurora 命名歧义**：`Aurora::Colors::Cyan` 的实际值是 `0xFF3B82F6`（蓝色），而真正的青 hex 在 `correctedF0` 中硬编码为 `0xFF00FFFF`。命名是否需要校正？
3. **BlueBreeze 的 VU 色映射**：`vuClip` 硬编码为 `0xFFFF4A4A`（不是来自 `BlueBreeze::Colors` 的任何键）；应确认是否有意为之。
4. **DarkBlueGrey 使用 strokeThin=1.2f** 与 BlueBreeze/Aurora 的 1.0f 不一致；注释说"满足可见性下限"，应确认是否该统一。
5. **Aurora 的 glowRadius 硬编码 12.0f**（未来自 `Aurora::Style`）：应澄清是遗漏还是有意保留为 magic number。
