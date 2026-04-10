# OpenTune 统一选中机制改造任务分解

## 项目概述

**目标：** 将 OpenTune 的选中和编辑机制统一为基于音符的架构。DrawNote 创建标准音符；HandDraw 和 LineAnchor 作为精确音高绘制工具，在已有音符内部修改音高曲线。所有音符使用统一的选中、参数调整和渲染流程。

**核心理念：**
- **DrawNote：** 创建音符
- **HandDraw / LineAnchor：** 精确音高绘制（只在已有音符内部生效，自动截断超出音符区域的部分）

---

## 架构变化总览

### 当前架构（三轨制）
```
┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│  音符选中系统     │  │  HandDraw 系统   │  │  LineAnchor 系统 │
│  Note.selected   │  │  直接修改 F0     │  │  selectedLine    │
│                  │  │  无选中概念       │  │  AnchorSegmentIds│
└──────────────────┘  └──────────────────┘  └──────────────────┘
```

### 目标架构（统一）
```
┌─────────────────────────────────────────────────────┐
│              统一的音符选中系统                       │
│                                                     │
│  DrawNote 创建标准音符                               │
│  HandDraw / LineAnchor 在已有音符内部精确绘制音高     │
│  所有音符都可以选中、编辑、调整参数                    │
│  统一的修正算法和渲染流程                             │
└─────────────────────────────────────────────────────┘
```

### HandDraw / LineAnchor 工具行为模型
```
用户绘制范围:      |========================|
音符A:        [--------]
音符B:                    [--------]
音符C:                                  [------]
                                                  (空白)

实际生效范围:  [--------]   [--------]      [------|
                                            ↑ 超出音符区域的部分被截断

规则:
  1. 根据绘制范围自动确定覆盖的音符（无需预先选中）
  2. 只有落在音符区域内的绘制数据生效
  3. 跨音符绘制不截断（音符A→B→C 连续生效）
  4. 超出所有音符区域的部分自动截断丢弃
```

---

## 阶段 1：数据结构清理和简化

### 任务 1.1：简化 InteractionState
**文件：** `Source/Standalone/UI/PianoRoll/InteractionState.h`

**删除：**
- `vector<int> selectedLineAnchorSegmentIds`
- `vector<LineAnchor> pendingAnchors`（改为临时绘制状态）

**保留并简化：**
- `SelectionState` - 只保留临时拖拽框状态
- `NoteDragState` - 保持不变
- `NoteResizeState` - 保持不变
- `DrawingState` - 简化，移除 LineAnchor 特定字段

**预期结果：**
- InteractionState 结构更简洁
- 减少约 50 行代码

---

### 任务 1.2：简化 SelectionState
**文件：** `Source/Standalone/UI/PianoRoll/InteractionState.h`

**删除：**
```cpp
bool hasSelectionArea = false;
double selectionStartTime;
double selectionEndTime;
float selectionStartMidi;
float selectionEndMidi;
```

**保留：**
```cpp
// 临时拖拽框（只在拖拽时存在）
bool isDraggingSelectionBox = false;
juce::Point<int> dragStartPos;
juce::Point<int> dragCurrentPos;

// F0 选中范围（可选，用于特殊操作）
int selectedF0StartFrame = -1;
int selectedF0EndFrame = -1;
bool hasF0Selection = false;
```

**预期结果：**
- 选中框只在拖拽时显示，松开后消失
- 选中状态完全由 Note.selected 表示

---

### 任务 1.3：简化 CorrectedSegment::Source 枚举
**文件：** `Source/Utils/PitchCurve.h`

**当前：**
```cpp
enum class Source {
    NoteBased,
    HandDraw,
    LineAnchor,
    AutoTune
};
```

**改为：**
```cpp
enum class Source {
    NoteBased,   // 音符自身修正（DrawNote 创建 + retune/vibrato 参数驱动）
    HandDraw,    // 精确手绘音高覆盖（保留，用于区分渲染逻辑）
    LineAnchor,  // 精确锚点音高覆盖（保留，用于区分渲染逻辑）
    AutoTune     // 自动生成的音符
};
```

