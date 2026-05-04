# Phase 9: 生命周期绑定与失效收敛 - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in `09-CONTEXT.md` — this log preserves the alternatives considered.

**Date:** 2026-04-07
**Phase:** 09-生命周期绑定与失效收敛
**Areas discussed:** 生命周期真相源归属, replace 语义, cache 生命周期, 范围失效策略, 删除收敛语义

---

## 生命周期真相源归属

| Option | Description | Selected |
|--------|-------------|----------|
| DocumentController + Processor 共享核心 | AudioSource -> clipId -> renderCache 绑定上移到核心层，UI 仅消费 | ✓ |
| UI 持有绑定 | 继续由 PluginEditor 维护 ARA source 与 clip 绑定 | |
| 并行双真相源 | UI 与核心同时维护并互相同步 | |
| You decide | 由 agent 决定 | |

**User's choice:** DocumentController + Processor 共享核心
**Notes:** 明确拒绝并行结构和双真相源。

---

## replace 语义（READ-03 / STAB-01 核心）

| Option | Description | Selected |
|--------|-------------|----------|
| 保留 clipId + 复用 renderCache | replace 仅更新音频与受影响范围，clip 身份稳定 | ✓ |
| 新建 clipId + 新建 cache | replace 等价删除再导入 | |
| clipId 保留但重建 cache | 语义稳定但仍触发全量失效 | |
| You decide | 由 agent 决定 | |

**User's choice:** 保留 clipId + 复用 renderCache
**Notes:** 用户明确确认：replace 必须保留原 clipId 并复用原 renderCache，仅做范围失效，不允许重建 cache。

---

## cache 生命周期策略

| Option | Description | Selected |
|--------|-------------|----------|
| 绑定内常驻，删除时统一释放 | source/clip 有效期内复用 cache，删除事件统一清理 | ✓ |
| 每次 content 变化重建 | 以“简单实现”换取稳定性损失 | |
| 并行旧新 cache 切换 | 双 cache 过渡后再回收 | |
| You decide | 由 agent 决定 | |

**User's choice:** 绑定内常驻，删除时统一释放
**Notes:** 不允许兼容层或并行旧新结构。

---

## 失效收敛策略

| Option | Description | Selected |
|--------|-------------|----------|
| 范围失效 | 按 diff/edit 区间失效受影响 chunk，revision 去抖沿用现有机制 | ✓ |
| 全量失效 | 每次替换都全片段重排 pending | |
| 混合策略 | 高频时全量、低频时局部 | |
| You decide | 由 agent 决定 | |

**User's choice:** 范围失效
**Notes:** 目标是抑制 revision storm，保持持续编辑下可回放性。

---

## 删除收敛语义

| Option | Description | Selected |
|--------|-------------|----------|
| 单向收敛删除 | willDestroyAudioSource 触发解绑映射 + 清理 pending + 删除对应 clip | ✓ |
| 仅删 source 映射 | clip 留存等待人工清理 | |
| 软删除 + 后台回收 | 延后清理，保留可恢复层 | |
| You decide | 由 agent 决定 | |

**User's choice:** 单向收敛删除
**Notes:** 必须确保无悬空引用与不可达绑定残留。

---

## The Agent's Discretion

- 映射索引的数据结构（例如单 map 或双向索引）与线程同步粒度。
- 范围失效边界计算细节（diff 与编辑区间合并规则）。
- 诊断日志字段与采样节流策略（以排障可观测为目标）。

## Deferred Ideas

- replace 时重建 renderCache（已拒绝，原因：触发全量失效风暴，违背 STAB-01）。
- UI 层继续维护 ARA source -> clip 绑定（已拒绝，原因：双真相源风险）。
