/**
 * EQ Band Interaction — 动态滤波器列表拖拽交互逻辑（实现）
 *
 * 拖拽映射：anchor 直接跟随鼠标位置，使用 filter 的 type 决定约束。
 */

#include "EqBandInteraction.h"
#include "EqGraphRenderer.h"
#include <cmath>
#include <algorithm>

namespace OpenTune {

void EqBandInteraction::startDrag(int filterIndex, juce::Point<float> startPos, const EqSettings& currentSettings)
{
    dragFilterIndex_ = filterIndex;
    dragStartPos_ = startPos;
    dragStartSettings_ = currentSettings;
    isDragging_ = true;
}

EqSettings EqBandInteraction::updateDrag(juce::Point<float> currentPos, const EqSettings& currentSettings) const
{
    if (!isDragging_ || dragFilterIndex_ < 0 || dragFilterIndex_ >= static_cast<int>(currentSettings.filters.size()))
        return currentSettings;

    EqSettings result = dragStartSettings_;
    auto& f = result.filters[dragFilterIndex_];
    const double freq = renderer_->xToFreq(currentPos.getX());

    switch (f.type)
    {
    case EqFilterType::LowCut:
    case EqFilterType::HighCut:
        f.frequencyHz = static_cast<float>(std::clamp(freq, 20.0, 20000.0));
        break;
    case EqFilterType::LowShelf:
    case EqFilterType::HighShelf:
        f.frequencyHz = static_cast<float>(std::clamp(freq, 20.0, 20000.0));
        f.gainDb = std::clamp(
            static_cast<float>(renderer_->yToGain(currentPos.getY())),
            -12.0f, 12.0f);
        break;
    case EqFilterType::Peak:
        f.frequencyHz = static_cast<float>(std::clamp(freq, 500.0, 12000.0));
        f.gainDb = std::clamp(
            static_cast<float>(renderer_->yToGain(currentPos.getY())),
            -12.0f, 12.0f);
        break;
    }

    return result;
}

void EqBandInteraction::endDrag()
{
    isDragging_ = false;
    dragFilterIndex_ = -1;
}

bool EqBandInteraction::hasDragThreshold(juce::Point<float> startPos, juce::Point<float> currentPos,
                                         float threshold) const
{
    return startPos.getDistanceFrom(currentPos) >= threshold;
}

} // namespace OpenTune
