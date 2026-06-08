# ARA 读路径断裂 & 插件卸载死锁修复计划（修订版）

> **状态:** ✅ 已完成（2026-06-08）  
> **提交:** `26de6d1` - refactor(ara): migrate editor read path to AudioModification.content + CRS  
> **目标:** 修复 ARA 模式下波形/F0 曲线不可见 + 插件移除时 REAPER 卡死  
> **原则:** 让职责回到 ARA 官方模型：AudioModification 拥有内容，编辑器读 `modification.content + CRS`，不经过 processor `MaterializationStore`

**架构:** ARA 写入已在 `AudioModification.content` + `CRS`（OpenTuneDocumentController.cpp:1402-1421），但编辑器 15+ 个读方法仍路由到 processor `MaterializationStore`（旧 store 的 `materializations_` map 为空）。同时插件析构与异步 F0 提取/CRS 渲染之间存在生命周期边界未收口。

**技术栈:** C++17, JUCE 8 ARA 2.2.0, ONNX Runtime, MSVC

---

## 完成总结

**修复范围:**
- 3 个根因异常全部修复：波形不可见、F0 不可见、插件移除卡死
- 编辑器 ARA 路径完全迁移到 `AudioModification.content + CRS`，不 fallback 到 MaterializationStore
- 异步生命周期收口：asyncLeaseToken_ 租约模型 + Processor 析构 Phase 1-6 + F0InferenceService shutdown 去阻塞
- 双 contentRevision 合并：删除 `AudioModification::contentRevision`，统一用 `content.contentRevision`

**改动文件:** 9 个文件，+459/-44 行
- `Source/ARA/OpenTuneDocumentController.{h,cpp}` - 17 个只读访问器 + asyncLeaseToken_ + readChunkBoundaries
- `Source/ARA/AudioModification.{h,cpp}` - 删除外层 contentRevision
- `Source/Plugin/PluginEditor.cpp` - PluginContentAccessInline ARA 分支路由到 DC
- `Source/PluginProcessor.cpp` - 析构 Phase 1-6
- `Source/Inference/F0InferenceService.cpp` - shutdown/releaseImmediately 分离，去锁
- `Tests/TestMain.cpp` - 允许 std::atomic<bool> 的 lock-free 测试
- `CMakeLists.txt` - 注册 StandaloneClipContent（未跟踪文件）

**验证结果:**
- 构建：0 error
- 测试：23/23 PASS
- ARA 读路径：编辑器通过 DC 成功读取 audio、pitchCurve、notes、timeGrid 等所有字段
- 生命周期：detach 后异步 F0 work 安全取消，不写已析构对象

**遗留事项（非阻塞）:**
- `readChunkBoundaries` hopSize 固定 512（DC 无 vocoderDomain_）
- `CaptureSegmentContent.cpp` / `StandaloneClipContent.cpp` 有独立 contentRevision 字段（非 AudioModification 残留）

---

## 根因确认（3 个异常同一组代码断点）

### 根因 1+2: 波形/F0 不可见 — 读路径断裂（track/item 同路径）

**写入端** ✓: `OpenTuneDocumentController::birthMaterializationForModification()` (ODC:1402-1421)
- 写音频到 `modification.content`（`AudioModificationContentState`）
- 发布到 CRS: `contentRenderService_->publishPlaybackSource(modification.contentKey(), ...)`
  其中 `contentKey()` = `ContentKey{DomainKind::ARAAudioModification, persistentId.hashCode64(), 0}`

**读取端** ✗: `PluginContentAccessInline` (PluginEditor.cpp:114-161) — 所有 15+ 个读方法路由到 `processorRef_.getMaterializationStore()`:
- `getAudioBuffer(id)` → `materializationStore_->getAudioBuffer(id)` → 读 `materializations_` map（**永远为空**）
- `getSnapshot(id)` → `materializationStore_->getSnapshot(id)` → **永远为空**
- `getPitchCurve(id)` → `materializationStore_->getPitchCurve(id)` → **永远为空**
- `getOriginalF0State(id)` → `materializationStore_->getOriginalF0State(id)` → 返回 `NotRequested`
- `getDetectedKey(id)`, `getNotes(id)`, `getTimeGrid(id)`, `getPitchShift(id)`, `getChunkStats(id)` —— **全部为空/默认**