**说明：** 保留 `HandDraw` 和 `LineAnchor` 枚举值，因为它们的修正数据本质上是用户手绘的精确 F0 曲线，与 `NoteBased` 由算法（retune speed / vibrato 参数）驱动的修正逻辑不同。保留来源标记有助于：
- 渲染时正确选择处理路径（精确 F0 vs 参数驱动修正）
- 撤销/重做时恢复正确状态
- UI 上区分显示样式（如不同颜色的修正曲线）

---

## 阶段 2：HandDraw 工具改造

### 任务 2.1：修改 HandDraw 工具流程
**文件：** `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp`

**当前流程：**
```
拖拽 → 收集 F0 数据 → 创建 ManualOp → 直接写入 PitchCurve
```

**改造后流程：**
```
拖拽 → 收集 F0 数据 → 自动确定覆盖的音符 → 截断到音符范围 → 写入精确 F0 修正 → 触发渲染
```

**具体步骤：**

1. **保持拖拽采样逻辑不变**
   - `handleDrawCurveTool()` 继续收集 `handDrawBuffer`

2. **修改 `handleDrawCurveUp()`**
   - 计算绘制的时间范围：`drawStartTime`, `drawEndTime`
   - 查找与绘制范围重叠的所有音符（自动确定，无需预先选中）
   - 如果没有覆盖任何音符，忽略本次绘制（可选提示用户）
   - 对每个被覆盖的音符：截取绘制数据中落在该音符 `[startTime, endTime)` 内的部分
   - 将截取后的 F0 数据作为精确修正写入 PitchCurve
   - 触发修正和渲染

3. **新增辅助函数**
   ```cpp
   /** 查找与时间范围重叠的所有音符 */
   std::vector<Note*> findNotesInTimeRange(double startTime, double endTime);

   /** 将手绘 F0 数据截断到指定音符范围，返回截取后的数据 */
   std::vector<float> clipHandDrawToNoteRange(
       const std::vector<std::pair<double, float>>& handDrawBuffer,
       double noteStartTime,
       double noteEndTime
   );
   ```

4. **跨音符绘制支持**
   - 一次连续的手绘操作可能跨越多个音符
   - 对每个音符独立截取并写入修正，但在音符边界处保持绘制曲线的连续性
   - 音符之间的空白区域（不属于任何音符的部分）自动丢弃

**预期结果：**
- HandDraw 不创建音符，只修改已有音符内部的音高
- 无需预先选中音符，根据绘制范围自动确定目标音符
- 跨音符绘制自然支持
- 音符区域外的绘制数据自动截断

**预计工作量：** 2-3 小时

---

### 任务 2.2：实现音符范围自动匹配与截断逻辑
**文件：** `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp`

**实现：**
```cpp
struct ClippedDrawData {
    int noteIndex;         // 目标音符在 NoteSequence 中的索引
    int startFrame;        // 截断后的起始帧（相对于音符）
    int endFrame;          // 截断后的结束帧
    std::vector<float> f0; // 截断后的 F0 数据
};

std::vector<ClippedDrawData> clipDrawDataToNotes(
    const std::vector<std::pair<double, float>>& handDrawBuffer,
    const NoteSequence& notes)
{
    std::vector<ClippedDrawData> result;

    const double drawStart = handDrawBuffer.front().first;
    const double drawEnd   = handDrawBuffer.back().first;

    for (int i = 0; i < notes.size(); ++i) {
        const auto& note = notes[i];

        // 检查音符是否与绘制范围重叠
        if (note.endTime <= drawStart || note.startTime >= drawEnd)
            continue;

        // 计算重叠区间
        const double overlapStart = std::max(note.startTime, drawStart);
        const double overlapEnd   = std::min(note.endTime,   drawEnd);

        // 从 handDrawBuffer 中提取重叠区间的 F0 数据
        // 重采样到 100 fps
        ClippedDrawData clip;
        clip.noteIndex  = i;
        clip.startFrame = timeToFrame(overlapStart);
        clip.endFrame   = timeToFrame(overlapEnd);
        clip.f0         = resampleHandDrawToFrames(
            handDrawBuffer, overlapStart, overlapEnd);

        result.push_back(std::move(clip));
    }

    return result;
}
```

