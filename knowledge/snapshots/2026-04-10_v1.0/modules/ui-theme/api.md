---
module: ui-theme
type: api
generated: true
source_scan: true
note: "本模块无 HTTP Controller，此文档记录主题系统对外暴露的编程接口契约"
---

# ui-theme API Reference

> 本模块无 HTTP Controller，此文档记录主题系统对外暴露的编程接口契约。
> 基于源码扫描生成。

## 1. 核心类总览

| 类/结构体 | 头文件 | 职责 |
|-----------|--------|------|
| `Theme` | `ThemeTokens.h` | 静态主题管理器，负责切换活跃主题、获取 Tokens / Style |
| `ThemeTokens` | `ThemeTokens.h` | Token 值对象：所有语义颜色 + 全局圆角 |
| `ThemeStyle` | `ThemeTokens.h` | 样式值对象：圆角、线宽、阴影、发光、动画参数 |
| `ThemeId` | `ThemeTokens.h` | `enum class`，标识三套主题 |
| `UIColors` | `UIColors.h` | 全局 `static inline` 色值运行时缓存 + 辅助绘制工具函数 |
| `GlowAnimation` | `OpenTuneLookAndFeel.h` | Hover 发光动画辅助类 |
| `OpenTuneLookAndFeel` | `OpenTuneLookAndFeel.h` | 通用 LookAndFeel 基类（派发到各主题绘制路径） |
| `BlueBreezeLookAndFeel` | `BlueBreezeLookAndFeel.h` | BlueBreeze 专用独立 LookAndFeel |
| `DarkBlueGreyLookAndFeel` | `DarkBlueGreyLookAndFeel.h` | DarkBlueGrey 专用独立 LookAndFeel |
| `AuroraLookAndFeel` | `AuroraLookAndFeel.h` | Aurora 专用独立 LookAndFeel |
| `SmallFontTextButton` | `UIColors.h` | 12px 字号小按钮组件 |

---

## 2. Theme — 静态主题管理器

**文件**: `Source/Standalone/UI/ThemeTokens.h:117-487`

### 公开静态方法

```cpp
static void setActiveTheme(ThemeId themeId);
```
- 设置当前活跃主题。修改 `activeThemeId_`（`static inline`）。
- **线程安全**: 无同步保护，应在 UI 线程调用。

```cpp
static ThemeId getActiveTheme();
```
- 返回当前活跃主题 ID。

```cpp
static const ThemeTokens& getTokens(ThemeId themeId);
```
- 按 ID 获取主题 Token 集合（静态局部变量，懒初始化，生命周期=进程）。

```cpp
static const ThemeTokens& getActiveTokens();
```
- 便捷方法：`getTokens(getActiveTheme())`。

```cpp
static const ThemeStyle& getStyle(ThemeId themeId);
```
- 按 ID 获取主题样式参数。

```cpp
static const ThemeStyle& getActiveStyle();
```
- 便捷方法：`getStyle(getActiveTheme())`。

### ThemeId 枚举

```cpp
enum class ThemeId : int {
    BlueBreeze   = 0,
    DarkBlueGrey = 1,
    Aurora       = 2
};
```

### 默认值

```cpp
static inline ThemeId activeThemeId_ = ThemeId::Aurora;  // 默认主题为 Aurora
```

---

## 3. UIColors — 全局色值缓存 + 绘制工具

**文件**: `Source/Standalone/UI/UIColors.h:9-436`

### 核心方法

```cpp
static void applyTheme(const ThemeTokens& tokens);
```
- **主题切换入口**: 将 `ThemeTokens` 中的所有值拷贝到 `UIColors` 的 `static inline` 字段。
- 调用后，所有通过 `UIColors::xxx` 访问颜色的组件立即获取新值。

### 绘制辅助函数

| 方法 | 签名 | 说明 |
|------|------|------|
| `drawShadow` | `(Graphics&, Rectangle<float>&)` | 默认 Ambient 级别阴影 |
| `drawShadow` | `(Graphics&, Rectangle<float>&, ShadowLevel)` | 分层阴影（Ambient/Float/Pop） |
| `fillSoothe2CanvasBackground` | `(Graphics&, Rectangle<float>&, float radius)` | 画布背景（Soothe2 风格渐变） |
| `fillSoothe2SpectrumBackground` | `(Graphics&, Rectangle<float>&, float radius)` | 频谱区背景 |
| `fillPanelBackground` | `(Graphics&, Rectangle<float>&, float radius)` | 通用面板背景（带渐变+高光+暗角） |
| `drawPanelFrame` | `(Graphics&, Rectangle<float>&, float radius)` | 面板边框（含内高光线+可选发光） |

### 字体工厂

