# Architecture Patterns (Milestone v1.1)

**Domain:** OpenTune brownfield refactor (Standalone + VST3/ARA)  
**Researched:** 2026-04-06  
**Scope:** authoritative playback-read path, deterministic AudioSource↔clipId↔renderCache lifecycle, UI overlay from render stats, revision invalidation anti-storm

## Current Constraints

1. **编译边界不可破坏（硬约束）**
   - `Source/PluginProcessor.*` 是双格式共享核心。
   - `Source/Standalone/PluginEditor.*` 与 `Source/Plugin/PluginEditor.*` 必须继续完全隔离。
   - ARA 仅在 `JucePlugin_Enable_ARA` 下生效；Standalone 不引入 ARA 依赖路径。

2. **当前播放读路径分裂（根因）**
   - 普通播放：`OpenTuneAudioProcessor::processBlock()` 直接读 `RenderCache::readAtTimeForRate()`，miss 时回退 dry。
   - ARA 播放：`OpenTunePlaybackRenderer::processBlock()` 当前直接读 `AudioSourceState::audioBuffer`（dry），未走 `RenderCache`。
   - 结果：同一 clip 在 ARA 与普通播放可能输出不同音频语义。

3. **生命周期映射现在偏 UI 驱动，不是内核驱动**
   - `PluginEditor` 中的 `AraImportedClipBinding` 维护 `audioSource + clipId + revisions`。
   - 该绑定随 Editor 生命周期存在，关闭/重建编辑器后需重建上下文，权威性不足。

4. **Revision 机制已有基础，但缺少“事务级”去抖**
   - `RenderCache::requestRenderPending()` 每次调用都会 `++desiredRevision`。
   - `completeChunkRender()` 对 stale revision 会 requeue（正确），但高频编辑仍可能造成 revision 风暴和日志/调度抖动。

5. **官方 API 约束（JUCE ARA）**
   - `AudioProcessorARAExtension::processBlockForARA()` 是 ARA 角色调度入口。
   - `ARADocumentControllerSpecialisation::doCreatePlaybackRenderer()` 是 PlaybackRenderer 注入点。
   - `ARAPlaybackRenderer` 负责输出块渲染，可返回 false 触发非 ARA fallback（需谨慎）。

## Target Architecture

### 目标原则

**一条读路径、一个生命周期真相源、一个渲染状态源。**

### 目标结构（增量改造，不推翻现有）

```text
UI(Editors)
  └─ OverlayStateAdapter (new, UI-only)

PluginProcessor (shared core)
  ├─ ClipRegistry (new, authoritative mapping state)
  ├─ PlaybackReadService (new, authoritative read path)
  ├─ RenderInvalidationCoalescer (new, anti-storm)
  └─ RenderCache / chunkRenderWorkerLoop (existing, modified)

ARA Layer (JucePlugin_Enable_ARA)
  ├─ OpenTuneDocumentController (modified: report source lifecycle to ClipRegistry)
  └─ OpenTunePlaybackRenderer (modified: delegate to PlaybackReadService)
```

### 关键决策（明确取舍）

1. **保留 ARA 的 playback renderer 角色，但读取逻辑委托到 Processor 共用服务。**
   - 不在 ARA 层再维护独立“读 dry”实现。
2. **把 AudioSource↔clipId↔renderCache 映射从 Editor 提升到 Processor/ARA 核心。**
   - Editor 只消费状态，不拥有映射真相。
3. **Revision 由“每次编辑即递增”改为“编辑事务窗口内合并递增”。**
   - 目标是保留最终一致性，同时降低 requeue churn。

## Component Changes

### Modified Components