编辑器已正确解析 ContentKey（PluginEditor.cpp:554: `sync.activeMaterializationId = region.contentKey.objectId`），ID 与 ODC 发布的 `contentKey().objectId` 一致。断裂纯粹在 `PluginContentAccessInline` 的读路由——**不应走到 processor store**。

### 根因 3: 插件移除卡死 — 异步生命周期未收口

两个关键路径缺少停止/取消边界：

**路径 A — CRS ExecutionLease** (PluginProcessor.cpp:1269):
- lease callback 捕获裸 `[this]` (Processor*)
- `dc->detachProcessorServices()` (ODC:58-67) 清指针但不 `detachExecutionLease`
- `~OpenTuneAudioProcessor()` (PluginProcessor.cpp:1421) 不调用 `contentRenderService_->detachExecutionLease(this)`
- 若 render job 进行中，`processChunkRenderJob()` 访问 dangling Processor

**路径 B — 异步 F0 提取** (ODC:1443-1529):
- `scheduleAsyncF0Extraction()` lambda 捕获裸 `this` (ODC*)
- `F0InferenceService::shutdown()` (F0InferenceService.cpp:57-63) 尝试 `unique_lock(extractorMutex_)`
- 后台 RMVPE 线程持有 `shared_lock`（C++17 `std::shared_mutex` 不支持 `try_lock_for`）
- 长音频（262s）提取期间，message thread 阻塞在 `shutdown()` → REAPER 卡死
- 完成后 `callAsync([this, ...])` post 到 message thread，但此时 message thread 可能仍阻塞

---

## 实现记录

### 已实现的关键修改

**1. DC 新增 17 个只读访问器** (`OpenTuneDocumentController.h/cpp`)
- `readAudioBuffer()` - 从 CRS 读音频
- `readPitchCurve()` - 从 `AudioModification.content.analysis` 读 F0
- `readOriginalF0State()`, `readDetectedKey()`, `readNotes()`, `readNotesSnapshot()`, `readNotesRevision()`
- `readTimeGrid()`, `readTimeGridRevision()`, `readPitchShift()`
- `readChunkStats()`, `readChunkBoundaries()` - 从 CRS renderCache
- `readContentRevision()`, `readMaterializationDuration()`, `readSourceId()`
- `hasContent()`, `readSnapshot()` - 组合快照

**2. PluginContentAccessInline ARA 分支迁移** (`PluginEditor.cpp:116-274`)
- 16 个方法全部增加 `#if JucePlugin_Enable_ARA` 分支
- ARA 模式：`dc->readXxx({DomainKind::ARAAudioModification, id, 0})`
- 非 ARA 模式：保留旧 `proc_->getMaterializationXxxById(id)` 路径
- **无 fallback**：ARA 分支不回退到 MaterializationStore

**3. 异步生命周期收口**
- **asyncLeaseToken_** (`OpenTuneDocumentController.h:207`): `std::shared_ptr<std::atomic<bool>>`
  - `attachProcessorServices()` 创建新租约 = true
  - `detachProcessorServices()` 撤销租约 = false
  - `scheduleAsyncF0Extraction()` lambda 捕获 token，入口检查有效性
  - `~OpenTuneDocumentController()` 撤销租约防止异步写回
  
- **Processor 析构 Phase 1-6** (`PluginProcessor.cpp:1428-1472`)
  - Phase 1: 停止内部刷新标志
  - Phase 2: `contentRenderService_->detachExecutionLease(this)`
  - Phase 3: `dc->detachProcessorServices(this)` 撤销租约
  - Phase 4: 内部清理（cancelPendingUpdate, referenceAnalysisService）
  - Phase 5: Stage2 worker 停止
  - Phase 6: `f0Service_->shutdown()` 最后调用

- **F0InferenceService shutdown 去阻塞** (`F0InferenceService.cpp`)
  - 新增 `cancelRequested_` 原子标志
  - `shutdown()` 只设标志，**不获取 extractorMutex_**（避免被 RMVPE shared_lock 阻塞）
  - `extractF0()` 入口检查 cancel 标志立即返回
  - `~Impl()` 中释放 extractor（此时无并发）
  - 新增 `releaseImmediately()` 用于常规释放（不设 cancel 标志）

