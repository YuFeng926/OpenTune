# Phase 22: Clip-Aware 刷新与集成守护 - Research

**Researched:** 2026-04-15  
**Domain:** PianoRoll live-tree clip-aware repaint 与集成守护  
**Confidence:** HIGH

## User Constraints

无 phase `CONTEXT.md`；本研究直接以当前 live tree、`.planning/*`、`AGENTS.md` 与已完成的 Phase 20/21 产物为约束。[VERIFIED: live tree code]

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| PAINT-01 | 用户看到播放头 strip repaint 时，主层只重绘必要脏区，不恢复第二层播放头方案 | 已定位 `getPlayheadDirtyBounds()` / `updateMainLayerPlayhead()` / `flushPendingVisualInvalidation()` 形成现有 strip dirty contract；Phase 22 需要补齐 renderer 侧 clip-aware 才能让该 contract 真正可承受。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp] |
| PAINT-02 | 可见范围外的音符、波形和 F0 不会在 strip repaint 中被无意义全量遍历，主层绘制保持 clip-aware | 已验证 `drawWaveform()` 与 `drawF0Curve()` 基于 visible range 工作，但 `drawNotes()` 仍全量遍历；Phase 22 核心是把 notes 也收敛到同一 visible-range 语义。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp] |
| FLOW-01 | 继续保持现有播放头投影、continuous/page scroll 和 fit-to-screen 行为 | 已验证 playhead/scroll/sample-authoritative projection 的现有入口仍在 `PianoRollComponent`；现有自动化只覆盖 continuous/page scroll，fit-to-screen 仍缺直接 regression。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp][VERIFIED: Tests/TestMain.cpp] |
</phase_requirements>

## Project Constraints (from AGENTS.md)

- 规划/研究必须先看 `.planning/codebase/` 记忆文档，并对其中提到的 live tree 事实重新用 Glob/Read/Grep 核对。[VERIFIED: AGENTS.md]
- 执行前必须读 `.planning/PROJECT.md`、`.planning/REQUIREMENTS.md`、`.planning/ROADMAP.md`、`.planning/STATE.md`。[VERIFIED: AGENTS.md]
- 中文输出。[VERIFIED: AGENTS.md]
- 不能采用止血、最小改动、防御性编程、兜底、兼容层、并行旧新结构。[VERIFIED: AGENTS.md]
- 必须审查逻辑一致性、架构错配、接口/引用残留、语法风险点。[VERIFIED: AGENTS.md]
- 必须继续守住 UI 隔离：`Source/Standalone/*` 与 `Source/Plugin/*` editor 壳层隔离，`Source/PluginProcessor.*` 为共享核心。[VERIFIED: AGENTS.md]
- Standalone 优先；任何 Phase 22 方案都不能破坏 Standalone 现有行为。[VERIFIED: AGENTS.md]

## Summary

Phase 22 在当前 live tree 上**尚未开始**，因为 Phase 目录不存在、`Tests/TestMain.cpp` 中没有任何 `PAINT_*` / `FLOW_01_*` 回归、`drawNotes()` 仍对整个 note 向量做全量遍历。[VERIFIED: .planning/phases directory][VERIFIED: Tests/TestMain.cpp][VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp]

当前单层/单入口/单 VBlank 的主干已经落地：播放头 strip dirty 仍由 `getPlayheadDirtyBounds()` + `updateMainLayerPlayhead()` 生成，dirty merge/flush 仍由 Phase 20 的 reducer + `FrameScheduler` 单出口完成，scroll/playhead 仍在 Phase 21 的 `onVisualVBlankCallback(...)` 单 tick 中推进。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp][VERIFIED: .planning/phases/20-统一失效入口/20-VERIFICATION.md][VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md]

真正的 live-tree 缺口是：waveform/F0 已经按可见区裁剪，但 notes 还没有；因此 strip repaint 虽然 dirty rect 已窄化，主层内容绘制仍会在 `drawNotes()` 内把所有音符重新扫描一遍，Phase 22 应以**renderer 内部 visible-range helper + note iterator window + fresh closure guards**完成，而不是回退 overlay、editor cadence 或第二套 flush 路径。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp]

