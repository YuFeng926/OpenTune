#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>

#include "../../Content/ContentKey.h"
#include "../../Utils/ContentTimelineProjection.h"
#include "TimelineViewportCamera.h"
#include "WaveformMipmap.h"

namespace OpenTune {

class TimelineOverviewComponent : public juce::Component
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void overviewNavigateRequested(double visibleStartSeconds,
                                               double pixelsPerSecond) = 0;
    };

    explicit TimelineOverviewComponent(WaveformMipmapCache& waveformMipmapCache);
    ~TimelineOverviewComponent() override = default;

    void paint(juce::Graphics& g) override;
    void onHeartbeatTick(ContentKey contentKey,
                         const ContentTimelineProjection& projection,
                         TimelineViewportCamera camera,
                         int viewportWidthPx);

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    struct Geometry
    {
        juce::Rectangle<float> contentBounds;
        double spanSeconds = 0.0;
        double previewPixelsPerSecond = 0.0;
        double visibleDurationSeconds = 0.0;
        double visibleStartSeconds = 0.0;
        double maxVisibleStartSeconds = 0.0;
    };

    juce::Rectangle<float> getContentBounds() const noexcept;
    uint64_t calculateContentSignature() const;
    Geometry calculateGeometry() const;
    void requestNavigation(double visibleStartSeconds);
    void updateMouseCursor(juce::Point<float> position);

    WaveformMipmapCache& waveformMipmapCache_;
    juce::ListenerList<Listener> listeners_;

    ContentKey contentKey_{};
    ContentTimelineProjection projection_{};
    TimelineViewportCamera camera_{};
    TimelineViewportCamera lastCamera_{};
    int viewportWidthPx_ = 0;
    uint64_t lastSignature_ = 0;

    bool isDragging_ = false;
    double dragPointerOffsetSeconds_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimelineOverviewComponent)
};

} // namespace OpenTune
