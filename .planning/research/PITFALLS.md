# Domain Pitfalls — Milestone v1.1（ARA/VST3 播放-渲染协同重构）

**Domain:** Realtime audio plugin（JUCE + ARA2 + VST3）  
**Researched:** 2026-04-06  
**Scope:** 统一 playback-read 逻辑、ARA 生命周期映射、现有 OpenTune ARA/VST3 代码集成风险

---

## Risk Catalog

> 说明：以下均为“本里程碑特有”风险，不是泛化 C++ 问题。每项含失败模式、根因、后果、置信度。

| Risk ID | Failure Mode（会怎么坏） | Root Cause（为何发生） | Consequence（后果） | Confidence |
|---|---|---|---|---|
| R1 | 在错误线程调用 ARA PlaybackController（尤其 render/audio 线程） | ARA SDK 明确 PlaybackController 接口“可并发但不可在 render-threads 调用” | DAW 传输控制随机失效、宿主拒绝请求、潜在卡顿/死锁 | **HIGH** |
| R2 | `processBlockForARA()` 与自有渲染链路重入/重复执行 | JUCE ARA 扩展语义是“已绑定 ARA 时由其处理并返回 true；false 代表需 fallback”，统一逻辑时容易双路径同时跑 | 重复混音、相位错误、音量翻倍或静音 | **HIGH** |
| R3 | AudioSource 状态 map 在 ARA 回调、延时 Timer、PlaybackRenderer 间并发读写，无同步保护 | `audioSourceStates_` / `audioSourceSyncStates_` 目前是普通 `std::map`，`getAudioSourceState()` 在 renderer 中被读，销毁/更新路径会 erase/写入 | 数据竞争、偶发崩溃、野指针访问 | **HIGH** |
| R4 | 生命周期“悬空回调”：`Timer::callAfterDelay` 捕获 `this` 与 `audioSource*`，对象销毁后仍触发 | 读音频重试逻辑无统一 cancellation token / weak guard | 随机 UAF、在 DAW 关闭工程/删除 item 后崩溃 | **HIGH** |
| R5 | 播放区域映射不完整：忽略 source offset / 只按 playback start 算读取偏移 | 统一 read 逻辑时将 region 映射简化为“playhead - regionStart” | 读错片段、导入错位、编辑与播放不一致 | **HIGH** |
| R6 | 多 region 混音被覆盖（resample 分支 `copyFrom` 覆盖先前内容） | 非重采样分支 `addFrom`，重采样分支 `copyFrom`，行为不一致 | 多段叠加场景只听到最后一个 region 或前面被冲掉 | **HIGH** |
| R7 | 实时线程发生隐式分配/重采样抖动（每块 vector/resample 临时对象） | Renderer 在 processBlock 中逐通道 `upsampleForHost`，易触发堆分配和不可控耗时 | xruns、爆音、P99 回调超时 | **MEDIUM-HIGH** |
| R8 | 架构文档/路线图与代码漂移（例如 PlayheadSync/TimelineSync 在文档存在，源码缺失） | 多轮迁移后未做“契约对账”，里程碑按旧假设继续设计 | 计划阶段误判依赖，开发到中后期才发现缺件返工 | **HIGH** |
| R9 | ARA 对象身份绑定不稳定（以 `ARAAudioSource*` 裸指针做长期键） | Host 可能在编辑操作中重建对象，指针身份不保证跨生命周期稳定 | 旧 binding 命中错误对象，出现“看似成功但数据错源” | **MEDIUM** |
| R10 | 传输状态单向化：UI 触发成功但未建立 host→plugin 状态回读一致性 | 只做 requestStart/Stop/SetPosition 不够，缺少统一状态回流与 revision gate | UI 播放态与 DAW 真实态漂移（按钮亮但宿主未播） | **MEDIUM-HIGH** |

---

## Prevention Strategies