**Primary recommendation:** 先把 `PianoRollRenderer` 内部的“可见时间窗”抽成统一 helper，再让 `drawNotes()` 基于已排序且 non-overlapping 的 notes contract 只遍历可见 iterator window，并以新 `PAINT_* / FLOW_01_*` regressions + fresh closure 文档完成收口。[VERIFIED: Source/Utils/Note.h][VERIFIED: Source/PluginProcessor.cpp][VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp]

## Standard Stack

### Core
| Library / Module | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| `PianoRollComponent` + `PianoRollRenderer` | live tree | 单层场景宿主 + 主层渲染路径 | 这是当前 v1.3.1 已收敛后的唯一正确 PianoRoll 绘制结构，Phase 22 不能绕开它另建 overlay/第二层。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp][VERIFIED: .planning/PROJECT.md] |
| `PianoRollVisualInvalidation` + `FrameScheduler` bridge | live tree | dirty merge 与单 flush 出口 | Phase 20 已把 invalidation 收敛到一个 reducer + 一个 scheduler bridge，Phase 22 只能复用这一结构。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp][VERIFIED: .planning/phases/20-统一失效入口/20-VERIFICATION.md] |
| `juce::VBlankAttachment` visual tick | live tree | 唯一视觉时钟 | Phase 21 已把 playhead/scroll/deco/waveform incremental work 收敛到单 tick；Phase 22 只能在该 tick 下优化 painter 成本。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp][VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md] |

### Supporting
| Library / Module | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| `Note` / `normalizeNonOverlapping()` | live tree | 保证 notes 按 `startTime` 排序且无重叠 | `drawNotes()` 做 visible-range iterator window 时应显式建立在这个存储 contract 上。[VERIFIED: Source/Utils/Note.h][VERIFIED: Source/PluginProcessor.cpp] |
| `OpenTuneAudioProcessor::projectClipPlayheadById()` | live tree | sample-authoritative playhead 投影 | 所有 playhead/scroll guard 都应继续从这里投影，不要在 renderer/UI 侧重新发明时间真相源。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp][VERIFIED: .planning/STATE.md] |
| `OpenTuneTests` / `Tests/TestMain.cpp` | live tree | phase regression gate | 当前 Phase 18/20/21 守护都在这里；Phase 22 应继续沿用同一测试入口与 fresh build gate 习惯。[VERIFIED: Tests/TestMain.cpp][VERIFIED: .planning/phases/20-统一失效入口/20-VERIFICATION.md][VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md] |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| 在主层 renderer 内做 visible-range 裁剪 | 恢复 overlay / 第二层播放头 | 与 Phase 19-21 的单层、单入口、单 VBlank 结构直接冲突，属明确 out-of-scope。[VERIFIED: .planning/PROJECT.md][VERIFIED: .planning/REQUIREMENTS.md] |
| 复用现有 invalidation + VBlank 结构 | 在 editor timer / direct repaint 中补性能例外 | 会重新制造第二视觉时钟/第二视觉入口，违反既有 phase 闭环。[VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md] |
| renderer 内统一 visible time window helper | 分别在 component、renderer、tool handler 各自复制 visible-range 算法 | 会制造时间窗语义漂移，后续维护成本高，也更容易破坏 sample-authoritative timeline 行为。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp][ASSUMED] |

## Architecture Patterns

### Recommended Project Structure
```text
Source/Standalone/UI/
├── PianoRollComponent.cpp                 # playhead/scroll/VBlank/invalidation 主循环
├── PianoRoll/PianoRollRenderer.h/.cpp    # renderer 内部 visible-range helper + draw* paths
└── PianoRoll/PianoRollVisualInvalidation.* # dirty merge / flush 决策

Tests/
└── TestMain.cpp                          # PAINT_01 / PAINT_02 / FLOW_01 regressions

.planning/phases/22-Clip-Aware 刷新与集成守护/
├── 22-RESEARCH.md
├── 22-TEST-VERIFICATION.md
└── 22-VERIFICATION.md
```

### Pattern 1: 保持现有 strip dirty contract，不改 flush 架构
**What:** 播放头 dirty area 已由 `getPlayheadDirtyBounds()` 生成，并在 `updateMainLayerPlayhead()` 中合并旧/新位置，再通过 `invalidateVisual(...dirtyArea...)` 进入 Phase 20 reducer。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp]

