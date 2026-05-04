---
status: resolved
trigger: "Investigate issue: vst3-plugin-editor-isolation-regression"
created: 2026-04-15T09:16:37.1258559+08:00
updated: 2026-04-15T09:51:25.8318272+08:00
---

## Current Focus

hypothesis: 用户已在真实宿主确认修复，准备归档会话并写入知识库。
test: 将会话文件移动到 `resolved/`，并确认知识库记录已写入。
expecting: active debug 会话消失，resolved 文件与 knowledge base 条目都可读取。
next_action: archive session and verify resolved files exist

## Symptoms

expected: 和之前一致，VST3 构建应显示插件版界面（`Source/Plugin/*` 壳层），而不是 Standalone 壳层。
actual: 直接编译/打开 VST3 后显示成了 Standalone 界面。
errors: 无明确编译报错或运行时报错，属于行为回归。
reproduction: 直接编译 VST3 并加载即可复现。
started: 最近一次合并后开始出现。

## Eliminated

## Evidence

- timestamp: 2026-04-15T09:16:37.1258559+08:00
  checked: `.planning/PROJECT.md`, `.planning/REQUIREMENTS.md`, `.planning/ROADMAP.md`, `.planning/STATE.md`, `.planning/codebase/VST3Merge.md`, `.planning/codebase/STRUCTURE.md`, `.planning/codebase/ARCHITECTURE.md`
  found: 项目与架构文档一致要求 `Source/Standalone/PluginEditor.*` 和 `Source/Plugin/PluginEditor.*` 通过条件编译完全隔离；editor 选择统一经 `createOpenTuneEditor()` 进入。
  implication: 问题优先排查 editor factory 与编译宏，而不是 processor 共享核心。

- timestamp: 2026-04-15T09:16:37.1258559+08:00
  checked: `.planning/debug/knowledge-base.md`
  found: 知识库文件不存在。
  implication: 本次调查不能复用既有已知模式，需要从 live tree 建立证据。

- timestamp: 2026-04-15T09:16:37.1258559+08:00
  checked: `Source/PluginProcessor.cpp`, `Source/Editor/EditorFactoryPlugin.cpp`, `Source/Standalone/EditorFactoryStandalone.cpp`, `CMakeLists.txt`
  found: `OpenTuneAudioProcessor::createEditor()` 只调用 `createOpenTuneEditor(*this)`；两个 factory 都在主 target source 列表中，通过 `#if JucePlugin_Build_Standalone` / `#if !JucePlugin_Build_Standalone` 选择实现。
  implication: 一旦 `JucePlugin_Build_Standalone` 在共享代码层求值错误，VST3 会直接选错整个 editor 壳层。

- timestamp: 2026-04-15T09:16:37.1258559+08:00
  checked: `Source/Plugin/PluginEditor.h`, `Source/Plugin/PluginEditor.cpp`, `Source/Standalone/PluginEditor.h`, `Source/Standalone/PluginEditor.cpp`
  found: VST3 editor 类定义在 `OpenTune::PluginUI::OpenTuneAudioProcessorEditor`，Standalone editor 类定义在 `OpenTune::OpenTuneAudioProcessorEditor`，两者不是同一个类型。
  implication: 如果 VST3 实际显示 Standalone UI，说明问题更可能出在 factory 编译分支或构建目标宏，而不是运行时条件分派到同名类。

- timestamp: 2026-04-15T09:31:58.0000000+08:00
  checked: `build-debug-vs/OpenTune.vcxproj`, `build-debug-vs/OpenTune_VST3.vcxproj`
  found: `OpenTune.vcxproj`（shared code）同时编译 `Source/Standalone/EditorFactoryStandalone.cpp`、`Source/Standalone/PluginEditor.cpp`、`Source/Editor/EditorFactoryPlugin.cpp`、`Source/Plugin/PluginEditor.cpp`，并定义 `JucePlugin_Build_Standalone=1` 与 `JucePlugin_Build_VST3=1`；`OpenTune_VST3.vcxproj` 只链接 `OpenTune_SharedCode.lib`，自身不重新编译这些源。
  implication: 当前 VST3 wrapper 实际消费的是在 shared code 里被 `JucePlugin_Build_Standalone` 预处理过的结果；这正是 Standalone shell 泄漏到 VST3 的直接机制。

