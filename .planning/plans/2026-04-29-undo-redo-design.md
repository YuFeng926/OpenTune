# Undo/Redo 系统设计

## 背景

### 旧系统问题

原 Undo/Redo 系统存在根本性架构缺陷，导致 VST3 插件在 AUTO 操作时崩溃（vector erase iterator outside range）。已被完全删除。

**崩溃根因**：`UndoManager::addAction()` 内部的 `actions_.erase(actions_.begin())` 在 VST3 宿主环境下被并发触发。

**旧系统的问题清单**：

| # | 问题 |
|---|------|
| 1 | commitAutoTuneGeneratedNotes 写入 Store 后，commitTransaction 重新读 Store 来"发现"变化——间接推导，AUTO 明确知道自己改了什么 |
| 2 | UndoManager 内含 Scale+AutoTune 合并逻辑——业务逻辑泄露到容器中 |
| 3 | beginTransaction/commitTransaction 事务系统对 AUTO 这种"明确知道 before/after"的操作是多余的间接层 |
| 4 | addAction 加了 mutex 说明存在多线程调用预期，但 undo 操作应只在 UI 线程执行 |
| 5 | VST3 宿主可能通过自己的 Undo 系统在不同线程触发 undo/redo，与插件内部 UndoManager 冲突 |

---

## 设计决策

| 决策 | 选择 | 原因 |
|------|------|------|
| 核心模式 | Command + Snapshot | 业界主流（Melodyne、Auto-Tune、Logic），简单可靠 |
| 快照粒度 | 全量 notes + correctedSegments | 数据量小（每步 ~50KB），500 步 ≈ 25MB，无压力 |
| 合并策略 | 不合并 | 每个操作独立一条记录，简单明确 |
| 线程模型 | 纯 UI 线程，不加锁 | 业界共识；异步操作完成后通过 callAsync 回 UI 线程再 addAction |
| 容量管理 | 步数限制（500） | 数据量小，不需要内存限制 |
| 递归防护 | isPerformingUndoRedo_ flag | 防止 undo()/redo() 内部的副作用触发 addAction() |
| 变更通知 | onChange_ 回调 | UI 按钮自动感知 canUndo/canRedo 状态变化 |
| ARA 宿主集成 | 宿主 ARA 模型变更时 clear() | 宿主不会直接撤销插件内部编辑，但会 destroy/restore 对象导致引用失效 |

---

## 业界调研结论

### 主流 DAW/插件的 Undo 模式

| 产品 | 模式 |
|------|------|
| Melodyne | Command + Snapshot，不合并，纯 UI 线程，独立于宿主 |
| Auto-Tune | 全量 Snapshot，不合并 |
| Logic Pro | Command Pattern，不合并 |
| JUCE UndoManager | Command Pattern + 可选合并 + 事务分组 |

### ARA2 插件 Undo 与宿主的关系

- **宿主 Ctrl+Z 不会撤销插件内部的音符/曲线编辑**——DAW 完全不知道这些编辑的存在
- 宿主 undo 只通过 ARA 模型图操作（destroy/restore/update）间接影响插件
- 插件内部 UndoManager 必须完全独立
- 唯一需要的防护：宿主通过 ARA 销毁/恢复对象时，清空内部 undo 历史

---

## 架构设计

### UndoAction 基类

```cpp
class UndoAction {
public:
    virtual ~UndoAction() = default;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual juce::String getDescription() const = 0;
};
```

### UndoManager — 纯容器

```cpp
class UndoManager {
public:
    using ChangeCallback = std::function<void()>;

    void addAction(std::unique_ptr<UndoAction> action);
    bool undo();
    bool redo();
    bool canUndo() const;
    bool canRedo() const;
    void clear();

    juce::String getUndoDescription() const;
    juce::String getRedoDescription() const;

    void setOnChange(ChangeCallback cb);

private:
    std::vector<std::unique_ptr<UndoAction>> actions_;
    int cursor_{0};
    static constexpr int maxSize_ = 500;
    bool isPerformingUndoRedo_{false};
    ChangeCallback onChange_;
};
```

**行为规则**：
1. `addAction`：如果 `isPerformingUndoRedo_` 为 true，忽略。否则清除 cursor_ 之后的 action，push_back，超限时 erase(begin()) 并 cursor_--，cursor_ 指向末尾，调 `onChange_`
2. `undo()`：设 `isPerformingUndoRedo_=true`，调 `actions_[--cursor_]->undo()`，恢复 flag，调 `onChange_`
3. `redo()`：同上，调 `actions_[cursor_++]->redo()`
4. `clear()`：清空 vector + cursor=0，调 `onChange_`

---

## Action 分类

### 类型 1：Placement 操作（ArrangementView）

| Action 类 | 快照数据 |
|-----------|---------|
| `SplitPlacementAction` | before: 原 placement + 原 materialization 完整快照; after: 2 个新 placement + 2 个新 materialization 完整快照 |
| `MergePlacementAction` | before: 2 个 placement + 2 个 materialization 完整快照; after: 合并后的 1 个 |
| `DeletePlacementAction` | before: placement + materialization 完整快照; after: 无 |
| `MovePlacementAction` | before/after: `placement.timelineStartSeconds` |
| `GainChangeAction` | before/after: `placement.gain` |

### 类型 2：PianoRoll 编辑（统一 Action）

