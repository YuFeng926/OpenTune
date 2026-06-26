#pragma once

namespace OpenTune {

/**
 * TimelineViewportCamera - 时间轴视口相机状态
 * 
 * 表示时间轴的唯一相机状态，跨视图共享。
 * 两个视图（ArrangementView 和 PianoRoll）从相机派生自己的 scrollOffset/pixelsPerSecond。
 */
struct TimelineViewportCamera {
    static constexpr double kDefaultPixelsPerSecond = 100.0;

    double visibleStartSeconds = 0.0;  // 可见窗口的绝对起始时间（秒）
    double pixelsPerSecond = kDefaultPixelsPerSecond;    // 缩放级别（像素/秒）
};

} // namespace OpenTune