| Risk ID | Prevention（预防策略） | Verification Expectation（必须验证什么） |
|---|---|---|
| R1 | 建立“线程白名单”：PlaybackController 仅 UI/控制线程可调用；在 API 门面层加线程断言（Debug） | L1 静态：禁止在 audio/render 路径调用 request*；L5 DAW 场景：高频点击 transport 不丢命令 |
| R2 | 抽象单一入口 `renderFrame(mode)`：ARA 模式与 fallback 模式互斥；严格以 `processBlockForARA` 返回值驱动分流 | L3 集成：ARA on/off 两套用例波形互斥；L6 回归：同一输入不出现双倍能量峰 |
| R3 | 为 `audioSourceStates_/syncStates` 引入统一并发策略（RW lock 或原子快照容器），禁止无锁跨线程 map 访问 | L2 单测：并发 add/update/destroy + read 无崩溃；TSAN（如可用）无 data race |
| R4 | 所有异步重试改为 token/cancellation 模式；Timer 回调使用弱引用守卫 + 文档控制器销毁时集中取消 | L3 集成：反复加载/删除 ARA item + 立即关闭工程不崩溃 |
| R5 | 把 region 映射升级为“playback range ↔ source range”双向公式（统一函数、统一精度），禁止散落计算 | L2 单测：非零 source offset、变速率、短片段边界；L5 DAW 对齐：点击位置=导入位置 |
| R6 | 统一混音语义：所有分支都做 add/mix，覆盖仅允许在显式清缓冲阶段 | L3 集成：双 region 重叠回放能量符合预期（不被后者覆盖） |
| R7 | 预分配与缓存重采样工作区；实时路径零堆分配（或严格上限）；重采样器按采样率变更生命周期管理 | L4 性能契约：audio callback P99 < 20ms；L6 压力回归：高密度 region 无 xrun |
| R8 | 立项前执行“文档-代码契约审计”（ROADMAP/ARCHITECTURE/STATE vs 源码符号）并冻结 v1.1 基线 | L0 规格完整性：缺件清单与责任 phase 明确；L1 grep 验证关键文件/符号存在性 |
| R9 | 将长期绑定键从裸指针升级为稳定 ID（persistent ID / host ref 封装），并在对象重建时做迁移 | L2 单测：对象重建后 binding 连续；L5 DAW 反复 split/glue item 后仍对齐 |
| R10 | 建立“请求-观测闭环”：UI 发请求后必须等待 host position/play state 回读确认再更新最终 UI 态 | L3 集成：宿主拒绝请求时 UI 不假阳性；L5 REAPER 手动/快捷键操作下状态一致 |

---

## Detection Signals

| Risk ID | 早期信号（日志/现象） | 触发后可观测症状 |
|---|---|---|
| R1 | 日志出现 request* 与 audio callback 时间戳重叠；宿主偶发忽略 transport 请求 | 点击播放偶发无效，或仅在某些 DAW/工程负载下失效 |
| R2 | ARA 模式下仍进入 fallback 渲染计数；同 block 出现双路径命中 | 音量异常、波形翻倍、相位怪异/忽大忽小 |
| R3 | 偶发访问冲突、不可复现崩溃；删除 item 后随机异常 | DAW 运行数分钟后随机闪退或音频线程异常 |
| R4 | 工程关闭/切换后仍有“retry N/10”日志继续输出 | 关闭工程瞬间崩溃，或下一个工程出现旧对象日志 |
| R5 | 导入 clip 起点与 DAW item 起点偏移；边界样本不连续 | 点击某位置读到别处音频，编辑区与听感不同步 |
| R6 | 多 region 重叠时 RMS 与单 region 接近；前段声音消失 | 只听到最后一个片段，前片段被吞掉 |
| R7 | 回调耗时直方图长尾上升；CPU 峰值与 region 数线性恶化 | 高负载爆音、transport 卡顿、离线导出与实时表现差异大 |
| R8 | 计划引用不存在文件/符号（如 PlayheadSync/TimelineSync） | Phase 中途大面积返工、需求与实现口径不一致 |
| R9 | 同一操作后 clip 绑定 revision 不变但内容源已变 | “看起来同步了”但实际读到旧/错源音频 |
| R10 | UI 按钮状态更新快于 host 回读；拒绝请求时无反馈分支 | 按钮显示播放但 DAW 停止，或停止后按钮仍亮 |

---

## Phase Assignment

> 面向 v1.1 的建议 phase（用于 requirements/roadmap/execution planning）。