**预期结果：**
- 精确匹配绘制范围与音符范围
- 跨音符绘制时对每个音符独立截取
- 音符之间的空白区域自动跳过

**预计工作量：** 1-2 小时

---

### 任务 2.3：添加用户反馈
**文件：** `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp`

**实现：**
- 绘制过程中实时高亮显示将被影响的音符区域
- 绘制完成后，被修改的音符自动选中（`note.selected = true`）
- 如果绘制范围未覆盖任何音符，可选显示提示
- 立即触发渲染，用户可以听到效果

**预期结果：**
- 用户能直观看到哪些音符会被影响
- 如果不满意，可以 Ctrl+Z 撤销重画

**预计工作量：** 1 小时

---

## 阶段 3：LineAnchor 工具改造

### 任务 3.1：修改 LineAnchor 工具流程
**文件：** `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp`

**当前流程：**
```
点击锚点 → 插值 → 创建 ManualOp → 写入 PitchCurve
```

**改造后流程：**
```
点击锚点 → 插值生成 F0 曲线 → 自动确定覆盖的音符 → 截断到音符范围 → 写入精确 F0 修正 → 触发渲染
```

**具体步骤：**

1. **第一次点击**
   - 记录第一个锚点位置（时间 + 音高）
   - 进入 "Placing Anchors" 模式

2. **第二次及后续点击**
   - 在上一个锚点和新锚点之间线性插值生成 F0 曲线
   - 查找插值范围内重叠的所有音符
   - 对每个音符截取落在其范围内的 F0 数据
   - 将截取后的精确 F0 写入修正
   - 触发渲染
   - 记录新锚点

3. **双击或 Escape**
   - 完成放置
   - 被修改的音符自动选中
   - 清除临时状态

4. **跨音符锚点连线**
   - 两个锚点之间的连线可能跨越多个音符和空白区域
   - 连线中落在音符内的部分生效，落在空白区域的部分丢弃
   - 音符边界处不截断连线的插值逻辑（保证连续性）

**预期结果：**
- LineAnchor 不创建音符，只修改已有音符内部的音高
- 无需预先选中音符
- 跨音符的锚点连线自然支持
- 可以用 Select 工具操作被修改的音符

**预计工作量：** 3-4 小时

---

### 任务 3.2：实现锚点插值与截断机制
**文件：** `Source/Utils/PitchCurve.cpp`, `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp`

**实现：**

1. **锚点之间的 F0 插值**
   ```cpp
   std::vector<std::pair<double, float>> interpolateAnchors(
       const LineAnchor& from,
       const LineAnchor& to,
       int frameRate = 100)
   {
       std::vector<std::pair<double, float>> f0Curve;
       const int numFrames = static_cast<int>(
           (to.time - from.time) * frameRate);

       for (int i = 0; i <= numFrames; ++i) {
           const double t = from.time + i / static_cast<double>(frameRate);
           const float alpha = static_cast<float>(i) / numFrames;

           // 在 log2 空间插值（音高感知是对数的）
           const float logF0 = std::log2(from.pitch) * (1.0f - alpha)
                              + std::log2(to.pitch) * alpha;
           f0Curve.emplace_back(t, std::pow(2.0f, logF0));
       }

       return f0Curve;
   }
   ```

2. **复用 HandDraw 的截断逻辑**
   - 插值生成的 F0 曲线等价于手绘曲线
   - 复用 `clipDrawDataToNotes()` 进行音符范围截断
   - 写入修正的路径与 HandDraw 统一

**预期结果：**
- 锚点之间的插值在 log2 空间进行（符合音高感知）
- 截断逻辑与 HandDraw 共享，减少重复代码

**预计工作量：** 2-3 小时

---

## 阶段 4：删除旧代码

### 任务 4.1：删除 LineAnchor 特定选中方法
**文件：** `Source/Standalone/UI/PianoRollComponent.cpp`, `PianoRollComponent.h`

