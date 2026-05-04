#pragma once

/**
 * 加速后端检测模块
 * 
 * 检测可用的硬件加速后端。
 * Windows: 枚举 GPU + 检查 ORT DML EP 可用性 → DirectML 或 CPU
 * macOS:   CoreML 系统框架 → CoreML 或 CPU
 * 
 * GPU 枚举信息（名称/VRAM/adapterIndex）用于日志和 UI 展示。
 * 实际 DML 可用性由 ORT GetExecutionProviderApi("DML") 判断。
 */

#include <juce_core/juce_core.h>
#include <string>
#include <vector>
#include <cstdint>

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
     * @param forceCpu 若为 true，跳过 GPU 检测直接使用 CPU
     */
    void detect(bool forceCpu = false);

    /**
     * 重置检测状态，允许重新检测
     * 调用者必须确保无并发的 detect()/getSelectedBackend() 调用
     */
    void reset();

    /**
     * 由 VocoderFactory 在 DML session 创建失败 fallback CPU 时调用，
     * 确保检测状态与实际后端一致。
     */
    void overrideBackend(AccelBackend backend) { selectedBackend_ = backend; }

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
     * 获取 DirectML 使用的 DXGI adapter index
     */
    int getDirectMLDeviceId() const { return dmlAdapterIndex_; }

    /**
     * 获取检测到的GPU设备列表
     */
    const std::vector<GpuDeviceInfo>& getGpuDevices() const { return gpuDevices_; }

    /**
     * 获取选中的GPU设备信息
     */
    const GpuDeviceInfo& getSelectedGpu() const { return selectedGpu_; }

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

    int dmlAdapterIndex_ = 0;

    std::vector<GpuDeviceInfo> gpuDevices_;
    GpuDeviceInfo selectedGpu_;
};

} // namespace OpenTune
