---
module: ui-theme
type: api
generated: true
source_scan: true
updated: 2026-05-05
---

# ui-theme API 契约

> 所有类型位于 `namespace OpenTune`（主题常量在嵌套子命名空间 `BlueBreeze` / `DarkBlueGrey` / `Aurora`）。

## 1. ThemeId 枚举

**文件**：`ThemeTokens.h`

```cpp
enum class ThemeId : int
{
    BlueBreeze   = 0,
    DarkBlueGrey = 1,
    Aurora       = 2
};
```

序号固定（作为 `AppPreferences.shared.theme` 的持久化值），不可重排。

## 2. Theme 静态管理器

**文件**：`ThemeTokens.h`

```cpp
class Theme
{
public:
    static const ThemeTokens& getTokens(ThemeId themeId);
    static const ThemeStyle&  getStyle (ThemeId themeId);
};
```

| 方法 | 返回 | 行为 | 线程 |
|------|------|------|------|
| `getTokens(id)` | `const ThemeTokens&` | switch→返回对应主题的 static 实例；未知 id 回退 BlueBreeze | 任意线程（纯读，无状态） |
| `getStyle(id)` | `const ThemeStyle&` | 同上，返回 style 实例 | 任意线程 |

内部私有工厂方法（仅内部使用）：`blueBreezeTokens()` / `darkBlueGreyTokens()` / `auroraTokens()` / `blueBreezeStyle()` / `darkBlueGreyStyle()` / `auroraStyle()` — 均返回 function-local `static const` 单例。

## 3. UIColors 全局缓存

**文件**：`UIColors.h`（`struct UIColors`，所有成员为 `static inline`）

### 3.1 主题切换 API

```cpp
static void applyTheme(const ThemeTokens& tokens);  // 仅拷贝 tokens，不动 currentThemeId_
static void applyTheme(ThemeId themeId);            // 完整切换：写 currentThemeId_/style + 拷贝 tokens
static ThemeId            currentThemeId();
static const ThemeTokens& currentTokens();
static const ThemeStyle&  currentThemeStyle();
```

| 方法 | 用途 | 副作用 |
|------|------|--------|
| `applyTheme(const ThemeTokens&)` | 低层：只拷贝颜色字段，不修改 themeId 状态 | 写入 44 个静态色字段 + cornerRadius |
| `applyTheme(ThemeId)` | 高层：整套切换 | 设置 `currentThemeId_` + `currentThemeStyle_` + 调用上面重载 |
| `currentThemeId()` | 查询当前主题 id | 无 |
| `currentTokens()` | 重新从 `Theme::getTokens` 取引用（不读本地缓存） | 无 |
| `currentThemeStyle()` | 返回缓存的 style | 无 |

⚠️ `applyTheme` 必须在 UI 线程调用。全局静态字段无原子保护。

### 3.2 导航栏度量常量

```cpp
static constexpr int   navControlHeight   = 50;    // 1.25x 缩放
static constexpr float navFontHeight      = 20.0f;
static constexpr float navMonoFontHeight  = 20.0f;
```

### 3.3 阴影层级

```cpp
enum class ShadowLevel : int { Ambient = 0, Float = 1, Pop = 2 };

static void drawShadow(juce::Graphics& g, const juce::Rectangle<float>& bounds);
static void drawShadow(juce::Graphics& g, const juce::Rectangle<float>& bounds, ShadowLevel level);
```

| 方法 | 行为 |
|------|------|
| `drawShadow(g, bounds)` | 默认使用 `Ambient` 级 |
| `drawShadow(g, bounds, level)` | DarkBlueGrey 主题使用冷色分层阴影（#050A12 + 不同 alpha/radius/offset）；其他主题使用 style.shadowAlpha/shadowRadius/shadowOffset |

### 3.4 背景与面板绘制辅助