**When to use:** 任何 Phase 22 绘制优化都必须建立在这个 contract 之上，只优化 dirty 区域里“画什么/遍历什么”，而不是改 dirty 入口或改单 VBlank cadence。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp][VERIFIED: .planning/REQUIREMENTS.md]

**Example:**
```cpp
// Source: Source/Standalone/UI/PianoRollComponent.cpp
juce::Rectangle<int> dirtyArea = getPlayheadDirtyBounds(playheadTimeSeconds);
if (lastPaintedPlayheadTime_ >= 0.0)
    dirtyArea = dirtyArea.getUnion(getPlayheadDirtyBounds(lastPaintedPlayheadTime_));

invalidateVisual(toInvalidationMask(PianoRollVisualInvalidationReason::Playhead),
                 dirtyArea,
                 PianoRollVisualInvalidationPriority::Interactive);
```

### Pattern 2: renderer 内统一“可见时间窗”语义
**What:** `drawWaveform()` 与 `drawF0Curve()` 已在 renderer 内根据 `ctx.xToTime(contentStartX/contentEndX)` 计算 visible range；Phase 22 最正确的结构是把这套计算抽成 renderer 私有 helper，并让 `drawNotes()` 共用，而不是在 component 或测试侧再发明第二套时间窗算法。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp][ASSUMED]

**When to use:** 用于 waveform/F0/notes 三条主内容路径统一 clip-aware 定义，保证 strip repaint 时三类内容都遵守同一内容可见区。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp]

**Example:**
```cpp
// Source pattern: derived from Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp
const int contentStartX = ctx.pianoKeyWidth;
const int contentEndX = ctx.width;
const double visibleStartTime = ctx.xToTime(contentStartX);
const double visibleEndTime = ctx.xToTime(contentEndX);
const double visibleClipStart = visibleStartTime - ctx.trackOffsetSeconds;
const double visibleClipEnd = visibleEndTime - ctx.trackOffsetSeconds;
```

### Pattern 3: notes 使用 iterator window，而不是 full scan
**What:** `normalizeNonOverlapping()` 会把 notes 按 `startTime` 排序并消除重叠；因此 `drawNotes()` 可以先通过 `lower_bound` 找到第一个可能命中可见区的 note，再迭代到 `note.startTime >= visibleClipEnd` 为止。[VERIFIED: Source/Utils/Note.h][VERIFIED: Source/PluginProcessor.cpp][ASSUMED]

**When to use:** `drawNotes()` 的 visible-range 裁剪必须建立在“存储 notes 已排序”这一 live-tree contract 上，否则只是在 full scan 里多加 if，无法兑现 Phase 22 的真正性能目标。[VERIFIED: Source/Utils/Note.h][VERIFIED: Source/PluginProcessor.cpp]

**Example:**
```cpp
// Source contract: Source/Utils/Note.h + Source/PluginProcessor.cpp
auto first = std::lower_bound(notes.begin(), notes.end(), visibleClipStart,
    [](const Note& note, double time) { return note.startTime < time; });
if (first != notes.begin()) {
    auto prev = std::prev(first);
    if (prev->endTime > visibleClipStart)
        first = prev;
}

for (auto it = first; it != notes.end() && it->startTime < visibleClipEnd; ++it) {
    // draw only visible notes
}
```

### Anti-Patterns to Avoid
- **在 `drawNotes()` 里继续 full scan 再 `if (x2 < left || x1 > right) continue`:** 这仍然会为 strip repaint 遍历全部音符，不能满足 `PAINT-02`。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp][ASSUMED]
- **把 visible-range 裁剪放回 `PianoRollComponent::paint()`:** 会让 component 重新承担内容级筛选逻辑，破坏 renderer 单一职责。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp][ASSUMED]
- **为性能恢复 overlay 或第二 cadence:** 明确违反 milestone 目标与 out-of-scope。[VERIFIED: .planning/PROJECT.md][VERIFIED: .planning/REQUIREMENTS.md]
- **在 editor shell 新增 `pianoRoll_.invalidateVisual(...)` / `onVisualVBlankCallback(...)` / `requestInvalidate(pianoRoll_)`:** 会直接打破 Phase 21 已完成的边界。[VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md]

## Live-Tree Truths Required for Completion

