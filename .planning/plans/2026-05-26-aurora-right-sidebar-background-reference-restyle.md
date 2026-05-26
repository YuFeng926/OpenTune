# Aurora Right Sidebar Background Reference Restyle Plan

> **For implementers:** 先按本方案收敛视觉合同，再做代码改动；本次范围严格限定在 Aurora 主题下右侧栏背景壳层，不改旋钮、不改工具按钮、不改任何交互逻辑。

**Goal:** 让 Aurora / “激光玻璃” 主题下的右侧参数栏背景更贴近用户提供的参考图，重点修正“整体渐变方向不对、层次不够细腻、边缘若隐若现的柔光缺失、现在像把顶栏托盘纵向拉高”的问题。
**Architecture:** 本次改动限定在 Standalone UI 外观层：`ParameterPanel` 负责右侧栏组件入口，`UIColors` / `AuroraTheme` / `ThemeTokens` 提供 Aurora 右侧栏专用壳层绘制与语义 token；不修改旋钮绘制、按钮 chrome、控件布局逻辑、处理器与音频真值线。
**Tech Stack:** C++17、JUCE Graphics、现有 `ParameterPanel` / `UIColors` / `AuroraTheme` / `ThemeTokens`。
**Verification Source:** `.planning/plans/2026-05-26-aurora-right-sidebar-background-reference-restyle-test-verification.md`

---

## Current Findings

- 当前右侧栏背景入口是 `ParameterPanel::paint()`。它在裁剪圆角路径后，直接调用 `UIColors::fillPanelBackground()` 和 `UIColors::drawPanelFrame()`；Aurora 下这两者又继续走 `fillAuroraGlass()` / `drawAuroraGlassFrame()`。
- `fillAuroraGlass()` 现在使用的是 `TrayTop` / `TrayMid` / `TrayBottom` / `TraySideGlow` / `TrayTopHighlight` 这组“顶栏托盘语义”色值。它的主体是“顶部薄高光 + 斜向 sourceLift + 横向 sideAura + 下半部 lowerSettle”，更适合矮而横向展开的顶部托盘，不适合高而窄的竖向右侧栏。
- `drawAuroraGlassFrame()` 也是托盘取向：外框、内唇、顶部一条细冷光，整体比较克制，但缺少参考图右侧栏那种“左右边缘若隐若现、上角和下角有轻微聚光、壳体体积感更完整”的感觉。
- 当前右侧栏外侧没有 Aurora 专属的柔光壳层。`ParameterPanel::paint()` 只画了统一阴影 `UIColors::drawShadow()`，没有独立的蓝色边缘 aura，所以看起来更像普通深色圆角面板，而不是参考图那种有轻微悬浮感的玻璃舱体。
- 因为 `fillPanelBackground()` 是公共面板入口，直接重做 Aurora 的通用 panel 背景会波及其他 Aurora 面板；而用户这次只要求“右侧栏背景”逼近参考图，所以不应再走“改全局 Aurora panel”的路径。
- 用户明确要求“旋钮按钮部分不用修改”，所以这次方案必须避开 `drawAuroraKnob()`、`drawAuroraButtonChrome()`、`ToolIconButton::paintButton()` 的外观逻辑，只处理壳层背景与边框。

## Visual Contract

- 右侧栏首先应当读成“深色竖向玻璃舱体”，而不是“把顶部托盘纵向拉长后的通用玻璃板”。
- 主体明暗应更稳，中心区保留一定深度，不能整片泛蓝或整片发雾。
- 顶部高光必须更薄、更窄、更靠边，只做壳体上沿的冷光提示，不做大面积漂白。
- 左右两侧边缘需要有若隐若现的冷色柔光，但必须是贴边、低透明度、局部聚集的，不是整面横向蓝雾。
- 下部需要有轻微的“收口”和体积感，让面板看起来像一块厚玻璃壳体落在深色背景上，而不是平铺矩形。
- 外框应分为“结构边”和“玻璃内唇”两层，亮度受控，不能比内部旋钮和按钮更抢眼。
- 控件本体应继续退居前景内容层；壳层只负责承托与质感，不抢旋钮和工具按钮的视觉主导权。

## Scope Boundary

- 只改 Aurora 主题下右侧 `ParameterPanel` 的背景填充、外缘 aura、边框层次、可选的壳层分隔线。
- 不改旋钮外观：`drawAuroraKnob()`、旋钮光圈、指示点、数值框样式全部不在这次范围内。
- 不改工具按钮外观：`drawAuroraButtonChrome()`、按钮 hover / active / idle 状态全部不在这次范围内。
- 不改右侧栏内部排版、间距、控件尺寸、标题字重、交互逻辑。
- 不改左侧栏、顶部栏、PianoRoll、TransportBar 或其他 Aurora 面板。

## Task 1: 为 Aurora 右侧栏补专用语义 Token

**Files:**
- Modify: `Source/Standalone/UI/AuroraTheme.h`
- Modify: `Source/Standalone/UI/ThemeTokens.h`
- Modify: `Source/Standalone/UI/UIColors.h`

