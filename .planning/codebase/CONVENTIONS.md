# Coding Conventions

**Analysis Date:** 2026-05-05

## Naming Patterns

**Files:**
- 生产代码继续以 `PascalCase` 文件名为主，头源同名成对出现，例如 `Source/SourceStore.h`, `Source/SourceStore.cpp`, `Source/MaterializationStore.h`, `Source/MaterializationStore.cpp`, `Source/StandaloneArrangement.h`, `Source/StandaloneArrangement.cpp`。
- 格式边界主要靠目录表达，而不是靠文件名后缀：Standalone UI 在 `Source/Standalone/...`，VST3 UI 在 `Source/Plugin/...`，ARA 适配在 `Source/ARA/...`，共享状态与 helper 在 `Source/Utils/...`。
- UI 组件和 helper 常用语义后缀：`*Component`, `*Renderer`, `*Worker`, `*Support`, `*LookAndFeel`，例如 `Source/Standalone/UI/PianoRollComponent.h`, `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h`, `Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.h`。

**Types and enums:**
- 类、结构体、枚举使用 `PascalCase`，例如 `AppPreferencesState` in `Source/Utils/AppPreferences.h:32`, `CommittedPlacement` in `Source/PluginProcessor.h:164`, `UndoExecutionResult` in `Source/Utils/UndoAction.h:68`。
- 2026-04-21 的 owner-model 澄清后，新的 persisted owner 命名目标应转向 `sourceId` / `materializationId` / `placementId`；当前 live tree 的 `contentId` 更接近“provisional materialization id”，不应再被当成最终共享内容 owner 名称。
- live tree 里仍有少量局部变量沿用旧 `clipId` 或 `contentId` 命名，说明内部清理仍在进行中，例如 `clipId` in `Source/Plugin/PluginEditor.cpp:1056` 和 `selectedClipId_` in `Source/Standalone/UI/ArrangementViewComponent.h:180`。

**Functions and variables:**
- 方法、自由函数、局部变量、成员函数统一用 `camelCase`，例如 `resolveCurrentContentProjection()` in `Source/Plugin/PluginEditor.h:98`, `publishPlaybackSnapshotLocked()` in `Source/StandaloneArrangement.h:121`, `trimFrameRangeToEditableBounds()` in `Source/Utils/AudioEditingScheme.h:106`。
- 布尔辅助函数沿用 `is*` / `has*` / `can*` / `should*` 前缀，例如 `isValid()` in `Source/StandaloneArrangement.h:31`, `canEditFrame()` in `Source/Utils/AudioEditingScheme.h:97`, `shouldSelectNotesForEditedFrameRange()` in `Source/Utils/AudioEditingScheme.h:128`。
- 长生命周期成员常用尾部下划线，例子包括 `storageOptions_` in `Source/Utils/AppPreferences.h:65`, `stateMutex_` in `Source/ARA/VST3AraSession.h:287`, `nextPlacementId_` in `Source/StandaloneArrangement.h:127`。
- 常量命名混用 `k*` 与全大写 static constexpr：`kTrackCount` in `Source/StandaloneArrangement.h:16`, `kHeartbeatHz` in `Source/Plugin/PluginEditor.h:143`, `MAX_TRACKS` in `Source/PluginProcessor.h:100`。

## Code Style

**Formatting:**
- 仓库根目录未检测到 `.editorconfig`, `.clang-format`, ESLint, Prettier 或 Biome 配置；当前风格主要靠现有源码维持一致。
- 头文件统一使用 `#pragma once`，例如 `Source/Utils/AppPreferences.h:1`, `Source/SourceStore.h:1`, `Tests/TestSupport.h:1`。
- 当前 C++ 代码普遍使用四空格缩进、Allman 风格大括号和较宽松的空行分段，例子见 `Source/Utils/AudioEditingScheme.h:8`, `Source/Utils/UndoAction.h:19`, `Source/ARA/VST3AraSession.h:23`。

