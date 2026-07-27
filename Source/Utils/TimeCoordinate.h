#pragma once

#include <cstdint>
#include <cmath>

namespace OpenTune::TimeCoordinate {

/**
 * kRenderSampleRate - 44.1kHz 渲染采样率（单一真实来源）
 * 
 * 所有内部音频存储和渲染处理统一使用 44.1kHz 采样率。
 * 其他位置的采样率常量（RenderCache::kSampleRate,
 * SilentGapDetector::kInternalSampleRate）
 * 均引用此常量，确保一致性。
 */
constexpr double kRenderSampleRate = 44100.0;

inline double samplesToSeconds(int64_t samples, double sampleRate) {
    if (sampleRate <= 0.0) return 0.0;
    return static_cast<double>(samples) / sampleRate;
}

inline double secondsToSamplesExact(double seconds, double sampleRate) {
    if (sampleRate <= 0.0) return 0.0;
    return seconds * sampleRate;
}

inline int64_t secondsToSamples(double seconds, double sampleRate) {
    if (sampleRate <= 0.0) return 0;
    return static_cast<int64_t>(secondsToSamplesExact(seconds, sampleRate));
}

inline int64_t secondsToSamplesFloor(double seconds, double sampleRate) {
    if (sampleRate <= 0.0) return 0;
    return static_cast<int64_t>(std::floor(secondsToSamplesExact(seconds, sampleRate)));
}

inline int64_t secondsToSamplesCeil(double seconds, double sampleRate) {
    if (sampleRate <= 0.0) return 0;
    return static_cast<int64_t>(std::ceil(secondsToSamplesExact(seconds, sampleRate)));
}

/** 将样本位置从一个采样率投影到另一个采样率。
 *  timeSeconds = samples / fromSr 是精确量（不取整），
 *  目标样本 = round(timeSeconds * toSr)，
 *  保证相邻 chunk 投影后边界连续无缝隙。
 */
inline int64_t sampleRateProject(int64_t sample, double fromSr, double toSr) {
    if (fromSr <= 0.0 || toSr <= 0.0) return 0;
    const double seconds = static_cast<double>(sample) / fromSr;
    return static_cast<int64_t>(std::round(seconds * toSr));
}

} // namespace OpenTune::TimeCoordinate
