# Testing Patterns

**Analysis Date:** 2026-04-20

## Test Framework

**Runner:**
- 唯一可执行自动化测试目标仍是 `OpenTuneTests`，由 `option(OPENTUNE_BUILD_TESTS "Build unit tests" ON)` 和 `add_executable(OpenTuneTests ...)` 声明于 `CMakeLists.txt:860` 和 `CMakeLists.txt:863`。
- CTest 只注册了一个入口 `OpenTuneCoreTests`，但命令实际运行整个 `OpenTuneTests` 可执行文件，而不是只跑 `core` suite，见 `CMakeLists.txt:961` and `CMakeLists.txt:963`。
- 断言方式仍是手写 `logFail(...)` / `logPass(...)` 加全局失败标记 `gHasTestFailure`，入口在 `Tests/TestMain.cpp:2624`, `Tests/TestMain.cpp:2626`, `Tests/TestMain.cpp:2631`。

## Executable Suites

**Suite registry:**
- 当前 suite 固定为 4 个：`core`, `processor`, `ui`, `architecture`，定义在 `Tests/TestMain.cpp:30`。
- 子集运行与 suite 列表 CLI 仍可用：`--list-suites` in `Tests/TestMain.cpp:2858`，suite 分发在 `Tests/TestMain.cpp:2863`。

**What each suite currently does:**
- `core`: 目前只打印 section banner，没有实际测试体，见 `Tests/TestMain.cpp:4118`。
- `processor`: 当前只跑一条 `runClipDerivedRefreshDoesNotMutateStandaloneSelectionTest()`，见 `Tests/TestMain.cpp:4123`。
- `ui`: 覆盖 app preferences round-trip、shared/standalone preferences page 组合、editing scheme/parameter sync、PianoRoll source guards、visual invalidation、audio format registry、Standalone import、undo result-chain，以及新增的 `PianoRollProjection_ConsumesMaterializationIdAndPlacementProjectionOnly` / `EditingCommand_DoesNotMutatePlacement` owner guards，入口列表见 `Tests/TestMain.cpp:4131`。
- `architecture`: 覆盖 placement/materialization owner 约束、Standalone playback placement window、ARA snapshot/render span、mac Standalone packaging 结构守护、undo architecture cleanup guard，以及新增的 `MaterializationCommands_DoNotMutateTimelinePlacementTruth` / `PlacementCommands_DoNotMutateClipCoreTruth` / `AraSession_SnapshotExposesSourceMaterializationAndPlacementOwnership` / `ProcessorModel_RejectsMixedClipOwnerApis`，入口列表见 `Tests/TestMain.cpp:4185`。

## Commands In Use Now

**Build the test binary:**
```powershell
cmake --build build-debug --target OpenTuneTests --config Debug
```
- `OpenTuneTests` 是显式 CMake target，见 `CMakeLists.txt:863`。

**Run all suites:**
```powershell
build-debug\Debug\OpenTuneTests.exe
```

**Run one suite or list suites:**
```powershell
build-debug\Debug\OpenTuneTests.exe --list-suites
build-debug\Debug\OpenTuneTests.exe ui
build-debug\Debug\OpenTuneTests.exe architecture
```
- CLI 分支在 `Tests/TestMain.cpp:2856`。

**Run through CTest:**
```powershell
ctest --test-dir build-debug -C Debug --output-on-failure
```

**Windows configure/build helper:**
```powershell
pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-debug -Target OpenTuneTests -Config Debug
```
- helper 脚本参数和默认值在 `.planning/scripts/invoke-msvc-cmake.ps1:1`，其内部会先 configure 再 build，见 `.planning/scripts/invoke-msvc-cmake.ps1:43`。

**Format build verification also matters:**
```powershell
cmake --build build-debug --target OpenTune_Standalone --config Debug
cmake --build build-debug --target OpenTune_VST3 --config Debug
```
- 两个 target 都在 `CMakeLists.txt` 中显式存在，见 `CMakeLists.txt:504` and `CMakeLists.txt:518`。

## Test File Organization

- 测试仍集中在 `Tests/`，当前 live files 是 `Tests/TestMain.cpp`, `Tests/TestSupport.h`, `Tests/TestSupport.cpp`, `Tests/TestEditorFactoryStub.cpp`。
- `Tests/TestMain.cpp` 仍是单文件主测试体和 suite registry，长度 2881 行，见 `Tests/TestMain.cpp:2881`。
- `Tests/TestSupport.h` / `Tests/TestSupport.cpp` 提供 probe、mock、共享 helper，例如 `VST3AraSessionTestProbe` in `Tests/TestSupport.h:55` 和 `MockVocoderService` in `Tests/TestSupport.h:118`。
- `Tests/TestEditorFactoryStub.cpp` 继续用于阻断真实 editor 创建，这个角色在旧文档中存在，live tree 仍保留该文件。

## Test Patterns Present Now

