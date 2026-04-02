#include "CpuFeatures.h"
#include "AppLogger.h"
#include <juce_core/juce_core.h>

#ifdef _WIN32
#include <intrin.h>
#endif

#include <thread>

namespace OpenTune {

CpuFeatures& CpuFeatures::getInstance() {
    static CpuFeatures instance;
    return instance;
}

void CpuFeatures::detect() {
    if (detected_) return;
    
    // 使用 JUCE 的 SystemStats 进行基础检测
    hasSSE2_ = juce::SystemStats::hasSSE2();
    hasSSE41_ = juce::SystemStats::hasSSE41();
    hasAVX_ = juce::SystemStats::hasAVX();
    hasAVX2_ = juce::SystemStats::hasAVX2();
    
#ifdef _WIN32
    // 使用 __cpuid 检测 FMA 和 AVX-512
    int cpuInfo[4] = {0};
    
    // 检测 FMA (CPUID.1:ECX.FMA[bit 12])
    __cpuid(cpuInfo, 1);
    hasFMA_ = (cpuInfo[2] & (1 << 12)) != 0;
    
    // 检测 AVX-512 (CPUID.7.0:EBX.AVX512F[bit 16])
    // AVX-512 需要多个子特性，我们只检测基础的 AVX512F
    __cpuidex(cpuInfo, 7, 0);
    bool avx512f = (cpuInfo[1] & (1 << 16)) != 0;
    
    // 额外检查：AVX-512 需要 OS 支持（通过 XGETBV 检查 XCR0 位 5-7）
    // 为了安全，只在 AVX512F 存在且 AVX2 也存在时才启用
    hasAVX512_ = avx512f && hasAVX2_;
    
    // 获取 CPU 品牌字符串
    char brand[49] = {0};
    __cpuid(cpuInfo, 0x80000000);
    if (static_cast<unsigned int>(cpuInfo[0]) >= 0x80000004) {
        __cpuid(reinterpret_cast<int*>(brand), 0x80000002);
        __cpuid(reinterpret_cast<int*>(brand + 16), 0x80000003);
        __cpuid(reinterpret_cast<int*>(brand + 32), 0x80000004);
        cpuBrand_ = brand;
        // 去除前后空格
        size_t start = cpuBrand_.find_first_not_of(' ');
        if (start != std::string::npos) {
            cpuBrand_ = cpuBrand_.substr(start);
        }
    }
#else
    // macOS/Linux 使用不同的检测方法
    hasFMA_ = hasAVX2_; // 通常 AVX2 CPU 都有 FMA
    hasAVX512_ = false; // 保守起见，非 Windows 平台禁用 AVX-512
#endif
    
    // 获取核心数
    logicalCores_ = static_cast<int>(std::thread::hardware_concurrency());
    physicalCores_ = juce::SystemStats::getNumPhysicalCpus();
    if (physicalCores_ <= 0) physicalCores_ = logicalCores_ / 2;
    if (physicalCores_ <= 0) physicalCores_ = 1;
    
    // 确定 SIMD 级别
    if (hasAVX512_) {
        simdLevel_ = SimdLevel::AVX512;
    } else if (hasAVX2_ && hasFMA_) {
        simdLevel_ = SimdLevel::AVX2;
    } else if (hasAVX_) {
        simdLevel_ = SimdLevel::AVX;
    } else if (hasSSE41_) {
        simdLevel_ = SimdLevel::SSE41;
    } else if (hasSSE2_) {
        simdLevel_ = SimdLevel::SSE2;
    } else {
        simdLevel_ = SimdLevel::None;
    }
    
    detected_ = true;
    
    AppLogger::debug("[CpuFeatures] " + juce::String(getCpuInfoString()));
}

std::string CpuFeatures::getCpuInfoString() const {
    std::string result;
    
    if (!cpuBrand_.empty()) {
        result = cpuBrand_ + " | ";
    }
    
    result += "SIMD: ";
    switch (simdLevel_) {
        case SimdLevel::AVX512: result += "AVX-512"; break;
        case SimdLevel::AVX2: result += "AVX2+FMA"; break;
        case SimdLevel::AVX: result += "AVX"; break;
        case SimdLevel::SSE41: result += "SSE4.1"; break;
        case SimdLevel::SSE2: result += "SSE2"; break;
        default: result += "None"; break;
    }
    
    result += " | Cores: " + std::to_string(physicalCores_) + "P/" + std::to_string(logicalCores_) + "L";
    
    return result;
}

} // namespace OpenTune
