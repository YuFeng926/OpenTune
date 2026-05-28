# Standalone Experimental Features Gate And TimeTool Anchor Seed Test And Verification

**日期:** 2026-05-27  
**状态:** Verified
**关联执行方案:** `.planning/plans/2026-05-27-standalone-experimental-features-gate-and-time-tool-anchor-seed.md`

## 1. Verification Goal

证明 Standalone 实验功能入口与 TimeTool 首次 seed 已收束为单一合同：

- `experimentalFeaturesEnabled` 只控制 UI exposure。
- AUTO Ref mode preference 只控制 reference alignment behavior。
- TimeTool seed 由 processor 单入口负责。
- Seed 只生成 identity handles，不改变播放 timing。

## 2. Required Gates

### L1 Static Check

```powershell
git diff --check
```

Expected:

- exit code 0。
- 无 patch 格式错误。
- 无尾随空白。

### L2 Build

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```

Expected:

- `OpenTuneTests` 编译成功。

### L3 Focused Suites

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe auto-ref-architecture
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe auto-ref-integration
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe reference-features-cache-smoke
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe reference-features-cache
```

Expected:

- all focused suites pass。

## 3. Verified Behaviors

- Shared preferences expose a standalone experimental-features boolean。
- TimeTool toolbar, tool menu, shortcut path, and current-tool safe reset obey the same gate。
- Arrangement reference button draw, hover, cursor, click, and menu obey the same gate。
- AUTO Ref mode preference remains independent from the UI exposure gate。
- TimeTool first-entry seed creates internal identity handles from shared timing features。
- Seeded handles preserve `source_seconds == output_seconds`。
- Seed does not change clip duration or playback timing。
- Re-entry does not overwrite user stretch edits。
- UI does not build time grids directly。

## 4. Manual Journey

Manual L5 validation remains useful before release, but focused contract verification is complete.

Recommended manual checks:

- Default preference state hides TimeTool and reference UI.
- Enabling experimental features exposes TimeTool and reference UI.
- First TimeTool entry shows seeded handles without audible timing change.
- A user-edited stretch grid survives leaving and re-entering TimeTool.
- Disabling experimental features hides entry points without deleting existing data.

## 5. Final Kill List

Verification must continue to reject:

- split UI gates across toolbar/menu/shortcut/click paths。
- UI-owned time grid construction。
- TimeTool seed that changes timing output。
- TimeTool seed that triggers reference alignment。
- seed re-entry that overwrites user edits。
- destructive cleanup when experimental features are disabled。
- stale mode or analysis terminology in active planning docs。

## 6. Completion Statement

The experimental-features gate and TimeTool identity seed contract are landed and focused verification is complete.
Remaining v1.5 open work is unrelated unless a future change touches the files listed in the execution plan.
