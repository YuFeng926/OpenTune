# ARA 共享 Store 架构修复 实施计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 MaterializationStore/SourceStore 的所有权从 Processor 提升到 DocumentController，使多个 Processor 实例共享同一份数据，修复 VST3 渲染后干声问题。

**Architecture:** ARA2 标准模式——DocumentController 是 document-scoped 单例，持有所有共享编辑/渲染状态。多个 Processor 实例在 `didBindToARA()` 时 attach 到 DocumentController 的共享 store。Standalone 模式保持 Processor 本地持有不变。

**Tech Stack:** JUCE 7 + ARA2 SDK + C++17

---

## 核心设计

### 所有权变更

**之前：**
```
Processor #1 (UI)          Processor #2 (Audio)
  ├─ sourceStore_ (A)        ├─ sourceStore_ (B)         ← 各自独立
  ├─ materializationStore_ (A)  ├─ materializationStore_ (B)
  ├─ resamplingManager_      ├─ resamplingManager_
  ├─ f0Service_              ├─ f0Service_
  └─ vocoderDomain_          └─ vocoderDomain_

DocumentController
  └─ processor_ → Processor #2 (最后 bind 的覆盖)
```

**之后：**
```
DocumentController (document-scoped 单例)
  ├─ shared sourceStore_
  ├─ shared materializationStore_
  ├─ shared resamplingManager_
  ├─ shared f0Service_
  ├─ shared vocoderDomain_
  └─ chunk render worker (单一实例)

Processor #1 / #2
  ├─ ARA 模式：指向 DocumentController 的共享 store
  └─ Standalone 模式：本地持有（不变）
```

### 关键约束

1. **Standalone 不走 ARA**：`#if JucePlugin_Build_Standalone` 路径完全不受影响
2. **接口不变**：`getMaterializationStore()` 等 getter 签名不变，只是底层指针来源不同
3. **生命周期**：DocumentController 比任何 Processor 长寿（ARA2 保证）
4. **线程安全**：MaterializationStore 已有 ReadWriteLock，RenderCache 已有 SpinLock

---

## Task 1: DocumentController 持有共享 Store

**Files:**
- Modify: `Source/ARA/OpenTuneDocumentController.h`
- Modify: `Source/ARA/OpenTuneDocumentController.cpp`

**Step 1: 在 DocumentController 中添加共享 store 成员**

在 `OpenTuneDocumentController.h` 的 private 区域添加：

```cpp
// Document-scoped shared state (ARA2 standard: all edit/render state lives here)
std::shared_ptr<SourceStore> sharedSourceStore_;
std::shared_ptr<MaterializationStore> sharedMaterializationStore_;
std::shared_ptr<ResamplingManager> sharedResamplingManager_;
```

添加 public getter：

```cpp
std::shared_ptr<SourceStore> getSharedSourceStore() const { return sharedSourceStore_; }
std::shared_ptr<MaterializationStore> getSharedMaterializationStore() const { return sharedMaterializationStore_; }
std::shared_ptr<ResamplingManager> getSharedResamplingManager() const { return sharedResamplingManager_; }
```

**Step 2: 在 DocumentController 构造函数中创建共享 store**

在 `OpenTuneDocumentController.cpp` 构造函数中：

```cpp
OpenTuneDocumentController::OpenTuneDocumentController(...)
    : ARADocumentControllerSpecialisation(entry, instance)
    , session_(std::make_unique<VST3AraSession>())
    , sharedSourceStore_(std::make_shared<SourceStore>())
    , sharedMaterializationStore_(std::make_shared<MaterializationStore>())
    , sharedResamplingManager_(std::make_shared<ResamplingManager>())
{
    AppLogger::log("ARA: DocumentController created, session owned");
}
```

**Step 3: 编译验证**

Run: `cmake --build build --config Release --target OpenTune_VST3`
Expected: 编译通过，无行为变化

**Step 4: Commit**

```
feat(ARA): add shared store members to DocumentController
```

---

## Task 2: Processor 在 ARA 模式下 attach 共享 Store

**Files:**
- Modify: `Source/PluginProcessor.h`
- Modify: `Source/PluginProcessor.cpp`

**Step 1: 将 store 成员从 unique_ptr 改为 shared_ptr**

在 `PluginProcessor.h` 将：
```cpp
std::unique_ptr<SourceStore> sourceStore_;
std::unique_ptr<MaterializationStore> materializationStore_;
```
改为：
```cpp
std::shared_ptr<SourceStore> sourceStore_;
std::shared_ptr<MaterializationStore> materializationStore_;
```

同时将 `resamplingManager_` 从 `unique_ptr` 改为 `shared_ptr`：
```cpp
std::shared_ptr<ResamplingManager> resamplingManager_;
```

**Step 2: getter 返回类型保持裸指针不变**

`getMaterializationStore()` 等 getter 签名不变（返回裸指针），内部仍然是 `.get()`。确认 `PluginProcessor.h` 里的 getter 无需改动。

**Step 3: 构造函数调整**

在 `PluginProcessor.cpp` 构造函数中，将 `make_unique` 改为 `make_shared`：

