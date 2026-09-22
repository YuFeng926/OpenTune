#include "SilentGapDetector.h"
#include "TimeCoordinate.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>

namespace OpenTune {

// ============================================================================
// 静息处检测
// ============================================================================

std::vector<SilentGap> SilentGapDetector::detectAllGapsAdaptive(
    const juce::AudioBuffer<float>& audio)
{
    std::vector<SilentGap> result;
    
    const int numSamples = audio.getNumSamples();
    const int numChannels = audio.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0) return result;
    
    const DetectionConfig cfg{};
    const double sampleRate = kInternalSampleRate;
    const int64_t minGapSamples = TimeCoordinate::secondsToSamples(getMinGapDurationSec(cfg.minGapDurationMs), sampleRate);
    
    // 分析窗口大小：约 2ms，用于平滑电平检测
    const int64_t windowSize = std::max<int64_t>(1, TimeCoordinate::secondsToSamples(0.002, sampleRate));

    // 混合到单声道（支持多声道输入）
    std::vector<float> mono(static_cast<size_t>(numSamples), 0.0f);
    const float invChannels = 1.0f / static_cast<float>(numChannels);
    for (int ch = 0; ch < numChannels; ++ch) {
        const float* channelData = audio.getReadPointer(ch);
        for (int i = 0; i < numSamples; ++i) {
            mono[static_cast<size_t>(i)] += channelData[i] * invChannels;
        }
    }

    // 频域判定预处理：先高通 60Hz，再得到 <=2kHz 低频带信号
    std::vector<float> highPassed = mono;
    std::vector<float> lowBand = mono;

    // 改为 juce::dsp 路径：高阶 Butterworth IIR 链（更利于 DSP 优化路径）
    auto processIIRChain = [numSamples](std::vector<float>& data,
                                        const auto& coeffs) {
        float* channelData = data.data();
        juce::dsp::AudioBlock<float> block(&channelData, 1, static_cast<size_t>(numSamples));
        juce::dsp::ProcessContextReplacing<float> context(block);

        for (const auto& c : coeffs) {
            juce::dsp::IIR::Filter<float> filter;
            filter.coefficients = c;
            filter.reset();
            filter.process(context);
        }
    };

    // 高通：4阶 Butterworth（60Hz）
    auto hpCoeffs = juce::dsp::FilterDesign<float>::designIIRHighpassHighOrderButterworthMethod(
        static_cast<float>(cfg.highPassCutoffHz), sampleRate, 4);
    processIIRChain(highPassed, hpCoeffs);

    // 低频带：在高通结果上再做低通到 lowBandUpperHz（4阶）
    lowBand = highPassed;
    auto lpCoeffs = juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod(
        static_cast<float>(cfg.lowBandUpperHz), sampleRate, 4);
    processIIRChain(lowBand, lpCoeffs);

    // 窗口 RMS 优化：预平方 + 前缀和，O(1) 窗口查询，避免每窗重复平方
    std::vector<float> highPassedSq(static_cast<size_t>(numSamples), 0.0f);
    std::vector<float> lowBandSq(static_cast<size_t>(numSamples), 0.0f);
    juce::FloatVectorOperations::multiply(highPassedSq.data(), highPassed.data(), highPassed.data(), numSamples);
    juce::FloatVectorOperations::multiply(lowBandSq.data(), lowBand.data(), lowBand.data(), numSamples);

    std::vector<double> prefixHigh(static_cast<size_t>(numSamples) + 1, 0.0);
    std::vector<double> prefixLow(static_cast<size_t>(numSamples) + 1, 0.0);
    for (int i = 0; i < numSamples; ++i) {
        prefixHigh[static_cast<size_t>(i) + 1] = prefixHigh[static_cast<size_t>(i)] + static_cast<double>(highPassedSq[static_cast<size_t>(i)]);
        prefixLow[static_cast<size_t>(i) + 1] = prefixLow[static_cast<size_t>(i)] + static_cast<double>(lowBandSq[static_cast<size_t>(i)]);
    }
    
    // 状态机：跟踪是否在静息段内
    bool inSilence = false;
    int64_t silenceStart = 0;
    float minLevelInGap = 0.0f;
    
    for (int64_t pos = 0; pos < numSamples; pos += windowSize) {
        // 计算当前窗口的电平特征（高通后总电平 + 低频带电平）
        int64_t windowEnd = std::min<int64_t>(pos + windowSize, numSamples);
        int64_t windowLen = windowEnd - pos;

        const double sumHigh = prefixHigh[static_cast<size_t>(windowEnd)] - prefixHigh[static_cast<size_t>(pos)];
        const double sumLow = prefixLow[static_cast<size_t>(windowEnd)] - prefixLow[static_cast<size_t>(pos)];
        const float rmsHigh = static_cast<float>(std::sqrt(sumHigh / static_cast<double>(windowLen)));
        const float rmsLow = static_cast<float>(std::sqrt(sumLow / static_cast<double>(windowLen)));
        float totalLevel_dB = linearToDb(rmsHigh);
        float lowBandLevel_dB = linearToDb(rmsLow);

        // 判定逻辑（两级）（均为配置驱动）：
        // 1) 严格阈值：总电平 <= cfg.strictThreshold_dB 视为静息
        // 2) 放宽频域规则：总电平 <= cfg.relaxedTotalThreshold_dB，且低频带(<= cfg.lowBandUpperHz)平均电平 < cfg.lowBandThreshold_dB
        const bool passStrictThreshold = (totalLevel_dB <= cfg.strictThreshold_dB);
        const bool passRelaxedFreqRule =
            (totalLevel_dB <= cfg.relaxedTotalThreshold_dB) &&
            (lowBandLevel_dB < cfg.lowBandThreshold_dB);

        bool isSilent = passStrictThreshold || passRelaxedFreqRule;
        
        if (isSilent && !inSilence) {
            // 进入静息段
            inSilence = true;
            silenceStart = pos;
            minLevelInGap = totalLevel_dB;
        }
        else if (isSilent && inSilence) {
            // 仍在静息段内，更新最低电平
            float levelDb = totalLevel_dB;
            if (levelDb < minLevelInGap) {
                minLevelInGap = levelDb;
            }
        }
        else if (!isSilent && inSilence) {
            // 离开静息段，检查是否满足最小长度
            int64_t gapLength = pos - silenceStart;
            if (gapLength >= minGapSamples) {
                SilentGap gap;
                gap.startSample = silenceStart;
                gap.endSampleExclusive = pos;
                gap.minLevel_dB = minLevelInGap;
                result.push_back(gap);
            }
            inSilence = false;
        }
    }
    
    // 处理末尾的静息段
    if (inSilence) {
        int64_t gapLength = numSamples - silenceStart;
        if (gapLength >= minGapSamples) {
            SilentGap gap;
            gap.startSample = silenceStart;
            gap.endSampleExclusive = numSamples;
            gap.minLevel_dB = minLevelInGap;
            result.push_back(gap);
        }
    }
    
    return result;
}

} // namespace OpenTune
