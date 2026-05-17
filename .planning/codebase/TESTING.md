# Testing Patterns

**Analysis Date:** 2026-05-15

## Test Framework

**Runner:**
- 唯一可执行自动化测试目标仍是 `OpenTuneTests`，由 `option(OPENTUNE_BUILD_TESTS "Build unit tests" ON)` 和 `add_executable(OpenTuneTests ...)` 声明于 `CMakeLists.txt:1072`。
- CTest 只注册了一个入口 `OpenTuneCoreTests`，但命令实际运行整个 `OpenTuneTests` 可执行文件，而不是只跑 `core` suite，见 `CMakeLists.txt:1179`。
- 断言方式仍是手写 `logFail(...)` / `logPass(...)` 加全局失败标记 `gHasTestFailure`。

## Executable Suites

**Suite registry:**
- 当前 suite 固定为 6 个：`core`, `processor`, `ui`, `architecture`, `undo`, `memory`，定义在 `Tests/TestMain.cpp:30`。
- 子集运行与 suite 列表 CLI 仍可用：`--list-suites` 与单 suite 参数分发在 `Tests/TestMain.cpp` 的 `main()`。

**What each suite currently does:**
- `core`: 覆盖 lock-free queue、renderer block span、SIMD helper、channel layout/capture segment numeric guards。
- `processor`: 覆盖 forced source owner、import source/materialization/placement 分离、derived refresh selection guard、channel layout import guard、capture persistence/state persistence、PianoRoll edit action affected-range。
- `ui`: 覆盖 app preferences round-trip、shared/standalone preferences page 组合、editing scheme/parameter sync、PianoRoll source guards、visual invalidation、audio format registry、Standalone import、undo result-chain，以及新增的 `PianoRollProjection_ConsumesMaterializationIdAndPlacementProjectionOnly` / `EditingCommand_DoesNotMutatePlacement` owner guards，入口列表见 `Tests/TestMain.cpp:4131`。
- `architecture`: 覆盖 placement/materialization owner 约束、Standalone playback placement window、ARA snapshot/render span、mac Standalone packaging 结构守护、undo architecture cleanup guard，以及 ARA persistentID binding guards：`AraBinding_NewPersistentIdSameSourceWindowCreatesIndependentMaterialization`、`AraBinding_MultiplePlaybackRegionsSameAudioModificationShareMaterialization`、`AraBinding_ArchiveHooksPersistPersistentIdMaterializationBindings`、`AraBinding_RestoredPersistentIdRebindsNewPlaybackRegion`、`AraEditor_AttachesRenderableBindingWithoutReadAudioArm`。
- `undo`: 覆盖 undo/redo 边界测试，包括空栈、redo 裁剪、溢出等场景。
- `memory`: 覆盖 ONNX 运行时内存生命周期测试。

## Commands In Use Now

**Configure current Release build dirs:**
```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --preset windows-ara-vs2022"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --preset windows-nonara-vs2022"
```

**Build the test binary in the current Release ARA build dir:**
```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```
- `OpenTuneTests` 是显式 CMake target，见 `CMakeLists.txt:1072`。

**Run targeted suites:**
```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
```

**Run one suite or list suites:**
```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe --list-suites
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe ui
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
```

**Run through CTest:**
```powershell
ctest --test-dir build-ara-overlay-vs18-clean -C Release --output-on-failure
```

**Format build verification also matters:**
```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
```
- Avoid parallel MSBuild invocations in the same build directory. A prior parallel run hit a `.tlog` permission lock and passed after serial rerun.

## Test File Organization

- 测试仍集中在 `Tests/`，当前 live files 是 `Tests/TestMain.cpp`, `Tests/TestSupport.h`, `Tests/TestEditorFactoryStub.cpp`。
- `Tests/TestMain.cpp` 仍是单文件主测试体和 suite registry，长度约 5.5k 行。
- `Tests/TestSupport.h` / `Tests/TestSupport.cpp` 提供 probe、mock、共享 helper，例如 `VST3AraSessionTestProbe` 和 `MockVocoderService`。
- `Tests/TestEditorFactoryStub.cpp` 继续用于阻断真实 editor 创建，这个角色在旧文档中存在，live tree 仍保留该文件。

## Test Patterns Present Now

**Direct state assertions:**
- 测试更倾向直接实例化真实共享对象并断言状态，而不是经由 UI 黑盒观察，例如 `SourceStore`, `MaterializationStore`, `StandaloneArrangement`, `VST3AraSession`, `OpenTuneAudioProcessor` 均被 `Tests/TestSupport.h:10`-`Tests/TestSupport.h:24` 直接引入。

