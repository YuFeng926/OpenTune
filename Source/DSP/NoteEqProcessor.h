#pragma once

/**
 * Per-note EQ processor — 契约版 DSP（docs/plans/2026-08-13-per-note-eq-tool-plan.md 第 3 节）
 *
 * - LowCut / HighCut：JUCE FilterDesign 8 阶 Butterworth 高低通，各级联 4 个最小相位二阶段。
 * - LowShelf / Peak / HighShelf：三个 RBJ 最小相位二阶段（makeLowShelf / makePeakFilter /
 *   makeHighShelf），Q 固定 2.0，频率与增益取自 EqSettings。
 * - 固定声道契约：产品固定单声道/立体声，左右两套完整 ChannelState 用固定 std::array 表达，
 *   无动态声道结构；声道数在 process 处以 jassert 锁定为 1 或 2，是固定产品不变量，
 *   不做运行时兜底。
 * - ChannelState 直接表达固定领域顺序 LowCut[4] → LowShelf → Peak → HighShelf → HighCut[4]，
 *   不用扁平段向量掩盖领域顺序；左右声道状态互不共享。
 * - 状态生命周期：每个 Note 起点调用 reset()；Note 内部连续处理；不同 Note 之间绝不延续状态，
 *   不做 crossfade、不做状态复用。
 * - 系数准备：prepare 在渲染工作线程按采样率与参数计算系数并绑定到左右两套状态；
 *   播放音频线程不得出现任何系数计算、滤波器分配、状态分配。
 * - process 零分配：直接就地处理 AudioBuffer，支持 mono/stereo。
 * - magnitudeDb：只读解析频响查询（double 计算，按 z = e^{-jw} 遍历已绑定二阶段求总 |H|），
 *   供测试与诊断使用；不触碰滤波器状态、不改变生产处理路径。
 * - 视觉与 DSP 是两个明确职责：本文件只承载音频滤波；视觉曲线由 EqGraphRenderer 复刻 SRC，
 *   视觉数学不进 DSP，DSP 系数不进视觉。
 */

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>

#include "../Utils/NoteEqSettings.h"

namespace OpenTune {

class NoteEqProcessor {
public:
    static constexpr int kButterworthOrder = 8;    // 8 阶 = 48 dB/oct
    static constexpr int kSectionsPerCut = kButterworthOrder / 2;  // 每 Cut 4 个二阶段
    static constexpr double kShelfQ = 2.0;         // Q 固定 2.0（Cut 无 Q 参数），DSP 层常量

    // 每声道二阶段总数：LowCut(4) + LowShelf(1) + Peak(1) + HighShelf(1) + HighCut(4)
    static constexpr int kSectionsPerChannel = kSectionsPerCut + 3 + kSectionsPerCut;

    // 固定产品不变量：单声道/立体声
    static constexpr int kMaxChannels = 2;

    NoteEqProcessor() = default;

    /** 渲染工作线程：按采样率与参数计算系数并绑定到左右两套固定状态。 */
    void prepare(double sampleRate, const EqSettings& settings)
    {
        active_ = settings.active;
        sampleRate_ = sampleRate;

        const auto coefficients = designCoefficients(sampleRate, settings);
        for (auto& channel : channels_)
            channel.bind(coefficients);

        reset();
    }

    /** 每个 Note 起点重置全部声道状态；Note 内部连续处理，跨 Note 绝不延续。 */
    void reset()
    {
        for (auto& channel : channels_)
            channel.reset();
    }

    /** 播放音频线程：只应用已就绪的系数，零分配就地处理，支持 mono/stereo。 */
    void process(juce::AudioBuffer<float>& buffer)
    {
        if (!active_)
            return;

        const int numChannels = buffer.getNumChannels();
        jassert(numChannels == 1 || numChannels == 2);  // 固定产品不变量，不加运行时兜底

        for (int c = 0; c < numChannels; ++c) {
            auto& channel = channels_[static_cast<size_t>(c)];
            auto* samples = buffer.getWritePointer(c);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                samples[i] = channel.processSample(samples[i]);
        }
    }

    bool isActive() const { return active_; }

    /**
     * 只读频响查询：返回整链在 frequencyHz 处的幅值响应（dB）。
     * 遍历已绑定二阶段系数按 z = e^{-jw} 计算总 |H|（double 计算），
     * 不触碰滤波器状态、不改变生产处理路径。
     * 必须在 prepare 之后调用：以 jassert 表达该不变量，不做运行时兜底。
     */
    double magnitudeDb(double frequencyHz) const
    {
        jassert(sampleRate_ > 0.0);  // prepare 后调用不变量；无运行时兜底

        const double w = 2.0 * juce::MathConstants<double>::pi * frequencyHz / sampleRate_;
        const double c1 = std::cos(w);
        const double s1 = std::sin(w);
        const double c2 = std::cos(2.0 * w);
        const double s2 = std::sin(2.0 * w);

        // 两声道系数绑定完全相同，只求一声道整链响应
        const auto& channel = channels_[0];
        double squaredMagnitude = 1.0;
        for (const auto& filter : channel.lowCut)
            squaredMagnitude *= sectionSquaredMagnitude(*filter.coefficients, c1, s1, c2, s2);
        squaredMagnitude *= sectionSquaredMagnitude(*channel.lowShelf.coefficients, c1, s1, c2, s2);
        squaredMagnitude *= sectionSquaredMagnitude(*channel.peak.coefficients, c1, s1, c2, s2);
        squaredMagnitude *= sectionSquaredMagnitude(*channel.highShelf.coefficients, c1, s1, c2, s2);
        for (const auto& filter : channel.highCut)
            squaredMagnitude *= sectionSquaredMagnitude(*filter.coefficients, c1, s1, c2, s2);

        return 10.0 * std::log10(squaredMagnitude);
    }

private:
    using Coefficients = juce::ReferenceCountedObjectPtr<juce::dsp::IIR::Coefficients<float>>;

