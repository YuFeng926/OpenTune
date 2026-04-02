#include "TimelineComponent.h"

namespace OpenTune {

TimelineComponent::TimelineComponent()
    : timeConverter_(nullptr),
      playheadTime_(0.0),
      viewportScrollX_(0),
      zoomLevel_(1.0),
      timeUnit_(TimeUnit::Seconds),
      bpm_(120.0)
{
}

TimelineComponent::~TimelineComponent()
{
}

void TimelineComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Background
    g.fillAll(UIColors::backgroundDark);

    // Top border
    g.setColour(UIColors::panelBorder);
    g.drawLine(0.0f, 0.0f, static_cast<float>(bounds.getWidth()), 0.0f, 2.0f);

    // Draw ruler and markers
    drawRuler(g);
    drawTimeMarkers(g);
    drawBeatGrid(g);
}

void TimelineComponent::resized()
{
    // No UI controls in timeline anymore
}

void TimelineComponent::mouseDown(const juce::MouseEvent& event)
{
    if (timeConverter_ == nullptr)
        return;

    // Calculate clicked time position
    int pixelX = event.x + viewportScrollX_;
    double time = timeConverter_->pixelToTime(pixelX);

    setPlayheadPosition(time);
    listeners_.call([time](Listener& l) { l.playheadPositionChanged(time); });
}

void TimelineComponent::mouseDrag(const juce::MouseEvent& event)
{
    mouseDown(event);
}

void TimelineComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    // Horizontal zoom with wheel
    if (event.mods.isCtrlDown())
    {
        double newZoom = zoomLevel_ * (1.0 + wheel.deltaY * 0.5);
        // 限制缩放范围：0.02~10.0（允许缩小到更大范围）
        newZoom = juce::jlimit(0.02, 10.0, newZoom);

        setZoomLevel(newZoom);
        listeners_.call([newZoom](Listener& l) { l.zoomLevelChanged(newZoom); });

        repaint();
    }
}

void TimelineComponent::addListener(Listener* listener)
{
    listeners_.add(listener);
}

void TimelineComponent::removeListener(Listener* listener)
{
    listeners_.remove(listener);
}

void TimelineComponent::setTimeConverter(TimeConverter* converter)
{
    timeConverter_ = converter;
    repaint();
}

void TimelineComponent::setPlayheadPosition(double timeInSeconds)
{
    playheadTime_ = timeInSeconds;
    repaint();
}

double TimelineComponent::getPlayheadPosition() const
{
    return playheadTime_;
}

void TimelineComponent::setViewportScroll(int scrollX)
{
    viewportScrollX_ = scrollX;
    repaint();
}

int TimelineComponent::getViewportScroll() const
{
    return viewportScrollX_;
}

void TimelineComponent::setZoomLevel(double zoom)
{
    // 限制缩放范围：0.02~10.0（支持更长音频的完整显示）
    zoomLevel_ = juce::jlimit(0.02, 10.0, zoom);

    if (timeConverter_ != nullptr)
    {
        timeConverter_->setZoom(zoomLevel_);
    }
}

double TimelineComponent::getZoomLevel() const
{
    return zoomLevel_;
}

void TimelineComponent::setTimeUnit(TimeUnit unit)
{
    timeUnit_ = unit;
    repaint();
}

void TimelineComponent::setBpm(double bpm)
{
    bpm_ = juce::jlimit(30.0, 300.0, bpm);
    repaint();
}

void TimelineComponent::drawRuler(juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Ruler background
    g.setColour(UIColors::backgroundMedium);
    g.fillRect(bounds);
}