**删除方法：**
```cpp
int findLineAnchorSegmentNear(int x, int y) const;
void selectLineAnchorSegment(int idx);
void toggleLineAnchorSegmentSelection(int idx);
void clearLineAnchorSegmentSelection();
bool applyRetuneSpeedToSelectedLineAnchorSegments(float speed);
```

**删除回调：**
```cpp
toolCtx.findLineAnchorSegmentNear = ...;
toolCtx.selectLineAnchorSegment = ...;
toolCtx.toggleLineAnchorSegmentSelection = ...;
toolCtx.clearLineAnchorSegmentSelection = ...;
```

**预期结果：**
- 减少约 100-150 行代码
- 简化 PianoRollComponent 接口

**预计工作量：** 1 小时

---

### 任务 4.2：删除 HandDraw 特定方法
**文件：** `Source/Standalone/UI/PianoRollComponent.cpp`, `PianoRollComponent.h`

**删除方法：**
```cpp
bool hasHandDrawCorrectionInRange(int startFrame, int endFrame) const;
```

**预期结果：**
- 进一步简化代码

**预计工作量：** 0.5 小时

---

### 任务 4.3：简化参数调整方法
**文件：** `Source/Standalone/UI/PianoRollComponent.cpp`, `PluginEditor.cpp`

**删除方法：**
```cpp
bool applyRetuneSpeedToSelectionArea(...);
bool applyVibratoToSelectionArea(...);
```

**简化方法：**
```cpp
bool applyRetuneSpeedToSelection(float speed) {
    auto selectedNotes = getSelectedNotes();
    if (selectedNotes.empty()) return false;

    beginEditTransaction("Change Retune Speed");

    for (auto* note : selectedNotes) {
        note->retuneSpeed = speed;
    }

    auto [startFrame, endFrame] = getSelectedNotesFrameRange();
    enqueueCorrection(selectedNotes, startFrame, endFrame);

    commitEditTransaction();
    return true;
}
```

**预期结果：**
- 参数调整逻辑统一，只有一条路径
- 减少约 50-100 行代码

**预计工作量：** 1-2 小时

---

## 阶段 5：选中交互优化

### 任务 5.1：修复选中框持久化
**文件：** `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp`

**当前问题：**
- 选中框在 mouseUp 后消失
- `isSelectingArea` 被清除

**修改：**
```cpp
void PianoRollToolHandler::handleSelectUp(const juce::MouseEvent& e) {
    // 不要清除 hasSelectionArea
    // 只清除拖拽状态
    ctx_.getState().selection.isDraggingSelectionBox = false;

    // 检查框的大小
    double timeDelta = abs(selectionEndTime - selectionStartTime);
    float midiDelta = abs(selectionEndMidi - selectionStartMidi);

    if (timeDelta < 0.01 || midiDelta < 0.5) {
        // 框太小，清除选中
        deselectAllNotes();
    }
    // 否则保持选中框和选中的音符
}
```

**预期结果：**
- 选中框在松开鼠标后持久显示
- 只有在明确取消选中时才消失（Escape、点击空白、切换工具）

**预计工作量：** 1 小时

---

### 任务 5.2：实现明确的取消选中触发
**文件：** `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp`

**实现：**

1. **Escape 键**
   ```cpp
   bool PianoRollToolHandler::keyPressed(const juce::KeyPress& key) {
       if (key == juce::KeyPress::escapeKey) {
           deselectAllNotes();
           ctx_.getState().selection.hasSelectionArea = false;
           ctx_.requestRepaint();
           return true;
       }
       // ...
   }
   ```

2. **点击空白区域**
   ```cpp
   void PianoRollToolHandler::handleSelectDown(const juce::MouseEvent& e) {
       if (!clickedNote && !e.mods.isCtrlDown() && !e.mods.isShiftDown()) {
           deselectAllNotes();
           ctx_.getState().selection.hasSelectionArea = false;
       }
       // ...
   }
   ```