### `PAINT-01` 完成所需 truths
1. 播放头 dirty 仍由 `getPlayheadDirtyBounds()` + `updateMainLayerPlayhead()` 生成，且进入的是 interactive dirty-area invalidation，不是 full repaint。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp]
2. `flushPendingVisualInvalidation()` 仍是唯一 flush bridge，并把 dirty rect 交给 `FrameScheduler`，没有恢复 overlay 或第二重绘入口。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp][VERIFIED: .planning/phases/20-统一失效入口/20-VERIFICATION.md]
3. `PianoRollComponent` 仍无 `PlayheadOverlayComponent` child，主层播放头 contract 不回退。[VERIFIED: Tests/TestMain.cpp]
4. strip dirty 之下主层内容绘制已变成 clip-aware，播放头窄 dirty 不再隐含全量 note traversal。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp][ASSUMED]

### `PAINT-02` 完成所需 truths
1. `drawWaveform()` 继续只按当前可见时间窗访问 mipmap peaks。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp]
2. `drawF0Curve()` 继续只按 visible frame range 迭代，并保留 selection/visibleMask 语义。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp]
3. `drawNotes()` 不再全量遍历 `notes`；它必须先求 visible note window，再只迭代 window 内 notes。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp][ASSUMED]
4. notes 的 visible-range 计算必须基于与 waveform/F0 相同的 renderer visible time window，而不是额外时间基线。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp][ASSUMED]

### `FLOW-01` 完成所需 truths
1. playhead 仍经由 `readProjectedPlayheadTime()` 消费 processor 的 `projectClipPlayheadById()`，继续以 sample-authoritative projection 为上游真相源。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp][VERIFIED: .planning/STATE.md]
2. continuous/page scroll 仍只在 `onVisualVBlankCallback(...)` 内推进，不移回 editor cadence 或其他 callback。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp][VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md]
3. `fitToScreen()` 仍优先使用 `getActiveClipTimelineProjection(...)` 的投影结果，而不是重新从 renderer 或 UI 层估算时间长度。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp]
4. Phase 18 的 `TIME_01_*` guards 继续保持绿，并补齐 fit-to-screen regression，防止“性能优化成功但 timeline 行为回归”。[VERIFIED: Tests/TestMain.cpp][ASSUMED]

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| 播放头性能压力 | 第二层播放头 overlay | 现有 main-layer playhead + dirty strip + clip-aware renderer | milestone 已明确“性能问题靠 renderer 裁剪解决”。[VERIFIED: .planning/PROJECT.md][VERIFIED: .planning/ROADMAP.md] |
| 多入口重绘 | editor-side direct invalidate / custom repaint side path | 现有 `invalidateVisual(...)` + `flushPendingVisualInvalidation()` | Phase 20 已完成统一入口，Phase 22 不应重新开旁路。[VERIFIED: .planning/phases/20-统一失效入口/20-VERIFICATION.md] |
| 第二视觉时钟 | editor timer / heartbeat / component timer | 现有 `juce::VBlankAttachment` | Phase 21 已证明单 VBlank 闭环。[VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md] |
| notes 可见区判定 | component 侧 ad-hoc if/else 或测试专用时间映射 | renderer 内部统一 visible time window helper | 避免 waveform/F0/notes 的 visible-range 语义漂移。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp][ASSUMED] |

**Key insight:** Phase 22 不是“再找一条更快的重绘路径”，而是让 Phase 19-21 已正确建立的主层路径变得可承受；所以只能优化 renderer 的可见区遍历边界，不能增加任何新层、新入口、新时钟。[VERIFIED: .planning/PROJECT.md][VERIFIED: .planning/ROADMAP.md]

## Common Pitfalls

### Pitfall 1: 只把 dirty rect 做窄，但 `drawNotes()` 仍 full scan
**What goes wrong:** strip repaint 看似只刷窄条，但主层仍为每帧扫描全部 notes，性能瓶颈没有真正消失。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp]
**Why it happens:** 把 Phase 22 误解成 repaint rect 优化，而不是内容遍历优化。[ASSUMED]
**How to avoid:** 先建立 renderer visible time window helper，再让 notes 走 iterator window。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp][ASSUMED]
**Warning signs:** `drawNotes()` 仍以 `for (const auto& note : notes)` 起手；`PAINT_02_*` tests 缺席。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp][VERIFIED: Tests/TestMain.cpp]

