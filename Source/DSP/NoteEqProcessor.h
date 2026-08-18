#pragma once

/**
 * Per-note EQ processor — 动态滤波器链 DSP
 *
 * - 动态遍历 EqSettings.filters 向量，按类型分配二阶段：
 *   - LowCut / HighCut：JUCE FilterDesign 8 阶 Butterworth，4 个最小相位二阶段级联
 *   - LowShelf / HighShelf：RBJ 最小相位二阶段
 *   - Peak：RBJ 最小相位二阶段，使用 filter.q（默认 2.0）
 * - 预分配最大 kMaxFilters*kMaxSectionsPerFilter 的固定 std::array 状态，零动态分配。
 * - prepare 在渲染工作线程计算系数并绑定到状态；process 音频线程零分配。
 * - magnitudeDb 遍历动态 section，只读频响查询。
 */

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <cstddef>

#include "../Utils/NoteEqSettings.h"

namespace OpenTune {

class NoteEqProcessor {
public:
    static constexpr int kButterworthOrder = 8;
    static constexpr int kSectionsPerCut = kButterworthOrder / 2;
    static constexpr int kMaxSectionsPerFilter = kSectionsPerCut;  // Cut 最多 4 个二阶段
    static constexpr int kMaxTotalSections = EqSettings::kMaxFilters * kMaxSectionsPerFilter;
    static constexpr int kMaxChannels = 2;

    NoteEqProcessor() = default;

    void prepare(double sampleRate, const EqSettings& settings)
    {
        active_ = settings.active && !settings.filters.empty();
        sampleRate_ = sampleRate;
        activeSectionCount_ = 0;

        designAndBind(sampleRate, settings);
        reset();
    }

    void reset()
    {
        for (auto& channel : channels_)
            channel.reset(activeSectionCount_);
    }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (!active_)
            return;

        const int numChannels = buffer.getNumChannels();
        jassert(numChannels == 1 || numChannels == 2);

        for (int c = 0; c < numChannels; ++c) {
            auto& channel = channels_[static_cast<size_t>(c)];
            auto* samples = buffer.getWritePointer(c);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                samples[i] = channel.processSample(samples[i], activeSectionCount_);
        }
    }

    bool isActive() const { return active_; }

    double magnitudeDb(double frequencyHz) const
    {
        jassert(sampleRate_ > 0.0);

        const double w = 2.0 * juce::MathConstants<double>::pi * frequencyHz / sampleRate_;
        const double c1 = std::cos(w);
        const double s1 = std::sin(w);
        const double c2 = std::cos(2.0 * w);
        const double s2 = std::sin(2.0 * w);

        const auto& channel = channels_[0];
        double squaredMagnitude = 1.0;
        for (int i = 0; i < activeSectionCount_; ++i)
            squaredMagnitude *= sectionSquaredMagnitude(*channel.sections[i].coefficients, c1, s1, c2, s2);

        return 10.0 * std::log10(squaredMagnitude);
    }

private:
    using Coefficients = juce::ReferenceCountedObjectPtr<juce::dsp::IIR::Coefficients<float>>;

    struct ChannelState {
        std::array<juce::dsp::IIR::Filter<float>, kMaxTotalSections> sections;

        void reset(int count)
        {
            for (int i = 0; i < count; ++i)
                sections[static_cast<size_t>(i)].reset();
        }

        float processSample(float sample, int count)
        {
            for (int i = 0; i < count; ++i)
                sample = sections[static_cast<size_t>(i)].processSample(sample);
            return sample;
        }
    };

    static double sectionSquaredMagnitude(const juce::dsp::IIR::Coefficients<float>& coefficients,
                                          double c1, double s1, double c2, double s2)
    {
        const float* raw = coefficients.getRawCoefficients();
        const double b0 = raw[0], b1 = raw[1], b2 = raw[2], a1 = raw[3], a2 = raw[4];
        const double numRe = b0 + b1 * c1 + b2 * c2;
        const double numIm = -(b1 * s1 + b2 * s2);
        const double denRe = 1.0 + a1 * c1 + a2 * c2;
        const double denIm = -(a1 * s1 + a2 * s2);
        return (numRe * numRe + numIm * numIm) / (denRe * denRe + denIm * denIm);
    }

    void designAndBind(double sampleRate, const EqSettings& settings)
    {
        std::array<Coefficients, kMaxTotalSections> coefficients;
        int sectionIdx = 0;

        for (const auto& filter : settings.filters) {
            if (sectionIdx >= kMaxTotalSections)
                break;

            switch (filter.type) {
            case EqFilterType::LowCut: {
                const auto lowCut = juce::dsp::FilterDesign<float>::
                    designIIRHighpassHighOrderButterworthMethod(
                        filter.frequencyHz, sampleRate, kButterworthOrder);
                for (int i = 0; i < kSectionsPerCut && sectionIdx < kMaxTotalSections; ++i)
                    coefficients[static_cast<size_t>(sectionIdx++)] = lowCut[i];
                break;
            }
            case EqFilterType::HighCut: {
                const auto highCut = juce::dsp::FilterDesign<float>::
                    designIIRLowpassHighOrderButterworthMethod(
                        filter.frequencyHz, sampleRate, kButterworthOrder);
                for (int i = 0; i < kSectionsPerCut && sectionIdx < kMaxTotalSections; ++i)
                    coefficients[static_cast<size_t>(sectionIdx++)] = highCut[i];
                break;
            }
            case EqFilterType::LowShelf:
                coefficients[static_cast<size_t>(sectionIdx++)] =
                    juce::dsp::IIR::Coefficients<float>::makeLowShelf(
                        sampleRate, filter.frequencyHz, filter.q,
                        juce::Decibels::decibelsToGain(filter.gainDb));
                break;
            case EqFilterType::HighShelf:
                coefficients[static_cast<size_t>(sectionIdx++)] =
                    juce::dsp::IIR::Coefficients<float>::makeHighShelf(
                        sampleRate, filter.frequencyHz, filter.q,
                        juce::Decibels::decibelsToGain(filter.gainDb));
                break;
            case EqFilterType::Peak:
                coefficients[static_cast<size_t>(sectionIdx++)] =
                    juce::dsp::IIR::Coefficients<float>::makePeakFilter(
                        sampleRate, filter.frequencyHz, filter.q,
                        juce::Decibels::decibelsToGain(filter.gainDb));
                break;
            }
        }

        activeSectionCount_ = sectionIdx;

        for (auto& channel : channels_)
            for (int i = 0; i < activeSectionCount_; ++i)
                channel.sections[static_cast<size_t>(i)].coefficients = coefficients[static_cast<size_t>(i)];
    }

    bool active_ = false;
    double sampleRate_ = 0.0;
    int activeSectionCount_ = 0;
    std::array<ChannelState, kMaxChannels> channels_;
};

} // namespace OpenTune