| Component | 修改内容 | 备注 |
|---|---|---|
| `Source/PluginProcessor.h/.cpp` | 新增并托管 `ClipRegistry`、`PlaybackReadService`、`RenderInvalidationCoalescer`；统一对外 read 接口 | 双格式共享核心，不加 UI 分支 |
| `Source/ARA/OpenTunePlaybackRenderer.h/.cpp` | 删除直接读 `AudioSourceState::audioBuffer` 逻辑，改为调用 Processor 统一 read API | 保留 ARA renderer 壳层 |
| `Source/ARA/OpenTuneDocumentController.h/.cpp` | 在 audio source/region 更新点维护 source revision + range，并通知 `ClipRegistry` 同步 | 只在 ARA 编译下 |
| `Source/Inference/RenderCache.h/.cpp` | 增加“可选批量/合并失效”接口（如 begin/end invalidation batch 或 merge window） | 不改底层 chunk 状态机语义 |
| `Source/Standalone/PluginEditor.*` | Overlay 改为只读 `RenderWorkSnapshot`（不推导业务状态） | 保持 Standalone UI 独立 |
| `Source/Plugin/PluginEditor.*` | 同上；移除 `AraImportedClipBinding` 的“权威映射职责”，改成展示态缓存 | 保持 VST3 UI 独立 |

### New Components

| Component | 责任 | 所在边界 |
|---|---|---|
| `Source/Playback/PlaybackReadService.h/.cpp` | `readClipSegment(...)`：统一“rendered-first, dry-fallback”读策略；供普通播放和 ARA renderer 共用 | Core（双格式共享） |
| `Source/Playback/ClipRegistry.h/.cpp` | 权威维护 `audioSourceRef -> clipId -> renderCacheRef -> mapping/content revisions` 生命周期图 | Core + ARA桥接 |
| `Source/Inference/RenderInvalidationCoalescer.h/.cpp` | 合并高频编辑失效请求（按 clip + 时间窗）后再触发 `requestRenderPending` | Core（调度前） |
| `Source/UI/RenderWorkSnapshot.h`（或放 Processor 公共头） | 统一输出 overlay 所需最小状态：`hasActiveWork / pending / running / lastFailure` | UI消费模型 |

> 说明：不新建“第二套 Processor”。继续沿用现有 `PluginProcessor` 单核心，避免并行架构。

## Data Flow

### 1) Render request -> cache

1. UI/PianoRoll 编辑触发 `enqueuePartialRenderById(trackId, clipId, start, end)`。
2. `RenderInvalidationCoalescer` 按 `clipId` 聚合时间窗口（短时间内多次编辑合并）。
3. 聚合窗口 flush 后，按 silent-gap chunk 切分并调用 `RenderCache::requestRenderPending(...)`。
4. `chunkRenderWorkerLoop` 拉取 `PendingJob`，计算 mel + f0，提交 vocoder。
5. completion 回调写入 `addChunk/addResampledChunk`，再 `completeChunkRender(...)`。

### 2) cache -> playback（统一读路径）

1. **普通播放**：`PluginProcessor::processBlock()` 对每个 overlap clip 调 `PlaybackReadService::readClipSegment()`。
2. **ARA 播放**：`OpenTunePlaybackRenderer::processBlock()` 同样调 `PlaybackReadService::readClipSegment()`，只负责 region 遍历与时间映射。
3. `PlaybackReadService` 内部策略：
   - 先尝试 `RenderCache`（目标 sampleRate）。
   - miss 或 revision 未发布时回退 dry（与普通播放一致）。
4. 输出保持“同 clip 同时刻同策略”，消除 ARA/非ARA 分叉。

### 3) ARA source lifecycle -> clip lifecycle

1. `OpenTuneDocumentController` 在 `didUpdateAudioSourceProperties`/`didUpdatePlaybackRegionProperties`/`willDestroyAudioSource` 更新 source 状态与 revision。
2. `ClipRegistry` 维护映射状态机：
   - `Unbound` -> `Bound(source, clipId)` -> `Dirty(content|mapping)` -> `Rebound/Destroyed`。
3. `syncImportedAraClipIfNeeded` 从“主动管理映射”降级为“触发同步动作的消费者”，真实映射查询走 `ClipRegistry`。

### 4) playback -> UI status

1. Processor 每个 timer tick 可提供 `RenderWorkSnapshot(clipId)`（来自 `RenderCache::ChunkStats` + 最近 completion）。
2. 两套 Editor 仅根据 snapshot 显示 overlay：
   - `hasActiveWork=true` => 显示“正在渲染中”
   - `hasActiveWork=false && total>0` => 结束 overlay
