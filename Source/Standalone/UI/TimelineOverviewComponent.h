#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include <vector>

#include "../../Content/ContentKey.h"
#include "../../Utils/ContentTimelineProjection.h"
#include "TimelineViewportCamera.h"
#include "WaveformMipmap.h"
#include "PianoRoll/PianoRollRenderer.h"

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

    /// Melodyne-style: waveform is background, viewport rectangle is a dark overlay scrollbar.
    /// Now always enabled - horizontal scrollbar is replaced by overview strip.

    void paint(juce::Graphics& g) override;
    void onHeartbeatTick(ContentKey contentKey,
                         const ContentTimelineProjection& projection,
                         TimelineViewportCamera camera,
                         int viewportWidthPx);

    /// Regular-capture multi-segment overview entry.
    /// placements: full set of TimelineContentPlacement (order preserved).
    /// timelineStart/timelineEnd: overall timeline range computed by Plugin.
    void onHeartbeatTickRegular(std::vector<TimelineContentPlacement> placements,
                                double timelineStart,
                                double timelineEnd,
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
    uint64_t calculateRegularSignature() const;
    Geometry calculateGeometry() const;
    Geometry calculateRegularGeometry() const;
    void requestNavigation(double visibleStartSeconds);
    void updateMouseCursor(juce::Point<float> position);

    WaveformMipmapCache& waveformMipmapCache_;
    juce::ListenerList<Listener> listeners_;

    // Single-content mode (ARA / Standalone)
    ContentKey contentKey_{};
    ContentTimelineProjection projection_{};
    TimelineViewportCamera camera_{};
    TimelineViewportCamera lastCamera_{};
    int viewportWidthPx_ = 0;
    uint64_t lastSignature_ = 0;
    float lastBuildProgress_ = 0.0f;

    // Regular-capture multi-segment mode
    bool regularCaptureMode_ = false;
    std::vector<TimelineContentPlacement> regularPlacements_;
    double regularTimelineStart_ = 0.0;
    double regularTimelineEnd_ = 0.0;
    uint64_t lastRegularSignature_ = 0;

    bool isDragging_ = false;
    double dragPointerOffsetSeconds_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimelineOverviewComponent)
};

} // namespace OpenTune