### Phase A — 契约基线与生命周期建模（先做）
- **处理风险**: R8, R10, R5
- **目标**: 先把“谁驱动谁、状态如何闭环、时间映射公式”定成单一真相。
- **交付**:
  1. 生命周期时序图（Host↔DocumentController↔Renderer↔Editor）
  2. playback/source range 统一映射函数与误差预算
  3. transport 请求-确认状态机（含宿主拒绝分支）

### Phase B — 线程模型与并发安全收敛
- **处理风险**: R1, R3, R4
- **目标**: 消除 map 并发读写与悬空异步回调；明确线程调用边界。
- **交付**:
  1. 状态容器并发改造（锁/快照二选一并全量替换）
  2. 异步重试 cancellation 机制
  3. 线程断言与禁用调用点扫描

### Phase C — 统一 playback-read 引擎重构
- **处理风险**: R2, R5, R6, R9
- **目标**: 让 ARA/fallback 互斥、映射一致、混音一致、绑定稳定。
- **交付**:
  1. 单入口渲染分流器（ARA/fallback）
  2. region/source 映射统一模块
  3. 稳定 ID 绑定与迁移逻辑

### Phase D — Realtime 性能与宿主集成验证
- **处理风险**: R7, R10（回归）, R1（回归）
- **目标**: 在真实 DAW 负载下确认无 xrun、无状态漂移。
- **交付**:
  1. 回调耗时/分配监控（P99、峰值）
  2. REAPER 场景测试矩阵（播放、循环、跳播、删除/重建 item）
  3. 失败信号阈值与回归门禁

---

## Sources

### HIGH confidence（官方/源码）
- ARA SDK 2.2.0 `ARAInterface.h`：PlaybackController 接口线程约束（“may be called concurrently, but not from render-threads”）  
  `VST3 References/ARA_SDK-releases-2.2.0/ARA_API/ARAInterface.h` (lines ~2276-2281, 2293+)
- ARA SDK C++ Library `ARAPlug.h`：`DocumentController::getHostPlaybackController()` 与 renderer/document 生命周期说明  
  `VST3 References/ARA_SDK-releases-2.2.0/ARA_Library/PlugIn/ARAPlug.h` (lines ~1231, 1543-1547)
- JUCE docs `juce::AudioProcessor`：processBlock 实时线程约束、可变 block size、禁止 UI/阻塞操作  
  https://docs.juce.com/master/classjuce_1_1AudioProcessor.html
- JUCE docs `juce::AudioProcessorARAExtension`：`processBlockForARA()` 语义（true=ARA 已处理，false=fallback）  
  https://docs.juce.com/master/classjuce_1_1AudioProcessorARAExtension.html
- JUCE docs `juce::ARAPlaybackRenderer`：realtime 参数语义与 fallback 约定  
  https://docs.juce.com/master/classjuce_1_1ARAPlaybackRenderer.html
- Steinberg VST3 docs：`IAudioProcessor::process` 线程语义、`setProcessing` 可能 UI/processing 线程、`ProcessData` 结构  
  https://steinbergmedia.github.io/vst3_doc/vstinterfaces/classSteinberg_1_1Vst_1_1IAudioProcessor.html  
  https://steinbergmedia.github.io/vst3_doc/vstinterfaces/structSteinberg_1_1Vst_1_1ProcessData.html
- 本仓源码证据：
  - `Source/ARA/OpenTunePlaybackRenderer.cpp`
  - `Source/ARA/OpenTuneDocumentController.cpp`
  - `Source/ARA/OpenTuneDocumentController.h`
  - `Source/PluginProcessor.cpp`
  - `Source/Plugin/PluginEditor.cpp`

### MEDIUM confidence（项目文档一致性）
- `.planning/PROJECT.md`, `.planning/STATE.md`, `.planning/codebase/ARCHITECTURE.md`, `.planning/ROADMAP.md`, `.planning/REQUIREMENTS.md`

### Explicit gap noticed
- 规划文档曾提及 `PlayheadSync/TimelineSync`，当前主工作区 `Source/ARA/` 未发现对应文件（已用 Glob 验证）。该差异已纳入 R8。
