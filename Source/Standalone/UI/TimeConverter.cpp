#include "TimeConverter.h"
#include <cmath>

namespace OpenTune {

TimeConverter::TimeConverter() {
}

TimeConverter::~TimeConverter() {
}

void TimeConverter::setContext(double bpm, int timeSignatureNum, int timeSignatureDenom) {
    bpm_ = bpm;
    timeSignatureNum_ = timeSignatureNum;
    timeSignatureDenom_ = timeSignatureDenom;
}

void TimeConverter::setZoom(double zoomLevel) {
    zoomLevel_ = std::max(0.02, std::min(10.0, zoomLevel));
}

void TimeConverter::setScrollOffset(double offset) {
    scrollOffset_ = offset;
}

int TimeConverter::timeToPixel(double timeInSeconds) const {
    return static_cast<int>(std::round(timeInSeconds * pixelsPerSecondBase_ * zoomLevel_ - scrollOffset_));
}

double TimeConverter::pixelToTime(int pixelX) const {
    return static_cast<double>(pixelX + scrollOffset_) / (pixelsPerSecondBase_ * zoomLevel_);
}

int TimeConverter::snapToGrid(int pixelX, GridResolution resolution) const {
    double timeInSeconds = pixelToTime(pixelX);
    double beatDuration = 60.0 / bpm_;
    double gridDuration = beatDuration;

    switch (resolution) {
        case GridResolution::Bar:
            gridDuration = beatDuration * static_cast<double>(timeSignatureNum_);
            break;
        case GridResolution::Beat:
            gridDuration = beatDuration;
            break;
        case GridResolution::HalfBeat:
            gridDuration = beatDuration / 2.0;
            break;
        case GridResolution::QuarterBeat:
            gridDuration = beatDuration / 4.0;
            break;
        case GridResolution::Sixteenth:
            gridDuration = beatDuration / 16.0;
            break;
    }

    double snappedTime = std::round(timeInSeconds / gridDuration) * gridDuration;
    return timeToPixel(snappedTime);
}

double TimeConverter::getBpm() const {
    return bpm_;
}

double TimeConverter::getZoomLevel() const {
    return zoomLevel_;
}

} // namespace OpenTune
