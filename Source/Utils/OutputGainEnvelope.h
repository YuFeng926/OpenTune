#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace OpenTune {

struct Note;
class TimeGridSnapshot;

// B 层：Sibilant Balance 结果。数据点使用 content-local 绝对时间（秒），值使用 dB，正值为增强、负值为衰减。
struct SibilantGainEnvelopePoint { double time = 0.0; float gainDb = 0.0f; };
using SibilantGainEnvelope = std::vector<SibilantGainEnvelopePoint>;

// 不可变 canonical 最终增益包络：canonical 采样率下逐样本线性增益（A+B 合成后，无 A/B 保留）。
struct OutputGainEnvelopeSnapshot
{
    double sampleRate{0.0};
    std::vector<float> linearGains;
};

// 目标播放采样率下逐样本线性增益 + canonical 身份（Publisher 复用 prepared gain 时必须同时匹配身份与采样率）。
struct PreparedOutputGainEnvelope
{
    std::shared_ptr<const OutputGainEnvelopeSnapshot> canonicalIdentity;
    double sampleRate{0.0};
    std::vector<float> linearGains;
};

// 共享 builder：输入只来自 owner snapshot（notes / B 层 / TimeGrid）与 canonical 时长。
// 每个 source-time 位置：A = 包含它的半开区间 Note 的 outputGainDb（无覆盖 = 0 dB）；
// B = 按时间排序的点分段常数（点之间保持前值，之前无点 = 0 dB）；
// TimeGrid 投影：output-time → tauInverse → source-time 再取 A/B；Glinear = 10^((A+B)/20)。
// timeGrid 为 null 时按恒等映射处理（装配点既有语义，见 timeGridIsIdentity）。
std::shared_ptr<const OutputGainEnvelopeSnapshot> buildOutputGainEnvelope(
    const std::vector<Note>& notes,
    const SibilantGainEnvelope& sibilant,
    const std::shared_ptr<const TimeGridSnapshot>& timeGrid,
    int64_t durationSamples,
    double sampleRate);

// canonical → 目标采样率线性插值重采样，生成 PreparedOutputGainEnvelope（构建期/消息线程执行）。
std::shared_ptr<const PreparedOutputGainEnvelope> prepareOutputGainEnvelope(
    const std::shared_ptr<const OutputGainEnvelopeSnapshot>& canonical,
    double targetSampleRate);

} // namespace OpenTune