**Source-inspection guards:**
- 当前 repo 继续用 `readWorkspaceFile()`, `extractWorkspaceFileSection()`, `sourceContains()`, `workspaceFileExists()` 直接读 live 源码做结构守护。
- 这类 guard 仍在验证 preferences 组合、scheme manager 清理、mac packaging owner 边界等，例如 `runMacStandalonePackagingPlistMergeBelongsToStandaloneTargetOnlyTest()` in `Tests/TestMain.cpp:2700` and `runAudioEditingSchemeRulesUseExplicitSchemeInputTest()` referenced from `Tests/TestMain.cpp:2793`。

**Manual fake / probe, not external mocking library:**
- `MockVocoderService` 仍用手写 fake 记录并发与失败路径。
- 私有 seam probe 继续通过 `OPENTUNE_TEST_BUILD` 暴露，例如 `VST3AraSessionTestProbe`。

## Coverage Signals

- 仓库里仍没有 gcov/lcov/llvm-cov 之类覆盖率配置；当前 coverage signal 主要来自 smoke suites + architecture guards，而不是覆盖率百分比。
- `ui` 和 `architecture` 仍是当前最高信号 suite；`core` 和 `processor` 已不再为空，但覆盖仍偏 smoke/contract 级。
- 2026-05-15 ARA multi-region 自动化现实：`OpenTuneTests` Release build PASS，`architecture/core/processor/memory/undo` suites PASS，ARA VST3 build PASS，non-ARA VST3 build PASS。`ui` suite 当前 exit=1 且无 `[FAIL]` 文本，不得继续写成 full-suite PASS。

## Manual Validation Expectations

- repo 当前仍把 manual DAW journey + `AppLogger` trace 当作 host-specific verification layer，见 `.planning/STATE.md:25` and `.planning/ROADMAP.md:54`。
- 与 VST3/ARA 相关的问题，现行文档仍要求在真实 DAW 中复现并检查日志，REAPER 调试样例可见 `.planning/debug/reaper-vst3-record-originalf0-missing.md:10` and `.planning/debug/reaper-vst3-record-originalf0-missing.md:63`。
- 生产代码里的 trace family 仍是手工验证的重要抓手，例如 `RecordTrace`, `MappingTrace`, `AutoTuneTrace`, `RenderTrace` 分别位于 `Source/Plugin/PluginEditor.cpp:999`, `Source/Plugin/PluginEditor.cpp:1106`, `Source/Plugin/PluginEditor.cpp:1258`, `Source/Standalone/PluginEditor.cpp:2597`。

## Explicit Gaps Verified From Repo Files

- `OpenTuneTests.exe ui` 当前在 `PianoRoll_DrawNoteDraft_SurvivesMultiEventDrag` PASS 后以 exit code 1 结束且没有 `[FAIL]` 文本；这是 runner/后续测试状态待查项。
- Full `OpenTuneTests.exe` PASS 口径已过期，恢复前必须先解释 UI suite exit behavior。
- 没有 repo-level CI pipeline：工作区下未检测到 `.github/`，根目录也未检测到仓库级 `*.yml` / `*.yaml` CI 配置；自动化门禁仍依赖本地 CMake/CTest 流程。
- L5 Standalone / VST3 手工旅程仍是显式 deferred gap，不是 PASS，见 `.planning/ROADMAP.md:53`, `.planning/ROADMAP.md:56`, `.planning/STATE.md:71`。
- macOS 真实 `.app` bundle inspection 仍待 macOS 环境执行，见 `.planning/REQUIREMENTS.md:25`, `.planning/STATE.md:80`。
- 由于 CTest 名称仍叫 `OpenTuneCoreTests`，但实际跑的是整包 `OpenTuneTests`，自动化入口名称与真实范围不一致，见 `CMakeLists.txt:1179`。

## Practical Guidance For New Work

- 需要高信号回归守护时，优先往 `ui` 或 `architecture` 增加 focused smoke test；`core` / `processor` 可承载 leaf/runtime smoke，但不要把它们误当充分覆盖。
- 需要守护结构清理时，继续使用 workspace source-inspection pattern，但优先补真实行为 test，例如 ARA binding archive restore 后重建 PlaybackRegion 的 rebind 测试。
- 遇到 host-only 或 ARA-only 回归时，继续组合使用：`OpenTuneTests` 局部 smoke、格式 build、真实 DAW 手工旅程、`AppLogger` trace。
- 写测试时继续优先断言显式 state carrier 和 published snapshot，不要引入新的隐藏 manager mock 层。

---

*Testing analysis: 2026-05-15*
