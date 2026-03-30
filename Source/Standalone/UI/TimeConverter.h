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

    void setContext(double bpm, int timeSignatureNum, int timeSignatureDenom);
    void setZoom(double zoomLevel);
    void setScrollOffset(double offset);

    int timeToPixel(double timeInSeconds) const;
    double pixelToTime(int pixelX) const;

    enum class GridResolution {
        Bar,
        Beat,
        HalfBeat,
        QuarterBeat,
        Sixteenth
    };
    int snapToGrid(int pixelX, GridResolution resolution) const;

    double getBpm() const;
    double getZoomLevel() const;

private:
    double bpm_ = 120.0;
    int timeSignatureNum_ = 4;
    int timeSignatureDenom_ = 4;
    double zoomLevel_ = 1.0;
    double scrollOffset_ = 0.0;

    static constexpr double pixelsPerSecondBase_ = 100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimeConverter)
};

} // namespace OpenTune