```cpp
static void fillSoothe2CanvasBackground (juce::Graphics&, const juce::Rectangle<float>&, float radius);
static void fillSoothe2SpectrumBackground(juce::Graphics&, const juce::Rectangle<float>&, float radius);
static void fillPanelBackground          (juce::Graphics&, const juce::Rectangle<float>&, float radius);
static void drawPanelFrame               (juce::Graphics&, const juce::Rectangle<float>&, float radius);
```

| 方法 | 行为 |
|------|------|
| `fillSoothe2CanvasBackground` | BlueBreeze 用暖米→冷灰蓝渐变 + 中心高光；DarkBlueGrey 走普通 gradient 分支 |
| `fillSoothe2SpectrumBackground` | 仅 DarkBlueGrey 实现（深灰渐变 + 细格线）；其他主题填 `backgroundMedium` 回退 |
| `fillPanelBackground` | DarkBlueGrey：微渐变 + 顶部高光线 + 内阴影；BlueBreeze：gradientTop→gradientBottom + 软高光 + vignette + accent glow |
| `drawPanelFrame` | 主边框 + 内高光边框，glowAlpha>0 时叠加 accent glow |

### 3.5 字体工厂

```cpp
static juce::Font getUIFont    (float height = 16.0f);  // Regular，底限 16
static juce::Font getHeaderFont(float height = 18.0f);  // Semibold，底限 18
static juce::Font getLabelFont (float height = 14.0f);  // Regular，底限 14
static juce::Font getMonoFont  (float height = 20.0f);  // 系统等宽 Bold
```

### 3.6 颜色变换工具

```cpp
static juce::Colour withAlpha   (const juce::Colour&, float alpha);
static juce::Colour interpolate (const juce::Colour& a, const juce::Colour& b, float ratio);
static juce::Colour brighten    (const juce::Colour&, float amount);
static juce::Colour darken      (const juce::Colour&, float amount);
```

### 3.7 颜色字段（44 个）