| 方法 | 默认高度 | 字重 |
|------|---------|------|
| `getUIFont(float)` | 16.0f | Regular |
| `getHeaderFont(float)` | 18.0f | Semibold |
| `getLabelFont(float)` | 14.0f | Regular |
| `getMonoFont(float)` | 20.0f | Bold (Monospaced) |

### 颜色操作工具

```cpp
static Colour withAlpha(const Colour& base, float alpha);
static Colour interpolate(const Colour& a, const Colour& b, float ratio);
static Colour brighten(const Colour& color, float amount);
static Colour darken(const Colour& color, float amount);
```

### 阴影层级枚举

```cpp
enum class ShadowLevel : int {
    Ambient = 0,  // 面板贴底
    Float   = 1,  // 悬浮控件（顶部条/工具条）
    Pop     = 2   // 弹窗/菜单
};
```

### 导航栏度量常量

```cpp
static constexpr int   navControlHeight  = 50;     // 导航控件统一高度
static constexpr float navFontHeight     = 20.0f;  // 导航字体统一大小
static constexpr float navMonoFontHeight = 20.0f;  // 等宽字体大小
```

---

## 4. OpenTuneLookAndFeel — 通用基类

**文件**: `Source/Standalone/UI/OpenTuneLookAndFeel.h:55-1337`

继承 `juce::LookAndFeel_V4`。构造时加载 HONOR Sans CN Medium 字体为默认无衬线字体。

### 覆写方法清单

| JUCE 虚方法 | 主题派发逻辑 |
|-------------|-------------|
| `drawButtonBackground` | DarkBlueGrey → `drawDarkBlueGreyButton`；其他 → `drawBlueBreezeButton` |
| `drawToggleButton` | BlueBreeze → 自定义极简；其他 → `LookAndFeel_V4::drawToggleButton` |
| `drawTickBox` | BlueBreeze → 自定义勾选框；其他 → `LookAndFeel_V4::drawTickBox` |
| `drawLabel` | 仅对可编辑数值框/滑块文本框定制（DarkBlueGrey + BlueBreeze） |
| `drawLinearSlider` | 水平/垂直分别处理；BlueBreeze 自定义胶囊滑块 |
| `drawRotarySlider` | DarkBlueGrey → 拟物旋钮；BlueBreeze → 钢琴漆旋钮；Aurora → 默认弧线 |
| `drawMenuBarBackground` | DarkBlueGrey → 渐变；BlueBreeze → 平色 |
| `drawMenuBarItem` | DarkBlueGrey → hover 洗色 |
| `fillTextEditorBackground` | DarkBlueGrey → 渐变+高光线；BlueBreeze → 平色 |
| `drawTextEditorOutline` | 各主题独立焦点环样式 |
| `drawComboBox` | BlueBreeze → 胶囊形；DarkBlueGrey → 标准 |
| `drawPopupMenuBackground` | BlueBreeze → 白色平底；其他 → `LookAndFeel_V4` |
| `drawPopupMenuItem` | BlueBreeze → 蓝色高亮；其他 → `LookAndFeel_V4` |
| `drawDocumentWindowTitleBar` | DarkBlueGrey → 深色标题栏 |
| `drawScrollbar` | 三套主题各自灰色磨砂滚动条 |
| `getDefaultScrollbarWidth` | 15px |

### 字体覆写

| 方法 | 行为 |
|------|------|
| `getTextButtonFont` | 优先读 `properties["fontHeight"]`/`properties["navFont"]`，否则按高度 |
| `getComboBoxFont` | 优先读 `properties["fontHeight"]`，否则 16.0f |
| `getPopupMenuFont` | 固定 16.0f |
| `getLabelFont` | 固定 16.0f |
| `getAlertWindowTitleFont` | HONOR Sans CN Medium 18.0f |
| `getAlertWindowMessageFont` | HONOR Sans CN Medium 16.0f |
| `getSliderPopupFont` | 14.0f |

### 内部绘制方法（private/protected 辅助）

| 方法 | 说明 |
|------|------|
| `drawSoftShadow(g, area, radius, color)` | 静态阴影绘制工具 |
| `drawBlueBreezeButton(...)` | BlueBreeze 按钮（白色激活态+透明非激活态） |
| `drawDarkBlueGreyButton(...)` | DarkBlueGrey 按钮（6 步渐变+描边） |
| `draw3DRaised(...)` | 凸起 3D 效果 |
| `draw3DRecessed(...)` | 凹陷 3D 效果 |
| `drawVerticalSlider(...)` | 垂直滑块（派发到各主题） |
| `drawBlueBreezeVerticalSlider(...)` | BlueBreeze 垂直滑块 |
| `drawDarkBlueGreySliderThumb(...)` | DarkBlueGrey 滑块 Thumb |
| `drawBlueBreezeKnob(...)` | BlueBreeze 旋钮（弧线+中心数值） |
| `drawDarkBlueGreyKnob(...)` | DarkBlueGrey 拟物旋钮（9 步绘制） |
| `drawBlueBreezeToggleButton(...)` | BlueBreeze 切换按钮 |

