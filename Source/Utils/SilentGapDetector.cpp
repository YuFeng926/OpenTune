#include "SilentGapDetector.h"
#include "SimdAccelerator.h"
#include "TimeCoordinate.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <mutex>

namespace OpenTune {

namespace {

SilentGapDetector::DetectionConfig makeDefaultConfig()
{
    SilentGapDetector::DetectionConfig cfg;
    cfg.strictThreshold_dB = SilentGapDetector::kDefaultThreshold_dB;
    cfg.relaxedTotalThreshold_dB = SilentGapDetector::kRelaxedTotalThreshold_dB;
    cfg.lowBandThreshold_dB = SilentGapDetector::kLowBandThreshold_dB;
    cfg.highPassCutoffHz = SilentGapDetector::kHighPassCutoffHz;
    cfg.lowBandUpperHz = SilentGapDetector::kLowBandUpperHz;
    cfg.minGapDurationMs = SilentGapDetector::kMinGapDurationMs;
    return cfg;
}

SilentGapDetector::DetectionConfig sanitizeConfig(SilentGapDetector::DetectionConfig cfg)
{
    // 合法范围保护：稳定优先
    cfg.strictThreshold_dB = juce::jlimit(-120.0f, 0.0f, cfg.strictThreshold_dB);
    cfg.relaxedTotalThreshold_dB = juce::jlimit(-120.0f, 0.0f, cfg.relaxedTotalThreshold_dB);
    cfg.lowBandThreshold_dB = juce::jlimit(-120.0f, 0.0f, cfg.lowBandThreshold_dB);
    cfg.highPassCutoffHz = juce::jlimit(1.0, 500.0, cfg.highPassCutoffHz);
    cfg.lowBandUpperHz = juce::jlimit(200.0, 20000.0, cfg.lowBandUpperHz);
    cfg.minGapDurationMs = juce::jlimit(1.0, 200.0, cfg.minGapDurationMs);

    // 保证“放宽阈值”不比“严格阈值”更严格
    if (cfg.relaxedTotalThreshold_dB < cfg.strictThreshold_dB) {
        cfg.relaxedTotalThreshold_dB = cfg.strictThreshold_dB;
    }

    // 保证低频带上限高于高通截止
    if (cfg.lowBandUpperHz <= cfg.highPassCutoffHz) {
        cfg.lowBandUpperHz = std::min(20000.0, cfg.highPassCutoffHz + 100.0);
    }

    return cfg;
}

std::mutex& configMutex()
{
    static std::mutex m;
    return m;
}

SilentGapDetector::DetectionConfig& globalConfig()
{
    static SilentGapDetector::DetectionConfig cfg = makeDefaultConfig();
    return cfg;
}

} // namespace

SilentGapDetector::DetectionConfig SilentGapDetector::getConfig()
{
    std::lock_guard<std::mutex> lock(configMutex());
    return globalConfig();
}

void SilentGapDetector::setConfig(const DetectionConfig& config)
{
    std::lock_guard<std::mutex> lock(configMutex());
    globalConfig() = sanitizeConfig(config);
}

void SilentGapDetector::resetConfig()
{
    std::lock_guard<std::mutex> lock(configMutex());
    globalConfig() = makeDefaultConfig();
}

// ============================================================================
// 私有辅助方法
// ============================================================================

float SilentGapDetector::calculateRmsDb(const float* data, int64_t numSamples)
{
    // 计算 RMS 电平（dB）
    if (numSamples <= 0 || data == nullptr) return -100.0f;
    
    // 使用 SIMD 加速平方和计算
    auto& simd = SimdAccelerator::getInstance();
    float sumSquares = simd.sumOfSquares(data, static_cast<size_t>(numSamples));
    
    float rms = std::sqrt(sumSquares / static_cast<float>(numSamples));
    return linearToDb(rms);
}

std::vector<SilentGap>::const_iterator SilentGapDetector::findGapAtOrAfter(
    const std::vector<SilentGap>& gaps,
    double positionSeconds)
{
    // 二分查找：找到第一个 endSeconds > positionSeconds 的静息处
    return std::lower_bound(gaps.begin(), gaps.end(), positionSeconds,
        [](const SilentGap& gap, double posSec) {
            return gap.endSeconds <= posSec;
        });
}

// ============================================================================
// 静息处检测
// ============================================================================

std::vector<SilentGap> SilentGapDetector::detectAllGaps(
    const juce::AudioBuffer<float>& audio,
    float threshold_dB)
{
    std::vector<SilentGap> result;
    
    const int numSamples = audio.getNumSamples();
    const int numChannels = audio.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0) return result;
    