```cpp
sourceStore_ = std::make_shared<SourceStore>();
materializationStore_ = std::make_shared<MaterializationStore>();
resamplingManager_ = std::make_shared<ResamplingManager>();
```

这样 Standalone 和 ARA 模式下构造时都先创建本地 store（ARA 模式会在 `didBindToARA` 中被替换）。

**Step 4: didBindToARA() 中 attach 共享 store**

在 `PluginProcessor.cpp` 的 `didBindToARA()` 中，获取 DocumentController 的共享 store 并替换本地的：

```cpp
void OpenTuneAudioProcessor::didBindToARA() noexcept
{
    juce::AudioProcessorARAExtension::didBindToARA();

    if (auto* dc = getDocumentController())
    {
        // ARA2 标准：共享 store 由 DocumentController 持有
        auto sharedSourceStore = dc->getSharedSourceStore();
        auto sharedMaterializationStore = dc->getSharedMaterializationStore();
        auto sharedResamplingManager = dc->getSharedResamplingManager();

        if (sharedSourceStore && sharedMaterializationStore && sharedResamplingManager)
        {
            sourceStore_ = sharedSourceStore;
            materializationStore_ = sharedMaterializationStore;
            resamplingManager_ = sharedResamplingManager;
        }

        dc->setProcessor(this);
        AppLogger::log("ARA: didBindToARA - DocumentController connected to processor"
            " sourceStore=" + juce::String::toHexString((uintptr_t)sourceStore_.get())
            + " materializationStore=" + juce::String::toHexString((uintptr_t)materializationStore_.get()));
    }
}
```

**Step 5: 编译验证**

Run: `cmake --build build --config Release --target OpenTune_VST3`
Expected: 编译通过

**Step 6: Commit**

```
feat(ARA): processor attaches to DocumentController shared stores on didBindToARA
```

---

## Task 3: 修复 Processor 析构时误解绑

**Files:**
- Modify: `Source/PluginProcessor.cpp`

**Step 1: 条件解绑**

当前析构函数中 `dc->setProcessor(nullptr)` 会无条件清空 DocumentController 的 processor 指针。如果 Processor #1 先析构，会把 #2 已设置的指针清掉。

修改为 only-if-current：

```cpp
OpenTuneAudioProcessor::~OpenTuneAudioProcessor() {
#if JucePlugin_Enable_ARA
    if (auto* dc = getDocumentController()) {
        if (dc->getProcessor() == this)
            dc->setProcessor(nullptr);
    }
#endif
    // ... 其余不变
}
```

**Step 2: 修复 auto-birth 重复执行**

在 `VST3AraSession.cpp` 的 hydration worker 中，auto-birth 当前会用 `processor_.load()` 获取 processor，两个 Processor 都绑定后，可能用不同的 processor 执行 birth。但现在 store 是共享的，所以第二次 birth 会在同一个 store 里发现 materialization 已存在。

需要在 `ensureAraRegionMaterialization` 中加检查：如果 materializationStore 已有该 sourceWindow 对应的 materialization，直接返回已有的。

在 `PluginProcessor.cpp` 的 `ensureAraRegionMaterialization` 函数开头添加：

```cpp
// 共享 store 下防止重复 birth：检查是否已有该 source+window 的 materialization
{
    jassert(materializationStore_ != nullptr);
    const auto existingId = materializationStore_->findMaterializationBySourceWindow(sourceId, sourceWindow);
    if (existingId != 0) {
        AraRegionMaterializationBirthResult result;
        result.sourceId = sourceId;
        result.materializationId = existingId;
        result.materializationRevision = 0;
        result.materializationDurationSeconds = getMaterializationAudioDurationById(existingId);
        AppLogger::log("ARA auto-birth: reuse existing materializationId="
            + juce::String(static_cast<juce::int64>(existingId)));
        return result;
    }
}
```

**Step 3: 在 MaterializationStore 中添加 findMaterializationBySourceWindow**

**Files:**
- Modify: `Source/MaterializationStore.h`
- Modify: `Source/MaterializationStore.cpp`

```cpp
// .h 中添加 public 方法
uint64_t findMaterializationBySourceWindow(uint64_t sourceId, const SourceWindow& window) const;

// .cpp 中实现
uint64_t MaterializationStore::findMaterializationBySourceWindow(uint64_t sourceId, const SourceWindow& window) const
{
    const juce::ScopedReadLock readLock(lock_);
    for (const auto& [id, entry] : materializations_) {
        if (entry.isRetired_) continue;
        if (entry.sourceId == sourceId
            && std::abs(entry.sourceWindow.sourceStartSeconds - window.sourceStartSeconds) < 0.001
            && std::abs(entry.sourceWindow.sourceEndSeconds - window.sourceEndSeconds) < 0.001) {
            return id;
        }
    }
    return 0;
}
```

**Step 4: 编译验证**

Run: `cmake --build build --config Release --target OpenTune_VST3`
Expected: 编译通过

**Step 5: Commit**

```
fix(ARA): conditional processor detach + prevent duplicate auto-birth with shared store
```

---

