#pragma once

/**
 * TimeConverter - 时间坐标转换工具
 * 
 * 负责时间（秒）与像素坐标之间的转换，考虑：
 * - BPM（每分钟节拍数）
 * - 节拍时间签名
 * - 缩放级别
 * - 滚动偏移
 */

#include <juce_core/juce_core.h>

namespace OpenTune {

class TimeConverter {
public:
    TimeConverter();
    ~TimeConverter();

    void setZoom(double zoomLevel);
    void setScrollOffset(double offset);

    int timeToPixel(double timeInSeconds) const;
    double pixelToTime(int pixelX) const;

    double getPixelsPerSecond() const;

    static constexpr double kBasePixelsPerSecond = 100.0;

private:
    double zoomLevel_ = 1.0;
    double scrollOffset_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimeConverter)
};

} // namespace OpenTune