**Direct state assertions:**
- 测试更倾向直接实例化真实共享对象并断言状态，而不是经由 UI 黑盒观察，例如 `SourceStore`, `MaterializationStore`, `StandaloneArrangement`, `VST3AraSession`, `OpenTuneAudioProcessor` 均被 `Tests/TestSupport.h:10`-`Tests/TestSupport.h:24` 直接引入。

**Source-inspection guards:**
- 当前 repo 继续用 `readWorkspaceFile()`, `extractWorkspaceFileSection()`, `sourceContains()`, `workspaceFileExists()` 直接读 live 源码做结构守护，定义在 `Tests/TestMain.cpp:153`, `Tests/TestMain.cpp:168`, `Tests/TestMain.cpp:186`, `Tests/TestMain.cpp:191`。
- 这类 guard 仍在验证 preferences 组合、scheme manager 清理、mac packaging owner 边界等，例如 `runMacStandalonePackagingPlistMergeBelongsToStandaloneTargetOnlyTest()` in `Tests/TestMain.cpp:2700` and `runAudioEditingSchemeRulesUseExplicitSchemeInputTest()` referenced from `Tests/TestMain.cpp:2793`。

**Manual fake / probe, not external mocking library:**
- `MockVocoderService` 仍用手写 fake 记录并发与失败路径，定义在 `Tests/TestSupport.h:118`，实现于 `Tests/TestSupport.cpp:5`。
- 私有 seam probe 继续通过 `OPENTUNE_TEST_BUILD` 暴露，例如 `VST3AraSessionTestProbe` in `Tests/TestSupport.h:54`。

## Coverage Signals

- 仓库里仍没有 gcov/lcov/llvm-cov 之类覆盖率配置；当前 coverage signal 主要来自 smoke suites + architecture guards，而不是覆盖率百分比。
- `ui` 和 `architecture` 是当前高信号 suite；`core` 为空、`processor` 很薄，因此不能把现状描述成均衡单元测试覆盖，证据见 `Tests/TestMain.cpp:2766` and `Tests/TestMain.cpp:2771`。
- 2026-04-21 planning 明确记录了当前自动化验证现实：`OpenTuneTests.exe ui` PASS、`OpenTuneTests.exe architecture` PASS、全量 `OpenTuneTests.exe` PASS、`ctest` PASS、`OpenTune_Standalone` / `OpenTune_VST3` build PASS，见 `.planning/PROJECT.md`, `.planning/ROADMAP.md`, `.planning/STATE.md`。

## Manual Validation Expectations

- repo 当前仍把 manual DAW journey + `AppLogger` trace 当作 host-specific verification layer，见 `.planning/STATE.md:25` and `.planning/ROADMAP.md:54`。
- 与 VST3/ARA 相关的问题，现行文档仍要求在真实 DAW 中复现并检查日志，REAPER 调试样例可见 `.planning/debug/reaper-vst3-record-originalf0-missing.md:10` and `.planning/debug/reaper-vst3-record-originalf0-missing.md:63`。
- 生产代码里的 trace family 仍是手工验证的重要抓手，例如 `RecordTrace`, `MappingTrace`, `AutoTuneTrace`, `RenderTrace` 分别位于 `Source/Plugin/PluginEditor.cpp:999`, `Source/Plugin/PluginEditor.cpp:1106`, `Source/Plugin/PluginEditor.cpp:1258`, `Source/Standalone/PluginEditor.cpp:2597`。

## Explicit Gaps Verified From Repo Files

- `core` suite 目前没有任何真实测试，只是 banner，见 `Tests/TestMain.cpp:2766`。
- `processor` suite 目前只有一条测试，见 `Tests/TestMain.cpp:2771`。
- 没有 repo-level CI pipeline：工作区下未检测到 `.github/`，根目录也未检测到仓库级 `*.yml` / `*.yaml` CI 配置；自动化门禁仍依赖本地 CMake/CTest 流程。
- L5 Standalone / VST3 手工旅程仍是显式 deferred gap，不是 PASS，见 `.planning/ROADMAP.md:53`, `.planning/ROADMAP.md:56`, `.planning/STATE.md:71`。
- macOS 真实 `.app` bundle inspection 仍待 macOS 环境执行，见 `.planning/REQUIREMENTS.md:25`, `.planning/STATE.md:80`。
- 由于 CTest 名称仍叫 `OpenTuneCoreTests`，但实际跑的是整包 `OpenTuneTests`，自动化入口名称与真实范围不一致，见 `CMakeLists.txt:963`。

## Practical Guidance For New Work

- 需要高信号回归守护时，优先往 `ui` 或 `architecture` 增加 focused smoke test；不要把当前 `core` / `processor` 现状误当成充分覆盖。
- 需要守护结构清理时，继续使用 workspace source-inspection pattern，参考 `Tests/TestMain.cpp:2704`。
- 遇到 host-only 或 ARA-only 回归时，继续组合使用：`OpenTuneTests` 局部 smoke、格式 build、真实 DAW 手工旅程、`AppLogger` trace。
- 写测试时继续优先断言显式 state carrier 和 published snapshot，不要引入新的隐藏 manager mock 层。

---

*Testing analysis: 2026-04-20*