- timestamp: 2026-04-15T09:31:58.0000000+08:00
  checked: `Source/Editor/EditorFactoryPlugin.cpp`, `Source/Standalone/EditorFactoryStandalone.cpp`
  found: plugin factory 用 `#if !JucePlugin_Build_Standalone` 包裹，standalone factory 用 `#if JucePlugin_Build_Standalone` 包裹；在 shared code 双宏同开的前提下，plugin factory 会被排除而 standalone factory 会保留。
  implication: 根因已确认，不是“最近 merge 后偶发串线”，而是“把格式专属 editor 选择放进 shared code 并依赖多格式宏做分支”这一结构错误。

- timestamp: 2026-04-15T09:27:02.7242423+08:00
  checked: `cmake --build build-debug-vs --config Debug --target OpenTune_VST3 OpenTuneTests`
  found: `OpenTune_SharedCode.lib` 通过；`OpenTune_VST3` 开始直接编译 `Source/Editor/EditorFactoryPlugin.cpp` 与 `Source/Plugin/PluginEditor.cpp` 后，首先命中大量 `C4819` 编码 warning，随后在 `Source/Utils/Note.h` / `Source/Utils/PitchUtils.h` / `Source/DSP/ScaleInference.h` 爆出语法错误。
  implication: wrapper target 现在已正确接手 VST3 editor 源，但还缺 shared code 上已有的 `/utf-8` 编译选项；这是修复链路上的 build config 缺口，不是新的 editor 归属根因。

- timestamp: 2026-04-15T09:28:58.4290411+08:00
  checked: `cmake --build build-debug-vs --config Debug --target OpenTune_VST3 OpenTuneTests`, `cmake --build build-debug-vs --config Debug --target OpenTune_Standalone`, `Source/Standalone/PluginEditor.cpp`, `Source/Standalone/PluginEditor.h`
  found: 补齐 `/utf-8 /bigobj` 后，`OpenTune_VST3` 与 `OpenTuneTests` 均通过；`OpenTune_Standalone` 继续失败，但失败点已收敛到 `Source/Standalone/PluginEditor.cpp:1021` 使用未声明成员 `diagnosticHeartbeatCounter_`，全仓仅此两处引用。
  implication: VST3 修复链路已闭合；剩余 Standalone 编译失败是原文件内真实缺失声明，修复它即可完成本次双壳层验证。

- timestamp: 2026-04-15T09:30:02.9467331+08:00
  checked: `build-debug-vs/OpenTune.vcxproj`, `build-debug-vs/OpenTune_VST3.vcxproj`, `build-debug-vs/OpenTune_Standalone.vcxproj`, `ctest --test-dir build-debug-vs -C Debug --output-on-failure -R OpenTuneCoreTests`, `cmake --build build-debug-vs --config Debug --target OpenTune_VST3 OpenTuneTests`, `cmake --build build-debug-vs --config Debug --target OpenTune_Standalone`
  found: `OpenTune.vcxproj` 已不再编译任何 format-specific editor/factory；`OpenTune_VST3.vcxproj` 直接编译 plugin editor/factory 且带 `JucePlugin_Build_Standalone=0`、`JucePlugin_Build_VST3=1`；`OpenTune_Standalone.vcxproj` 只编译 standalone editor/factory 且带 `JucePlugin_Build_Standalone=1`、`JucePlugin_Build_VST3=0`；`OpenTune_VST3`、`OpenTune_Standalone` 均编译通过，`OpenTuneCoreTests` 通过。
  implication: 编译期 editor 归属与 wrapper 隔离已经恢复，当前只差真实宿主中的人工确认。

