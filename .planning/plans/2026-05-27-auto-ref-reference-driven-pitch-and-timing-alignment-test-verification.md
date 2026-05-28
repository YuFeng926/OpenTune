# AUTO(REF) Reference-Driven Pitch And Timing Alignment Test And Verification

**日期:** 2026-05-27  
**状态:** Verified
**关联执行方案:** `.planning/plans/2026-05-27-auto-ref-reference-driven-pitch-and-timing-alignment.md`

## 1. Verification Goal

证明 AUTO(REF) 已收束为一条正式主链：

```text
binding truth
-> ReferenceFeatureSet cache
-> processor-built request
-> ReferenceAutoAlign patch
-> TimeGridPatchBuilder
-> atomic store commit
```

验证范围只覆盖 AUTO(REF) 主合同与 TimeTool seed 的共享边界，不代表 v1.5 全仓所有 open 项已完成。

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
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe auto-ref-failure
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe reference-features-cache-smoke
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe reference-features-cache
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe ref-analysis-svc
```

Expected:

- all focused suites pass。

### L4 Contract Sweep

```powershell
rg -n "ReferenceFeatureSet|ReferencePitchFeatures|ReferenceTimingFeatures|EffectiveTimeMap|TimeGridPatchBuilder|executeReferenceAlignmentForPlacement|ensureTimeToolAnchorSeed" Source Tests
```

Manual review confirms:

- feature contract is shared and explicit。
- processor owns request assembly。
- alignment patch is pure algorithm output。
- timing intents compile through `TimeGridPatchBuilder`。
- TimeTool seed only reuses timing features and does not perform reference alignment。

## 3. Verified Behaviors

- AUTO(REF) can run without a user-created TimeTool grid.
- Target and reference features are both represented by `ReferenceFeatureSet`.
- Reference timing context uses `EffectiveTimeMap`.
- Timing result writes normal `TimeGridSnapshot`.
- AUTO timing patch honors the 0.8x-1.3x local speed window.
- Infeasible timing intents are projected or rejected rather than silently forcing invalid output.
- TimeTool first-entry seed creates identity handles only.
- TimeTool re-entry does not overwrite existing stretch edits.
- Project persistence does not store runtime feature cache or alignment intermediate.

## 4. Final Evidence

Recorded verification for this contract:

- `cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests` passed.
- `OpenTuneTests.exe auto-ref-architecture` passed.
- `OpenTuneTests.exe auto-ref-integration` passed.
- `OpenTuneTests.exe auto-ref-failure` passed.
- `OpenTuneTests.exe reference-features-cache-smoke` passed.
- `OpenTuneTests.exe reference-features-cache` passed.
- `OpenTuneTests.exe ref-analysis-svc` passed.
- `git diff --check` had no new format issue beyond existing line-ending warnings.

## 5. Final Kill List

AUTO(REF) verification must continue to reject:

- UI-owned alignment logic.
- UI-owned TimeGrid generation.
- duplicated feature extraction path outside the shared feature contract.
- timing compiler outside `TimeGridPatchBuilder`.
- project persistence of runtime feature cache.
- reference-only timing truth.
- silent bypass that avoids `ReferenceFeatureSet`.
- stale mode or analysis terminology in active planning docs.

## 6. Completion Statement

AUTO(REF) main contract is landed and focused verification is complete. Remaining v1.5 open work is unrelated to this
contract unless a future change touches the files listed in the execution plan.
