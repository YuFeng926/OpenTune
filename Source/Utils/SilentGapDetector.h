#pragma once

/**
 * 静息处检测工具类
 * 
 * 静息处（Silent Gap）定义：
 * - 电平低于阈值（默认 -40dBFS）
 * - 持续时长不小于 50ms
 * 
 * 用途：
 * - 作为渲染 chunk 的天然边界
 * - 避免在有声区域做 crossfade 导致的相位问题
 * - 保护辅音和尾音不被切断
 * 
 * 设计原则：
 * - 导入时预计算，运行时 O(log n) 查找
 * - 采用频域约束放宽静息判定（低频能量门限）
 */

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <optional>
#include <cstdint>
#include <cmath>
#include <limits>
#include "TimeCoordinate.h"

namespace OpenTune {

/**
 * SilentGap - 静息处数据结构
 * 表示一段低电平区域
 * 
 * 注意：所有坐标均为秒数，与设备采样率无关
 */
struct SilentGap {
    double startSeconds;    // 静息处起始时间（秒，包含）
    double endSeconds;      // 静息处结束时间（秒，不包含）
    float minLevel_dB;      // 该区域内的最低电平（dB）
    
    /** 获取静息处时长（秒） */
    double length() const { return endSeconds - startSeconds; }
    
    /** 获取静息处中点位置（秒） */
    double midpoint() const { return startSeconds + length() * 0.5; }
    
    /** 检查某个时间位置是否在此静息处内 */
    bool contains(double seconds) const {
        return seconds >= startSeconds && seconds < endSeconds;
    }
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
        float strictThreshold_dB = -40.0f;        // 原规则：总电平低于该值直接判静息
        float relaxedTotalThreshold_dB = -30.0f;  // 放宽规则：总电平上限
        float lowBandThreshold_dB = -40.0f;       // 放宽规则：低频带(<=lowBandUpperHz)上限
        double highPassCutoffHz = 60.0;           // 检测前高通截止频率
        double lowBandUpperHz = 3000.0;           // 低频带上限频率
        double minGapDurationMs = 100.0;          // 最小静息时长
    };

    // ============================================================================
    // 默认参数定义（用于初始化配置）
    // ============================================================================
    
    /** 默认静息阈值 -40dBFS（配置初始值） */
    static constexpr float kDefaultThreshold_dB = -40.0f;
    
    /** 放宽判定：总电平阈值 -30dBFS（配置初始值） */
    static constexpr float kRelaxedTotalThreshold_dB = -30.0f;

    /** 放宽判定：低频带(<=3kHz)平均电平阈值 -40dBFS（配置初始值） */
    static constexpr float kLowBandThreshold_dB = -40.0f;

    /** 预处理高通截止频率（Hz，配置初始值） */
    static constexpr double kHighPassCutoffHz = 60.0;

    /** 低频带上限频率（Hz，配置初始值） */
    static constexpr double kLowBandUpperHz = 3000.0;

    /** 最小静息时长（ms，配置初始值） */
    static constexpr double kMinGapDurationMs = 100.0;

    /** 获取当前检测配置（线程安全） */
    static DetectionConfig getConfig();

    /** 设置检测配置（线程安全，内部会做参数归一化） */
    static void setConfig(const DetectionConfig& config);

    /** 恢复默认检测配置 */
    static void resetConfig();
    
    /** 固定采样率 44.1kHz（内部音频存储标准） */
    static constexpr double kInternalSampleRate = TimeCoordinate::kRenderSampleRate;
    
    /** 最大搜索距离（秒，默认 20 秒） */
    static constexpr double kMaxSearchDistanceSec = 20.0;
    
    // ============================================================================
    // 静息处检测
    // ============================================================================
    
    /**
     * 检测音频中的所有静息处（返回秒数坐标）
     * 
     * 注意：音频必须是 44.1kHz 采样率（符合内部存储标准）
     * 
     * @param audio 音频缓冲区（44.1kHz）
     * @param threshold_dB 电平阈值（默认 -40dBFS）
     * @return 按起始位置排序的静息处列表（秒数坐标）
     */
    static std::vector<SilentGap> detectAllGaps(
        const juce::AudioBuffer<float>& audio,
        float threshold_dB = std::numeric_limits<float>::quiet_NaN());
    
