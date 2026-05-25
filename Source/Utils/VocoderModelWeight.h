#pragma once
namespace OpenTune {

/** @brief 声码器模型权重选择。 */
enum class VocoderModelWeight
{
    Community = 0,  // 社区声码器 → hifigan.onnx
    Coulin9V4 = 1   // Coulin9 微调版 → hifigan_coulin9.onnx
};

} // namespace OpenTune
