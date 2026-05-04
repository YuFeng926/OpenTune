#include "TimeConverter.h"
#include <cmath>

namespace OpenTune {

TimeConverter::TimeConverter() {
}

TimeConverter::~TimeConverter() {
}

void TimeConverter::setZoom(double zoomLevel) {
    zoomLevel_ = std::max(0.02, std::min(10.0, zoomLevel));
}

void TimeConverter::setScrollOffset(double offset) {
    scrollOffset_ = offset;
}

int TimeConverter::timeToPixel(double timeInSeconds) const {
    return static_cast<int>(std::round(timeInSeconds * kBasePixelsPerSecond * zoomLevel_ - scrollOffset_));
}

double TimeConverter::pixelToTime(int pixelX) const {
    return static_cast<double>(pixelX + scrollOffset_) / (kBasePixelsPerSecond * zoomLevel_);
}

double TimeConverter::getPixelsPerSecond() const {
    return kBasePixelsPerSecond * zoomLevel_;
}

} // namespace OpenTune
