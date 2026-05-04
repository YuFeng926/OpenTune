---
status: investigating
trigger: "/gsd-debug 修复 REAPER cut 后钢琴卷帘默认对焦到累积时长而非当前 DAW 真实时间轴位置的 UI 时间基线 bug；修复后清理与当前项目实现相悖的旧代码、旧结构、冗余结构、残留代码；严格只修改 VST3 侧，保持 Standalone 不受影响"
created: 2026-04-13T00:00:00Z
updated: 2026-04-13T00:35:00Z
---

## Current Focus

hypothesis: 根因已收敛：VST3/ARA 把 clip 的宿主绝对起点写入 `trackOffsetSeconds_`，但 `PianoRollComponent` 的默认聚焦/自动滚动没有和 overlay / xToTime / timeToX 使用同一时间基线；cut 后 offset 变大时，视口就按旧的绝对/累积基线滚动
test: 为 VST3 编译分支补一个时间基线一致性测试，然后修正聚焦/自动滚动只走“playhead 与绘制共用同一换算”
expecting: 修复后 cut 后的默认聚焦与播放跟随都以 `absolutePlayhead - trackOffsetSeconds_ (+ alignmentOffsetSeconds_)` 为同一基线，Standalone 分支行为保持不变
next_action: 先补失败测试，再实现 VST3 分支下的统一换算与旧逻辑清理

## Symptoms

expected: REAPER cut 后，钢琴卷帘默认对焦到当前 DAW 真实时间轴位置
actual: REAPER cut 后，钢琴卷帘默认对焦到累积时长位置，而不是当前 DAW 真实时间轴位置
errors: 
reproduction: 在 REAPER 中对当前 ARA/VST3 clip 执行 cut 后打开/观察钢琴卷帘默认对焦位置
started: Phase 11 L5 人工验证时发现（2026-04-13）

## Eliminated

## Evidence

- timestamp: 2026-04-13T00:00:00Z
  checked: .planning/STATE.md 与路线图
  found: 当前里程碑阻塞点就是 REAPER cut 后卷帘时间基线错误；约束要求严格只改 VST3 侧且 Standalone 不受影响
  implication: 调试范围应锁定在 VST3/ARA 时间映射与 VST3 编辑器/UI 聚焦链路，不能改 Standalone 语义

- timestamp: 2026-04-13T00:20:00Z
  checked: Source/Plugin/PluginEditor.cpp:1678-1725, Source/PluginProcessor.cpp:2060-2109, Source/ARA/OpenTuneDocumentController.cpp:413-436
  found: `syncClipToPianoRoll()` 会把 `processorRef_.getPluginClipStartSeconds(clipId)` 写进 `pianoRoll_.setTrackTimeOffset(...)`；而 `getPluginClipStartSeconds()` 最终就是 clip 的 `startSeconds`；VST3/ARA 路径里这个值来自 `playbackRegion->getStartInPlaybackTime()`
  implication: cut / mapping 后，钢琴卷帘收到的是宿主真实时间轴上的绝对起点，不是 clip 局部时间

- timestamp: 2026-04-13T00:24:00Z
  checked: Source/Standalone/UI/PlayheadOverlayComponent.cpp:63-77, Source/Standalone/UI/PianoRollComponent.cpp:1829-1835
  found: overlay 与坐标换算都使用 `seconds - trackOffsetSeconds_ + alignmentOffsetSeconds_`
  implication: 播放头绘制、命中与尺子坐标的正确基线已经定义出来了：必须先减 `trackOffsetSeconds_`

- timestamp: 2026-04-13T00:27:00Z
  checked: Source/Standalone/UI/PianoRollComponent.cpp:1262-1287, 1353-1418
  found: `updateAutoScroll()` 与 `onScrollVBlankCallback()` 直接用 `playheadTime * pixelsPerSecond` 计算 `playheadVisualX/targetScroll`，没有减 `trackOffsetSeconds_`，也没有复用 overlay/timeToX 的同一换算
  implication: 播放跟随基线与绘制基线分裂；offset 越大，窗口越会向累积绝对时间漂移

- timestamp: 2026-04-13T00:31:00Z
  checked: Source/Standalone/UI/PianoRollComponent.cpp:1733-1795
  found: `setHasUserAudio(true)` 每次都会触发 `fitToScreen()`；而 `fitToScreen()` 在有音频时会把 `scrollOffset` 直接设为 `trackOffsetSeconds_ * 100 * zoomLevel_`
  implication: 默认对焦路径也混入了旧基线；sync 后可能在播放开始前就把视口推到错误位置

## Resolution

root_cause: `PianoRollComponent` 在 VST3/ARA 路径下同时承载“宿主绝对时间”和“clip 视口时间”，但默认对焦与自动滚动没有复用 overlay / xToTime / timeToX 已定义的同一换算；`trackOffsetSeconds_` 已被同步成宿主绝对起点后，滚动仍按绝对 `playheadTime` 与旧 scroll 公式前进，导致 REAPER cut 后视口对焦到累积时间
fix: 待实现——把 VST3 编译分支的默认聚焦/自动滚动统一到同一时间基线，并清理与当前实现相悖的旧 scroll 公式与冗余结构
verification: 待补——先做失败测试，再跑 OpenTuneTests、OpenTune_VST3、OpenTune_Standalone，并保留 L5 手工验证给用户确认
files_changed: []
