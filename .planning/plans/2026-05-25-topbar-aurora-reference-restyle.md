# Top Bar Aurora Reference Restyle Plan

> **For implementers:** 先按本方案收敛视觉合同，再做代码改动；不要跳过对应的测试与视觉验收文档。

**Goal:** 让 Aurora / “激光玻璃”主题下的顶部栏外观更贴近用户提供的参考图，重点修正顶部托盘背景渐变不自然、按钮边框柔光失真、激活态蓝光铺满、图标层级偏浮的问题。

**Architecture:** 本次改动严格限定在 Standalone UI 外观层。`TopBarComponent`、`TransportBarComponent`、`AuroraLookAndFeel`、`UIColors`、`AuroraTheme` 负责新的托盘与控件 chrome；不修改处理器、渲染缓存、时间轴、主题切换机制和任何音频真值线。

**Tech Stack:** C++17、JUCE Graphics、现有 `TopBarComponent` / `TransportBarComponent` / `AuroraLookAndFeel` / `UIColors` / `ThemeTokens`、原生 `OpenTuneTests`。

**Verification Source:** `.planning/plans/2026-05-25-topbar-aurora-reference-restyle-test-verification.md`

---

## Current Findings

- 当前顶部栏外托盘绘制链路是 `TopBarComponent::paint()` -> `UIColors::fillAuroraGlass()` -> `UIColors::drawAuroraGlassFrame()`，之后又额外叠加 `bottomGlow` 与 `bottomFade`。这会让整块顶栏底板同时出现纵向高光、侧向冷光、底部蓝雾和底部压暗，读感偏“整面发光”，不像参考图那种克制的深色玻璃托盘。
- 当前 Aurora 按钮外观的公共入口是 `UIColors::drawAuroraButtonChrome()`，`UnifiedToolbarButton`、时间显示、BPM 输入框、ComboBox 都共用这套 chrome。也就是说，按钮“不像参考图”的根因不是某一个控件，而是公共按钮壳层本身。
- `drawAuroraButtonChrome()` 目前使用整轮廓 `DropShadow` 外发光、较宽的顶部 sheen、整圈连续外边框和较强的激活态蓝色填充，因此更像柔软 CG 玻璃，而不是参考图那种边缘折射更硬、中心更沉、光更局部的实物质感。
- `UnifiedToolbarButton::paintButton()` 目前还会在 hover/active 下进一步抬升图标亮度与缩放，因此会把图标和边框一起推亮，使“按钮壳体”和“图标内容”同时抢注意力，削弱参考图那种深色主体上少量精确亮点的控制感。
- 顶部栏布局节奏也比参考图松。当前 `TransportBarComponent::resized()` 中按钮宽度、间距和组间距偏大，时间框、BPM 框、下拉框与按钮群之间的节奏更像功能分组面板，不像参考图那种更紧凑、几乎一体成列的控制条。

## Visual Contract

- 顶部栏托盘应当变成“深色承托层”，不是“主发光体”。
- 托盘主体明暗过渡应更均匀，仅保留薄的上沿冷高光和非常克制的边缘提亮。
- 按钮外发光应从“整圈平均扩散”改为“贴边、贴角、局部热点”的边缘光感。
- 按钮内部需要更深的中心质量感，更薄的顶部高光，更弱的底部雾化。
- 激活态应保留深色底，不允许简单把整块按钮填成蓝面；蓝色主要出现在边缘折射、内腔晕光和少量面光。
- 图标与文本要重新退后半级，避免与壳层高光同时冲到最前。
- 时间显示、BPM、下拉框、图标按钮必须共享同一套 Aurora chrome 语言，不能按钮像参考图、输入框却还停留在旧风格。

## Task 1: 收紧 Aurora 主题 Token

**Files:**
- Modify: `Source/Standalone/UI/AuroraTheme.h`
- Modify: `Source/Standalone/UI/ThemeTokens.h`
- Modify: `Source/Standalone/UI/UIColors.h`

**Steps:**
1. 重新定义 Aurora 顶栏相关 token 的职责，至少拆开“托盘背景”、“托盘边缘”、“按钮主面”、“按钮边缘光”、“激活态内晕”、“全局 panel glow”。
2. 将现有偏亮、偏连续、偏蓝紫混光的 `GlassHighlight` / `GlassEdge` / `PanelGlow` / `ButtonActive` 收敛为更深、更冷、更薄的控制组。
3. 若现有 token 粒度不足，优先补充语义 token，而不是在每个控件里硬编码不同颜色。

**Definition of Done:**
- Token 命名能区分托盘与按钮两类光学角色。
- Aurora 主题不再依赖单一 `PanelGlow` 同时承担托盘与按钮的所有发光任务。

## Task 2: 重做顶部托盘背景层次

**Files:**
- Modify: `Source/Standalone/UI/TopBarComponent.cpp`
- Modify: `Source/Standalone/UI/UIColors.h`

