#pragma once

#include <cstdint>

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

inline int64_t projectSamplesToRenderSamples(int64_t projectSamples, double projectSampleRate) {
    if (projectSampleRate <= 0.0) return 0;
    return static_cast<int64_t>((static_cast<double>(projectSamples) * kRenderSampleRate) / projectSampleRate);
}

inline int64_t renderSamplesToProjectSamples(int64_t renderSamples, double projectSampleRate) {
    if (projectSampleRate <= 0.0) return 0;
    return static_cast<int64_t>((static_cast<double>(renderSamples) * projectSampleRate) / kRenderSampleRate);
}

inline int64_t f0FrameToSamples(int frameIndex, int hopSize, double f0SampleRate, double projectSampleRate) {
    if (hopSize <= 0 || f0SampleRate <= 0.0 || projectSampleRate <= 0.0) return 0;
    const double sec = (static_cast<double>(frameIndex) * hopSize) / f0SampleRate;
    return secondsToSamples(sec, projectSampleRate);
}

inline int samplesToF0Frame(int64_t samples, int hopSize, double f0SampleRate, double projectSampleRate) {
    if (hopSize <= 0 || f0SampleRate <= 0.0 || projectSampleRate <= 0.0) return 0;
    const double sec = samplesToSeconds(samples, projectSampleRate);
    const double frames = sec * (f0SampleRate / static_cast<double>(hopSize));
    return static_cast<int>(frames);
}

} // namespace OpenTune::TimeCoordinate