**Includes:**
- 常见顺序是：JUCE / 标准库 / 项目头；例如 `Source/PluginProcessor.h:19`, `Source/PluginProcessor.h:20`, `Source/PluginProcessor.h:30`。
- 不使用路径别名或 barrel header；包含语句直接写相对 source-root 路径，例如 `#include "Utils/AppPreferences.h"` in `Source/Plugin/PluginEditor.h:15` 和 `#include "PluginProcessor.h"` in `Source/Standalone/PluginEditor.h:19`。

## Architectural Idioms

**显式状态载体，而不是隐藏 manager：**
- 当前 repo 用小型 state struct 和显式输入规则承载 UI / 编辑策略，例如 `SharedPreferencesState` in `Source/Utils/AppPreferences.h:18`, `ParameterPanelSyncContext` in `Source/Utils/ParameterPanelSync.h:7`, `AutoTuneTargetContext` in `Source/Utils/AudioEditingScheme.h:57`。
- `AudioEditingScheme` 和 `ParameterPanelSync` 继续以纯 helper 决策为主，不依赖隐藏 scheme manager，相关入口在 `Source/Utils/AudioEditingScheme.h:72` 和 `Source/Utils/ParameterPanelSync.h:36`。

**双格式边界靠目录和条件编译隔离：**
- VST3 editor 明确包在 `#if JucePlugin_Build_VST3` 下，见 `Source/Plugin/PluginEditor.h:3`；Standalone editor 独立存在于 `Source/Standalone/PluginEditor.h:40`。
- 共享 runtime 继续围绕 `SourceStore`, `MaterializationStore`, `StandaloneArrangement`, `VST3AraSession` 展开，入口可见 `Source/PluginProcessor.h:30`, `Source/PluginProcessor.h:31`, `Source/PluginProcessor.h:60`。

**决策 helper 小而纯，UI 协调函数可大：**
- 规则 helper 常直接写成 header inline 函数，例如 `resolveParameterTarget()` in `Source/Utils/AudioEditingScheme.h:138` 和 `resolveParameterPanelSyncDecision()` in `Source/Utils/ParameterPanelSync.h:36`。
- UI 壳层函数允许承担较大的 orchestration 逻辑，例如导入流程与异步弹窗在 `Source/Standalone/PluginEditor.cpp:1334` 和 ARA 同步流程在 `Source/Plugin/PluginEditor.cpp:1014`。

## Error Handling

**Patterns present now:**
- 领域失败继续使用 `OpenTune::Result<T>` / `OpenTune::Error`，定义在 `Source/Utils/Error.h:79` 和 `Source/Utils/Error.h:102`；测试 fake 也沿用同一抽象，见 `Tests/TestSupport.cpp:17`。
- 许多 API 仍以 `bool` 返回成功与否，并配合 early return，而不是再套一层兼容结构，例如 `commitPreparedImportAsPlacement()` usage in `Source/Standalone/PluginEditor.cpp:1564` and `replaceMaterializationAudioById()` guard in `Source/Plugin/PluginEditor.cpp:1149`。
- debug 期用 `jassert` / `jassertfalse` 抓程序员错误，例如 `jassertfalse` in `Source/Standalone/PluginEditor.cpp:448`, `jassert` in `Source/ARA/OpenTunePlaybackRenderer.cpp:207`, `jassertfalse` (e.g. in the piano-roll editing layer)。
- 用户可恢复分支通常直接提示并返回，不走兜底链路，例如 `AlertWindow::showMessageBoxAsync(...)` + `return` in `Source/Plugin/PluginEditor.cpp:939` and `Source/Plugin/PluginEditor.cpp:963`。

## Logging

**Framework:** `AppLogger`

- 仓库内仍有统一日志入口 `AppLogger`，接口在 `Source/Utils/AppLogger.h:21`。
- 实际日志字符串习惯带语义前缀，便于手工验证时按 trace family 检索，例如 `RecordTrace` in `Source/Plugin/PluginEditor.cpp:999`, `MappingTrace` in `Source/Plugin/PluginEditor.cpp:1106`, `AutoTuneTrace` in `Source/Plugin/PluginEditor.cpp:1258`, `RenderTrace` in `Source/Standalone/PluginEditor.cpp:2597`。
- 调试消息和结构检查失败也会直接记日志，例如 `AppLogger::log("Debug self-tests failed")` in `Source/Standalone/PluginEditor.cpp:447`。