### Pitfall 2: 为了补性能把逻辑抬回 editor 或 overlay
**What goes wrong:** playhead/scroll/timeline 又出现第二视觉层或 editor cadence residue。[VERIFIED: .planning/PROJECT.md][VERIFIED: .planning/REQUIREMENTS.md]
**Why it happens:** 试图绕开 renderer 成本，而不是在主层解决它。[ASSUMED]
**How to avoid:** 把 Phase 22 明确限定在 `PianoRollRenderer` + `PianoRollComponent` 主层契约 + tests/closure docs，不扩张到 editor 重新调度。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp][VERIFIED: .planning/ROADMAP.md]
**Warning signs:** 出现新的 `pianoRoll_.invalidateVisual(...)`、`pianoRoll_.onVisualVBlankCallback(...)`、`requestInvalidate(pianoRoll_)` 审计命中。[VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md]

### Pitfall 3: 忘记守 continuous/page/fit-to-screen 语义
**What goes wrong:** repaint 优化后 scroll/playhead/timeline 看起来偶尔跳页、对不齐或 fit-to-screen 起点错误。[ASSUMED]
**Why it happens:** 只测 painter，不测 sample-authoritative timeline 行为。[ASSUMED]
**How to avoid:** 保留 Phase 18/19/20/21 retained guards，并新增 `FLOW_01_PianoRollFitToScreenUsesClipTimelineProjection` 一类回归。[VERIFIED: Tests/TestMain.cpp][ASSUMED]
**Warning signs:** planner 只列 painter tests，没有 retained `TIME_01_*` / `LAYER_01_*` / `INVAL_*` / `CLOCK_*` gate。[VERIFIED: Tests/TestMain.cpp][ASSUMED]

## Code Examples

### 当前 strip dirty contract
```cpp
// Source: Source/Standalone/UI/PianoRollComponent.cpp
juce::Rectangle<int> PianoRollComponent::getPlayheadDirtyBounds(double playheadTimeSeconds) const
{
    constexpr int playheadHalfWidth = 10;
    const int playheadX = getPlayheadVisualX(playheadTimeSeconds);
    const int left = juce::jlimit(pianoKeyWidth_, getWidth(), playheadX - playheadHalfWidth);
    const int right = juce::jlimit(pianoKeyWidth_, getWidth(), playheadX + playheadHalfWidth);
    if (right <= left)
        return {};

    return { left, 0, right - left, getHeight() };
}
```

### 当前 F0 visible-range 参考实现
```cpp
// Source: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp
const int contentStartX = ctx.pianoKeyWidth;
const int contentEndX = ctx.width;
double visibleStartTime = ctx.xToTime(contentStartX);
double visibleEndTime = ctx.xToTime(contentEndX);

const double framesPerSecond = ctx.f0SampleRate / static_cast<double>(ctx.hopSize);
const int startFrame = std::max(0, static_cast<int>(std::floor((visibleStartTime - ctx.trackOffsetSeconds) * framesPerSecond)) - marginFrames);
const int endFrame = std::min(static_cast<int>(f0.size()), static_cast<int>(std::ceil((visibleEndTime - ctx.trackOffsetSeconds) * framesPerSecond)) + marginFrames);
```