    /**
     * 使用自适应阈值检测静息处（返回秒数坐标）
     * 使用频域约束进行检测
     * 
     * 注意：音频必须是 44.1kHz 采样率（符合内部存储标准）
     * 
     * @param audio 音频缓冲区（44.1kHz）
     * @param maxSearchDistanceSec 保留参数（当前未使用）
     * @return 静息处列表（秒数坐标）
     */
    static std::vector<SilentGap> detectAllGapsAdaptive(
        const juce::AudioBuffer<float>& audio,
        double maxSearchDistanceSec = -1.0);  // -1 表示使用默认值
    
    // ============================================================================
    // 静息处查找
    // ============================================================================
    
    /**
     * 从指定位置向前或向后查找最近的静息处
     * 
     * @param gaps 预计算的静息处列表（必须按 startSeconds 排序）
     * @param positionSeconds 当前位置（秒）
     * @param maxSearchDistanceSec 最大搜索距离（秒）
     * @param searchForward true=向后搜索，false=向前搜索
     * @return 找到的静息处，如果没找到返回 nullopt
     */
    static std::optional<SilentGap> findNearestGap(
        const std::vector<SilentGap>& gaps,
        double positionSeconds,
        double maxSearchDistanceSec,
        bool searchForward);
    
    /**
     * 查找包含指定位置的静息处
     * 
     * @param gaps 预计算的静息处列表
     * @param positionSeconds 要检查的位置（秒）
     * @return 包含该位置的静息处，如果不在任何静息处内返回 nullopt
     */
    static std::optional<SilentGap> findGapContaining(
        const std::vector<SilentGap>& gaps,
        double positionSeconds);
    
    /**
     * 检查两个位置之间是否存在静息处
     * 
     * @param gaps 预计算的静息处列表
     * @param startSeconds 起始位置（秒）
     * @param endSeconds 结束位置（秒）
     * @return 如果存在静息处返回 true
     */
    static bool hasGapBetween(
        const std::vector<SilentGap>& gaps,
        double startSeconds,
        double endSeconds);
    
    /**
     * 获取两个位置之间的所有静息处
     * 
     * @param gaps 预计算的静息处列表
     * @param startSeconds 起始位置（秒）
     * @param endSeconds 结束位置（秒）
     * @return 该范围内的静息处列表
     */
    static std::vector<SilentGap> getGapsBetween(
        const std::vector<SilentGap>& gaps,
        double startSeconds,
        double endSeconds);
    
    // ============================================================================
    // 辅助函数
    // ============================================================================
    
    /**
     * 获取最小静息持续时长（秒）
     * @param minDurationMs 最小持续时长（毫秒，默认 100ms）
     * @return 秒数
     */
    static double getMinGapDurationSec(double minDurationMs = 100.0) {
        return minDurationMs / 1000.0;
    }
    
    /**
     * 获取最大搜索距离（秒）
     * @param maxSearchSeconds 最大搜索距离（秒，默认 20 秒）
     * @return 秒数
     */
    static double getMaxSearchDistanceSec(double maxSearchSeconds = 20.0) {
        return maxSearchSeconds;
    }
    
    /**
     * 将线性幅度转换为 dB
     */
    static float linearToDb(float linear) {
        if (linear <= 0.0f) return -100.0f;
        return 20.0f * std::log10(linear);
    }
    
    /**
     * 将 dB 转换为线性幅度
     */
    static float dbToLinear(float db) {
        return std::pow(10.0f, db / 20.0f);
    }

private:
    // 私有辅助方法
    
    /**
     * 计算音频块的 RMS 电平（dB）
     */
    static float calculateRmsDb(const float* data, int64_t numSamples);
    
    /**
     * 使用二分查找在排序的静息处列表中查找
     */
    static std::vector<SilentGap>::const_iterator findGapAtOrAfter(
        const std::vector<SilentGap>& gaps,
        double positionSeconds);
};

} // namespace OpenTune
