# Phase 10 Test Verification

## Scope

Phase 10 只验证两类合同：

1. `RenderStatus` / `RenderStatusSnapshot` 的可观测状态分类是否成立。
2. `DiagnosticInfo` / `recordControlCall()` 的只读诊断语义是否成立。

当前 phase 不再把宿主导入后的 F0 提取问题当作本 phase gate，因为那属于相邻导入/提取链路，不是 status/diagnostic contract 本身。

## Gates

| Level | Command | Expected |
| --- | --- | --- |
| L1 | `cmake --build build --target OpenTuneTests --config Release` | 测试目标构建成功 |
| L1 | `cmake --build build --target OpenTune_Standalone --config Release` | Standalone 构建成功 |
| L1 | `cmake --build build --target OpenTune_VST3 --config Release` | VST3 构建成功 |
| L2/L3/L6 | `build\OpenTuneTests.exe` | `STAB-02` 与 `STAB-03 / SAFE-01` 全部 PASS |
| L4 | `rg -n "RenderStatus|DiagnosticInfo|getRenderStatusSnapshot|getDiagnosticInfo|recordControlCall" Source` | 关键合同均可定位 |

## L5 Applicability

- L5: Not applicable.
- Reason: 当前 Phase 10 的 closure 以状态合同和诊断合同为主，已经有确定性的自动化证据；旧的宿主 item F0 现象不再计为本 phase 阻断项。
