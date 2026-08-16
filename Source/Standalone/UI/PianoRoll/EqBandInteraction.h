/**
 * EQ Band Interaction — 5 锚点拖拽交互逻辑
 *
 * 交互规则（源自契约 §7.5 和 SRC）：
 * - Cut/Shelf/Peak 全部水平拖频率
 * - Shelf/Peak 垂直拖增益；Cut 无增益
 * - 频率范围 Cut/Shelf 20-20000、Peak 500-12000
 * - 增益 ±12 dB
 * - Q 只读 2.0
 * - 点击 anchor 未形成拖拽时弹数值输入
 * - 禁止滚轮 Q、右键菜单、band 多选、band 增删、逐段 bypass
 *
 * 拖拽映射：anchor 直接跟随鼠标坐标
 * - Cut: xToFreq(mouseX), Y 只读
 * - Shelf/Peak: xToFreq(mouseX) + yToGain(mouseY)
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utils/NoteEqSettings.h"

namespace OpenTune {

class EqGraphRenderer;

class EqBandInteraction {
public:
    // 频段类型枚举（对应 EqSettings 5 段）
    enum class BandType { LowCut, LowShelf, Peak, HighShelf, HighCut };

    EqBandInteraction() = default;

    // 设置 renderer 引用用于坐标映射（无拥有权）
    void setRenderer(const EqGraphRenderer* renderer) { renderer_ = renderer; }

    // 获取频段类型
    static BandType bandType(int bandIndex);

    // 拖拽起始
    void startDrag(int bandIndex, juce::Point<float> startPos, const EqSettings& currentSettings);

    // 拖拽更新（使用 renderer 坐标映射直接锚定鼠标位置）
    EqSettings updateDrag(juce::Point<float> currentPos, const EqSettings& currentSettings) const;

    // 拖拽结束
    void endDrag();

    // 是否正在拖拽
    bool isDragging() const { return isDragging_; }

    // 判断是否形成了有效拖拽（位移超过阈值）
    bool hasDragThreshold(juce::Point<float> startPos, juce::Point<float> currentPos,
                          float threshold = 5.0f) const;

private:
    const EqGraphRenderer* renderer_ = nullptr;
    int dragBandIndex_ = -1;
    juce::Point<float> dragStartPos_;
    EqSettings dragStartSettings_;
    bool isDragging_ = false;
};

} // namespace OpenTune