### 当前 notes 缺口
```cpp
// Source: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp
for (const auto& note : notes)
{
    double noteStartTime = note.startTime + trackOffsetSeconds;
    double noteEndTime = note.endTime + trackOffsetSeconds;
    int x1 = ctx.timeToX(noteStartTime);
    int x2 = ctx.timeToX(noteEndTime);
    // 当前 live tree 到这里仍然已经扫描了所有 note
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| overlay 播放头 + 双层场景 | main-layer playhead + `PianoRollRenderer::drawPlayhead(...)` | Phase 19 完成后 | Phase 22 不允许以性能为理由回退 overlay。[VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md][VERIFIED: Tests/TestMain.cpp] |
| 多入口 repaint | `invalidateVisual(...)` + reducer + single flush bridge | Phase 20 完成后 | Phase 22 必须在单入口上做 painter 优化，而不是重开重绘 API。[VERIFIED: .planning/phases/20-统一失效入口/20-VERIFICATION.md] |
| heartbeat/timer/旁路 cadence | `juce::VBlankAttachment` single visual tick | Phase 21 完成后 | Phase 22 只能优化 tick 内内容成本，不能改 cadence 模型。[VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md] |
| waveform/F0/notes 裁剪不一致 | waveform/F0 已 visible-range，notes 仍未完成 | current live tree | 这正是 Phase 22 唯一核心缺口。[VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp] |

**Deprecated/outdated:**
- 用 overlay 解决 strip repaint 性能。[VERIFIED: .planning/REQUIREMENTS.md]
- 用 editor-side visual cadence 或 direct invalidate 驱动 PianoRoll flush。[VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md]

## Assumptions Log

| # | Claim | Section | Risk if Wrong |
|---|-------|---------|---------------|
| A1 | renderer 内部统一 visible time window helper 是最正确的最终结构 | Architecture Patterns | 若项目更偏好 component 侧裁剪，plan 需要重写文件边界与测试策略 |
| A2 | `drawNotes()` 应利用 notes 已排序且 non-overlapping contract，用 iterator window 替代 full scan | Architecture Patterns | 若未来存储 contract 放宽，binary search helper 需改写 |
| A3 | 需要新增 fit-to-screen regression 才能完整证明 `FLOW-01` | Live-Tree Truths Required for Completion | 若团队接受人工验证，plan 数量可减少，但自动化守护会变弱 |

## Open Questions (RESOLVED)

1. **Phase 22 是否要顺手清理 editor shell 中现有的 `pianoRoll_.repaint()` 调用？**
   - Resolution: **不纳入 Phase 22 主目标。**
   - Final scope decision: Phase 22 只要求 retained audit 继续证明两套 editor shell **不得新增** `pianoRoll_.onVisualVBlankCallback(...)`、`pianoRoll_.invalidateVisual(...)`、`requestInvalidate(pianoRoll_)` 或新的 visual cadence residue；现有少量 `pianoRoll_.repaint()` 不升级为“必须全面清理”的重构课题。[VERIFIED: Source/Standalone/PluginEditor.cpp][VERIFIED: Source/Plugin/PluginEditor.cpp][VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md]
   - Why resolved this way: 这与 `.planning/REQUIREMENTS.md` 的 scope 一致，能够避免 Phase 22 漂移成 editor shell 全面重构，同时继续守住“不新增 cadence / direct invalidate 边界破口”的核心约束。[VERIFIED: .planning/REQUIREMENTS.md]

2. **Phase 22 是否需要额外的 `*-VALIDATION.md` 文档？**
   - Resolution: **不需要。**
   - Final documentation contract: 当前仓库 Phase 20/21 的既有闭环产物是 `*-TEST-VERIFICATION.md` + `*-VERIFICATION.md`，并不存在独立 `*-VALIDATION.md` 约定；Phase 22 应继续沿用同一套 live-tree 文档契约，而不是额外发明新文档类型。[VERIFIED: .planning/phases/20-统一失效入口/20-TEST-VERIFICATION.md][VERIFIED: .planning/phases/20-统一失效入口/20-VERIFICATION.md][VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-TEST-VERIFICATION.md][VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md]

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| CMake | fresh phase build/test closure | ✓ | 3.31.5 | — [VERIFIED: local shell] |
| Node.js | gsd tooling / docs workflow | ✓ | v24.12.0 | — [VERIFIED: local shell] |
| Python | repo tooling fallback | ✓ | 3.13.0 | — [VERIFIED: local shell] |
| MSVC `cl` in current shell | Windows test build | ✗ | — | 需先进入正确 VS Developer shell [VERIFIED: local shell] |
| `VsDevCmd.bat` discovered via `vswhere.exe` | 复用既有 20/21 closure 命令 | ✗ | — | 更新 closure 命令到动态发现版本，或由用户提供构建环境 [VERIFIED: local shell] |

**Missing dependencies with no fallback:**
- 无法在当前 shell 直接确认可用的 MSVC toolchain；这会阻塞之后的 fresh `OpenTuneTests` build closure。[VERIFIED: local shell]

**Missing dependencies with fallback:**
- 既有 phase 文档里的 `VsDevCmd.bat` 绝对路径在本机不存在；planner 可把“修正 build closure command 到 `vswhere.exe` 动态发现版本”纳入 closure plan。[VERIFIED: local shell][ASSUMED]

## Security Domain

### Applicable ASVS Categories

| ASVS Category | Applies | Standard Control |
|---------------|---------|-----------------|
| V2 Authentication | no | N/A for local repaint-only phase.[VERIFIED: .planning/codebase/ARCHITECTURE.md] |
| V3 Session Management | no | N/A for local repaint-only phase.[VERIFIED: .planning/codebase/ARCHITECTURE.md] |
| V4 Access Control | no | N/A for local repaint-only phase.[VERIFIED: .planning/codebase/ARCHITECTURE.md] |
| V5 Input Validation | yes | 保持现有 bounds/clip/window 计算中的 range clamp，不引入越界迭代或负宽度 dirty rect。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp][VERIFIED: Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp] |
| V6 Cryptography | no | N/A for local repaint-only phase.[VERIFIED: .planning/codebase/ARCHITECTURE.md] |

### Known Threat Patterns for current stack

| Pattern | STRIDE | Standard Mitigation |
|---------|--------|---------------------|
| stale repaint / wrong dirty rect causing visual desync | Tampering | 保持单 invalidation reducer、单 VBlank flush、retained regression guards。[VERIFIED: .planning/phases/20-统一失效入口/20-VERIFICATION.md][VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md] |
| iterator window 越界或空区间处理错误 | Denial of Service | 在 visible-range helper 中统一 clamp 到 content bounds / note range，并加纯逻辑 tests。[VERIFIED: Source/Standalone/UI/PianoRollComponent.cpp][ASSUMED] |
| editor shell 越权重新驱动 PianoRoll cadence | Elevation of Privilege | retained L4 审计继续禁止 `requestInvalidate(pianoRoll_)` / `pianoRoll_.onVisualVBlankCallback` / `pianoRoll_.invalidateVisual` 等命中。[VERIFIED: .planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md] |

## Sources

### Primary (HIGH confidence)
- `AGENTS.md` - 项目级工作方式与 Standalone/VST3 UI 隔离约束
- `.planning/PROJECT.md` - v1.3.1 目标、out-of-scope、主张与约束
- `.planning/REQUIREMENTS.md` - `PAINT-01` / `PAINT-02` / `FLOW-01` requirement 定义
- `.planning/ROADMAP.md` - Phase 22 定义与当前 pending 状态
- `.planning/STATE.md` - 当前 milestone 状态与 Phase 22 边界提醒
- `.planning/codebase/ARCHITECTURE.md` - 共享 core / editor shell / UI 隔离架构
- `.planning/codebase/VST3Merge.md` - Standalone/VST3 合并边界背景
- `.planning/phases/20-统一失效入口/20-VERIFICATION.md` - unified invalidation 现有 truth
- `.planning/phases/21-单 VBlank 视觉循环/21-VERIFICATION.md` - single VBlank / shell boundary 现有 truth
- `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h` / `.cpp` - live-tree renderer 事实
- `Source/Standalone/UI/PianoRollComponent.h` / `.cpp` - playhead/scroll/invalidation/VBlank 实现事实
- `Source/Utils/Note.h` - notes 排序与 non-overlap contract
- `Source/PluginProcessor.cpp` - notes 存储前 `normalizeStoredNotes(...)` contract
- `Source/Standalone/PluginEditor.cpp` / `Source/Plugin/PluginEditor.cpp` - shell 边界与现有 repaint residue
- `Tests/TestMain.cpp` - Phase 18/19/20/21 现有自动化 guards 与 Phase 22 缺口

### Secondary (MEDIUM confidence)
- 无

### Tertiary (LOW confidence)
- 无

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - 全部来自 current live tree 与 phase verification 文档。[VERIFIED: live tree code]
- Architecture: HIGH - 关键边界已有 20/21 verification 与代码双重证据。[VERIFIED: live tree code]
- Pitfalls: MEDIUM - pitfall 根因判断有少量结构性推断，但都建立在已验证 live-tree 缺口之上。[VERIFIED: live tree code][ASSUMED]

**Research date:** 2026-04-15  
**Valid until:** 2026-05-15