    const auto cfg = getConfig();
    const float effectiveStrictThreshold = std::isfinite(threshold_dB) ? threshold_dB : cfg.strictThreshold_dB;
    const double sampleRate = kInternalSampleRate;
    const int64_t minGapSamples = static_cast<int64_t>(getMinGapDurationSec(cfg.minGapDurationMs) * sampleRate);
    
    // 分析窗口大小：约 2ms，用于平滑电平检测
    const int64_t windowSize = std::max<int64_t>(1, static_cast<int64_t>(sampleRate * 0.002));

    // 先混合到单声道（项目主路径通常是单声道，此处兼容多声道）
    std::vector<float> mono(static_cast<size_t>(numSamples), 0.0f);
    const float invChannels = 1.0f / static_cast<float>(numChannels);
    for (int ch = 0; ch < numChannels; ++ch) {
        const float* channelData = audio.getReadPointer(ch);
        for (int i = 0; i < numSamples; ++i) {
            mono[static_cast<size_t>(i)] += channelData[i] * invChannels;
        }
    }

    // 频域判定预处理：先高通 60Hz，再得到 <=3kHz 低频带信号
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

    // 窗口 RMS 优化：预平方 + 前缀和，O(1) 窗口查询，避免每窗重复 sumOfSquares
    auto& simd = SimdAccelerator::getInstance();
    std::vector<float> highPassedSq(static_cast<size_t>(numSamples), 0.0f);
    std::vector<float> lowBandSq(static_cast<size_t>(numSamples), 0.0f);
    simd.multiply(highPassedSq.data(), highPassed.data(), highPassed.data(), static_cast<size_t>(numSamples));
    simd.multiply(lowBandSq.data(), lowBand.data(), lowBand.data(), static_cast<size_t>(numSamples));

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

        // 判定逻辑（放宽版）：
        // 1) 兼容旧机制：总电平 <= threshold_dB(默认 -40dBFS) 必然视为静息
        // 2) 放宽机制：总电平 <= -30dBFS 且 <=3kHz 低频带平均电平 < -40dBFS
        const bool passLegacyVeryLow = (totalLevel_dB <= effectiveStrictThreshold);
        const bool passRelaxedFreqRule =
            (totalLevel_dB <= cfg.relaxedTotalThreshold_dB) &&
            (lowBandLevel_dB < cfg.lowBandThreshold_dB);

        bool isSilent = passLegacyVeryLow || passRelaxedFreqRule;
        
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
                gap.startSeconds = TimeCoordinate::samplesToSeconds(silenceStart, sampleRate);
                gap.endSeconds = TimeCoordinate::samplesToSeconds(pos, sampleRate);
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
            gap.startSeconds = TimeCoordinate::samplesToSeconds(silenceStart, sampleRate);
            gap.endSeconds = TimeCoordinate::samplesToSeconds(numSamples, sampleRate);
            gap.minLevel_dB = minLevelInGap;
            result.push_back(gap);
        }
    }
    
    return result;
}

std::vector<SilentGap> SilentGapDetector::detectAllGapsAdaptive(
    const juce::AudioBuffer<float>& audio,
    double maxSearchDistanceSec)
{
    // maxSearchDistanceSec 参数保留用于将来扩展，当前使用主检测逻辑
    juce::ignoreUnused(maxSearchDistanceSec);
    return detectAllGaps(audio);
}

// ============================================================================
// 静息处查找
// ============================================================================

