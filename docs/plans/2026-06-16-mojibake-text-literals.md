# Mojibake Text Literals Implementation Plan

> **For Codex:** REQUIRED SKILLS: Best-Engineering and test-driven-spec. Execute the plan against `docs/plans/2026-06-16-mojibake-text-literals-test-verification.md`.

**Goal:** Restore corrupted compiled text literals to their original intended content without adding runtime encoding workarounds.

**Architecture:** This is an in-place source text restoration. The only truth after the fix is the corrected UTF-8 literal at the call site; there is no parallel bad-to-good map, decoder, fallback string, or compatibility layer. The task deliberately avoids a broader localization migration because the requirement is to repair corruption, not change ownership of text.

**Tech Stack:** C++17, JUCE `juce::String`, UTF-8 `u8"..."` literals, ripgrep, Git history, existing CMake/MSBuild build directory.

---

## Contract

Restore only the corrupted compiled literals confirmed in the current source scan and history review.

Do not touch:

- comments with mojibake
- unrelated Chinese text that is already correct
- `LocalizationManager` dictionaries
- UI layout, dialog behavior, render state, ARA state, or project-save logic

Do not add:

- runtime mojibake detection
- string recoding helpers
- fallback strings
- lookup tables from bad text to good text
- compatibility wrappers
- new files outside this plan and its verification document

## Evidence

The Standalone UI mojibake was introduced during the ContentKey migration at commit `419e5cb` (`Refactor ARA content ownership and ContentKey flow`). The parent revision still carried the intended Chinese labels.

The remaining `PluginProcessor.cpp` compiled bad literals are older migration residue around note backend logging and export error text. Comment-only mojibake in that file is deliberately excluded from this task.

## Files

Modify:

- `Source/Standalone/PluginEditor.cpp`
- `Source/Standalone/UI/PianoRollComponent.cpp`
- `Source/PluginProcessor.cpp`

Verify with:

- `docs/plans/2026-06-16-mojibake-text-literals-test-verification.md`

## Replacement Table

### `Source/Standalone/PluginEditor.cpp`

Replace the import and processing strings:

| Current bad literal | Restored text |
|---|---|
| `瀵煎叆闊抽` | `导入音频` |
| `姝ｅ湪澶勭悊闊抽` | `正在处理音频` |
| `閫夋嫨瑕佸鍏ョ殑闊抽鏂囦欢` | `选择要导入的音频文件` |
| `鍙栨秷` | `取消` |
| `瀵煎叆澶辫触` | `导入失败` |

Replace the export dialog strings:

| Current bad literal | Restored text |
|---|---|
| `瀵煎嚭瀹屾垚` | `导出完成` |
| ` 宸插鍑哄埌: ` | ` 已导出到: ` |
| `鏃犳硶瀵煎嚭闊抽鍒? ` | `无法导出音频到: ` |
| `\n鍘熷洜: ` | `\n原因: ` |
| `瀵煎嚭澶辫触` | `导出失败` |

Replace the project dialog strings:

| Current bad literal | Restored text |
|---|---|
| `淇濆瓨宸ョ▼澶辫触` | `保存工程失败` |
| `纭畾` | `确定` |
| `褰撳墠宸ョ▼灏氭湭淇濆瓨` | `当前工程尚未保存` |
| `鎵撳紑鍏朵粬宸ョ▼鍓嶏紝鏄惁淇濆瓨褰撳墠宸ョ▼鐨勬洿鏀癸紵` | `打开其他工程前，是否保存当前工程的更改？` |
| `淇濆瓨` | `保存` |
| `鍙栨秷` | `取消` |
| `鎵撳紑宸ョ▼` | `打开工程` |
| `鎵撳紑宸ョ▼澶辫触` | `打开工程失败` |
| `淇濆瓨宸ョ▼` | `保存工程` |
| `瑕嗙洊` | `覆盖` |

Replace the reference clip menu strings:

| Current bad literal | Restored text |
|---|---|
| `涓嶄娇鐢ㄥ弬鑰?Clip` | `不使用参考 Clip` |
| `閫夋嫨鍙傝€?Clip` | `选择参考 Clip` |
| `(鏃犲彲鐢ㄧ殑鍙傝€?Clip)` | `(无可用的参考 Clip)` |

Encoding rule for this file:

- Keep existing `juce::String::fromUTF8(...)` call sites.
- Prefer `juce::String::fromUTF8(u8"...")` for restored Chinese literals.
- If the call site already concatenates `juce::String`, restore only the literal content and preserve the surrounding expression.

### `Source/Standalone/UI/PianoRollComponent.cpp`

Replace:

| Current bad literal | Restored text |
|---|---|
| `缂栬緫鏃堕棿缃戞牸` | `编辑时间网格` |

Encoding rule:

- Use an explicit UTF-8 construction at the existing fallback expression, for example `juce::String::fromUTF8(u8"编辑时间网格")`.
- Do not change the edit command flow.

### `Source/PluginProcessor.cpp`

Replace the remaining compiled bad literals:

| Current bad literal | Restored text |
|---|---|
| ` �?forceLegacy=` | ` -> forceLegacy=` |
| ` �?falling back to Legacy` | ` -- falling back to Legacy` |
| `无效的片段索�? ` | `无效的片段索引: ` |

Encoding rule:

- The note backend log punctuation is restored as ASCII punctuation to avoid reintroducing source-encoding risk in logs.
- The export error remains a Chinese user-facing error and must use correct UTF-8 text.
- Do not touch comment-only mojibake in this pass.

## Tasks

### Task 1: Freeze The Target Set

Run the negative scan from the verification document before editing.

Expected: hits only in the three planned source files. Classify comment-only hits separately and do not add them to this task.

### Task 2: Patch `PluginEditor.cpp`

Replace the import, export, project dialog, and reference clip menu literals from the table above.

Keep the current dialog calls, callbacks, button defaults, and chooser filters unchanged.

### Task 3: Patch `PianoRollComponent.cpp`

Replace the fallback undo description literal with correct UTF-8 text.

Do not change command routing, mutation hooks, note storage, or selection behavior.

### Task 4: Patch `PluginProcessor.cpp`

Replace only the three compiled bad literals from the table above.

Do not clean comment mojibake, logging structure, backend selection, or export control flow.

### Task 5: Run Static Verification

Run every L1 static gate from `docs/plans/2026-06-16-mojibake-text-literals-test-verification.md`.

If the negative scan still finds a compiled bad literal, fix that literal directly. If it finds only comments, record that they are out of scope.

### Task 6: Build

Run both build gates from the verification document using the existing configured build directory and absolute CMake path.

Do not refresh, rewrite, or sanitize `PATH`.

## Stop Rules

Stop and report instead of improvising if:

- a bad compiled literal is not in the replacement table and its original text cannot be recovered from history
- a needed change would require moving strings into `LocalizationManager` or `UiText`
- build failure points to unrelated current worktree changes
- fixing a failure would require UI behavior, ARA state, render scheduling, or project persistence changes

## Acceptance

The task is complete when the verification document passes and the diff contains only the planned literal restorations. Any remaining comment mojibake is reported as separate debt, not silently bundled into this fix.