    // 每声道完整状态：直接表达固定领域顺序 LowCut[4] → LowShelf → Peak → HighShelf → HighCut[4]
    struct ChannelState {
        std::array<juce::dsp::IIR::Filter<float>, kSectionsPerCut> lowCut;
        juce::dsp::IIR::Filter<float> lowShelf;
        juce::dsp::IIR::Filter<float> peak;
        juce::dsp::IIR::Filter<float> highShelf;
        std::array<juce::dsp::IIR::Filter<float>, kSectionsPerCut> highCut;

        void bind(const std::array<Coefficients, kSectionsPerChannel>& coefficients)
        {
            for (int i = 0; i < kSectionsPerCut; ++i)
                lowCut[static_cast<size_t>(i)].coefficients = coefficients[static_cast<size_t>(i)];
            lowShelf.coefficients = coefficients[kSectionsPerCut];
            peak.coefficients = coefficients[kSectionsPerCut + 1];
            highShelf.coefficients = coefficients[kSectionsPerCut + 2];
            for (int i = 0; i < kSectionsPerCut; ++i)
                highCut[static_cast<size_t>(i)].coefficients =
                    coefficients[static_cast<size_t>(kSectionsPerCut + 3 + i)];
        }

        void reset()
        {
            for (auto& filter : lowCut)
                filter.reset();
            lowShelf.reset();
            peak.reset();
            highShelf.reset();
            for (auto& filter : highCut)
                filter.reset();
        }

        // 固定处理顺序：LowCut → LowShelf → Peak → HighShelf → HighCut
        float processSample(float sample)
        {
            for (auto& filter : lowCut)
                sample = filter.processSample(sample);
            sample = lowShelf.processSample(sample);
            sample = peak.processSample(sample);
            sample = highShelf.processSample(sample);
            for (auto& filter : highCut)
                sample = filter.processSample(sample);
            return sample;
        }
    };

    // 单个二阶段在 z = e^{-jw} 处的 |H|²（double 计算），只读系数、不触碰状态：
    // H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
    // 系数经 JUCE Coefficients 归一化（a0 = 1）后按 getRawCoefficients() 布局
    // [b0, b1, b2, a1, a2] 存放。
    static double sectionSquaredMagnitude(const juce::dsp::IIR::Coefficients<float>& coefficients,
                                          double c1, double s1, double c2, double s2)
    {
        const float* raw = coefficients.getRawCoefficients();  // [b0, b1, b2, a1, a2]
        const double b0 = raw[0];
        const double b1 = raw[1];
        const double b2 = raw[2];
        const double a1 = raw[3];
        const double a2 = raw[4];

        const double numRe = b0 + b1 * c1 + b2 * c2;
        const double numIm = -(b1 * s1 + b2 * s2);
        const double denRe = 1.0 + a1 * c1 + a2 * c2;
        const double denIm = -(a1 * s1 + a2 * s2);

        return (numRe * numRe + numIm * numIm) / (denRe * denRe + denIm * denIm);
    }

    static std::array<Coefficients, kSectionsPerChannel> designCoefficients(
        double sampleRate, const EqSettings& settings)
    {
        std::array<Coefficients, kSectionsPerChannel> coefficients;

        // LowCut：8 阶 Butterworth 高通，4 个最小相位二阶段级联
        const auto lowCut = juce::dsp::FilterDesign<float>::designIIRHighpassHighOrderButterworthMethod(
            settings.lowCutFrequencyHz, sampleRate, kButterworthOrder);
        for (int i = 0; i < kSectionsPerCut; ++i)
            coefficients[i] = lowCut[i];

        // LowShelf / Peak / HighShelf：RBJ 最小相位二阶段，Q 固定 2.0
        const float shelfQ = static_cast<float>(kShelfQ);
        coefficients[kSectionsPerCut] = juce::dsp::IIR::Coefficients<float>::makeLowShelf(
            sampleRate, settings.lowShelfFrequencyHz, shelfQ,
            juce::Decibels::decibelsToGain(settings.lowShelfGainDb));
        coefficients[kSectionsPerCut + 1] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
            sampleRate, settings.peakFrequencyHz, shelfQ,
            juce::Decibels::decibelsToGain(settings.peakGainDb));
        coefficients[kSectionsPerCut + 2] = juce::dsp::IIR::Coefficients<float>::makeHighShelf(
            sampleRate, settings.highShelfFrequencyHz, shelfQ,
            juce::Decibels::decibelsToGain(settings.highShelfGainDb));

        // HighCut：8 阶 Butterworth 低通，4 个最小相位二阶段级联
        const auto highCut = juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod(
            settings.highCutFrequencyHz, sampleRate, kButterworthOrder);
        for (int i = 0; i < kSectionsPerCut; ++i)
            coefficients[kSectionsPerCut + 3 + i] = highCut[i];

        return coefficients;
    }

    bool active_ = false;
    double sampleRate_ = 0.0;  // prepare 记录，magnitudeDb 解析频响使用
    std::array<ChannelState, kMaxChannels> channels_;
};

} // namespace OpenTune
