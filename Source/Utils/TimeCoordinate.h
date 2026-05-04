#pragma once

#include <cstdint>
#include <cmath>

namespace OpenTune::TimeCoordinate {

/**
 * kRenderSampleRate - 44.1kHz 渲染采样率（单一真实来源）
 * 
 * 所有内部音频存储和渲染处理统一使用 44.1kHz 采样率。
 * 其他位置的采样率常量（RenderCache::kSampleRate,
 * SilentGapDetector::kInternalSampleRate, AudioConstants::StoredAudioSampleRate）
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

} // namespace OpenTune::TimeCoordinate
