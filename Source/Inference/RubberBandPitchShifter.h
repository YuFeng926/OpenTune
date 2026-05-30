#pragma once

#include <vector>
#include <memory>

namespace RubberBand { class RubberBandLiveShifter; }

namespace OpenTune {

/// 基于 RubberBandLiveShifter 的逐块 pitch-shift 包装器。
/// 每 block 独立设置 pitchScale，精确跟踪 F0 修正曲线。
class RubberBandPitchShifter {
public:
    explicit RubberBandPitchShifter(double sampleRate);
    ~RubberBandPitchShifter();

    /// 对整个 chunk 进行逐块 pitch-shift，跟踪 F0 曲线。
    /// @param input         原始单声道音频 (44.1kHz)
    /// @param numSamples    样本数
    /// @param originalF0    原始 F0（100fps，chunk 对应的全局片段）
    /// @param correctedF0   修正后 F0（100fps，chunk 局部）
    /// @param numF0Frames   F0 帧数
    /// @param f0FrameRate   F0 帧率（fps）
    /// @return 变调后音频（长度 == numSamples）
    std::vector<float> shiftChunk(
        const float* input, int numSamples,
        const float* originalF0, const float* correctedF0,
        int numF0Frames, double f0FrameRate);

    /// 获取内部 block size（构造后固定）
    size_t getBlockSize() const;

private:
    std::unique_ptr<RubberBand::RubberBandLiveShifter> shifter_;
    double sampleRate_;
    size_t blockSize_;
};

} // namespace OpenTune