## Task 4: 共享 chunk render worker（防止两个 Processor 各跑一个）

**Files:**
- Modify: `Source/PluginProcessor.cpp`

**Step 1: ensureChunkRenderWorkerStarted 只在第一个绑定的 Processor 上启动**

当前 `ensureChunkRenderWorkerStarted()` 用 `chunkRenderWorkerRunning_` atomic CAS 防止重复启动。但两个 Processor 各有独立的 `chunkRenderWorkerRunning_`。

修改方案：chunkRenderWorkerThread_ 和相关同步原语也需要共享。但这过于侵入。

**更简单的方案**：让 render worker 的触发通过 DocumentController 路由到一个 Processor。在 `enqueueMaterializationPartialRenderById` 中，如果当前是 ARA 模式，始终通过 DocumentController 获取"主 Processor"来触发。

实际上，因为 `materializationStore_` 已经是共享的，`enqueuePartialRender` 写入的是同一个 store，`hasPendingRenderJobs()` 在任何 Processor 的 worker 上都能看到。所以**只要有一个 Processor 的 worker 在跑就够了**。

当前代码用 `chunkRenderWorkerRunning_` 的 CAS 已经保证每个 Processor 只启动一个 worker。两个 Processor 可能各启动一个 worker，但因为 `schedulerCv_` 是 per-Processor 的，只有触发 `notify` 的那个 Processor 的 worker 会被唤醒。

**最小修复**：在 `enqueueMaterializationPartialRenderById` 中，确保 notify 能到达正确的 worker。因为 store 共享后两个 worker 都能 pull pending jobs，所以问题不大——但可能出现竞争。

**更安全的方案**：只让一个 Processor 跑 worker。在 `didBindToARA` 中记录"我是否是第一个 bind 的"，只有第一个才启动 worker。

```cpp
// PluginProcessor.h 添加
std::atomic<bool> isAraRenderOwner_{false};
```

```cpp
// didBindToARA() 中
if (!dc->hasRenderOwner()) {
    dc->setRenderOwner(this);
    isAraRenderOwner_ = true;
}
```

**但这又开始堆叠了。** 最简单的做法：**不动 worker 架构。** 两个 Processor 都可能启动 worker，但因为 MaterializationStore 是共享的 + `pullNextPendingRenderJob` 有写锁保护，不会出现重复渲染。第二个 worker 只会发现没有 pending job 然后 wait。

所以 **Task 4 不需要改代码**，只需验证并发安全性。

**Step 1: 代码审查确认**

确认 `MaterializationStore::pullNextPendingRenderJob` 用 `ScopedWriteLock` 保护，两个 worker 不会拉到同一个 job。已确认（line 639）。

确认 `RenderCache::getNextPendingJob` 也有 SpinLock 保护。已确认。

**Step 2: 无需代码修改，记录决策**

两个 Processor 的 worker 共享同一个 MaterializationStore，`pullNextPendingRenderJob` 的写锁保证互斥。不需要额外同步。

---

## Task 5: 清理诊断日志

**Files:**
- Modify: `Source/Inference/RenderCache.cpp`
- Modify: `Source/PluginProcessor.cpp`
- Modify: `Source/ARA/OpenTunePlaybackRenderer.cpp`

**Step 1: 移除 Task 之前添加的临时诊断日志**

将 Task 之前为调查根因添加的限频日志全部删除：
- `RenderCache.cpp` 中的 `OverlayTrace:` 日志
- `PluginProcessor.cpp` 中的 `RenderTrace: addResampledChunk OK` 和 `upsampleForHost returned EMPTY` 日志
- `OpenTunePlaybackRenderer.cpp` 中的 `PlaybackTrace:` 日志

保留 `PluginProcessor.cpp` line 16 的 `#include <atomic>`（如果有其他代码依赖）。

**Step 2: 编译验证**

Run: `cmake --build build --config Release --target OpenTune_VST3`

**Step 3: Commit**

```
chore: remove temporary diagnostic logs from ARA render path investigation
```

---

## Task 6: 端到端验证

**Step 1: 编译 VST3**

```
cmake --build build --config Release --target OpenTune_VST3
```

**Step 2: 部署并测试**

1. 在宿主中加载插件
2. 导入音频
3. 提取 F0
4. 按 AUTO 渲染
5. 验证日志中 `sourceStore=` 和 `materializationStore=` 地址在两个 `didBindToARA` 中一致
6. 播放验证听到湿声
7. 验证时间轴点击后播放头同步

**Step 3: Commit**

```
verify: ARA shared store end-to-end validation passed
```

---

## 风险缓解

| 风险 | 缓解 |
|------|------|
| Processor 析构顺序不确定 | only-if-current 条件解绑 |
| 两个 worker 竞争 | store 内部已有写锁保护 |
| Standalone 回归 | `didBindToARA` 仅 ARA 模式触发；Standalone 用本地 shared_ptr |
| didBindToARA 前访问 store | 构造时已创建本地 store，bind 后替换为共享 store |
| 共享 F0/Vocoder service | 暂不共享，各 Processor 独立初始化（按需后续优化） |