**Steps:**
1. 在 Aurora 主题中新增“右侧栏壳层”专用语义色，而不是继续复用 `Tray*`。
2. 最少拆出以下职责：
   - `SidebarShellTop`
   - `SidebarShellMid`
   - `SidebarShellBottom`
   - `SidebarTopLip`
   - `SidebarOuterRim`
   - `SidebarInnerRim`
   - `SidebarEdgeAura`
   - `SidebarCornerBloom`
   - `SidebarLowerSettle`
3. 如果需要，把这些 token 接进 `ThemeTokens` / `UIColors` 的当前主题缓存，避免在 `ParameterPanel` 里硬编码颜色。

**Definition of Done:**
- 从命名上能清楚区分“顶栏托盘语义”和“右侧栏壳层语义”。
- 右侧栏后续绘制不再依赖 `TrayTop` / `TrayMid` / `TrayBottom` 这组横向托盘 token。

## Task 2: 在 `UIColors` 中新增右侧栏专用背景绘制辅助

**Files:**
- Modify: `Source/Standalone/UI/UIColors.h`

**Steps:**
1. 新增类似 `fillAuroraSidebarShell()` 的专用填充函数，只服务竖向右侧栏，不替换现有 `fillAuroraGlass()`。
2. 绘制顺序建议拆成 4 层：
   - 外围极弱 aura：壳体外侧、贴边、低透明度，重点在左右边和四角，不做整圈平均模糊。
   - 主体 fill：竖向深色渐变，顶部略抬、中部稳定、底部轻收，不允许“上亮下整片蓝雾”。
   - 内部叠层：很薄的顶部冷光、两侧低对比冷提亮、极轻的底部 settle / vignette。
   - 结构边框：暗结构边 + 冷色内唇边，亮度都要克制。
3. 新增类似 `drawAuroraSidebarShellFrame()` 的专用边框函数，避免继续复用顶栏托盘的 frame。
4. 如果实现 aura 时现有 `DropShadow` 太“整圈平均”，优先使用多段渐变 / 局部路径 / 角落 bloom 的组合，而不是继续放大整路径阴影半径。

**Definition of Done:**
- `fillAuroraGlass()` 继续服务顶栏/通用玻璃托盘，不被右侧栏需求反向污染。
- 右侧栏拥有独立的 Aurora 玻璃舱体绘制语言。

## Task 3: 将 `ParameterPanel` Aurora 分支改为走右侧栏专用壳层

**Files:**
- Modify: `Source/Standalone/UI/ParameterPanel.cpp`

**Steps:**
1. 在 `ParameterPanel::paint()` 中保留现有的圆角路径、clip 和统一阴影入口。
2. 当 `themeId == ThemeId::Aurora` 时，不再调用通用 `fillPanelBackground()` / `drawPanelFrame()`，改为显式调用右侧栏专用 helper。
3. 非 Aurora 主题保持现状不变，避免给 BlueBreeze / DarkBlueGrey / Overdose 带来回归。
4. 只在这里做壳层切换，不把逻辑扩散到旋钮、标签或工具按钮子组件。

**Definition of Done:**
- 右侧栏背景改动局部化到 `ParameterPanel`，不会无意改变其他 Aurora 面板。
- 现有控件树、布局和交互路径保持不变。

## Task 4: 可选补一个极弱的分区承托线

**Files:**
- Modify: `Source/Standalone/UI/ParameterPanel.cpp`

**Steps:**
1. 仅在视觉上确有必要时，在“音高校正”与“工具”两段之间补一条非常弱的壳层分区线。
2. 这条线必须属于背景承托层，不能变成内容主元素，也不能抢按钮。
3. 如果没有现成的几何锚点可稳定复用，宁可不做，也不要为一条线引入新的布局耦合。

**Definition of Done:**
- 分区线若存在，只作为轻微层次提示；若做不到稳定克制，则直接省略。

## Task 5: 视觉回调原则

**Files:**
- No required source changes unless第一次 smoke 后仍明显偏离参考图

**Steps:**
1. 第一轮只调“壳层大关系”：整体明暗、边缘 aura、上沿高光、底部收口。
2. 第二轮才允许微调停点、透明度、边框粗细和角部聚光范围。
3. 如果出现“更蓝但不更像”的趋势，优先回退 glow 强度，而不是继续加亮。

**Definition of Done:**
- 调参方向始终围绕“更像参考图的深玻璃舱体”，不是“更强烈的霓虹风”。

## Kill List Review

- 不把 `fillAuroraGlass()` 直接改成同时服务顶栏和右侧栏的“万能玻璃函数”。
- 不修改 `drawAuroraButtonChrome()`、`drawAuroraKnob()`、按钮 hover/active、旋钮 rim/glow。
- 不为了一点边缘柔光去增加整圈平均外发光，导致面板重新变成“蓝雾板”。
- 不把右侧栏背景改成比旋钮和工具按钮更亮的主视觉。
- 不引入新主题系统、位图皮肤、OpenGL、shader 或离屏资源。
- 不顺手改左右内边距、按钮排布、工具区几何、字体或数值框。

## Open Review Points

- 如果第一轮 smoke 显示“边缘 aura 已够，但中心仍偏空”，优先补中段极弱体积感，而不是再加顶部高光。
- 如果参考图里那条工具区上方分隔线对整体气质帮助明显，可以在第二轮作为背景承托层引入；否则建议保持纯壳体方案，避免额外视觉元素。