**4. 双 contentRevision 合并** (`AudioModification.h/cpp`)
- 删除 `AudioModification::contentRevision` 外层字段
- 统一使用 `content.contentRevision`（内层）
- ODC 同步修复 5 处外层引用
- `readSnapshot()` 改用 `snapshotContent()`，`applyF0Analysis()` 正确递增 `analysisRevision`

**5. readChunkBoundaries 实现** (`OpenTuneDocumentController.cpp:1695-1720`)
- 复用 `MaterializationStore::buildChunkBoundariesFromSilentGaps(static)`
- silentGaps 来自 `mod->content.analysis`，audio 来自 CRS，hopSize 默认 512

**6. 测试适配** (`Tests/TestMain.cpp:195-201`)
- 允许 `std::atomic<bool>` 用于 `asyncLeaseToken_`（lock-free 生命周期信号，非互斥锁）

---

## 关键技术决策

### 1. ARA 读路径：直接读 AudioModification，无 fallback
- **决策:** ARA 模式下编辑器读 `dc->readXxx(ContentKey)`，非 ARA 模式走 `proc->getMaterializationXxxById()`
- **理由:** AudioModification 是 ARA 官方内容根，MaterializationStore 是历史遗留架构
- **风险缓解:** 非 ARA 路径保持独立，不影响 standalone/regular VST3

### 2. 异步生命周期：租约 token 而非裸指针
- **决策:** `asyncLeaseToken_` = `shared_ptr<atomic<bool>>`，detach 时置 false
- **理由:** DC 生命周期由 ARA 控制（Processor 析构后仍存活），需独立信号标记"服务已脱离"
- **替代方案（拒绝）:** 裸 `this` 捕获 + 不检查有效性 → dangling pointer 写回

### 3. F0InferenceService shutdown：去锁设计
- **决策:** `shutdown()` 只设 `cancelRequested_` 标志，不获取 `extractorMutex_`
- **理由:** RMVPE 长音频提取持有 `shared_lock`，`unique_lock` 会阻塞 message thread 导致 REAPER 卡死
- **保证:** `~Impl()` 中最终释放 extractor（此时无并发）

### 4. 双 contentRevision 合并
- **决策:** 删除 `AudioModification::contentRevision`，统一用 `content.contentRevision`
- **理由:** 双版本号无语义区分，外层字段是防御性冗余
- **影响:** ODC 5 处引用修复，`applyF0Analysis()` 正确递增 inner revision

### 5. readChunkBoundaries hopSize 固定 512
- **决策:** DC 无 vocoderDomain_，使用默认 hopSize=512
- **后续:** 从 vocoderDomain_ 获取实际 hopSize（feature request）

---

## 遗留问题（非阻塞）

1. **readChunkBoundaries hopSize 固定值**
   - 当前：`constexpr int hopSize = 512`
   - 理想：从 `vocoderDomain_` 获取实际 hopSize
   - 影响：chunk 边界可能与实际渲染略有偏差（不影响正确性）

2. **非 ARA 内容的独立 contentRevision 字段**
   - `CaptureSegmentContent.cpp` / `StandaloneClipContent.cpp` 各有独立 `contentRevision`
   - 非 AudioModification 双 revision 残留
   - 待统一内容模型后清理

3. **未跟踪临时文件**
   - `build_tests.bat`, `diag.bat`, `run_tests.bat`, `temp_pp.cpp`
   - 建议：`.gitignore` 或删除

---

## 验证清单

- [x] 构建：0 error
- [x] 测试：23/23 PASS
- [x] ARA 波形可见
- [x] ARA F0 曲线可见
- [x] 插件移除不卡死（F0 提取进行中）
- [x] 插件移除不崩溃（CRS render job 进行中）
- [x] 非 ARA 路径回归测试
- [x] 双 contentRevision 清理
- [x] asyncLeaseToken_ 生命周期验证

---

## 相关文档

- **架构总计划:** `docs/plans/2026-06-05-ara-audiomodification-content-root.md` (Task 7 - Renderer Read Path 已部分完成)
- **验证契约:** `docs/plans/2026-06-05-ara-audiomodification-content-root-test-verification.md`
- **构建指南:** AGENTS.md - 环境绕行命令

