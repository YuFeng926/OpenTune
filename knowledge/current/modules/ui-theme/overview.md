---
module: ui-theme
type: overview
generated: true
source_scan: true
---

# ui-theme Module Overview

## 模块定位

ui-theme 是 OpenTune 的**主题系统基础设施模块**，为整个应用提供 token-based 的色彩、样式和控件外观管理。支持三套完整主题：BlueBreeze（浅色 Soothe2 风）、DarkBlueGrey（深蓝灰清爽风）、Aurora（暗色霓虹风）。

## 架构概览

```
┌────────────────────────────────────────────────────┐
│                   Theme (static)                    │
│  setActiveTheme() / getTokens() / getStyle()        │
│  ┌──────────┐ ┌──────────────┐ ┌──────────────┐    │
│  │BlueBreeze│ │DarkBlueGrey  │ │   Aurora      │    │
│  │ Tokens   │ │   Tokens     │ │   Tokens      │    │
│  │ Style    │ │   Style      │ │   Style       │    │
│  └──────────┘ └──────────────┘ └──────────────┘    │
└──────────────────┬─────────────────────────────────┘
                   │ applyTheme()
                   ▼
┌────────────────────────────────────────────────────┐
│              UIColors (static inline)               │
│  39 语义色 + 圆角 + 绘制辅助函数 + 字体工厂         │
└──────────────────┬─────────────────────────────────┘
                   │ 直接引用
                   ▼
┌────────────────────────────────────────────────────┐
│               LookAndFeel 实现层                     │
│  ┌──────────────────────┐                           │
│  │ OpenTuneLookAndFeel  │← 一体式，内部 switch 派发  │
│  └──────────────────────┘                           │
│  ┌──────────────┐┌──────────────┐┌────────────────┐ │
│  │BlueBreeze LAF││DarkBlueGrey  ││  Aurora LAF    │ │
│  │              ││    LAF       ││ +drawNeonGlow  │ │
│  └──────────────┘└──────────────┘└────────────────┘ │
│  ↑ 三个独立 LookAndFeel，分离式                      │
└────────────────────────────────────────────────────┘
```

## 文件清单

| 文件 | 行数 | 职责 |
|------|------|------|
| `ThemeTokens.h` | 489 | Theme 管理器 + ThemeTokens/ThemeStyle 定义 + 三套 Token 实例化 |
| `UIColors.h` | 465 | 全局色值缓存 + 阴影/面板/字体辅助 + SmallFontTextButton |
| `OpenTuneLookAndFeel.h` | 1339 | 通用基类 LookAndFeel（含所有主题的绘制分支） |
| `BlueBreezeTheme.h` | 69 | BlueBreeze 原始色值 + 样式常量 |
| `BlueBreezeLookAndFeel.h` | 69 | BlueBreeze 独立 LookAndFeel 声明 |
| `BlueBreezeLookAndFeel.cpp` | 433 | BlueBreeze 独立 LookAndFeel 实现 |
| `DarkBlueGreyTheme.h` | 137 | DarkBlueGrey 原始色值 + 样式常量 |
| `DarkBlueGreyLookAndFeel.h` | 71 | DarkBlueGrey 独立 LookAndFeel 声明 |
| `DarkBlueGreyLookAndFeel.cpp` | 354 | DarkBlueGrey 独立 LookAndFeel 实现 |
| `AuroraTheme.h` | 63 | Aurora 原始色值 + 样式常量 |
| `AuroraLookAndFeel.h` | 68 | Aurora 独立 LookAndFeel 声明 |
| `AuroraLookAndFeel.cpp` | 440 | Aurora 独立 LookAndFeel 实现 |
| **合计** | **~3997** | |

## 关键设计决策

1. **三层 Token 体系**: 基础色（命名空间常量）→ 语义 Token（ThemeTokens struct）→ 全局缓存（UIColors static inline）。新增主题只需添加 Layer 1+2。

2. **双轨 LookAndFeel**: 一个一体式 `OpenTuneLookAndFeel` 和三个分离式独立 `XxxLookAndFeel` 并存。代码存在重复。

3. **静态全局状态**: 主题切换通过修改 `UIColors` 的静态字段生效，零运行时开销，但需确保仅在 UI 线程操作。

4. **统一圆角/间距**: 三套主题的 panelRadius(16)、controlRadius(10)、fieldRadius(10)、shadowAlpha(0.25) 已统一，保证布局一致性。

5. **主题特化绘制**: 每套主题有独特的控件绘制风格（BlueBreeze 钢琴漆旋钮、DarkBlueGrey 拟物金属旋钮、Aurora 霓虹动态弧线旋钮），不是简单换色。

## 关联文档

- [api.md](./api.md) — 完整 API 契约（类/方法/参数）
- [data-model.md](./data-model.md) — Token 全量列表 + 三套主题色值对比
- [business.md](./business.md) — 设计规约（Token 体系层级/主题切换时序/覆写策略）
