#pragma once

#include <vector>
#include <cstddef>
#include <cstdint>

namespace Ort { struct RunOptions; }

namespace OpenTune {

/** @brief 声码器条件谱类型：决定宿主端如何计算条件输入。
 *
 *  LogMel:         log-Mel 滤波器组（传统 NSF-HiFiGAN，条件维通常 128）
 *  LogLinearSpec:  自然对数线性幅度谱（log|STFT|，条件维 = n_fft/2+1）
 */
enum class VocoderConditioningType : uint8_t
{
    LogMel,
    LogLinearSpec
};

class VocoderInterface {
public:
    virtual ~VocoderInterface() = default;

    /** @param uv 1=voiced 的显式浊音掩码；为空时由 f0>0 推导（旧行为）。 */
    virtual std::vector<float> synthesize(
        const std::vector<float>& f0,
        const std::vector<float>& uv,
        const float* conditioning,
        size_t conditioningSize,
        Ort::RunOptions& runOptions
    ) = 0;

    virtual int getHopSize() const { return 512; }
    virtual int getSampleRate() const { return 44100; }

    /** @brief 条件谱维数：LogMel 为 mel bins，LogLinearSpec 为 n_fft/2+1。 */
    virtual int getConditioningBins() const { return 128; }

    virtual VocoderConditioningType getConditioningType() const {
        return VocoderConditioningType::LogMel;
    }

    // mel filterbank fmax used during training. Pure virtual: every concrete
    // vocoder MUST declare its own value to keep mel computation consistent
    // with the model. fmax is not encoded in ONNX schema, so this contract
    // is the only safeguard against silent config drift.
    virtual float getFMax() const = 0;

    // Sidecar override: apply fmax from the weight's companion yaml
    // (VocoderFactory calls this before the Nyquist validation).
    virtual void setMelFMax(float fMax) = 0;
};

} // namespace OpenTune
