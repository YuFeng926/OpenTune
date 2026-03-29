#pragma once

#include <cmath>
#include <cstdint>
#include <utility>

namespace OpenTune {

/**
 * 钢琴卷帘坐标系统
 * 提供时间-像素、频率-MIDI音符、帧索引-时间等坐标转换功能
 * 支持缩放、滚动、轨道偏移等参数的坐标计算
 */
class PianoRollCoordinateSystem {
public:
    PianoRollCoordinateSystem() = default;

    void setZoomLevel(double zoom) { zoomLevel_ = zoom; }
    void setScrollOffset(int offset) { scrollOffset_ = offset; }
    void setBpm(double bpm) { bpm_ = (bpm > 0.0) ? bpm : 120.0; }
    void setTimeSignature(int num, int denom) { timeSigNum_ = num; timeSigDenom_ = denom; }
    void setPianoKeyWidth(int width) { pianoKeyWidth_ = width; }
    void setHopSize(int hopSize) { hopSize_ = hopSize; }
    void setF0SampleRate(double rate) { f0SampleRate_ = rate; }
    void setTrackOffsetSeconds(double offset) { trackOffsetSeconds_ = offset; }

    float midiToY(float midiNote, float minMidi, float maxMidi, float totalHeight) const {
        const float range = maxMidi - minMidi;
        if (range <= 0.0f) return 0.0f;
        const float normalized = (midiNote - minMidi) / range;
        return totalHeight * (1.0f - normalized);
    }

    float yToMidi(float y, float minMidi, float maxMidi, float totalHeight) const {
        const float range = maxMidi - minMidi;
        if (range <= 0.0f) return minMidi;
        const float normalized = 1.0f - (y / totalHeight);
        return minMidi + normalized * range;
    }

    static float freqToMidi(float frequency) {
        if (frequency <= 0.0f) return 0.0f;
        return 69.0f + 12.0f * std::log2(frequency / 440.0f);
    }

    static float midiToFreq(float midiNote) {
        return 440.0f * std::pow(2.0f, (midiNote - 69.0f) / 12.0f);
    }

    float yToFreq(float y, float minMidi, float maxMidi, float totalHeight) const {
        return midiToFreq(yToMidi(y, minMidi, maxMidi, totalHeight));
    }

    float freqToY(float freq, float minMidi, float maxMidi, float totalHeight) const {
        return midiToY(freqToMidi(freq), minMidi, maxMidi, totalHeight);
    }

    int timeToX(double seconds) const {
        const double pixelsPerSecond = 100.0 * zoomLevel_;
        return static_cast<int>(seconds * pixelsPerSecond) - scrollOffset_ + pianoKeyWidth_;
    }

    double xToTime(int x) const {
        const double pixelsPerSecond = 100.0 * zoomLevel_;
        return static_cast<double>(x + scrollOffset_ - pianoKeyWidth_) / pixelsPerSecond;
    }

    double clipSecondsToTimelineSeconds(double clipSeconds) const {
        return clipSeconds + trackOffsetSeconds_;
    }

    double timelineSecondsToClipSeconds(double timelineSeconds) const {
        return timelineSeconds - trackOffsetSeconds_;
    }

    int clipSecondsToFrameIndex(double clipSeconds, size_t totalFrames = 0) const {
        if (hopSize_ <= 0 || f0SampleRate_ <= 0.0) return -1;
        const double frameDuration = static_cast<double>(hopSize_) / f0SampleRate_;
        const int frame = static_cast<int>(std::floor(clipSeconds / frameDuration));
        if (totalFrames > 0 && frame >= static_cast<int>(totalFrames)) {
            return static_cast<int>(totalFrames) - 1;
        }
        return frame;
    }

    int clipSecondsToFrameIndexCeil(double clipSeconds, size_t totalFrames = 0) const {
        if (hopSize_ <= 0 || f0SampleRate_ <= 0.0) return -1;
        const double frameDuration = static_cast<double>(hopSize_) / f0SampleRate_;
        int frame = static_cast<int>(std::ceil(clipSeconds / frameDuration));
        if (totalFrames > 0 && frame >= static_cast<int>(totalFrames)) {
            return static_cast<int>(totalFrames) - 1;
        }
        return frame;
    }

    double frameIndexToClipSeconds(int frameIndex) const {
        if (hopSize_ <= 0 || f0SampleRate_ <= 0.0) return 0.0;
        return static_cast<double>(frameIndex) * static_cast<double>(hopSize_) / f0SampleRate_;
    }

    std::pair<int, int> clipTimeRangeToFrameRangeHalfOpen(double startTime, double endTime, int maxFrameExclusive) const {
        const int startFrame = clipSecondsToFrameIndex(startTime, maxFrameExclusive);
        const int endFrame = clipSecondsToFrameIndexCeil(endTime, maxFrameExclusive);
        return { std::max(0, startFrame), std::min(maxFrameExclusive, endFrame) };
    }

    int timelineSecondsToFrameIndex(double timelineSeconds, size_t totalFrames = 0) const {
        const double clipSeconds = timelineSeconds - trackOffsetSeconds_;
        if (clipSeconds < 0.0) return -1;
        return clipSecondsToFrameIndex(clipSeconds, totalFrames);
    }

    double frameIndexToTimelineSeconds(int frameIndex) const {
        if (hopSize_ <= 0 || f0SampleRate_ <= 0.0) return trackOffsetSeconds_;
        const double clipSeconds = static_cast<double>(frameIndex) * static_cast<double>(hopSize_) / f0SampleRate_;
        return trackOffsetSeconds_ + clipSeconds;
    }

    double getBpm() const { return bpm_; }
    int getTimeSigNum() const { return timeSigNum_; }
    int getTimeSigDenom() const { return timeSigDenom_; }
    int getPianoKeyWidth() const { return pianoKeyWidth_; }
    int getScrollOffset() const { return scrollOffset_; }
    double getZoomLevel() const { return zoomLevel_; }
    int getHopSize() const { return hopSize_; }
    double getF0SampleRate() const { return f0SampleRate_; }
    double getTrackOffsetSeconds() const { return trackOffsetSeconds_; }

private:
    double zoomLevel_ = 1.0;
    int scrollOffset_ = 0;
    double bpm_ = 120.0;
    int timeSigNum_ = 4;
    int timeSigDenom_ = 4;
    int pianoKeyWidth_ = 60;
    int hopSize_ = 512;
    double f0SampleRate_ = 16000.0;
    double trackOffsetSeconds_ = 0.0;
};

} // namespace OpenTune
