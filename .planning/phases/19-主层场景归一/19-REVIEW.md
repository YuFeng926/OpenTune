---
status: issues
phase: 19-主层场景归一
reviewed: 2026-04-14T04:50:37.5529109Z
depth: standard
range:
  from: 99d5acf3c5a980599d3fa64b70013a3a62c4cff0
  to: dde83e0cdf3ad145d6c4bd115b50c70a9c244001
files_reviewed: 13
files_reviewed_list:
  - .planning/phases/19-主层场景归一/19-01-PLAN.md
  - .planning/phases/19-主层场景归一/19-02-PLAN.md
  - .planning/phases/19-主层场景归一/19-03-PLAN.md
  - .planning/phases/19-主层场景归一/19-01-SUMMARY.md
  - .planning/phases/19-主层场景归一/19-02-SUMMARY.md
  - .planning/phases/19-主层场景归一/19-03-SUMMARY.md
  - .planning/phases/19-主层场景归一/19-TEST-VERIFICATION.md
  - .planning/phases/19-主层场景归一/19-VERIFICATION.md
  - Tests/TestMain.cpp
  - Source/Standalone/UI/PianoRollComponent.h
  - Source/Standalone/UI/PianoRollComponent.cpp
  - AGENTS.md
  - .planning/ROADMAP.md
findings:
  critical: 0
  warning: 4
  info: 0
  total: 4
---

# Phase 19 Code Review

**Reviewed:** 2026-04-14T04:50:37.5529109Z
**Depth:** standard
**Status:** issues

## Summary

本次范围内的实际代码变更只有 `Tests/TestMain.cpp` 与 Phase 19 的验证/总结文档；`19-02-PLAN.md` 计划声称要落地的 `PianoRollComponent.*` / renderer 主层实现并没有出现在 `99d5acf3c5a980599d3fa64b70013a3a62c4cff0..dde83e0cdf3ad145d6c4bd115b50c70a9c244001` 的 diff 里。当前 live tree 的 `PianoRollComponent` 仍保留 overlay 结构，而且 Phase 19 的状态文档、验证命令和测试覆盖之间还存在会制造假阳性结论的断层。

## Warnings

### WR-01: Phase 19 仍被错误地写成已完成，和当前 gate 失败事实冲突

**File:** `.planning/phases/19-主层场景归一/19-02-SUMMARY.md:5`
**Issue:** `19-02-SUMMARY` 把 overlay removal / main-layer draw contract 写成“已完成工作”（`19-02-SUMMARY.md:5`, `19-02-SUMMARY.md:13`），同时当前路线图也把 Phase 19 标成已完成（`.planning/ROADMAP.md:62`, `.planning/ROADMAP.md:214`）。但当前 requirement 证据明确是 FAIL（`.planning/phases/19-主层场景归一/19-VERIFICATION.md:3`, `.planning/phases/19-主层场景归一/19-TEST-VERIFICATION.md:46`），而 live tree 仍保留 overlay 依赖（`Source/Standalone/UI/PianoRollComponent.h:18`, `Source/Standalone/UI/PianoRollComponent.h:422`, `Source/Standalone/UI/PianoRollComponent.cpp:54`, `Source/Standalone/UI/PianoRollComponent.cpp:1146`）。这会让后续 workflow 在错误 baseline 上继续推进，属于明确的假阳性验收风险。
**Fix:** 在 `PianoRollComponent` 主层实现真实落地且 L1/L2/L4/L6 全部转绿前，撤回 `19-02-SUMMARY` 与 `.planning/ROADMAP.md` 的完成态；让 Phase 状态只从最新 verification 结果派生，而不是从中间 summary 派生。

### WR-02: L4 静态审计命令不是 fail-closed，未来很容易误报通过

