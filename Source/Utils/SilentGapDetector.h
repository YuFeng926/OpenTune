#pragma once

/**
 * 静息处检测工具类
 * 
 * 静息处（Silent Gap）定义：
 * - 电平低于阈值（默认 -45dBFS）
 * - 持续时长不小于 50ms
 * 
 * 用途：
 * - 作为渲染 chunk 的天然边界
 * - 边界处施加交叉淡化（默认10ms）避免硬切分
 * - 保护辅音和尾音不被切断
 * 
 * 设计原则：
 * - 导入时预计算，运行时 O(log n) 查找
 * - 采用频域约束放宽静息判定（低频能量门限）
 */

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <cstdint>
#include <cmath>
#include "TimeCoordinate.h"

namespace OpenTune {

/**
 * SilentGap - 静息处数据结构
 * 表示一段低电平区域
 * 
 * 注意：所有事实坐标均为 44.1kHz sample span。
 * 秒值只作为从 sample 投影出来的视图，不再作为内部真相源。
 */
struct SilentGap {
    int64_t startSample{0};         // 静息处起始 sample（包含）
    int64_t endSampleExclusive{0};  // 静息处结束 sample（不包含）
    float minLevel_dB{0.0f};        // 该区域内的最低电平（dB）

    int64_t sampleCount() const { return endSampleExclusive - startSample; }

    bool isValid() const { return endSampleExclusive > startSample; }

    int64_t midpointSample() const { return startSample + sampleCount() / 2; }
};

/**
 * SilentGapDetector - 静息处检测器
 * 
 * 提供静态方法用于：
 * - 检测音频中的所有静息处
 * - 查找最近的静息处
 * - 根据静息处计算 chunk 边界
 */
class SilentGapDetector {
public:
    struct DetectionConfig {
        float strictThreshold_dB = -45.0f;        // 原规则：总电平低于该值直接判静息
        float relaxedTotalThreshold_dB = -40.0f;  // 放宽规则：总电平上限
        float lowBandThreshold_dB = -40.0f;       // 放宽规则：低频带(<=lowBandUpperHz)上限
        double highPassCutoffHz = 60.0;           // 检测前高通截止频率
        double lowBandUpperHz = 2000.0;           // 低频带上限频率
        double minGapDurationMs = 50.0;           // 最小静息时长
    };

    /** 固定采样率 44.1kHz（内部音频存储标准） */
    static constexpr double kInternalSampleRate = TimeCoordinate::kRenderSampleRate;
    
    // ============================================================================
    // 静息处检测
    // ============================================================================
    
    /**
     * 检测音频中的所有静息处（返回 sample span）
     * 
     * 判定规则（配置驱动）：
     * - 严格阈值：窗口总电平 <= cfg.strictThreshold_dB
     * - 放宽频域规则：窗口总电平 <= cfg.relaxedTotalThreshold_dB
     *   且低频带(<= cfg.lowBandUpperHz)电平 < cfg.lowBandThreshold_dB
     * 
     * 注意：音频必须是 44.1kHz 采样率（符合内部存储标准）
     * 
     * @param audio 音频缓冲区（44.1kHz）
     * @return 按起始 sample 排序的静息处列表（sample span）
     */
    static std::vector<SilentGap> detectAllGapsAdaptive(
        const juce::AudioBuffer<float>& audio);
    
    // ============================================================================
    // 辅助函数
    // ============================================================================
    
    /**
     * 获取最小静息持续时长（秒）
     * @param minDurationMs 最小持续时长（毫秒，默认 50ms）
     * @return 秒数
     */
    static double getMinGapDurationSec(double minDurationMs = 50.0) {
        return minDurationMs / 1000.0;
    }
    
    /**
     * 将线性幅度转换为 dB
     */
    static float linearToDb(float linear) {
        if (linear <= 0.0f) return -100.0f;
        return 20.0f * std::log10(linear);
    }
};

} // namespace OpenTune
