#pragma once

/**
 * 加速后端检测模块
 * 
 * 检测可用的硬件加速后端。
 * Windows: 枚举 GPU + 检查 ORT DML EP 可用性 → DirectML 或 CPU
 * macOS:   CoreML 系统框架 → CoreML 或 CPU
 * 
 * GPU 枚举信息（名称/VRAM/adapterIndex）用于检测日志。
 * 实际 DML 可用性由 ORT GetExecutionProviderApi("DML") 判断。
 */

#include <juce_core/juce_core.h>
#include <atomic>
#include <string>
#include <vector>
#include <cstdint>
#include <limits>

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

    struct BackendSelection {
        AccelBackend backend = AccelBackend::CPU;
        int dmlAdapterIndex = 0;
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

    /** 重置并完成一次新的检测，最后一次性发布后端选择。 */
    void resetAndDetect(bool forceCpu = false);

    /**
     * 由 VocoderFactory 在 DML session 创建失败 fallback CPU 时调用，
     * 确保检测状态与实际后端一致。
     */
    void overrideBackend(AccelBackend backend);

    /** 读取同一时刻的后端和 adapter 快照。 */
    BackendSelection getSelection() const noexcept;

private:
    AccelerationDetector() = default;
    ~AccelerationDetector() = default;

    // 禁止拷贝
    AccelerationDetector(const AccelerationDetector&) = delete;
    AccelerationDetector& operator=(const AccelerationDetector&) = delete;

    // 检测辅助函数
    bool detectDirectML(GpuDeviceInfo& selectedGpu, int& adapterIndex);
    bool detectCoreML();
    bool enumerateGpuDevices(std::vector<GpuDeviceInfo>& gpuDevices);

    static const char* backendName(AccelBackend backend) noexcept;
    static uint64_t encodeSelection(BackendSelection selection) noexcept;
    static BackendSelection decodeSelection(uint64_t encoded) noexcept;
    BackendSelection detectSelection(bool forceCpu);

    static constexpr uint64_t kUndetected = std::numeric_limits<uint64_t>::max();
    std::atomic<uint64_t> selection_{kUndetected};
};

} // namespace OpenTune
