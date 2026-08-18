/**
 * EQ Band Interaction — 动态滤波器列表拖拽交互逻辑
 *
 * 交互规则：
 * - Cut/Shelf/Peak 全部水平拖频率
 * - Shelf/Peak 垂直拖增益；Cut 无增益
 * - 频率范围 Peak 500-12000，其余 20-20000
 * - 增益 ±12 dB
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utils/NoteEqSettings.h"

namespace OpenTune {

class EqGraphRenderer;

class EqBandInteraction {
public:
    EqBandInteraction() = default;

    void setRenderer(const EqGraphRenderer* renderer) { renderer_ = renderer; }

    void startDrag(int filterIndex, juce::Point<float> startPos, const EqSettings& currentSettings);
    EqSettings updateDrag(juce::Point<float> currentPos, const EqSettings& currentSettings) const;
    void endDrag();

    bool isDragging() const { return isDragging_; }

    bool hasDragThreshold(juce::Point<float> startPos, juce::Point<float> currentPos,
                          float threshold = 5.0f) const;

private:
    const EqGraphRenderer* renderer_ = nullptr;
    int dragFilterIndex_ = -1;
    juce::Point<float> dragStartPos_;
    EqSettings dragStartSettings_;
    bool isDragging_ = false;
};

} // namespace OpenTune
