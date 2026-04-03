#pragma once

/**
 * 加速后端检测模块
 * 
 * 检测可用的硬件加速后端，自动选择最佳的ONNX Runtime执行提供程序。
 * 优先级：CoreML (macOS) / DirectML (Windows) > CPU
 * 
 * DirectML支持所有DirectX 12兼容的GPU：
 * - NVIDIA: GTX 600系列及更新（Kepler架构 2012+）
 * - AMD: Radeon HD 7000系列及更新（GCN 1.0架构 2012+）
 * - Intel: HD Graphics 4000及更新（Ivy Bridge 2012+），以及Intel Arc独显
 * 
 * CoreML支持所有macOS设备：
 * - Apple Silicon: Neural Engine (ANE) + GPU + CPU
 * - Intel Mac: Metal GPU + CPU
 */

#include <juce_core/juce_core.h>
#include <string>
#include <vector>
#include <cstdint>

#ifdef _WIN32
#include "DmlRuntimeVerifier.h"
#endif

namespace OpenTune {

/**
 * GpuDeviceInfo - GPU设备信息
 */
struct GpuDeviceInfo {
    std::string name;                   // GPU名称
    size_t dedicatedVideoMemory = 0;    // 专用显存（字节）
    size_t sharedSystemMemory = 0;      // 共享系统内存（字节）
    size_t vendorId = 0;                // 厂商ID（0x10DE = NVIDIA, 0x1002 = AMD, 0x8086 = Intel）
    size_t deviceId = 0;                // 设备ID
    uint32_t adapterIndex = 0;          // DXGI枚举索引（Windows）
    bool isIntegrated = false;          // 是否为集成显卡
};

/**
 * AccelerationDetector - 加速后端检测类
 * 
 * 单例模式，在程序启动时检测并缓存硬件加速信息。
 */
class AccelerationDetector {
public:
    /**
     * AccelBackend - 加速后端类型
     */
    enum class AccelBackend {
        CPU,        // 纯CPU推理
        DirectML,   // DirectML（Windows，支持所有DirectX 12 GPU）
        CoreML      // CoreML（macOS，支持ANE/GPU/CPU自动调度）
    };

    /**
     * 获取单例实例
     */
    static AccelerationDetector& getInstance();

    /**
     * 检测可用的加速后端（在程序启动时调用一次）
     * 自动选择最佳后端
     */
    void detect();

    /**
     * 获取选择的后端
     */
    AccelBackend getSelectedBackend() const { return selectedBackend_; }

    /**
     * 检查DirectML是否可用
     */
    bool isDirectMLAvailable() const { return directMLAvailable_; }

    /**
     * 检查CoreML是否可用
     */
    bool isCoreMLAvailable() const { return coreMLAvailable_; }

    /**
     * 获取后端名称字符串（用于日志）
     */
    std::string getBackendName() const;

    /**
     * 获取GPU信息字符串（用于日志）
     */
    std::string getGpuInfoString() const;

    /**
     * 获取完整的加速状态报告（用于UI显示）
     */
    std::string getAccelerationReport() const;

    /**
     * 获取DirectML设备ID（如果使用DirectML）
     */
    int getDirectMLDeviceId() const { return directMLDeviceId_; }

    /**
     * 获取检测到的GPU设备列表
     */
    const std::vector<GpuDeviceInfo>& getGpuDevices() const { return gpuDevices_; }

    /**
     * 获取选中的GPU设备信息
     */
    const GpuDeviceInfo& getSelectedGpu() const { return selectedGpu_; }

    /**
     * 获取推荐的GPU显存限制（字节数）
     * 返回选中GPU显存的60%，用于DML gpu_mem_limit参数
     */
    size_t getRecommendedGpuMemoryLimit() const;

    /**
     * 检查DML Provider DLL是否真正可用
     */
    bool isDmlProviderDllAvailable() const { return dmlProviderDllAvailable_; }

    /**
     * 获取DML Provider DLL路径（用于诊断）
     */
    std::string getDmlProviderDllPath() const { return dmlProviderDllPath_; }

private:
    AccelerationDetector() = default;
    ~AccelerationDetector() = default;

    // 禁止拷贝
    AccelerationDetector(const AccelerationDetector&) = delete;
    AccelerationDetector& operator=(const AccelerationDetector&) = delete;

    // 检测辅助函数
    bool detectDirectML();
    bool detectCoreML();
    bool enumerateGpuDevices();

    // 缓存的检测结果
    bool detected_ = false;
    AccelBackend selectedBackend_ = AccelBackend::CPU;

    bool directMLAvailable_ = false;
    bool coreMLAvailable_ = false;

    int directMLDeviceId_ = 0;

    std::vector<GpuDeviceInfo> gpuDevices_;
    GpuDeviceInfo selectedGpu_;
    
    // DML Provider DLL 检测结果
    bool dmlProviderDllAvailable_ = false;
    std::string dmlProviderDllPath_;
};

} // namespace OpenTune