std::optional<SilentGap> SilentGapDetector::findNearestGap(
    const std::vector<SilentGap>& gaps,
    double positionSeconds,
    double maxSearchDistanceSec,
    bool searchForward)
{
    if (gaps.empty()) return std::nullopt;
    
    if (searchForward) {
        // 向后搜索：找第一个 startSeconds >= positionSeconds 的静息处
        auto it = std::lower_bound(gaps.begin(), gaps.end(), positionSeconds,
            [](const SilentGap& gap, double posSec) {
                return gap.startSeconds < posSec;
            });
        
        // 也检查包含 positionSeconds 的静息处
        if (it != gaps.begin()) {
            auto prev = std::prev(it);
            if (prev->endSeconds > positionSeconds) {
                return *prev;  // positionSeconds 在这个静息处内部
            }
        }
        
        if (it != gaps.end() && (it->startSeconds - positionSeconds) <= maxSearchDistanceSec) {
            return *it;
        }
    }
    else {
        // 向前搜索：找最后一个 endSeconds <= positionSeconds 的静息处
        auto it = std::lower_bound(gaps.begin(), gaps.end(), positionSeconds,
            [](const SilentGap& gap, double posSec) {
                return gap.endSeconds <= posSec;
            });
        
        // 检查是否 positionSeconds 在某个静息处内部
        if (it != gaps.end() && it->startSeconds <= positionSeconds && it->endSeconds > positionSeconds) {
            return *it;  // positionSeconds 在这个静息处内部
        }
        
        // 否则找前一个静息处
        if (it != gaps.begin()) {
            auto prev = std::prev(it);
            if ((positionSeconds - prev->endSeconds) <= maxSearchDistanceSec) {
                return *prev;
            }
        }
    }
    
    return std::nullopt;
}

std::optional<SilentGap> SilentGapDetector::findGapContaining(
    const std::vector<SilentGap>& gaps,
    double positionSeconds)
{
    if (gaps.empty()) return std::nullopt;
    
    auto it = findGapAtOrAfter(gaps, positionSeconds);
    if (it != gaps.end() && it->startSeconds <= positionSeconds && it->endSeconds > positionSeconds) {
        return *it;
    }
    
    return std::nullopt;
}

bool SilentGapDetector::hasGapBetween(
    const std::vector<SilentGap>& gaps,
    double startSeconds,
    double endSeconds)
{
    if (gaps.empty() || endSeconds <= startSeconds) return false;
    const double minGapSec = getMinGapDurationSec();
    
    // 找第一个可能在范围内的静息处
    auto it = std::lower_bound(gaps.begin(), gaps.end(), startSeconds,
        [](const SilentGap& gap, double posSec) {
            return gap.endSeconds <= posSec;
        });
    
    // 检查是否有静息处完全在 [startSeconds, endSeconds] 范围内
    while (it != gaps.end() && it->startSeconds < endSeconds) {
        if (it->startSeconds >= startSeconds && it->endSeconds <= endSeconds) {
            return true;
        }
        // 也接受部分重叠的静息处（至少有 minGap/2 在范围内）
        double overlapStart = std::max(it->startSeconds, startSeconds);
        double overlapEnd = std::min(it->endSeconds, endSeconds);
        if (overlapEnd - overlapStart >= minGapSec / 2.0) {
            return true;
        }
        ++it;
    }
    
    return false;
}

std::vector<SilentGap> SilentGapDetector::getGapsBetween(
    const std::vector<SilentGap>& gaps,
    double startSeconds,
    double endSeconds)
{
    std::vector<SilentGap> result;
    if (gaps.empty() || endSeconds <= startSeconds) return result;
    
    auto it = std::lower_bound(gaps.begin(), gaps.end(), startSeconds,
        [](const SilentGap& gap, double posSec) {
            return gap.endSeconds <= posSec;
        });
    
    while (it != gaps.end() && it->startSeconds < endSeconds) {
        result.push_back(*it);
        ++it;
    }
    
    return result;
}

} // namespace OpenTune