**File:** `.planning/phases/19-主层场景归一/19-TEST-VERIFICATION.md:13`
**Issue:** L4 gate 现在用一条混合正负 token 的 `grep` 命令同时查 `playheadOverlay_` / `PlayheadOverlayComponent` / `drawPlayhead` / 测试名。这个命令并没有把“禁止残留”和“必须存在”拆开验证：在当前 broken tree 上，只对现有文件运行同类 `grep`，依然会因为 `runPhase19MainLayerSceneTests`、`TIME_01_*` 等字符串存在而返回成功；也就是说它不是靠 contract 检查来 fail，而只是今天恰好因为 renderer 路径缺失才失败。当前 `19-03-PLAN.md` 仍把这条命令当作自动化 gate（`19-03-PLAN.md:97`, `19-03-PLAN.md:100`），所以一旦 renderer 文件补上，这个 gate 仍可能在 overlay 残留时假阳性通过。
**Fix:** 把 L4 拆成三类显式断言：1) 文件存在性检查；2) 对 `playheadOverlay_` / `PlayheadOverlayComponent` 的负向检查，命中即失败；3) 对 `drawPlayhead` / `runPhase19MainLayerSceneTests` 的正向检查，缺失即失败。不要再用一条混合 `grep` 充当 closure gate。

### WR-03: live tree 仍引用不存在的 renderer 头文件，fresh build 风险被低估了

**File:** `Source/Standalone/UI/PianoRollComponent.h:35`
**Issue:** `PianoRollComponent.h` 直接 `#include "PianoRoll/PianoRollRenderer.h"`，`PianoRollComponent.cpp` 也在使用 `PianoRollRenderer::RenderContext`（`Source/Standalone/UI/PianoRollComponent.cpp:1712`），但 live tree 中并不存在 `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h` 或 `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp`。这意味着当前 L1 的“缺 ONNX 头文件”并不是唯一 fresh build blocker；即使把 ONNX 依赖补回，Phase 19 仍可能在更早的编译阶段失败。现在的 Phase 19 文档把问题主要归因到环境依赖，会低估源码本身的 build risk。
**Fix:** 在宣称 Phase 19 可继续关闭前，先恢复/移动 `PianoRollRenderer.*` 到真实路径，或者修正 `PianoRollComponent` 的 include 和类型依赖；同时把这个源码级 blocker 写进 verification 结论，而不是只写环境缺件。

### WR-04: Phase 19 新增测试没有真正覆盖单层场景契约，LAYER-02 仍有明显漏测

**File:** `Tests/TestMain.cpp:3451`
**Issue:** `runPhase19MainLayerSceneTests()` 里唯一新的结构性断言，是扫描 direct children 不得出现 `PlayheadOverlayComponent`（`Tests/TestMain.cpp:3467`）；其余两个 `LAYER_*` 测试只是复用了 Phase 18 的 continuous/page scroll 断言（`Tests/TestMain.cpp:3480`, `Tests/TestMain.cpp:3533`）。这组测试无法证明 `PianoRollComponent` 已经没有 overlay member / bounds-sync / zoom-sync / scroll-sync 路径；当前 live tree 就是反例：`playheadOverlay_` 仍然存在，相关同步调用也还在（`Source/Standalone/UI/PianoRollComponent.cpp:1242`, `Source/Standalone/UI/PianoRollComponent.cpp:1395`, `Source/Standalone/UI/PianoRollComponent.cpp:1456`），但如果只看 scroll 语义，这两个 `LAYER_*` 场景并不会暴露问题。换句话说，当前新增测试对 `LAYER-02` 的保护仍然偏弱，存在“行为没回归但结构根本没收敛”也能绿的风险。
**Fix:** 为 Phase 19 增加能直接约束单层 contract 的测试/审计：至少要显式失败于 overlay member/sync 调用残留，并补一条主层播放头绘制 contract 证据，而不是只复用 Phase 18 的 scroll 结果。

## Notes

- 我核对了 `99d5acf3c5a980599d3fa64b70013a3a62c4cff0..dde83e0cdf3ad145d6c4bd115b50c70a9c244001` 的 diff；其中没有任何 `Source/Standalone/UI/PianoRollComponent.*` 或 renderer 实现变更，只有测试和 verification 文档变更。
- 当前 live tree 额外存在 `.planning/ROADMAP.md` 的未提交修改；本 review 以上述 live tree 内容为准。

---

_Reviewer: Codex / gsd-code-reviewer_
_Result: issues_