---

## 5. 独立主题 LookAndFeel 类

三个独立 LookAndFeel 类各自继承 `juce::LookAndFeel_V4`，提供完全独立的覆写实现。

### 5.1 BlueBreezeLookAndFeel

**文件**: `BlueBreezeLookAndFeel.h/cpp`

覆写方法：`drawRotarySlider`（钢琴漆旋钮，7 步绘制流程）、`drawLinearSlider`、`drawButtonBackground`（胶囊形）、`drawToggleButton`、`fillTextEditorBackground`、`drawTextEditorOutline`、`drawComboBox`、`drawPopupMenuBackground`、`drawPopupMenuItem`、全套字体方法。

私有方法：`drawRotaryArc(...)`。

### 5.2 DarkBlueGreyLookAndFeel

**文件**: `DarkBlueGreyLookAndFeel.h/cpp`

覆写方法同上（风格：深蓝灰，清爽线条，冷色阴影）。

私有方法：`drawRotaryArc(...)`。

### 5.3 AuroraLookAndFeel

**文件**: `AuroraLookAndFeel.h/cpp`

覆写方法同上（风格：暗色霓虹，玻璃质感，动态色彩弧线）。

独有特性：
- `drawNeonGlow(g, path, color, intensity)` — 三层发光效果（内芯+中柔光+外大气散射）。
- 旋钮值弧颜色随值动态渐变：Blue → Green → Orange → Red。
- 悬停显示彩虹渐变数值文本。
- 按钮激活态：霓虹描边+发光，非激活态：玻璃渐变填充。

---

## 6. 主题原始色值/样式定义

### 6.1 BlueBreeze::Colors & BlueBreeze::Style

**文件**: `BlueBreezeTheme.h`

```
Colors: GraphBg(3), Sidebar(2), Knob(8), Interaction(3), Accent/Node(4), Text(3), Clip(5)
Style:  PanelRadius=16, ControlRadius=10, KnobRadius=999, ShadowAlpha=0.25, HoverGlowAmount=0.4
```

### 6.2 DarkBlueGrey::Colors & DarkBlueGrey::Style

**文件**: `DarkBlueGreyTheme.h`

```
Colors: Primary(4), Background(3), Gradient(2), UI(4), 3D(3), Text(4), PianoRoll(4),
        Pitch(3), Note(4), Playhead(3), Tool(3), Status(3), Waveform(2), Scale(1),
        Knob(2), VU(4), Time(2)
Style:  Unified with BlueBreeze (PanelRadius=16, ControlRadius=10), FieldRadius=10,
        ShadowRadius=16, BevelWidth=1, BevelIntensity=0.12, AnimationDurationMs=120
```

### 6.3 Aurora::Colors & Aurora::Style

**文件**: `AuroraTheme.h`

```
Colors: Background(3), Border(2), Accent/Neon(8), Text(3), Status(3), Knob(2)
Style:  Unified (PanelRadius=16, ControlRadius=10, FieldRadius=10), GlowAmount=0.6
```

---

## 7. GlowAnimation 辅助类

**文件**: `OpenTuneLookAndFeel.h:12-53`

```cpp
class GlowAnimation {
    void trigger();              // 启动动画
    void stop();                 // 停止动画
    void update(float deltaTime); // 按时间推进相位
    float getIntensity() const;  // 返回 0.0~1.0 正弦脉冲强度
    bool isActive() const;
};
```
- `speed_` = 3.0 cycles/sec（固定）。
- 正弦波脉冲：`0.5 + 0.5 * sin(phase * 2pi)`。

---

## 8. 待确认事项

- **Theme 切换线程安全**: `activeThemeId_` 为普通 `static inline`，非 `std::atomic`。若 UI 线程和音频线程同时访问（如在 `processBlock` 中读取主题色），存在数据竞争风险。需确认是否仅在 UI 线程使用。
- **OpenTuneLookAndFeel vs 独立 LookAndFeel**: 当前存在两套并行体系 —— `OpenTuneLookAndFeel` 内部按 `ThemeId` 分支派发，同时又有三个独立 `XxxLookAndFeel` 类。需确认哪套是主力、另一套是否已废弃。
- **UIColors::applyTheme 调用时机**: 切换主题时需同时调用 `Theme::setActiveTheme()` 和 `UIColors::applyTheme()`，两步操作未封装为原子操作。
- **字体嵌入**: 构造函数中硬编码加载 `BinaryData::HONORSansCNMedium_ttf`。字体许可证状态待确认。
