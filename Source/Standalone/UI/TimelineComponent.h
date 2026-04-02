#pragma once

/**
 * TimelineComponent - 时间线组件
 * 
 * 显示时间标尺和播放头位置，支持：
 * - 时间标记显示（秒或小节）
 * - 节拍网格绘制
 * - 播放头位置交互
 * - 缩放控制
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include "UIColors.h"
#include "TimeConverter.h"

namespace OpenTune {

class TimelineComponent : public juce::Component
{
public:
    enum class TimeUnit
    {
        Seconds,
        Bars
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void playheadPositionChanged(double timeInSeconds) = 0;
        virtual void zoomLevelChanged(double newZoom) = 0;
    };

    TimelineComponent();
    ~TimelineComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    // Timeline control
    void setTimeConverter(TimeConverter* converter);
    void setPlayheadPosition(double timeInSeconds);
    double getPlayheadPosition() const;

    void setViewportScroll(int scrollX);
    int getViewportScroll() const;

    void setZoomLevel(double zoom);
    double getZoomLevel() const;

    // Time unit and BPM control
    void setTimeUnit(TimeUnit unit);
    TimeUnit getTimeUnit() const { return timeUnit_; }

    void setBpm(double bpm);
    double getBpm() const { return bpm_; }

private:
    TimeConverter* timeConverter_;
    double playheadTime_;
    int viewportScrollX_;
    double zoomLevel_;
    TimeUnit timeUnit_;
    double bpm_;

    juce::ListenerList<Listener> listeners_;

    // Drawing helpers
    void drawRuler(juce::Graphics& g);
    void drawTimeMarkers(juce::Graphics& g);
    void drawBeatGrid(juce::Graphics& g);

    // Formatting helpers
    juce::String formatTime(double seconds) const;
    juce::String formatTimeInBars(double seconds) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimelineComponent)
};

} // namespace OpenTune