3. UI 不再自行组合“是否应解锁”的业务规则（减少分叉）。

## Thread-Safety Rules

1. **Audio 线程零阻塞原则不变**
   - 只允许 lock-free / try-lock 读。
   - `PlaybackReadService` 禁止等待 `std::mutex`；对 cache 用 nonBlocking read。

2. **锁分层（严格顺序）**
   - 允许顺序：`tracksLock_(R) -> ClipRegistry lock(R) -> RenderCache spinlock(try/short)`。
   - 禁止反向获取，避免 ABBA 死锁。

3. **生命周期所有权**
   - `clipId`：Processor 分配，单调递增，永不复用。
   - `RenderCache`：`shared_ptr` 挂在 clip；`ClipRegistry` 仅持弱引用或同生命周期引用，不单独拥有。
   - `audioSource` 销毁时必须原子清理 registry 映射，禁止悬垂指针。

4. **ARA 回调与 UI 线程边界**
   - ARA listener 回调只更新轻量状态（revision/range/flags），重任务（重导入、重渲染）通过消息线程/后台队列分发。

5. **Revision 合并规则（防风暴）**
   - 编辑高频期只记录 dirty window，不立即多次递增 chunk revision。
   - flush 时每 chunk 最多 +1 revision。
   - Running chunk 完成后若 stale，仅 requeue 一次最新 revision（沿用现有 `completeChunkRender` 语义）。

## Build Order

> 目标：先“抽读路径”，再“迁移映射真相源”，最后“收口 UI 状态”，每步可回归验证。

1. **Step 1 — 提取 PlaybackReadService（最低风险）**
   - 先让 `PluginProcessor::processBlock()` 使用新服务，但行为与现状完全一致。
   - 回归点：Standalone 播放输出、cache hit/miss 行为不变。

2. **Step 2 — ARA renderer 切换到同一 read 服务**
   - `OpenTunePlaybackRenderer` 改调 `PlaybackReadService`。
   - 回归点：ARA 模式下输出与普通播放策略一致；无崩溃/静音回归。

3. **Step 3 — 引入 ClipRegistry 并上移映射真相源**
   - 先双写（旧 `AraImportedClipBinding` + 新 registry），验证一致后去除旧权威职责。
   - 回归点：编辑器重开、source 变更、source 删除后映射稳定。

4. **Step 4 — 引入 RenderInvalidationCoalescer**
   - 在 `enqueuePartialRender*` 前接入合并层；保留 RenderCache 状态机。
   - 回归点：快速拖拽编辑下 pending/running 波动下降，最终音频一致。

5. **Step 5 — UI overlay 统一为 RenderWorkSnapshot 消费**
   - Standalone/VST3 编辑器分别接线，但只读同一 snapshot 契约。
   - 回归点：overlay 开/关时机与渲染状态一致，不依赖 editor 私有推导。

6. **Step 6 — 清理冗余路径与硬化约束**
   - 删除 ARA dry 直读残留、移除 UI 中生命周期权威逻辑。
   - 增加断言：`JucePlugin_Build_Standalone` 下不得触发 ARA 代码路径。

---

## Confidence & Sources

- **Confidence:** MEDIUM-HIGH（代码现状已验证；ARA/JUCE 接口约束有官方文档支撑）
- **Primary sources:**
  - `Source/PluginProcessor.cpp/.h`（当前播放、渲染、线程模型）
  - `Source/ARA/OpenTuneDocumentController.cpp/.h`（ARA source revision 与播放控制）
  - `Source/ARA/OpenTunePlaybackRenderer.cpp/.h`（当前 ARA 读路径）
  - `Source/Inference/RenderCache.cpp/.h`（revision + chunk 状态机）
  - JUCE docs:
    - `AudioProcessorARAExtension`  
      https://docs.juce.com/master/classjuce_1_1AudioProcessorARAExtension.html
    - `ARAPlaybackRenderer`  
      https://docs.juce.com/master/classjuce_1_1ARAPlaybackRenderer.html
    - `ARADocumentControllerSpecialisation`  
      https://docs.juce.com/master/classjuce_1_1ARADocumentControllerSpecialisation.html