- timestamp: 2026-04-15T09:51:25.8318272+08:00
  checked: `cmake -S . -B build-debug-vs-fresh -G "Visual Studio 18 2026" -A x64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, `cmake --build build-debug-vs-fresh --config Debug --target OpenTune_VST3 OpenTune_Standalone OpenTuneTests`, `ctest --test-dir build-debug-vs-fresh -C Debug --output-on-failure -R OpenTuneCoreTests`, `Resolve-Path` on fresh artifact outputs
  found: fresh 配置与 fresh Debug 构建成功；以下产物已真实存在：`build-debug-vs-fresh\OpenTune_artefacts\Debug\VST3\OpenTune.vst3`、`build-debug-vs-fresh\OpenTune_artefacts\Debug\VST3\OpenTune.vst3\Contents\x86_64-win\OpenTune.vst3`、`build-debug-vs-fresh\OpenTune_artefacts\Debug\Standalone\OpenTune.exe`；fresh `OpenTuneCoreTests` 也通过。
  implication: 用户现在可以直接使用 fresh build 目录中的真实产物继续做宿主验证；之前看不到文件不是因为本次修复未生成产物，而是因为当时引用的是旧 build 目录路径而非 fresh 明确核实后的路径。

- timestamp: 2026-04-15T09:51:25.8318272+08:00
  checked: 用户 human-verify checkpoint response
  found: 用户已在真实宿主中确认 `confirmed fixed`，VST3 界面恢复为插件版壳层。
  implication: 问题已完成端到端验证，可以关闭并归档本次 debug 会话。

## Resolution

root_cause:
  JUCE 生成的 `OpenTune_SharedCode` 同时定义了 `JucePlugin_Build_Standalone=1` 与 `JucePlugin_Build_VST3=1`。项目把 `createOpenTuneEditor()` 和两套 editor 源都放进 shared code，并用 `#if JucePlugin_Build_Standalone` / `#if !JucePlugin_Build_Standalone` 选壳层，结果 shared code 永远保留 Standalone factory、排除 plugin factory；VST3 wrapper 只链接这个 shared lib，所以最终加载成了 Standalone UI。
fix:
  把 `Source/Standalone/PluginEditor.*` + `Source/Standalone/EditorFactoryStandalone.cpp` 移到 `OpenTune_Standalone` target，把 `Source/Plugin/PluginEditor.*` + `Source/Editor/EditorFactoryPlugin.cpp` 移到 `OpenTune_VST3` target，并给两个 wrapper target 注入各自正确的 `JucePlugin_Build_Standalone` / `JucePlugin_Build_VST3` 宏与 `/utf-8 /bigobj` 编译选项；同时修正 plugin factory 对 `PluginUI::OpenTuneAudioProcessorEditor` 的实例化命名空间。为保持 `OpenTuneTests` 链接 shared processor，新增 test-only `createOpenTuneEditor` stub；另补回 Standalone debug 诊断路径缺失的 `diagnosticHeartbeatCounter_` 声明，保证 Standalone wrapper 重新单独编译通过。
verification:
  重新配置 `build-debug-vs` 后，`OpenTune.vcxproj` 不再包含任何 format-specific editor/factory 源；`OpenTune_VST3.vcxproj` 与 `OpenTune_Standalone.vcxproj` 分别只编译各自 editor 壳层且带互斥格式宏。随后在全新的 `build-debug-vs-fresh` 目录完成 fresh configure、fresh Debug build（`OpenTune_VST3` / `OpenTune_Standalone` / `OpenTuneTests`）以及 fresh `OpenTuneCoreTests`；并用 `Resolve-Path` 实际确认 VST3 bundle、VST3 binary 和 Standalone exe 均存在。
files_changed: [CMakeLists.txt, Source/Editor/EditorFactoryPlugin.cpp, Source/Standalone/PluginEditor.h, Tests/TestEditorFactoryStub.cpp]