## Threading And Lifetime

**Shared-state locking:**
- ARA session 用 `std::mutex` + 短临界区保护可变状态，字段在 `Source/ARA/VST3AraSession.h:287`，调用点密集分布在 `Source/ARA/VST3AraSession.cpp:135`, `Source/ARA/VST3AraSession.cpp:236`, `Source/ARA/VST3AraSession.cpp:707`。
- Standalone arrangement 用 `juce::ReadWriteLock` 保护轨道 / placement 状态，并用 `juce::SpinLock` 保护已发布 playback snapshot，见 `Source/StandaloneArrangement.h:123` and `Source/StandaloneArrangement.h:129`。

**Atomic-first read paths:**
- 音频与播放协调大量使用 `std::atomic`，例如 `currentSampleRate_`, `isPlaying_`, `hostBpm_` in `Source/PluginProcessor.h:216`, `Source/PluginProcessor.h:346`, `Source/PluginProcessor.h:364`。
- UI 组件也直接持有原子读侧状态，例如 `isPlaying_` in `Source/Standalone/UI/ArrangementViewComponent.h:159` and `Source/Standalone/UI/PianoRollComponent.h:377`。
- ARA published snapshot 通过原子 load/store 读写，见 `Source/ARA/VST3AraSession.cpp:231` and `Source/ARA/VST3AraSession.cpp:613`。

**Async UI lifetime:**
- 异步 UI 回调显式捕获 `juce::Component::SafePointer`，而不是裸 `this`，例如 `Source/Standalone/PluginEditor.cpp:568`, `Source/Standalone/PluginEditor.cpp:1429`, `Source/Standalone/PluginEditor.cpp:1491`, `Source/Standalone/UI/PianoRollComponent.cpp:2394`。

**Worker ownership:**
- 仓库中仍有显式后台线程/worker 成员，如 `hydrationWorkerThread_` in `Source/ARA/VST3AraSession.h:299`, `exportWorker_` in `Source/Standalone/PluginEditor.h:239`, `backgroundTasks_` in `Source/Standalone/PluginEditor.h:243`。

## Comments

**Observed style:**
- 注释以中文为主，常用块注释说明类职责或线程/架构边界，例如 `Source/PluginProcessor.h:3`, `Source/Standalone/PluginEditor.h:3`, `Source/Standalone/UI/ArrangementViewComponent.h:3`。
- 内联注释主要解释特定 UI 分支或状态字段语义，而不是逐行翻译代码，例如 `Source/Standalone/PluginEditor.cpp:1333`, `Source/Standalone/UI/ArrangementViewComponent.h:159`, `Source/PluginProcessor.h:137`。
- 共享 helper 头通常少注释、更多依靠命名自解释；`Source/Utils/AudioEditingScheme.h` 和 `Source/Utils/ParameterPanelSync.h` 是当前典型。

## Practical Rules For New Work

- 新的 shared policy / state helper 放在 `Source/Utils/...`，优先延续显式输入 + 小型返回结构的写法，参考 `Source/Utils/AudioEditingScheme.h:41` 和 `Source/Utils/ParameterPanelSync.h:23`。
- 涉及 persisted truth 的新公共接口优先使用 `sourceId` / `materializationId` / `placementId`；在源码彻底迁完之前，若暂时仍出现 `contentId`，也应把它视为待替换的 materialization-facing 过渡命名，而不是继续扩大 mixed `clipId` 语义。
- 新的异步 UI 行为继续使用 `SafePointer`，不要捕获裸 editor/component 指针，参考 `Source/Standalone/PluginEditor.cpp:1349`。
- 新的跨线程读路径优先走 snapshot / atomic 已发布状态，不要让音频线程直接碰可变 owner，参考 `Source/ARA/VST3AraSession.cpp:231` 和 `Source/StandaloneArrangement.h:71`。
- 新的格式专属 UI 继续放在 `Source/Standalone/...` 或 `Source/Plugin/...`，不要在共享 UI 层引入运行时兜底分支。

---

*Convention analysis: 2026-05-05*
