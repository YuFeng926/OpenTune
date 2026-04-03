#include "CpuFeatures.h"
#include "AppLogger.h"
#include <juce_core/juce_core.h>

#ifdef _WIN32
#include <intrin.h>
#endif

#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#include <thread>

namespace OpenTune {

CpuFeatures& CpuFeatures::getInstance() {
    static CpuFeatures instance;
    return instance;
}

void CpuFeatures::detect() {
    if (detected_) return;

#if defined(__aarch64__)
    // ARM64 (Apple Silicon): NEON is always available on AArch64
    hasSSE2_ = false;
    hasSSE41_ = false;
    hasAVX_ = false;
    hasAVX2_ = false;
    hasFMA_ = false;
    hasAVX512_ = false;
    hasNEON_ = true;
    simdLevel_ = SimdLevel::NEON;

    // Get CPU brand via sysctl
    char brand[256] = {0};
    size_t brandLen = sizeof(brand);
    if (sysctlbyname("machdep.cpu.brand_string", brand, &brandLen, nullptr, 0) == 0) {
        cpuBrand_ = brand;
    }
#else
    // x86 path: use JUCE SystemStats for basic detection
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
    __cpuidex(cpuInfo, 7, 0);
    bool avx512f = (cpuInfo[1] & (1 << 16)) != 0;

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
        size_t start = cpuBrand_.find_first_not_of(' ');
        if (start != std::string::npos) {
            cpuBrand_ = cpuBrand_.substr(start);
        }
    }
#elif defined(__APPLE__)
    // macOS x86_64: use sysctl for CPU brand and conservative SIMD detection
    hasFMA_ = hasAVX2_; // AVX2 CPUs typically have FMA
    hasAVX512_ = false;

    char brand[256] = {0};
    size_t brandLen = sizeof(brand);
    if (sysctlbyname("machdep.cpu.brand_string", brand, &brandLen, nullptr, 0) == 0) {
        cpuBrand_ = brand;
    }
#else
    // Linux/other x86
    hasFMA_ = hasAVX2_;
    hasAVX512_ = false;
#endif

    // 确定 x86 SIMD 级别
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
#endif // __aarch64__

    // 获取核心数
    logicalCores_ = static_cast<int>(std::thread::hardware_concurrency());
    physicalCores_ = juce::SystemStats::getNumPhysicalCpus();
    if (physicalCores_ <= 0) physicalCores_ = logicalCores_ / 2;
    if (physicalCores_ <= 0) physicalCores_ = 1;

    detected_ = true;

    AppLogger::info("[CpuFeatures] " + juce::String(getCpuInfoString()));
}

std::string CpuFeatures::getCpuInfoString() const {
    std::string result;
    
    if (!cpuBrand_.empty()) {
        result = cpuBrand_ + " | ";
    }
    
    result += "SIMD: ";
    switch (simdLevel_) {
        case SimdLevel::NEON: result += "NEON"; break;
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
