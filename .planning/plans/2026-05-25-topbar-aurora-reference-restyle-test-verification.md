# Top Bar Aurora Reference Restyle Test Verification

## Purpose

本文件是顶部栏 Aurora / “激光玻璃”参考图贴近化改造的验收真值文档。
目标是让顶部托盘、图标按钮、时间显示、BPM 框和下拉框的玻璃 chrome 更接近用户提供的参考图，
同时把改动严格限制在 UI 外观层，不改变任何音频行为、编辑语义和数据真值线。

## Verification Levels

### L0 Spec Completeness

- 确认实现方案引用本验收文档作为唯一验证来源。
- 确认计划明确把范围限制在 `TopBarComponent`、`TransportBarComponent`、`AuroraLookAndFeel`、`UIColors`、`AuroraTheme` 这条 UI 链路。
- 确认计划明确排除处理器、RenderCache、PitchCurve、Arrangement、ARA 会话与项目状态层改动。

### L1 Static Gate

- Command: `git diff --check`
- Goal: 确认补丁没有空白字符、编码和冲突标记问题。

- Command: `rg -n "fillAuroraGlass|drawAuroraGlassFrame|drawAuroraButtonChrome|UnifiedToolbarButton|DigitalTimeDisplay|BpmValueField|drawComboBox" Source/Standalone/UI Tests`
- Goal: 审计改动是否仍然收敛在预期的顶部栏 / Aurora UI 面内，而没有扩散进业务逻辑层。

### L2 Build Gate

- Build command:
  `cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone OpenTune_VST3 OpenTuneTests"`

- Goal:
  - 证明 Aurora 顶栏 restyle 能在当前主构建链路下编译通过。
  - 同时覆盖 Standalone 与 VST3 共享 UI 面。

Expected:
- 构建成功。
- 若仅有历史警告存在，应如实报告，不可包装成“完全绿色”。

### L3 Focused Contract Guard

- Test command:
  `build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe ui`

- Supplemental command:
  `build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture`

- Goal:
  - 保持 UI 相关用例与架构用例至少不出现新的明确失败。
  - 核心确认顶部栏 restyle 没有把 UI 外观代码耦合回处理器/业务层。

Known caveat:
- 根据 `.planning/STATE.md`，`OpenTuneTests.exe ui` 当前存在“退出码可能非零但没有 `[FAIL]`”的历史 runner 缺口。若复现该现象，必须原样报告，不能把它说成完整 UI 套件通过。

### L4 Source Audit

- Audit files:
  - `Source/Standalone/UI/AuroraTheme.h`
  - `Source/Standalone/UI/ThemeTokens.h`
  - `Source/Standalone/UI/UIColors.h`
  - `Source/Standalone/UI/TopBarComponent.cpp`
  - `Source/Standalone/UI/TransportBarComponent.cpp`
  - `Source/Standalone/UI/AuroraLookAndFeel.cpp`

- Required contract points:
  - 顶部托盘主体渐变明显收敛，底部额外蓝雾被移除或显著弱化。
  - `drawAuroraButtonChrome()` 的 glow 分层不再主要依赖整轮廓平均外扩。
  - 激活态保留深色基底，不是简单整块蓝色填充。
  - 图标按钮、时间框、BPM 框、下拉框共用同一套 Aurora chrome 语言。
  - 改动没有引入第二套长期并行的顶栏/按钮绘制体系。
  - 没有把颜色常量散落成多个局部硬编码。

### L5 Visual Smoke

- Build command:
  `cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_Standalone"`

- Launch journey:
  1. 启动 `build-nonara-overlay-vs18-clean\OpenTune_artefacts\Release\Standalone\OpenTune.exe`。
  2. 切换到 Aurora / 激光玻璃主题。
  3. 打开带顶部栏的主界面，并与用户上传的参考图并排对照。
  4. 分别检查普通态、hover、激活态。
  5. 截取至少一张完整顶部栏截图用于回看。

- Visual acceptance:
  - 顶部托盘远看应是深色承托条，不应先读成一整块蓝光面板。
  - 托盘顶部边缘可见细薄冷高光，但不应有明显横向蓝雾和厚重底部雾光。
  - 按钮边框的亮感主要贴边、贴角，不能再是整圈平均糊光。
  - 按钮中心区比当前版本更沉、更暗，更像深色玻璃而不是发亮塑料。
  - 激活态显眼但克制，仍能看见深底，不应像整块按钮被蓝色油漆覆盖。
  - 图标与文字层级稍退后，不与边框和 glow 一起争抢最亮层。
  - 时间显示、BPM、下拉框与图标按钮属于同一材质家族，没有明显“按钮改了，输入框没改”的割裂。
  - 控件排布比当前版本更紧凑，整体更接近参考图那种连续控制条节奏。

### L6 Residual Review

- 复跑静态检查和必要测试后，记录仍然存在的残差：
  - 托盘是否仍偏亮
  - 边缘高光是否仍偏软
  - 激活态是否仍偏满
  - 图标是否仍偏浮
  - 布局节奏是否仍偏松
- 如果残差只剩常量级差异，应继续通过 token / alpha / 渐变停点 / spacing 微调解决；不要升级为结构性重写。

## Exit Condition

本次 pass 只有在以下条件同时满足时才算完成：
- 顶部栏 restyle 代码编译通过；
- 已知 `ui` runner 缺口被如实报告，没有夸大测试结论；
- 源码审计确认改动仍然停留在 UI 外观层；
- 视觉烟测显示顶部托盘与按钮质感明显向参考图靠拢，且时间框、BPM、下拉框与按钮形成统一的 Aurora chrome 语言。
