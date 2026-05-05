---
module: ui-theme
type: overview
generated: true
source_scan: true
updated: 2026-05-05
---

# ui-theme Module Overview

## 模块职责

`ui-theme` 是 OpenTune 的**主题系统基础设施模块**，以 token-based 设计令牌方式统一管理整个应用的色彩、尺寸、间距、阴影、圆角、动画时长等视觉属性，并通过 JUCE `LookAndFeel` 机制把这些令牌落地到具体控件的绘制上。模块提供三套完整主题：

- **BlueBreeze** — Soothe2 风的浅色柔和蓝灰，钢琴漆旋钮
- **DarkBlueGrey** — 清爽现代的深蓝灰，拟物金属旋钮
- **Aurora** — 暗色霓虹毛玻璃，动态发光弧线

## 架构边界

### 上游（谁依赖本模块）
- `ui-main`（`PluginEditor` / `TopBarComponent` / `TrackPanelComponent` / `TransportBarComponent` / `MenuBarComponent` 等）— 几乎所有 UI 组件都直接引用 `UIColors::xxx` 读取当前主题色值、并调用 `applyTheme()` 响应主题切换
- `ui-piano-roll`（`PianoRollComponent` / `ArrangementViewComponent` 等）— 读取 `UIColors::playhead` / `rollBackground` / `laneC` / `noteBlock` 等 piano roll 专用色值

### 下游（本模块依赖谁）
- JUCE `juce::LookAndFeel_V4`（`OpenTuneLookAndFeel`、`AuroraLookAndFeel` 均继承它）
- JUCE `juce::Colour` / `juce::ColourGradient` / `juce::DropShadow` / `juce::Font` 绘图原语
- `BinaryData::HONORSansCNMedium_ttf`（嵌入字体资源，由 OpenTuneLookAndFeel 构造时注册为默认无衬线字体）

### 外部契约面
- `OpenTune::ThemeId` 枚举（BlueBreeze=0 / DarkBlueGrey=1 / Aurora=2）
- `OpenTune::UIColors::applyTheme(ThemeId)` — 切换全局主题
- `OpenTune::UIColors::currentThemeId()` / `currentTokens()` / `currentThemeStyle()` — 运行时查询
- 组件侧约定：收到主题切换信号后自己调用 `applyTheme()` 并 `repaint()`（没有统一的 ThemeListener 机制）

## 模块内部架构

```
┌────────────────────────────────────────────────────┐
│               Layer 1: 主题原始色值                  │
│  BlueBreezeTheme.h / DarkBlueGreyTheme.h /          │
│  AuroraTheme.h（各自独立的 Colors + Style 常量结构）   │
└──────────────────┬─────────────────────────────────┘
                   │ static const 组合
                   ▼
┌────────────────────────────────────────────────────┐
│            Layer 2: 语义 Token 层                    │
│  ThemeTokens  / ThemeStyle（40+ 色令牌、23 样式参数）  │
│  OpenTune::Theme::getTokens(ThemeId)                │
│  OpenTune::Theme::getStyle(ThemeId)                 │
└──────────────────┬─────────────────────────────────┘
                   │ applyTheme() 拷贝到全局
                   ▼
┌────────────────────────────────────────────────────┐
│          Layer 3: 全局缓存 / 绘制辅助                │
│  UIColors（static inline 44 个色字段 +               │
│            currentThemeId_/currentThemeStyle_ +      │
│            drawShadow / fillPanelBackground /        │
│            drawPanelFrame / 4 个字体工厂）           │
└──────────────────┬─────────────────────────────────┘
                   │ 组件直接读取 / LookAndFeel 查询
                   ▼
┌────────────────────────────────────────────────────┐
│          Layer 4: LookAndFeel 绘制实现               │
│  ┌──────────────────────────────┐                   │
│  │ OpenTuneLookAndFeel          │ 覆盖 ~25 个 draw* │
│  │ （BlueBreeze / DarkBlueGrey   │ 内部按 themeId   │
│  │   共用，通过 switch 分派）     │ 分派到           │
│  │                              │ drawBlueBreezeXxx │
│  │                              │ / drawDarkBlueXxx │
│  └──────────────────────────────┘                   │
│  ┌──────────────────────────────┐                   │
│  │ AuroraLookAndFeel            │ 独立继承          │
│  │  : public LookAndFeel_V4     │ 覆盖 ~12 个 draw* │
│  │  + drawNeonGlow 私有辅助     │ 不复用上面实现    │
│  └──────────────────────────────┘                   │
└────────────────────────────────────────────────────┘
```

