# Phase 8: 统一 Playback Read 核心 - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-07
**Phase:** 08-统一 Playback Read 核心
**Areas discussed:** 迁移顺序与验收, 混音语义一致, 回退策略细则, 读路径归属

---

## 迁移顺序与验收

| Option | Description | Selected |
|--------|-------------|----------|
| 先常规后ARA | 先常规播放接统一读核，再切 ARA；回归面更可控 | ✓ |
| 先ARA后常规 | 先改 ARA，再回填常规路径 | |
| 一次切换 | 两条路径在同一提交同时切换 | |
| You decide | 实现顺序由 agent 决定 | |

**User's choice:** 先常规后ARA
**Notes:** 用户同时选择“末端总验收”，不采用分步 A/B gate。

---

## 混音语义一致

| Option | Description | Selected |
|--------|-------------|----------|
| 全程add+末端限幅 | 统一 add/mix，最终安全限幅 | |
| 允许覆盖写入 | 允许 copy 覆盖提升简单性 | |
| 双语义并存 | ARA 与常规使用不同语义 | |
| You decide | 由 agent 决定 | |

**User's choice:** 统一语义（拒绝双语义并存）
**Notes:** 用户最初提出“双语义并存”，经范围约束澄清后改为“统一语义”；并明确“末端不做任何额外处理”。

---

## 回退策略细则

| Option | Description | Selected |
|--------|-------------|----------|
| 严格四级回退 | 固定 `resampled -> rendered -> dry -> blank` | ✓ |
| 动态回退 | 允许按场景调整优先级 | |
| You decide | 由 agent 决定 | |

**User's choice:** 严格四级回退
**Notes:** dry/blank 条件偏向“dry优先”（渲染不可用但源有效时优先 dry）。

---

## 读路径归属

| Option | Description | Selected |
|--------|-------------|----------|
| Processor共享层 | 统一 read API 放 Processor 共享层 | ✓ |
| ARA层中心 | 统一 read API 放 ARA 层 | |
| 独立服务层 | 抽象独立服务模块 | |
| You decide | 由 agent 决定 | |

**User's choice:** Processor共享层
**Notes:** 调用边界锁定为“仅 Processor/ARA renderer 调统一 read API；编辑器不直读 RenderCache 音频数据”。

---

## the agent's Discretion

- 统一 read API 的函数命名、参数结构与内部拆分策略。
- 日志字段与日志等级细节。

## Deferred Ideas

- 双语义并存（ARA/常规各自混音语义）——已评估为违反 Phase 8 固定范围，记录为不采纳方案。
