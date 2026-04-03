#pragma once

/**
 * CPU特性检测模块
 * 
 * 检测CPU支持的SIMD指令集（SSE2/AVX/AVX2/AVX-512），
 * 用于运行时自动选择最优的SIMD实现。
 */

#include <juce_core/juce_core.h>
#include <string>

namespace OpenTune {

/**
 * CpuFeatures - CPU特性检测类
 * 
 * 单例模式，在程序启动时检测并缓存CPU特性。
 */
class CpuFeatures {
public:
    /**
     * SimdLevel - SIMD支持级别枚举
     */
    enum class SimdLevel {
        None,      // 无SIMD支持
        SSE2,      // SSE2（所有x64都支持）
        SSE41,     // SSE4.1
        AVX,       // AVX
        AVX2,      // AVX2 + FMA
        AVX512,    // AVX-512（需要特别检测）
        NEON       // ARM NEON (Apple Silicon, 128-bit)
    };

    /**
     * 获取单例实例
     */
    static CpuFeatures& getInstance();

    /**
     * 检测并缓存CPU特性（在程序启动时调用一次）
     */
    void detect();

    /**
     * 获取最高支持的SIMD级别
     */
    SimdLevel getSimdLevel() const { return simdLevel_; }

    /**
     * 检查特定特性是否支持
     */
    bool hasSSE2() const { return hasSSE2_; }
    bool hasSSE41() const { return hasSSE41_; }
    bool hasAVX() const { return hasAVX_; }
    bool hasAVX2() const { return hasAVX2_; }
    bool hasFMA() const { return hasFMA_; }
    bool hasAVX512() const { return hasAVX512_; }
    bool hasNEON() const { return hasNEON_; }

    /**
     * 获取CPU信息字符串（用于日志）
     */
    std::string getCpuInfoString() const;

private:
    CpuFeatures() = default;
    ~CpuFeatures() = default;

    // 禁止拷贝
    CpuFeatures(const CpuFeatures&) = delete;
    CpuFeatures& operator=(const CpuFeatures&) = delete;

    // 缓存的检测结果
    bool detected_ = false;
    SimdLevel simdLevel_ = SimdLevel::SSE2;
    
    bool hasSSE2_ = true;       // x64默认支持
    bool hasSSE41_ = false;
    bool hasAVX_ = false;
    bool hasAVX2_ = false;
    bool hasFMA_ = false;
    bool hasAVX512_ = false;
    bool hasNEON_ = false;

    int physicalCores_ = 1;     // 物理核心数
    int logicalCores_ = 1;      // 逻辑核心数
    std::string cpuBrand_;      // CPU品牌字符串
};

} // namespace OpenTune