以 `static inline juce::Colour` 存储，完整列表见 [data-model.md](./data-model.md#uicolors-字段)。按分组：
- Primary / Accent（4）
- Background（3）+ Gradient（2）
- UI Element（4）+ Bevel/Glow（3）
- Text（4）
- Piano Roll（4）+ Pitch Curve（3）+ Note Block（4）
- Playhead / Timeline（3）
- Tool / Button（3）
- Status（3）
- Waveform（2）+ Scale（1）+ Knob（2）
- cornerRadius（float）+ currentThemeId_ / currentThemeStyle_

## 4. ThemeTokens / ThemeStyle 结构体

**文件**：`ThemeTokens.h`

完整字段列表见 [data-model.md](./data-model.md#themetokens-字段表)。此处仅列结构签名：

```cpp
struct ThemeTokens {
    juce::Colour primaryPurple, accent, lightPurple, darkPurple;
    juce::Colour backgroundDark, backgroundMedium, backgroundLight;
    juce::Colour gradientTop, gradientBottom;
    juce::Colour panelBorder, buttonNormal, buttonHover, buttonPressed;
    juce::Colour bevelLight, bevelDark, glowColor;
    juce::Colour textPrimary, textSecondary, textDisabled, textHighlight;
    juce::Colour rollBackground, laneC, laneOther, gridLine;
    juce::Colour originalF0, correctedF0, shadowTrack;
    juce::Colour noteBlock, noteBlockBorder, noteBlockSelected, noteBlockHover;
    juce::Colour playhead, timelineMarker, beatMarker;
    juce::Colour toolActive, toolInactive, buttonInactive;
    juce::Colour statusProcessing, statusReady, statusError;
    juce::Colour waveformFill, waveformOutline;
    juce::Colour scaleHighlight;
    juce::Colour knobBody, knobIndicator;
    float cornerRadius = 8.0f;
};  // 43 Colour + 1 float = 44 字段

struct ThemeStyle {
    float panelRadius = 16.0f;
    float controlRadius = 10.0f;
    float fieldRadius = 10.0f;
    float knobRadius = 999.0f;
    float strokeThin = 1.0f;
    float strokeThick = 2.0f;
    float focusRingThickness = 2.0f;
    float shadowAlpha = 0.25f;
    int   shadowRadius = 10;
    juce::Point<int> shadowOffset { 0, 4 };
    float glowAlpha = 0.0f;
    float glowRadius = 0.0f;
    float bevelWidth = 2.0f;
    float bevelIntensity = 0.3f;
    float animationDurationMs = 150.0f;
    float hoverGlowIntensity = 0.8f;
    juce::Colour vuLow, vuMid, vuHigh, vuClip;
    juce::Colour timeActive, timeInactive;
    bool timeSegmentStyle = true;
};  // 16 float/int + 6 Colour + 1 bool = 23 字段
```

## 5. OpenTuneLookAndFeel（一体式）

**文件**：`OpenTuneLookAndFeel.h`（header-only，所有实现均 inline 在头文件中）

```cpp
class OpenTuneLookAndFeel : public juce::LookAndFeel_V4 { ... };
```

### 5.1 构造

- 从 `BinaryData::HONORSansCNMedium_ttf` 加载 Typeface 并注册为 `juce::LookAndFeel::setDefaultSansSerifTypeface`（全局默认无衬线字体一次性设定）

### 5.2 覆盖方法（按类别）

| 类别 | `override` 方法 | 行数附近 |
|------|----------------|---------|
| 按钮 | `drawButtonBackground`, `drawToggleButton`, `drawTickBox` | 100 / 173 / 211 |
| 字体 | `getTextButtonFont`, `getComboBoxFont`, `getPopupMenuFont`, `getLabelFont`, `getAlertWindowTitleFont`, `getAlertWindowMessageFont`, `getAlertWindowFont`, `getSliderPopupFont` | 83 / 368–466 |
| 文本 | `drawLabel` | 385 |
| 滑块 | `drawLinearSlider`, `drawRotarySlider` | 471 / 656 |
| 菜单 | `drawMenuBarBackground`, `drawMenuBarItem`, `drawPopupMenuBackground`, `drawPopupMenuItem` | 826 / 856 / 1049 / 1063 |
| 文本编辑器 | `fillTextEditorBackground`, `drawTextEditorOutline` | 886 / 917 |
| ComboBox | `drawComboBox`, `createComboBoxTextBox`, `positionComboBoxText` | 966 / 1121 / 1135 |
| 窗口 | `drawDocumentWindowTitleBar` | 1150 |
| 滚动条 | `getDefaultScrollbarWidth`, `drawScrollbar` | 1189 / 1194 |

### 5.3 私有辅助方法（非 override）

用于 BlueBreeze / DarkBlueGrey 的主题内部分派（由上面的 override 根据 `UIColors::currentThemeId()` 调用）：

```cpp
void drawBlueBreezeButton        (juce::Graphics&, juce::Button&, juce::Rectangle<float>, float radius, ...);
void drawBlueBreezeToggleButton  (...);
void drawBlueBreezeVerticalSlider(...);
void drawBlueBreezeKnob          (juce::Graphics&, juce::Rectangle<float>, float cx, float cy, float radius, ...);

void drawDarkBlueGreyButton      (juce::Graphics&, juce::Button&, juce::Rectangle<float>, float radius, ...);
void drawDarkBlueGreySliderThumb (juce::Graphics&, juce::Rectangle<float> thumb, const ThemeStyle&);

void drawVerticalSlider (juce::Graphics&, juce::Rectangle<float>, float sliderPos, juce::Slider&);

// 3D bevel helper（仅 DarkBlueGrey 或需要拟物感的路径使用）
void draw3DRaised   (juce::Graphics&, juce::Rectangle<float> bounds, float cornerSize, float bevelWidth, bool isHighlighted);
void draw3DRecessed (juce::Graphics&, juce::Rectangle<float> bounds, float cornerSize, float bevelWidth);

// 通用静态阴影
static void drawSoftShadow(juce::Graphics&, juce::Rectangle<float> area, float radius, juce::Colour color);
```

### 5.4 辅助类 `GlowAnimation`

位于同一头文件内，用于按钮 hover 发光的时间相位（sine 波脉冲）：

```cpp
class GlowAnimation {
public:
    void trigger();
    void stop();
    void update(float deltaTime);
    float getIntensity() const;   // 0..1
    bool  isActive() const;
private:
    bool  active_ = false;
    float phase_  = 0.0f;
    static constexpr float speed_ = 3.0f;  // cycles/sec
};
```

## 6. AuroraLookAndFeel（独立）

**文件**：`AuroraLookAndFeel.h` / `AuroraLookAndFeel.cpp`

```cpp
class AuroraLookAndFeel : public juce::LookAndFeel_V4 {
public:
    AuroraLookAndFeel() = default;

    void drawRotarySlider      (juce::Graphics&, int x, int y, int w, int h,
                                float sliderPosProportional, float startAngle, float endAngle,
                                juce::Slider&) override;
    void drawLinearSlider      (juce::Graphics&, int x, int y, int w, int h,
                                float sliderPos, float minSliderPos, float maxSliderPos,
                                juce::Slider::SliderStyle, juce::Slider&) override;
    void drawButtonBackground  (juce::Graphics&, juce::Button&, const juce::Colour&,
                                bool highlighted, bool down) override;
    void drawToggleButton      (juce::Graphics&, juce::ToggleButton&, bool, bool) override;
    void fillTextEditorBackground(juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawComboBox          (juce::Graphics&, int w, int h, bool down,
                                int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void drawPopupMenuBackground(juce::Graphics&, int w, int h) override;
    void drawPopupMenuItem     (juce::Graphics&, const juce::Rectangle<int>&, bool sep,
                                bool active, bool highlighted, bool ticked, bool hasSubMenu,
                                const juce::String& text, const juce::String& shortcut,
                                const juce::Drawable* icon, const juce::Colour* textColour) override;

    juce::Font getTextButtonFont      (juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont           (juce::Label&)  override;
    juce::Font getComboBoxFont        (juce::ComboBox&) override;
    juce::Font getPopupMenuFont       () override;
    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont     () override;

private:
    void drawNeonGlow(juce::Graphics&, juce::Path&, juce::Colour color, float intensity);
};
```

### 6.1 drawNeonGlow 分层发光

三层发光叠加：
- **Inner core**：`1.5px` 描边，alpha `0.8 * intensity`
- **Middle glow**：`DropShadow` radius `8 * intensity`，alpha `0.4 * intensity`
- **Outer dispersion**：`DropShadow` radius `16 * intensity`，alpha `0.2 * intensity`

### 6.2 drawRotarySlider 动态色

按 `sliderPosProportional` 在 4 段之间插值：
- `< 0.33`：Cyan → NeonGreen
- `0.33..0.66`：NeonGreen → NeonOrange
- `>= 0.66`：NeonOrange → NeonRed

并用 `drawNeonGlow` 在 valueArc 上叠加发光。

### 6.3 私有静态辅助（匿名命名空间）

位于 `.cpp` 中：

```cpp
static juce::ColourGradient createGlassGradient(juce::Rectangle<float> bounds);  // BgSurface +/-5%
static void drawRainbowText(juce::Graphics&, const juce::String&,
                            juce::Rectangle<float>, juce::Justification);
```

## 7. BlueBreeze / DarkBlueGrey / Aurora 常量命名空间

三个文件均为**纯 header-only**，无 `.cpp`，结构相同：

```cpp
namespace OpenTune::BlueBreeze {   // 同：DarkBlueGrey、Aurora
    struct Colors { static const juce::uint32 ...; };
    struct Style  { static constexpr float ...; };
}
```

- **BlueBreeze::Colors**（25 键）：GraphBgDeep/Mid/Light、SidebarBg、PanelBorder、KnobBody/Light/Highlight/Edge/Indicator/Track/Shadow/Glow、ActiveWhite、HoverOverlay、ShadowColor、NodeRed/Yellow/Purple、AccentBlue、TextDark/Light/Dim、ClipGradientTop/Bottom、ClipBorder、ClipSelectedTop/Bottom
- **DarkBlueGrey::Colors**（48 键，见 data-model）
- **Aurora::Colors**（21 键）：BgDeep/Surface/Overlay、BorderLight/Glow、Cyan、Violet、ElectricBlue、Magenta、NeonGreen/Orange/Red/Yellow、TextPrimary/Secondary/Dim、Success、Warning、Error、KnobBody、KnobIndicator

- **BlueBreeze::Style**：PanelRadius(16) / ControlRadius(10) / KnobRadius(999) / StrokeThin(1) / StrokeThick(2) / ShadowAlpha(0.25) / ShadowRadius(10) / HoverGlowAmount(0.4)
- **DarkBlueGrey::Style**（13 键，完整最齐）：含 FieldRadius / FocusRingThickness / ShadowOffsetX/Y / GlowAlpha/Radius / BevelWidth/Intensity / AnimationDurationMs / HoverGlowIntensity / CornerRadius
- **Aurora::Style**：PanelRadius(16) / ControlRadius(10) / FieldRadius(10) / StrokeThin/Thick / ShadowAlpha(0.25) / ShadowRadius(10) / ShadowOffsetX(0)/Y(4) / GlowAmount(0.6)

## 8. SmallFontTextButton

**文件**：`UIColors.h`（同文件尾部）

```cpp
class SmallFontTextButton : public juce::TextButton {
public:
    SmallFontTextButton(const juce::String& name = {});
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
};
```

自定义绘制：直接使用 `UIColors::buttonPressed/buttonHover` + `findColour(TextButton::buttonColourId)`，固定 `"HONOR Sans CN" Medium 12pt`、圆角 4px。绕开了 `LookAndFeel` 的统一逻辑。

## 9. 与外部模块的调用约定

| 调用方 | 调用点 | 作用 |
|--------|--------|------|
| `PluginEditor::OpenTuneAudioProcessorEditor()`（ctor） | `UIColors::applyTheme(appPreferences_.getState().shared.theme)` | 启动时按持久化主题初始化全局色 |
| `PluginEditor::themeChanged(ThemeId)` | `appPreferences_.setTheme(id)` + `applyThemeToEditor(id)` | 响应菜单切换 |
| `PluginEditor::applyThemeToEditor(ThemeId)` | `UIColors::applyTheme(id)` + `setLookAndFeel(id==Aurora ? &auroraLookAndFeel_ : &openTuneLookAndFeel_)` + 子组件级联 `applyTheme()` + `sendLookAndFeelChange()` + `repaint()` | 主题切换主流程 |
| `MenuBarComponent::menuItemSelected` | `listeners_.call(themeChanged(ThemeId::xxx))` | 菜单 → PluginEditor 的信号通道 |
| `MenuBarComponent::getMenuForIndex` | `UIColors::currentThemeId() == ThemeId::xxx` | 菜单项打勾状态 |
| `TrackPanelComponent::applyTheme` / `TransportBarComponent::applyTheme` / `TopBarComponent::applyTheme` | 读取 `UIColors::currentThemeId()` 做内部分支样式 + 直接读取 `UIColors::xxx` 字段 | 子组件主题响应 |

## 待确认

1. **未定义 ThemeListener 接口** — 本模块没有提供 `addThemeListener` 机制。主题切换完全依赖 `PluginEditor::applyThemeToEditor` 中硬编码的子组件调用链（`topBar_.applyTheme()` 等）。如果新增顶级子组件，是否应在此模块提供通用广播机制？
2. **`applyTheme(const ThemeTokens&)` 重载的使用者未知** — 代码搜索中只有 `applyTheme(ThemeId)` 被调用。低层重载保留给谁？是否有热插拔自定义 token 的未来场景？
3. **`SmallFontTextButton` 是否该归入本模块** — 它物理上位于 `UIColors.h` 底部，但职责属于"具体组件"而非"主题系统"。
4. **`drawSoftShadow` 与 `UIColors::drawShadow` 职责重叠** — 前者是 `OpenTuneLookAndFeel` 的 static 方法，后者是 `UIColors` 静态方法。两者是否应统一？