void TimelineComponent::drawTimeMarkers(juce::Graphics& g)
{
    if (timeConverter_ == nullptr)
        return;

    auto bounds = getLocalBounds();
    const int width = bounds.getWidth();
    const int height = bounds.getHeight();

    g.setFont(UIColors::getLabelFont(13.0f));  // Increased from 12.0f (改大一号)

    // Calculate marker interval based on zoom level
    double secondsPerPixel = 1.0 / (100.0 * zoomLevel_);
    double markerInterval = 1.0; // Start with 1 second intervals

    if (timeUnit_ == TimeUnit::Bars)
    {
        // Use bar-based intervals
        double secondsPerBar = (60.0 / bpm_) * 4.0; // 4 beats per bar
        markerInterval = secondsPerBar;
    }
    else
    {
        // Adjust interval for readability in seconds mode
        if (secondsPerPixel > 0.1)
            markerInterval = 5.0;
        if (secondsPerPixel > 0.5)
            markerInterval = 10.0;
        if (secondsPerPixel > 1.0)
            markerInterval = 30.0;
    }

    const double pixelsPerSecond = 100.0 * zoomLevel_;
    double startTime = 0.0;
    double endTime = 0.0;

    if (pixelsPerSecond > 0.0)
    {
        startTime = timeConverter_->pixelToTime(viewportScrollX_);
        if (startTime < 0.0)
            startTime = 0.0;

        endTime = timeConverter_->pixelToTime(viewportScrollX_ + width);
        if (endTime < startTime)
            endTime = startTime;
    }

    if (markerInterval < 0.001)
        markerInterval = 1.0;

    double firstMarkerTime = std::floor(startTime / markerInterval) * markerInterval;

    for (double time = firstMarkerTime; time < endTime + markerInterval; time += markerInterval)
    {
        int pixelX = timeConverter_->timeToPixel(time) - viewportScrollX_;

        if (pixelX < -100 || pixelX > width + 100)
            continue;

        // Major marker line
        g.setColour(UIColors::gridLine);
        g.drawLine(static_cast<float>(pixelX), static_cast<float>(height - 15),
                   static_cast<float>(pixelX), static_cast<float>(height),
                   1.0f);

        // Time label
        g.setColour(UIColors::textSecondary);
        juce::String timeStr = (timeUnit_ == TimeUnit::Bars) ? formatTimeInBars(time) : formatTime(time);
        g.drawText(timeStr, pixelX - 30, 2, 60, height - 17, juce::Justification::centred);
    }
}

void TimelineComponent::drawBeatGrid(juce::Graphics& g)
{
    if (timeConverter_ == nullptr)
        return;

    auto bounds = getLocalBounds();
    const int width = bounds.getWidth();
    const int height = bounds.getHeight();

    double secondsPerBeat = 60.0 / bpm_;

    const double pixelsPerSecond = 100.0 * zoomLevel_;
    double startTime = 0.0;
    double endTime = 0.0;

    if (pixelsPerSecond > 0.0)
    {
        startTime = timeConverter_->pixelToTime(viewportScrollX_);
        if (startTime < 0.0)
            startTime = 0.0;

        endTime = timeConverter_->pixelToTime(viewportScrollX_ + width);
        if (endTime < startTime)
            endTime = startTime;
    }

    if (secondsPerBeat <= 0.0)
        secondsPerBeat = 1.0;

    double firstBeatTime = std::floor(startTime / secondsPerBeat) * secondsPerBeat;

    for (double time = firstBeatTime; time < endTime + secondsPerBeat; time += secondsPerBeat)
    {
        int pixelX = timeConverter_->timeToPixel(time) - viewportScrollX_;

        if (pixelX < -100 || pixelX > width + 100)
            continue;

        // Beat marker (smaller)
        g.setColour(UIColors::beatMarker);
        g.drawLine(static_cast<float>(pixelX), static_cast<float>(height - 10),
                   static_cast<float>(pixelX), static_cast<float>(height),
                   1.0f);
    }

    // Draw playhead position
    if (playheadTime_ >= 0.0)
    {
        int playheadX = timeConverter_->timeToPixel(playheadTime_) - viewportScrollX_;

        if (playheadX >= 0 && playheadX < width)
        {
            g.setColour(UIColors::playhead);
            g.drawLine(static_cast<float>(playheadX), 0.0f,
                       static_cast<float>(playheadX), static_cast<float>(height),
                       2.0f);
        }
    }
}

juce::String TimelineComponent::formatTime(double seconds) const
{
    int totalSecs = static_cast<int>(seconds);
    int mins = totalSecs / 60;
    int secs = totalSecs % 60;
    int millis = static_cast<int>((seconds - totalSecs) * 100);

    return juce::String::formatted("%d:%02d.%02d", mins, secs, millis);
}

juce::String TimelineComponent::formatTimeInBars(double seconds) const
{
    // Calculate bar number based on BPM (4 beats per bar)
    double secondsPerBar = (60.0 / bpm_) * 4.0;
    int barNumber = static_cast<int>(seconds / secondsPerBar) + 1;
    double beatInBar = (seconds / secondsPerBar - static_cast<int>(seconds / secondsPerBar)) * 4.0;
    int beat = static_cast<int>(beatInBar) + 1;

    return juce::String::formatted("%d.%d", barNumber, beat);
}

} // namespace OpenTune