3. **切换工具**
   ```cpp
   void PianoRollComponent::setCurrentTool(ToolId tool) {
       if (tool != currentTool_) {
           deselectAllNotes();
           interactionState_.selection.hasSelectionArea = false;
       }
       currentTool_ = tool;
   }
   ```

**预期结果：**
- 用户有明确的方式取消选中
- 行为符合预期

**预计工作量：** 1 小时

---

### 任务 5.3：统一 updateF0SelectionFromNotes 调用
**文件：** `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp`

**当前问题：**
- Ctrl+Click 和 Shift+Click 后调用
- 普通点击和框选后不调用
- 不一致

**修改：**
- 在所有选中操作后统一调用
- 或者完全移除（如果 F0 选中范围不需要）

**预期结果：**
- F0 选中范围和音符选中同步
- 行为一致

**预计工作量：** 0.5 小时

---

## 阶段 6：撤销/重做简化

### 任务 6.1：统一 UndoAction
**文件：** `Source/Utils/UndoAction.h`, `Source/Utils/UndoAction.cpp`

**当前：**
- `NotesChangeAction` - 音符变化
- `ManualCorrectionAction` - HandDraw/LineAnchor
- 其他特定 Action

**改造后：**
- 保留 `NotesChangeAction` 用于音符创建/删除/移动
- 新增或复用 `PitchDrawAction` 用于 HandDraw/LineAnchor 的精确音高修正
- 删除旧的 `ManualCorrectionAction`

**实现：**
```cpp
// 音符级操作（创建、删除、移动、参数修改）
class NotesChangeAction : public UndoAction {
    std::vector<Note> oldNotes_;
    std::vector<Note> newNotes_;

    void undo() override {
        replaceNotes(oldNotes_);
        triggerCorrection();
    }

    void redo() override {
        replaceNotes(newNotes_);
        triggerCorrection();
    }
};

// 精确音高绘制操作（HandDraw / LineAnchor 写入的 F0 修正）
class PitchDrawAction : public UndoAction {
    struct PerNoteF0 {
        int noteIndex;
        int startFrame;
        std::vector<float> oldF0;  // 旧的修正 F0（可能为空，表示无修正）
        std::vector<float> newF0;  // 新的修正 F0
    };
    std::vector<PerNoteF0> perNoteData_;

    void undo() override {
        for (const auto& d : perNoteData_)
            restoreF0Correction(d.noteIndex, d.startFrame, d.oldF0);
        triggerRender();
    }

    void redo() override {
        for (const auto& d : perNoteData_)
            applyF0Correction(d.noteIndex, d.startFrame, d.newF0);
        triggerRender();
    }
};
```

**预期结果：**
- 撤销/重做逻辑清晰：音符操作与音高绘制操作分离
- 删除旧的 `ManualCorrectionAction`
- HandDraw/LineAnchor 的精确 F0 数据可以正确恢复

**预计工作量：** 2-3 小时

---

## 阶段 7：测试和优化

### 任务 7.1：性能测试
**测试项目：**

1. **HandDraw 延迟测试**
   - 测量从 mouseUp 到渲染完成的时间
   - 目标：< 100ms
   - 如果 > 100ms，显示进度指示器

2. **LineAnchor 连续放置测试**
   - 测试快速点击多个锚点
   - 确保渲染不阻塞 UI

3. **大量音符选中测试**
   - 选中 100+ 音符
   - 测试参数调整的响应时间

**预期结果：**
- 所有操作 < 100ms 响应
- UI 不阻塞

**预计工作量：** 2-3 小时

---

### 任务 7.2：边界情况测试
**测试项目：**

1. **HandDraw 绘制在空白区域**
   - 绘制范围完全不覆盖任何音符
   - 应该忽略或提示"请在音符区域内绘制"

2. **HandDraw 部分覆盖音符**
   - 绘制起点在音符外，中间跨过音符，终点在音符外
   - 只有音符内部的部分生效，两端截断

3. **HandDraw 跨越多个音符和空白**
   - 绘制范围跨越 音符A → 空白 → 音符B → 空白 → 音符C
   - 每个音符内的部分独立生效，空白区域的数据丢弃

