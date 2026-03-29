#pragma once

/**
 * GPU检测模块
 * 
 * 检测可用的GPU设备，自动选择最佳的ONNX Runtime执行后端。
 * 优先级：DirectML > CPU
 * 
 * DirectML支持所有DirectX 12兼容的GPU：
 * - NVIDIA: GTX 600系列及更新（Kepler架构 2012+）
 * - AMD: Radeon HD 7000系列及更新（GCN 1.0架构 2012+）
 * - Intel: HD Graphics 4000及更新（Ivy Bridge 2012+），以及Intel Arc独显
 */

#include <juce_core/juce_core.h>
#include <string>
#include <vector>

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
    bool isIntegrated = false;          // 是否为集成显卡
};

/**
 * GpuDetector - GPU检测类
 * 
 * 单例模式，在程序启动时检测并缓存GPU信息。
 */
class GpuDetector {
public:
    /**
     * GpuBackend - GPU加速后端类型
     */
    enum class GpuBackend {
        CPU,        // 纯CPU推理
        DirectML    // DirectML（Windows，支持所有DirectX 12 GPU）
    };

    /**
     * 获取单例实例
     */
    static GpuDetector& getInstance();

    /**
     * 检测可用的GPU后端（在程序启动时调用一次）
     * 自动选择最佳后端
     */
    void detect();

    /**
     * 获取选择的后端
     */
    GpuBackend getSelectedBackend() const { return selectedBackend_; }

    /**
     * 检查DirectML是否可用
     */
    bool isDirectMLAvailable() const { return directMLAvailable_; }

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
    GpuDetector() = default;
    ~GpuDetector() = default;

    // 禁止拷贝
    GpuDetector(const GpuDetector&) = delete;
    GpuDetector& operator=(const GpuDetector&) = delete;

    // 检测辅助函数
    bool detectDirectML();
    bool enumerateGpuDevices();

    // 缓存的检测结果
    bool detected_ = false;
    GpuBackend selectedBackend_ = GpuBackend::CPU;

    bool directMLAvailable_ = false;

    int directMLDeviceId_ = 0;

    std::vector<GpuDeviceInfo> gpuDevices_;
    GpuDeviceInfo selectedGpu_;
    
    // DML Provider DLL 检测结果
    bool dmlProviderDllAvailable_ = false;
    std::string dmlProviderDllPath_;
};

} // namespace OpenTune