```cpp
class PianoRollEditAction : public UndoAction {
public:
    PianoRollEditAction(OpenTuneAudioProcessor& processor,
                        uint64_t materializationId,
                        juce::String description,
                        std::vector<Note> oldNotes,
                        std::vector<Note> newNotes,
                        std::vector<CorrectedSegment> oldSegments,
                        std::vector<CorrectedSegment> newSegments);

    void undo() override;  // processor.commitNotesAndPitchCurve(id, oldNotes, oldSegments)
    void redo() override;  // processor.commitNotesAndPitchCurve(id, newNotes, newSegments)
    juce::String getDescription() const override;

private:
    OpenTuneAudioProcessor& processor_;
    uint64_t materializationId_;
    juce::String description_;
    std::vector<Note> oldNotes_, newNotes_;
    std::vector<CorrectedSegment> oldSegments_, newSegments_;
};
```

**覆盖的操作**（全部用同一个类，只是 description 不同）：
- Draw Note → "绘制音符"
- Delete Note → "删除音符"
- Drag Note (pitch) → "移动音符"
- Resize Note → "调整音符长度"
- Hand Draw Curve → "手绘曲线"
- Line Anchor → "线锚点"
- AUTO → "自动调音"
- Retune Speed → "修改调速"
- Vibrato → "修改颤音"

### 类型 3：不进入 Undo 栈

| 操作 | 原因 |
|------|------|
| Select / Select All | 选区状态不是编辑 |
| Scale Change | 仅 UI 局部状态，不修改 Store |

---

## 异步操作的 addAction 时机

对于 AUTO 和其他异步操作（NoteBasedCorrection）：

```
mouseDown / 按钮点击
    │
    ▼
记录 beforeNotes, beforeSegments（UI 线程，同步）
    │
    ▼
Worker 异步执行（生成 notes/segments）
    │
    ▼
Worker 完成 → callAsync 回 UI 线程
    │
    ▼
commitNotesAndPitchCurve() 写入 Store（UI 线程，同步）
    │
    ▼
读取 afterNotes, afterSegments
    │
    ▼
undoManager.addAction(PianoRollEditAction{before, after})（UI 线程，同步）
```

**关键**：addAction 永远在 UI 线程、在数据写入完成后调用。不需要事务系统。

---

## ARA 集成防护

### 宿主 undo 导致模型变更时

```cpp
// OpenTuneDocumentController 或 VST3AraSession 中：
void willDestroyAudioSource(ARAAudioSource* source) {
    // 清理 MaterializationStore 中对该 source 的引用
    // 清空 UndoManager（引用可能失效）
    undoManager_.clear();
}
```

### 需要监听的 ARA 回调

| 回调 | 动作 |
|------|------|
| `willDestroyAudioSource` | 清理引用 + clear undo |
| `willDestroyAudioModification` | 清理引用 + clear undo |
| `willDestroyPlaybackRegion` | 清理引用 + clear undo |
| `doUpdateAudioSourceContent` | 重新读取音频内容，clear undo |

---

## 文件规划

| 文件 | 内容 |
|------|------|
| `Source/Utils/UndoManager.h` | UndoAction 基类 + UndoManager 类定义 |
| `Source/Utils/UndoManager.cpp` | UndoManager 实现 |
| `Source/Utils/PianoRollEditAction.h` | PianoRollEditAction 类定义 |
| `Source/Utils/PianoRollEditAction.cpp` | PianoRollEditAction 实现 |
| `Source/Utils/PlacementActions.h` | Split/Merge/Delete/Move/Gain Action 类定义 |
| `Source/Utils/PlacementActions.cpp` | Placement Action 实现 |

### PluginProcessor 集成

```cpp
// PluginProcessor.h
private:
    UndoManager undoManager_;

public:
    UndoManager& getUndoManager() { return undoManager_; }
```

### UI 集成

```cpp
// PluginEditor 构造函数中：
processor.getUndoManager().setOnChange([this] {
    undoButton_.setEnabled(processor.getUndoManager().canUndo());
    redoButton_.setEnabled(processor.getUndoManager().canRedo());
});
```

---

## 实施流程

### 阶段 1：实现 UndoManager 核心 + PianoRollEditAction

1. 编写 `UndoManager.h/cpp` 和 `PianoRollEditAction.h/cpp`
2. 集成到 `PluginProcessor`
3. 在 PianoRoll 编辑操作中接入 addAction

### 阶段 2：实现 Placement Actions

1. 编写 `PlacementActions.h/cpp`（Split/Merge/Delete/Move/Gain）
2. 在 ArrangementViewComponent 中接入 addAction

### 阶段 3：UI 集成

1. Undo/Redo 按钮 enable/disable 联动
2. 快捷键绑定（Ctrl+Z / Ctrl+Y）

### 阶段 4：测试（test-driven-spec）

使用 `test-driven-spec` 技能设计测试用例并执行验证：
- UndoManager 单元测试（add/undo/redo/clear/递归防护/容量限制/onChange 回调）
- PianoRollEditAction 集成测试（before/after 快照恢复正确性）
- PlacementAction 集成测试（split undo 恢复原始 materialization）
- 异步操作测试（AUTO 完成后 addAction 时机正确）
- ARA 防护测试（模型变更后 undo 历史被清空）

### 阶段 5：Oracle 审查

实现和测试全部完成后，交由 Oracle 审查：
- 架构合理性
- 线程安全
- 内存泄漏风险
- 接口一致性

**注意**：Oracle 意见仅作参考，不全信。其建议倾向防御性编程和兜底冗余，需判断是否损害代码简洁性后再采纳。

### 阶段 6：编译验证

- VST3 + Standalone 双端零错误编译
- 手动验证 AUTO undo/redo 功能正确