4. **LineAnchor 锚点在音符外**
   - 两个锚点都在音符外，但连线经过音符
   - 连线中经过音符的部分应该生效

5. **LineAnchor 锚点重叠**
   - 两个锚点时间相同
   - 应该拒绝或合并

6. **绘制范围极短**
   - 拖拽 < 50ms 或锚点间距极小
   - 应该忽略或提示

7. **音符边界处的连续性**
   - 跨音符绘制时，音符边界处的 F0 值应该连续（不跳变）

**预期结果：**
- 所有边界情况都有合理处理
- 不会崩溃或产生错误数据

**预计工作量：** 2-3 小时

---

### 任务 7.3：用户体验测试
**测试项目：**

1. **HandDraw 精确性**
   - 在音符内部手绘不同的音高曲线
   - 检查生成的 F0 修正是否准确反映绘制轨迹

2. **LineAnchor 连续性**
   - 在多个音符间放置锚点
   - 检查 F0 曲线在音符内部是否平滑，在音符边界是否正确截断

3. **自动音符匹配**
   - 不预先选中任何音符，直接绘制
   - 检查系统是否正确识别了被覆盖的音符

4. **选中交互流畅性**
   - 框选、Ctrl+Click、Shift+Click
   - 检查视觉反馈是否清晰

5. **参数调整即时性**
   - 调整 retuneSpeed、vibrato
   - 检查渲染是否立即触发

**预期结果：**
- 用户体验流畅
- 行为符合预期

**预计工作量：** 3-4 小时

---

## 阶段 8：文档和清理

### 任务 8.1：更新代码注释
**文件：** 所有修改的文件

**更新：**
- 删除过时的注释
- 添加新的架构说明（HandDraw/LineAnchor 为精确音高绘制工具，只在音符内部生效）
- 更新函数文档

**预计工作量：** 1-2 小时

---

### 任务 8.2：更新 AGENTS.md
**文件：** `AGENTS.md`

**更新内容：**
- 统一选中机制的说明
- HandDraw 和 LineAnchor 的新定位：精确音高绘制工具（只修改已有音符内部音高）
- 自动音符匹配与截断机制
- 删除旧的特殊选中系统说明

**预计工作量：** 1 小时

---

## 独立开发单元拆分

上述 8 个阶段可以按依赖关系重组为 5 个独立可交付的工作单元，每个单元完成后都应是可编译、可运行的状态。

### 依赖关系图

```
A（选中交互优化）────────────────────┐
                                     │
B（数据结构清理）──→ C（HandDraw）──→ D（LineAnchor + 删旧代码 + Undo）──→ E（测试/文档）
```

- **A 和 B** 之间无依赖，可以并行开发
- **C** 依赖 B
- **D** 依赖 B + C
- **E** 依赖全部

---

### 工作单元 A：选中交互优化 — 可最先独立开发

**对应阶段：** 阶段 5（任务 5.1、5.2、5.3）

**理由：** 这三个任务都是对现有选中交互行为的修复和统一，不依赖任何数据结构变更或工具改造。它们修改的是 `PianoRollToolHandler` 里已有的选中逻辑（选中框持久化、取消选中触发、F0 选中范围同步），与 HandDraw/LineAnchor 改造完全无关。

**预计工作量：** 2.5 小时

---

### 工作单元 B：数据结构清理 — 可独立开发，建议在 C/D 之前完成

**对应阶段：** 阶段 1（任务 1.1、1.2、1.3）

**理由：** 任务 1.1 和 1.2 清理 `InteractionState`，删除 `selectedLineAnchorSegmentIds` 等字段；任务 1.3 确认 `CorrectedSegment::Source` 枚举的最终形态。这些是后续改造的基础，但它们本身是纯删除/简化操作，可以独立完成并验证编译通过。

**注意：** 删除字段后需要同步修改所有引用点，否则编译会断。建议作为一次原子提交。

**预计工作量：** 2-3 小时

---

### 工作单元 C：HandDraw 工具改造 — 依赖 B