**Steps:**
1. 调整 `fillAuroraGlass()` 的主体渐变，让托盘中心区域回到更均匀、更暗的深蓝玻璃底。
2. 弱化或移除当前过于显眼的侧向冷光，让横向泛蓝不再成为主要观感。
3. 重新设计 `TopBarComponent::paint()` 中的 `bottomGlow` / `bottomFade`，优先改为非常窄、非常淡的底缘收口，而不是整片底部蓝雾。
4. 保留顶部栏与页面背景的分离感，但把视觉中心让给内部控件。

**Definition of Done:**
- 顶部托盘从远看首先是深色承托条，而不是一块蓝色霓虹面板。
- 底部不再出现明显的“发雾感”和过度压暗并存的矛盾层次。

## Task 3: 重构公共按钮 Chrome

**Files:**
- Modify: `Source/Standalone/UI/UIColors.h`

**Steps:**
1. 将 `drawAuroraButtonChrome()` 按“外缘辉光、主体玻璃、顶部镜面、内缘折射、激活态内晕”拆成可独立调节的层。
2. 用更窄、更贴边的 glow 取代当前整形状 `DropShadow` 的平均外扩效果。
3. 收紧顶部高光带宽度，让按钮上沿读起来更像硬质折射而不是一整片油亮塑料。
4. 增强边框“薄硬感”，减少整圈持续发亮的软边观感。
5. 调低底部暗雾和大面积面光，给按钮中心留出更深的体积感。

**Definition of Done:**
- 共享 chrome 一次调整后，图标按钮、时间框、BPM 框、下拉框都会同步变得更接近参考图。
- 按钮看起来更像深色玻璃胶囊，而不是带发光描边的 UI 贴片。

## Task 4: 收紧激活态与内容层级

**Files:**
- Modify: `Source/Standalone/UI/TransportBarComponent.cpp`
- Modify: `Source/Standalone/UI/AuroraLookAndFeel.cpp`
- Modify: `Source/Standalone/UI/UIColors.h`

**Steps:**
1. 调整 `UnifiedToolbarButton::paintButton()` 中 Aurora 分支的图标亮度、hover 缩放和 icon inset，降低内容层与壳层同时抬亮的程度。
2. 将激活态蓝光从“整块面变蓝”改为“保留深底 + 局部边缘/内腔增强”。
3. 复核 `DigitalTimeDisplay`、`BpmValueField`、ComboBox 的 Aurora 外观，确保这些非图标控件不会比按钮更亮或更糊。
4. 让 hover、toggle、keyboard focus 三类状态彼此可区分，但都服从同一个克制的高光体系。

**Definition of Done:**
- 激活态仍然显眼，但不再像直接替换成蓝底按钮。
- 图标、数字、文本在非激活状态下稍微退后，按钮壳层承担主要材质表达。

## Task 5: 微调顶部栏节奏与尺寸关系

**Files:**
- Modify: `Source/Standalone/UI/TransportBarComponent.cpp`
- Modify: `Source/Standalone/UI/TopBarComponent.cpp`

**Steps:**
1. 重新审视 `buttonWidth`、`controlHeight`、`spacing`、`groupGap`、时间框宽度、BPM 框宽度与左右切换按钮的节奏关系。
2. 在不破坏当前功能布局的前提下，把主控件列队变得更紧凑，让整体更接近参考图的“一整排胶囊控件”。
3. 保证缩紧后的布局在当前窗口最小宽度下仍不重叠、不截断。
4. 若需要，只在已有布局常量层面微调，不引入新的布局系统。

**Definition of Done:**
- 视觉节奏更紧，控件组之间更统一，不再显得松散或局部过宽。
- 参考图中那种控制条的一体感明显增强。

## Task 6: 视觉烟测与常量回调

**Files:**
- No required source changes unless smoke 结果显示仍有明显偏差。

**Steps:**
1. 按验证文档构建 Standalone。
2. 启动应用，在 Aurora / 激光玻璃主题下对比用户提供的参考图。
3. 重点检查：
   - 托盘主体是否退后
   - 边缘高光是否更薄更真实
   - 按钮外光是否仍有整圈糊光
   - 激活态是否过满
   - 图标是否仍然太浮
   - 时间框、BPM、下拉框是否和按钮处于同一材质语言
4. 如果仍有差距，只允许继续回调 token、透明度、渐变停点、边框强度、布局常量；不扩散到新组件体系。

## Kill List Review

- 不新增并行的 Aurora 按钮绘制路径和旧路径长期共存。
- 不在单个按钮、单个下拉框、单个时间框上打特殊补丁来掩盖公共 chrome 问题。
- 不引入新的主题系统、shader、OpenGL、QML 或离屏位图皮肤。
- 不让顶部托盘比内部控件更抢眼。
- 不为了“更炫”而把霓虹强度继续做高；目标是更像参考图，不是更亮。
- 不修改任何处理器、缓存、播放、时间轴或项目状态逻辑。

## Open Review Points

- 是否需要把当前左右侧的大切换按钮与中间 transport 按钮进一步统一成同一种边缘高光语气，可以在第一次 smoke 后再决定。
- 如果第一轮视觉结果已经接近参考图，应优先停在 token 和分层层面，不继续做细碎的逐控件特调。
