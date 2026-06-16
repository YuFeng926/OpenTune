# Mojibake Text Literals Test Verification

## Scope

This document is the acceptance contract for `docs/plans/2026-06-16-mojibake-text-literals.md`.

The target is compiled user-visible or log/error string literals that were corrupted during recent migrations. The fix restores the original intended text in-place. It does not introduce runtime decoders, fallback text maps, compatibility shims, or a broader localization migration.

Out of scope for this pass:

- comments with mojibake
- already-correct Chinese literals
- English/Russian/Spanish localization entries that are not corrupted
- replacing direct literals with new `LocalizationManager` or `UiText` structure

## L1 Static Gates

Run from `G:\OpenTune0519New1`.

```powershell
git diff --check
```

Expected: succeeds with no whitespace errors.

Run the negative scan for the bad compiled literals:

```powershell
rg -n "瀵煎叆|姝ｅ湪澶勭悊|閫夋嫨瑕佸|鍙栨秷|瀵煎嚭|淇濆瓨宸ョ▼|鎵撳紑宸ョ▼|纭畾|褰撳墠宸ョ▼|瑕嗙洊|涓嶄娇鐢ㄥ弬|閫夋嫨鍙傝|鏃犲彲鐢ㄧ殑鍙傝|缂栬緫鏃堕棿|无效的片段索�|�\?forceLegacy|�\?falling back" Source/Standalone/PluginEditor.cpp Source/Standalone/UI/PianoRollComponent.cpp Source/PluginProcessor.cpp
```

Expected: no hits in compiled string literals.

Run the positive scan for the restored text:

```powershell
rg -n "导入音频|正在处理音频|选择要导入的音频文件|导入失败|导出完成|已导出到|无法导出音频到|原因|导出失败|保存工程失败|确定|当前工程尚未保存|打开其他工程前，是否保存当前工程的更改|保存|取消|打开工程|打开工程失败|保存工程|覆盖|不使用参考 Clip|选择参考 Clip|无可用的参考 Clip|编辑时间网格|无效的片段索引|forceLegacy=|falling back to Legacy" Source/Standalone/PluginEditor.cpp Source/Standalone/UI/PianoRollComponent.cpp Source/PluginProcessor.cpp
```

Expected: every replacement target appears at the planned call sites.

Run a narrow replacement audit:

```powershell
git diff -- Source/Standalone/PluginEditor.cpp Source/Standalone/UI/PianoRollComponent.cpp Source/PluginProcessor.cpp
```

Expected:

- only literal contents or literal wrappers changed
- no control flow changes
- no new helper functions
- no bad-to-good lookup table
- no runtime encoding repair
- no localization architecture change

## L1 Build Gates

Use the existing configured build directory and the absolute CMake path. Do not refresh, rewrite, or sanitize `PATH`.

```powershell
& 'F:\VSC\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build --config Release --target OpenTune_Standalone
& 'F:\VSC\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build --config Release --target OpenTune_VST3
```

Expected:

- both targets build successfully
- no alternate compiler path or generated build script is used

## Higher-Level Tests

No L2-L6 test is defined for this text-only restoration. There is no domain logic, API contract, or host journey being changed. The blocking acceptance for this task is the static literal audit plus the two build gates above.

If a future change moves these strings into `LocalizationManager` or `UiText`, that is a separate feature and must define its own L2/L3 coverage for key lookup and fallback behavior.

## Acceptance

The fix is accepted only when:

- all bad compiled literal scans return no hits
- restored literal scan shows the expected text at the planned call sites
- diff review shows no defensive programming, fallback map, compatibility layer, or unrelated cleanup
- Standalone and VST3 build successfully through the existing CMake build directory