**对应阶段：** 阶段 2（任务 2.1、2.2、2.3）

**理由：** 2.1 改造 HandDraw 流程，2.2 实现截断逻辑（`clipDrawDataToNotes`），2.3 添加用户反馈。这三个任务紧密关联，应作为一个整体交付。其中 2.2 的 `clipDrawDataToNotes` 会被阶段 3（LineAnchor）复用，所以 C 必须在 D 之前完成。

**依赖：** 阶段 1 的数据结构需要先就位（`InteractionState` 中 LineAnchor 相关字段已删除，避免冲突）。

**预计工作量：** 4-6 小时

---

### 工作单元 D：LineAnchor 改造 + 旧代码删除 + 撤销重做统一 — 依赖 B、C

**对应阶段：** 阶段 3（任务 3.1、3.2）+ 阶段 4（任务 4.1、4.2、4.3）+ 阶段 6（任务 6.1）

**理由：**
- 阶段 3（LineAnchor 改造）复用阶段 2 的截断逻辑，必须在 C 之后。
- 阶段 4（删除旧代码）必须在阶段 2 和 3 都完成后才能安全删除旧方法。
- 阶段 6（撤销/重做）与阶段 2、3 的新写入逻辑直接相关（需要新的 `PitchDrawAction` 来支持 HandDraw/LineAnchor 写入的 F0 修正的 undo/redo）。

这些放在一起是因为它们形成一个"收尾"闭环——改完 LineAnchor、删掉旧代码、统一 undo 系统。

**可选进一步拆分：** 如果需要更小的 PR 粒度，可以把 3.1+3.2 单独做一个 PR，4.1+4.2+4.3 做一个 PR，6.1 做一个 PR，但它们之间的间隔不宜太长。

**预计工作量：** 7.5-13 小时

---

### 工作单元 E：测试和文档 — 依赖 A-D 全部完成

**对应阶段：** 阶段 7（任务 7.1、7.2、7.3）+ 阶段 8（任务 8.1、8.2）

**理由：** 集成测试和文档更新必须在所有功能改造完成后进行。

**预计工作量：** 9-13 小时

---

### 建议的开发顺序

| 顺序 | 工作单元 | 说明 |
|------|---------|------|
| 1 | A + B 并行 | 风险最低，立即可以开始 |
| 2 | C | HandDraw 改造，引入核心截断逻辑 |
| 3 | D | LineAnchor 改造 + 清理旧代码 + undo 统一 |
| 4 | E | 全面测试 + 文档 |

---

## 总结

### 预计总工作量
- **工作单元 A（选中交互优化）：** 2.5 小时
- **工作单元 B（数据结构清理）：** 2-3 小时
- **工作单元 C（HandDraw 改造）：** 4-6 小时
- **工作单元 D（LineAnchor + 删旧代码 + Undo）：** 7.5-13 小时
- **工作单元 E（测试/文档）：** 9-13 小时

**总计：** 25.5-37.5 小时（约 3-5 个工作日）

### 关键风险
1. **精度风险：** 手绘曲线截断到音符边界时可能不平滑
   - 缓解：在音符边界处添加短渐变（transition ramp），复用现有的 Stage 5 平滑逻辑

2. **交互风险：** 用户可能在无音符区域绘制，没有任何效果
   - 缓解：实时高亮显示将被影响的音符区域；绘制无效时给出视觉提示

3. **兼容性风险：** 现有项目文件中的 HandDraw/LineAnchor 数据
   - 缓解：需要迁移逻辑或保持向后兼容

### 成功标准
- DrawNote 创建标准音符
- HandDraw / LineAnchor 只修改已有音符内部的音高，不创建新音符
- 绘制到音符区域外的部分自动截断
- 无需预先选中音符，根据绘制范围自动匹配目标音符
- 跨音符绘制自然支持（只截断音符区域外的部分，不在音符边界截断）
- 选中系统统一，只有一套逻辑
- 参数调整总是触发渲染
- 代码减少 200-300 行
- 用户体验流畅，无明显延迟
- 所有边界情况测试通过
