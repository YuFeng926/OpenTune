/**
 * EQ Band Interaction — 5 锚点拖拽交互逻辑（实现）
 *
 * 拖拽映射：anchor 直接跟随鼠标位置
 * - Cut: xToFreq(mouseX), Y 只读
 * - Shelf/Peak: xToFreq(mouseX) + yToGain(mouseY)
 * - 频率/增益范围照契约
 */

#include "EqBandInteraction.h"
#include "EqGraphRenderer.h"
#include <cmath>
#include <algorithm>

namespace OpenTune {

EqBandInteraction::BandType EqBandInteraction::bandType(int bandIndex)
{
    static const BandType types[] = {
        BandType::LowCut, BandType::LowShelf, BandType::Peak,
        BandType::HighShelf, BandType::HighCut
    };
    return types[bandIndex];
}

void EqBandInteraction::startDrag(int bandIndex, juce::Point<float> startPos, const EqSettings& currentSettings)
{
    dragBandIndex_ = bandIndex;
    dragStartPos_ = startPos;
    dragStartSettings_ = currentSettings;
    isDragging_ = true;
}

EqSettings EqBandInteraction::updateDrag(juce::Point<float> currentPos, const EqSettings& currentSettings) const
{
    if (!isDragging_ || dragBandIndex_ < 0)
        return currentSettings;

    EqSettings result = dragStartSettings_;

    // ── 坐标映射：anchor 直接跟随鼠标位置 ──
    const double freq = renderer_->xToFreq(currentPos.getX());

    switch (dragBandIndex_)
    {
    case 0: // LowCut: 20-20000 Hz, Y 只读
        result.lowCutFrequencyHz = static_cast<float>(
            std::clamp(freq, 20.0, 20000.0));
        break;
    case 1: // LowShelf: 20-20000 Hz, gain ±12
        result.lowShelfFrequencyHz = static_cast<float>(
            std::clamp(freq, 20.0, 20000.0));
        result.lowShelfGainDb = std::clamp(
            static_cast<float>(renderer_->yToGain(currentPos.getY())),
            -12.0f, 12.0f);
        break;
    case 2: // Peak: 500-12000 Hz, gain ±12
        result.peakFrequencyHz = static_cast<float>(
            std::clamp(freq, 500.0, 12000.0));
        result.peakGainDb = std::clamp(
            static_cast<float>(renderer_->yToGain(currentPos.getY())),
            -12.0f, 12.0f);
        break;
    case 3: // HighShelf: 20-20000 Hz, gain ±12
        result.highShelfFrequencyHz = static_cast<float>(
            std::clamp(freq, 20.0, 20000.0));
        result.highShelfGainDb = std::clamp(
            static_cast<float>(renderer_->yToGain(currentPos.getY())),
            -12.0f, 12.0f);
        break;
    case 4: // HighCut: 20-20000 Hz, Y 只读
        result.highCutFrequencyHz = static_cast<float>(
            std::clamp(freq, 20.0, 20000.0));
        break;
    }

    return result;
}

void EqBandInteraction::endDrag()
{
    isDragging_ = false;
    dragBandIndex_ = -1;
}

bool EqBandInteraction::hasDragThreshold(juce::Point<float> startPos, juce::Point<float> currentPos,
                                         float threshold) const
{
    return startPos.getDistanceFrom(currentPos) >= threshold;
}

} // namespace OpenTune
