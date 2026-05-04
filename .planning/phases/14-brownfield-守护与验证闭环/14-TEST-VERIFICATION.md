# Phase 14 Test Verification

## Gates

| Level | Command | Expected |
| --- | --- | --- |
| L1 | `cmake --build build --target OpenTuneTests --config Release` | 测试目标构建成功 |
| L1 | `cmake --build build --target OpenTune_Standalone --config Release` | Standalone 构建成功 |
| L1 | `cmake --build build --target OpenTune_VST3 --config Release` | VST3 构建成功 |
| L2/L3/L6 | `build\OpenTuneTests.exe` | 全套回归 PASS |
| L4 | `rg -n "phase:|status: passed|Gate Status" .planning/phases` | Phase 1-13 verification 已闭环 |

## L5 Applicability

- L5: Not applicable.
- Reason: 当前 brownfield gate 以构建、回归和历史 verification 闭环为准。
