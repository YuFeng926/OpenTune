#pragma once
#include <string>

namespace OpenTune {

/** @brief 声码器模型权重标识 = 权重文件名（如 hifigan.onnx）。
 *
 *  内置权重位于 models/ 根目录；额外权重可放入 models/vocoder_weights/，
 *  程序启动时扫描该目录动态生成可选项，同名时 vocoder_weights/ 优先。 */
using VocoderModelWeight = std::string;

inline constexpr const char* kVocoderWeightCommunity  = "hifigan.onnx";
inline constexpr const char* kVocoderWeightCoulin9    = "hifigan_coulin9.onnx";
inline constexpr const char* kDefaultVocoderWeight    = kVocoderWeightCommunity;

} // namespace OpenTune
