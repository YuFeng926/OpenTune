# Aurora Right Sidebar Background Reference Restyle Test Verification

**Scope:** `.planning/plans/2026-05-26-aurora-right-sidebar-background-reference-restyle.md`
**Goal:** 只验证 Aurora 右侧参数栏背景壳层是否按计划收敛；不把旋钮、工具按钮、布局或其他主题面板的外观波动混入这次结论。

---

## Verification Layers

## 1. Source Guard

**Intent:** 先确认实现边界没有跑偏。

**Checks:**
- `ParameterPanel::paint()` 在 `ThemeId::Aurora` 下改为调用右侧栏专用 helper，而不是继续走通用 `fillPanelBackground()` / `drawPanelFrame()`。
- `UIColors.h` 中新增了 Aurora 右侧栏专用 helper，且命名能明确区分 tray 与 sidebar。
- `AuroraTheme.h` / `ThemeTokens.h` 中存在右侧栏专用语义 token，而不是复用顶栏 `Tray*`。
- `drawAuroraButtonChrome()` 与 `drawAuroraKnob()` 没有因为本次任务被顺手重做。

**Suggested Gate:**
- 若实现方式允许，可在 `Tests/TestMain.cpp` 加一个轻量 source guard，检查：
  - `ParameterPanel.cpp` 包含右侧栏 helper 名称。
  - `UIColors.h` 包含右侧栏 helper / token 名称。
  - `drawAuroraButtonChrome` / `drawAuroraKnob` 相关关键签名仍存在且未被右侧栏分支接管。

## 2. Build Gate

**Intent:** 确认这次仅 UI 壳层改动不会破坏可编译性。

**Commands:**
```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone OpenTuneTests"
```

**Pass Criteria:**
- `OpenTune_Standalone` 编译通过。
- `OpenTuneTests` 编译通过。

## 3. Focused Test Gate

**Intent:** 如果增加了 source guard，用最小测试集守住合同。

**Commands:**
```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& .\\build-ara-overlay-vs18-clean\\OpenTune_artefacts\\Release\\OpenTuneTests.exe ui"
```

**Pass Criteria:**
- 新增的右侧栏背景 source guard 通过。
- 如果 `ui` 套件仍有既有失败，必须在结论里明确标注为既有问题，不得伪装成这次改坏。

## 4. Manual Visual Smoke

**Intent:** 这是本次最重要的验收层，因为目标是“贴近参考图的视觉关系”。

**Steps:**
1. 启动 Standalone。
2. 切到 Aurora / 激光玻璃主题。
3. 打开包含右侧参数栏的主界面，使用和用户截图接近的窗口尺寸观察。
4. 只看右侧栏背景壳层，不把旋钮和按钮本体当成这次判断对象。

**Checklist:**
- 整个右侧栏是否首先读成“深色竖向玻璃舱体”而不是“拉高的顶栏托盘”。
- 主体渐变是否更稳，中心是否有深度，而不是整片泛蓝或整片灰。
- 左右边缘是否出现“若隐若现”的细腻冷光，而不是整圈平均发光。
- 上沿高光是否足够薄、足够克制。
- 下部是否有轻微收口与体积感，不再显得平。
- 外框是否分出结构边和内唇边，且两者都不过分抢眼。
- 旋钮与工具按钮视觉权重是否仍高于壳层背景。

## 5. Non-Goals Confirmation

**Intent:** 确认这轮没有越界。

**Checks:**
- 旋钮外观未变化。
- 工具按钮 idle / hover / active 外观未变化。
- 右侧栏内部布局、尺寸、按钮排列未变化。
- 左侧栏、顶部栏及其他 Aurora 面板未被这次实现牵连。

## Reporting Rules

- 若通过编译但视觉仍明显不像参考图，只能报告“实现完成但视觉未达标”，不能算完成。
- 若视觉已接近参考图，但为了达成它顺手改了旋钮或按钮，也不能算通过，必须先回到范围边界内。
- 若 `ui` 测试存在既有失败，报告时必须写明具体失败名与本次任务无关。

