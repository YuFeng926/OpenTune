# Phase 19-03 Summary

## Result

本次执行完成了 Phase 19 最后一轮验证文档收口，但 gate 结果为失败收口，不是 closure pass。

完成内容：
- 真实执行了 `19-03-PLAN.md` 要求的 L1 / L2 / L4 / L6
- 把真实结果写回 `.planning/phases/19-主层场景归一/19-TEST-VERIFICATION.md`
- 新建 requirement 级 `.planning/phases/19-主层场景归一/19-VERIFICATION.md`
- 记录当前工作区与 `19-02-SUMMARY.md` / plan contract 不一致的事实，而不是按文档假设 Phase 19 已完成

## Gate Results

- L1: Failed
  - 原样构建命令在当前环境真实执行后，CMake 配置失败
  - 直接阻塞是 `ThirdParty/onnxruntime-win-x64-1.24.4/include` 缺失
- L2: Failed
  - 现有 `build/OpenTuneTests.exe` 可以运行，但输出里没有 `Phase 19: Main Layer Scene Tests`
  - 两个 `TIME_01_*` 守护仍是绿色，说明当前可运行二进制更接近 Phase 18 状态而不是 fresh Phase 19 binary
- L4: Failed
  - `PianoRollComponent.h/cpp` 仍然包含 `PlayheadOverlayComponent` / `playheadOverlay_`
  - `paint()` 没有主层 `drawPlayhead(...)` 调用证据
  - 当前工作区缺少 `Source/Standalone/UI/PianoRoll/` 目录，计划引用的 renderer 文件无法审计
- L6: Failed
  - 二次全量回归与 L2 一致，Phase 19 regressions 仍未执行

## Key Findings

- 当前 live tree 与 `.planning/phases/19-主层场景归一/19-02-SUMMARY.md` 的描述不一致：summary 声称 overlay 已移除、renderer 已扩展，但当前源码并非如此。
- `Tests/TestMain.cpp` 源码里已经有 `runPhase19MainLayerSceneTests()` 与三个 `LAYER_*` 用例定义；问题不在“测试没写”，而在 fresh build 没成功，现有二进制没有把它们跑起来。
- Phase 18 projected-playhead regressions 仍通过，这一点已如实写回 Phase 19 证据，但它们不能替代 Phase 19 主层场景 gate。

## Files

- Modified: `.planning/phases/19-主层场景归一/19-TEST-VERIFICATION.md`
- Created: `.planning/phases/19-主层场景归一/19-VERIFICATION.md`
- Created: `.planning/phases/19-主层场景归一/19-03-SUMMARY.md`

## Readiness

Phase 19 不能在当前工作区宣称完成。要让 gate 重新具备可关闭条件，至少还需要：

- 恢复 fresh 构建依赖，先让 `OpenTuneTests` 能重新编译
- 对 live tree 重新核实 `PianoRollComponent` / renderer 主层绘制实现，而不是沿用 `19-02-SUMMARY.md` 的文字结论
- 重新执行 L1 / L2 / L4 / L6，直到 `Phase 19: Main Layer Scene Tests` 真正在 fresh binary 里出现并通过

## Self-Check

PASSED

- 三个 Phase 19-03 交付文档都已生成或更新
- 所有 gate 都按真实命令执行并落盘，没有把失败标成跳过或通过