## 文件清单（8 文件）

| 文件 | 大小 | 职责 |
|------|------|------|
| `Source/Standalone/UI/ThemeTokens.h` | ~15 KB | `ThemeId` 枚举、`ThemeTokens`/`ThemeStyle` 结构体、`Theme` 静态入口（getTokens/getStyle）、三套 token 与 style 的静态实例化 |
| `Source/Standalone/UI/UIColors.h` | ~18 KB | 全局静态色字段缓存 + `applyTheme()` 切换 + `drawShadow`/`fillPanelBackground`/`drawPanelFrame` 绘制辅助 + 4 个字体工厂 + `SmallFontTextButton` |
| `Source/Standalone/UI/OpenTuneLookAndFeel.h` | ~55 KB | `OpenTuneLookAndFeel : LookAndFeel_V4`（header-only）+ `GlowAnimation` 动画辅助；覆盖按钮/旋钮/线性滑块/文本编辑器/ComboBox/菜单/滚动条等；按 `UIColors::currentThemeId()` 分派到 `drawBlueBreezeXxx` / `drawDarkBlueGreyXxx` |
| `Source/Standalone/UI/BlueBreezeTheme.h` | 3 KB | `BlueBreeze::Colors` 原始色常量（25 个） + `BlueBreeze::Style` 尺寸常量（纯 header-only，无 cpp） |
| `Source/Standalone/UI/DarkBlueGreyTheme.h` | 6 KB | `DarkBlueGrey::Colors` 原始色常量（48 个） + `DarkBlueGrey::Style` 常量（纯 header-only，无 cpp） |
| `Source/Standalone/UI/AuroraTheme.h` | 3 KB | `Aurora::Colors` 霓虹原始色（21 个） + `Aurora::Style` 常量（纯 header-only，无 cpp） |
| `Source/Standalone/UI/AuroraLookAndFeel.h` | 3 KB | `AuroraLookAndFeel : LookAndFeel_V4` 声明（~12 覆写方法 + `drawNeonGlow` 私有） |
| `Source/Standalone/UI/AuroraLookAndFeel.cpp` | 16 KB | Aurora LookAndFeel 完整实现（玻璃渐变、霓虹动态色弧、彩虹文字） |

## 关键设计约束

1. **三层 Token 体系** — 基础色（命名空间常量）→ 语义 Token（`ThemeTokens` struct）→ 全局缓存（`UIColors` static inline）。新增主题只需添加 Layer 1 + 在 Layer 2 的 `Theme::xxxTokens()` / `xxxStyle()` 增加一条分支。

2. **双轨 LookAndFeel** — BlueBreeze 和 DarkBlueGrey 共用同一个一体式 `OpenTuneLookAndFeel`（内部 switch 分派）；Aurora 独立用 `AuroraLookAndFeel`。`PluginEditor::applyThemeToEditor` 按主题 id 挑选实例调用 `setLookAndFeel`。

3. **静态全局状态** — 主题切换通过修改 `UIColors` 的 static inline 字段生效，零运行时查表开销；但需保证**只在 UI 线程**调用 `applyTheme()`。

4. **无 ThemeListener 接口** — `MenuBarComponent::Listener::themeChanged(ThemeId)` 仅由 `PluginEditor` 实现并转发给各子组件自己的 `applyTheme()`。本模块不定义统一的主题订阅者接口。

5. **Token 统一** — 三套主题的 `panelRadius=16 / controlRadius=10 / fieldRadius=10 / shadowAlpha=0.25 / shadowOffset={0,4}` 已手动对齐，保证跨主题切换时布局不跳动。

6. **header-only 主题常量** — 三个 `XxxTheme.h` 只有 `static const juce::uint32` / `static constexpr float` 常量，不包含任何函数体，所以不需要 `.cpp` 配对文件。

## 关联文档

- [api.md](./api.md) — 类与方法级 API 契约
- [data-model.md](./data-model.md) — `ThemeTokens` 全量字段 + 三套主题色值对照 + `UIColors` 字段清单
- [business.md](./business.md) — 主题切换时序、LookAndFeel 应用链、颜色查询路径

## 待确认

- 已在 api/data-model/business 各文末列出，汇总见 [business.md](./business.md)。
