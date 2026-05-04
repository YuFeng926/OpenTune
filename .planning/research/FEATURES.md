# Feature Landscape（Milestone v1.1）

**Domain:** 既有音频应用（VST3/ARA 渲染-回放一致性增强）  
**Researched:** 2026-04-06  
**Scope声明:** 仅覆盖 v1.1 新能力；不重复 ONNX 调音、RenderCache 基础渲染、基础 ARA 集成、VST3 简化编辑器等已完成能力。

## Feature Categories

本里程碑目标不是“做更多功能”，而是“让用户听到的结果稳定等于当前编辑结果”，聚焦 **VST3 + ARA 路径的 render-to-playback 一致性**。

- **Table Stakes（必须有）**：缺失会直接造成“编辑了但听到不对/不同步”，用户认为产品不可靠。
- **Differentiators（加分项）**：在满足必须项后，提升流畅度、可解释性、专业工作流信任度。
- **Anti-features（本里程碑明确不做）**：避免范围膨胀，保护交付节奏与稳定性。

---

## Table Stakes

| Feature | 用户视角行为（可观察） | 系统视角行为（可验证） | Complexity | 里程碑必要性 |
|---|---|---|---|---|
| 1) 编辑后回放必一致（Edit→Playback Coherence） | 用户改完音高/参数后再次播放，听到的就是最新编辑，不会回到旧渲染内容。 | 建立“编辑状态版本号/脏标记 → RenderCache 失效 → 重新渲染 → 回放切换新缓存”的单向链路；禁止读取过期缓存。 | High | 核心目标，必须。
| 2) DAW 传输控制单一事实源（Transport Authority） | 在 DAW 里播放/暂停/定位，插件表现同步；在插件里点播放头定位，DAW 也跟随。 | 统一走 ARA playback controller（requestSetPlaybackPosition / requestStart / requestStop）；非 ARA 路径不触发这些调用。 | Med | 防“播放头看起来在这、声音却在那”。
| 3) 采样率/时间轴一致（Sample Rate & Timeline Consistency） | 不同项目采样率、跳播、循环播放时，听感速度/音高/对齐不漂移。 | 播放前重采样链路始终生效；时间映射（samples↔seconds↔musical time）在 seek/start/loop 边界保持一致。 | High | 防止“偶发错位/漂移”。
| 4) 回放可用性状态可见（Render Readiness UX） | 用户能看见“正在更新渲染/可播放最新结果”，不再靠猜。 | 暴露最小状态机：Idle / Dirty / Rendering / Ready / Error；UI 只读显示，不引入复杂交互。 | Med | 降低误判“系统坏了”的概率。
| 5) 启停与跳播无明显断裂（Start/Stop/Seek Stability） | 快速暂停-播放、拖动播放头后，声音快速稳定进入正确位置，不出现长时间静音或旧片段串音。 | 在跳播与启停路径做一致的缓存切片选择与边界处理；禁止使用与当前位置不匹配的旧 chunk。 | High | 提升“可用”到“可工作”。 |

**Table Stakes 测试化验收建议（用户可感知）**
- 连续 10 次“编辑→立即播放”，不出现旧内容回放。
- DAW/插件双向 start/stop/seek 操作后，播放位置偏差保持在可接受窗口（例如毫秒级）。
- 切换采样率项目后不出现明显速度/音高异常。

---

## Differentiators

| Feature | 用户价值 | 系统实现焦点 | Complexity | 何时做 |
|---|---|---|---|---|
| A) 局部重渲染优先（Region-aware Re-render） | 大工程下改一小段也能较快听到更新，等待时间明显降低。 | 基于编辑影响范围做 chunk 级增量失效，避免全量重渲染。 | High | Table Stakes 稳定后优先。
| B) 无缝切换到新渲染（Glitch-minimized Swap） | 渲染完成切换时听感更平滑，减少突变感。 | 回放缓冲切换点做安全边界与短过渡策略（不改音色逻辑）。 | Med | 作为体验增强。
| C) 一致性诊断信息（Consistency Diagnostics） | 出现异常时用户/测试能快速知道是“旧缓存”“渲染中”还是“控制不同步”。 | 记录最小可诊断事件：编辑版本、渲染版本、当前播放版本、最后一次 ARA 控制调用。 | Med | 有助于快速定位问题。 |

> 建议：v1.1 至少落地 A（若工期紧，可先做受限版本），B/C 可按风险和时间裁剪。

---

## Anti-features

以下能力 **本里程碑明确不做**（避免范围失控）：

| Anti-feature | 为什么现在不做 | 替代策略（v1.1） |
|---|---|---|
| 1) VST3 多轨/多 Clip 编排能力扩展 | 当前目标是“单 Clip 路径稳定一致”，扩展编排会引入新状态维度。 | 维持现有 VST3 简化编辑器与单 Clip 约束。
| 2) Standalone 架构改造或 UI 行为重构 | 违反“Standalone 优先且不受影响”原则。 | 仅改 VST3/ARA 相关链路，Standalone 行为保持。
| 3) 新模型/新算法（ONNX 推理能力升级） | 这是“音质能力”问题，不是“一致性”问题。 | 固定现有模型，先把渲染-回放一致性做稳。
| 4) 跨 DAW 全面适配优化（一次覆盖所有宿主） | 兼容矩阵过大，会稀释核心问题修复。 | 先以 REAPER 路径打穿并形成验证基线。
| 5) 复杂容错兼容层（并行旧新链路长期共存） | 会制造双路径分叉，增加长期不一致风险。 | 保持单一权威链路，问题在主链路上一次性修正。 |

---

## Dependency Notes

### 功能依赖图（建议顺序）

```text
F2 Transport Authority
  → F3 Sample Rate & Timeline Consistency
    → F1 Edit→Playback Coherence
      → F5 Start/Stop/Seek Stability
        → F4 Render Readiness UX

F1 稳定后：
  → D-A Region-aware Re-render
  → D-B Glitch-minimized Swap
  → D-C Consistency Diagnostics
```

### 关键约束

1. **先统一控制面，再优化渲染面**：未统一传输控制前，任何一致性优化都会被“控制不同步”掩盖。  
2. **先保证正确，再追求快**：先消灭“播放错误内容”，再做局部重渲染与无缝切换。  
3. **严格单路径**：同一动作（播放/定位/取缓存）只能有一个权威执行路径，避免隐性分叉。  
4. **里程碑验收以用户可观察行为为准**：不是“代码有接口”，而是“编辑后听到必正确”。

### 需求作者可直接引用的里程碑结构

1. **一致性基础层**：F2 + F3（先打通控制与时间基准）  
2. **正确性闭环层**：F1 + F5（确保听到的是最新且位置正确）  
3. **可见性与体验层**：F4 + D-A/B/C（状态透明、性能与平滑度优化）

---

**Confidence:** MEDIUM-HIGH（基于项目现状文档与已定义 Phase 5 目标；未引入外部生态假设，结论与当前仓库约束一致）。
