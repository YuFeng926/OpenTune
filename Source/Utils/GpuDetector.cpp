#include "GpuDetector.h"
#include "AppLogger.h"
#include <juce_core/juce_core.h>

#ifdef _WIN32
#include <Windows.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#pragma comment(lib, "dxgi.lib")
#endif

#include <sstream>
#include <iomanip>

namespace OpenTune {

GpuDetector& GpuDetector::getInstance() {
    static GpuDetector instance;
    return instance;
}

#ifdef _WIN32
bool GpuDetector::enumerateGpuDevices() {
    IDXGIFactory1* pFactory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory);
    
    if (FAILED(hr) || pFactory == nullptr) {
        AppLogger::warn("[GpuDetector] Failed to create DXGI factory");
        return false;
    }
    
    UINT i = 0;
    IDXGIAdapter1* pAdapter = nullptr;
    
    while (pFactory->EnumAdapters1(i, &pAdapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc;
        pAdapter->GetDesc1(&desc);
        
        GpuDeviceInfo info;
        
        char nameBuf[256];
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nameBuf, sizeof(nameBuf), nullptr, nullptr);
        info.name = nameBuf;
        
        info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
        info.sharedSystemMemory = desc.SharedSystemMemory;
        info.vendorId = desc.VendorId;
        info.deviceId = desc.DeviceId;
        
        // 先检查是否为软件渲染器（跳过，不是真实GPU）
        // DXGI_ADAPTER_FLAG_SOFTWARE 标记软件渲染器（如WARP），不是集成显卡
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            pAdapter->Release();
            i++;
            continue;  // 跳过软件渲染器
        }
        
        // 判断是否为集成显卡（基于厂商ID和显存大小）
        info.isIntegrated = false;
        if (desc.VendorId == 0x8086) {  // Intel
            // Intel Arc系列（设备ID 0x5690-0x56C0）是独立显卡，不应标记为集成
            bool isArc = (desc.DeviceId >= 0x5690 && desc.DeviceId <= 0x56C0);
            if (!isArc && desc.DedicatedVideoMemory < 512 * 1024 * 1024) {
                info.isIntegrated = true;
            }
        } else if (desc.VendorId == 0x1002) {  // AMD
            // AMD APU/核显：专用显存通常较小（<1GB）且有共享内存
            // AMD Radeon 760M等核显的VRAM约400-512MB
            // 独立显卡如RX 6600有8GB，RX 580有4GB
            if (desc.DedicatedVideoMemory < 1024 * 1024 * 1024 && desc.SharedSystemMemory > 0) {
                info.isIntegrated = true;
            }
        }
        
        // 检查是否为真正的 GPU（排除软件渲染器和无效设备）
        if (info.vendorId != 0 && !info.name.empty() && info.name != "Microsoft Basic Render Driver") {
            gpuDevices_.push_back(info);
            
            // 格式化显存大小
            float vramGB = static_cast<float>(info.dedicatedVideoMemory) / (1024.0f * 1024.0f * 1024.0f);
            AppLogger::debug("[GpuDetector] Found GPU: " + juce::String(info.name) 
                      + " (" + juce::String(vramGB, 1) + " GB VRAM)");
        }
        
        pAdapter->Release();
        i++;
    }
    
    pFactory->Release();
    
    return !gpuDevices_.empty();
}
#else
bool GpuDetector::enumerateGpuDevices() {
    return false;
}
#endif

bool GpuDetector::detectDirectML() {
#ifdef _WIN32
    // 首先枚举 GPU 设备
    if (!enumerateGpuDevices()) {
        AppLogger::warn("[GpuDetector] No DirectX 12 compatible GPU found");
        return false;
    }
    
    // 检查 DirectML 版本的 ONNX Runtime
    juce::File exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    
    // DirectML 支持有两种架构：
    // 1. NuGet 包版本：DML provider 内置在 onnxruntime.dll 中（DLL 较大，约 13MB）
    // 2. 独立 provider 版本：需要 onnxruntime.dll + onnxruntime_providers_dml.dll
    
    juce::File onnxDll = exeDir.getChildFile("onnxruntime.dll");
    if (!onnxDll.existsAsFile()) {
        AppLogger::warn("[GpuDetector] onnxruntime.dll not found in " + exeDir.getFullPathName());
        return false;
    }
    
    // 检查 DLL 大小来判断是否为 DirectML 版本
    // CPU-only 版本约 10MB，DirectML 版本约 13MB
    const int64_t dmlDllMinSize = 12 * 1024 * 1024; // 12MB
    int64_t dllSize = onnxDll.getSize();
    bool isDmlVersion = dllSize >= dmlDllMinSize;
    
    // 也检查独立的 DML Provider DLL（如果存在）
    juce::File dmlProviderDll = exeDir.getChildFile("onnxruntime_providers_dml.dll");
    bool hasDmlProviderDll = dmlProviderDll.existsAsFile();
    
    if (!isDmlVersion && !hasDmlProviderDll) {
        AppLogger::warn("[GpuDetector] onnxruntime.dll appears to be CPU-only version (size: " 
                  + juce::String((int)(dllSize / 1024 / 1024)) + " MB)");
        AppLogger::warn("[GpuDetector] DML acceleration will NOT work. Using CPU fallback.");
        AppLogger::warn("[GpuDetector] To enable DML, replace onnxruntime.dll with DirectML version (>= 12MB)");
        dmlProviderDllAvailable_ = false;
        dmlProviderDllPath_ = "";
        return false;
    }
    
    dmlProviderDllAvailable_ = true;
    if (hasDmlProviderDll) {
        dmlProviderDllPath_ = dmlProviderDll.getFullPathName().toStdString();
        AppLogger::debug("[GpuDetector] Found DML Provider DLL: " + juce::String(dmlProviderDllPath_));
    } else {
        dmlProviderDllPath_ = onnxDll.getFullPathName().toStdString();
        AppLogger::debug("[GpuDetector] Using DirectML-builtin onnxruntime.dll (size: " 
                  + juce::String((int)(dllSize / 1024 / 1024)) + " MB)");
    }
    
    // 检查共享库
    juce::File sharedDll = exeDir.getChildFile("onnxruntime_providers_shared.dll");
    if (!sharedDll.existsAsFile()) {
        AppLogger::debug("[GpuDetector] Note: onnxruntime_providers_shared.dll not found (may not be needed for builtin DML)");
    }
    
    // 选择最佳 GPU（优先选择独立显卡，然后选择显存最大的）
    if (!gpuDevices_.empty()) {
        std::sort(gpuDevices_.begin(), gpuDevices_.end(), [](const GpuDeviceInfo& a, const GpuDeviceInfo& b) {
            if (a.isIntegrated != b.isIntegrated) {
                return !a.isIntegrated;
            }
            return a.dedicatedVideoMemory > b.dedicatedVideoMemory;
        });
        
        selectedGpu_ = gpuDevices_[0];
        directMLDeviceId_ = 0;
        
        // 计算集成显卡的有效显存（考虑共享系统内存）
        // 集成显卡主要使用共享内存，dedicatedVideoMemory可能很小或为0
        size_t effectiveVram = selectedGpu_.dedicatedVideoMemory;
        if (selectedGpu_.isIntegrated) {
            if (selectedGpu_.dedicatedVideoMemory < 256 * 1024 * 1024) {
                // 专用显存<256MB：主要依赖共享内存的1/4
                effectiveVram = selectedGpu_.sharedSystemMemory / 4;
            } else {
                // 专用显存>=256MB：专用显存 + 共享内存/8
                effectiveVram = selectedGpu_.dedicatedVideoMemory + selectedGpu_.sharedSystemMemory / 8;
            }
        }
        
        // 有效显存小于 512MB 时回退到 CPU
        // 原因：显存过小会导致 DML 频繁 fallback 到 CPU，反而比纯 CPU 更慢
        const size_t minVramForDml = 512 * 1024 * 1024; // 512MB
        if (effectiveVram < minVramForDml) {
            float effectiveVramMB = static_cast<float>(effectiveVram) / (1024.0f * 1024.0f);
            AppLogger::warn("[GpuDetector] GPU has insufficient effective VRAM: " 
                      + juce::String((int)effectiveVramMB) + " MB (minimum 512 MB required)");
            if (selectedGpu_.isIntegrated) {
                float sharedMB = static_cast<float>(selectedGpu_.sharedSystemMemory) / (1024.0f * 1024.0f);
                AppLogger::warn("[GpuDetector] (Integrated GPU, shared memory: " 
                          + juce::String((int)sharedMB) + " MB)");
            }
            AppLogger::warn("[GpuDetector] Falling back to CPU for better performance");
            dmlProviderDllAvailable_ = false;
            return false;
        }
        
        juce::String gpuMsg = "[GpuDetector] Selected GPU for DirectML: " + juce::String(selectedGpu_.name);
        if (selectedGpu_.isIntegrated) {
            gpuMsg += " [Integrated]";
        }
        AppLogger::info(gpuMsg);
        
        size_t recommendedLimit = getRecommendedGpuMemoryLimit();
        float limitGB = static_cast<float>(recommendedLimit) / (1024.0f * 1024.0f * 1024.0f);
        AppLogger::debug("[GpuDetector] Recommended GPU memory limit: " 
                  + juce::String(limitGB, 2) + " GB (60% of VRAM)");
    }
    
    directMLAvailable_ = true;
    AppLogger::info("[GpuDetector] DirectML available and verified");
    return true;
#else
    AppLogger::debug("[GpuDetector] DirectML only available on Windows");
    return false;
#endif
}

void GpuDetector::detect() {
    if (detected_) return;
    
    AppLogger::debug("[GpuDetector] Detecting GPU acceleration capabilities...");
    
    if (detectDirectML()) {
        selectedBackend_ = GpuBackend::DirectML;
    } else {
        selectedBackend_ = GpuBackend::CPU;
        AppLogger::info("[GpuDetector] No GPU acceleration available, using CPU");
    }
    
    detected_ = true;
    AppLogger::info("[GpuDetector] Selected backend: " + juce::String(getBackendName()));
}

std::string GpuDetector::getBackendName() const {
    switch (selectedBackend_) {
        case GpuBackend::DirectML: return "DirectML";
        case GpuBackend::CPU: return "CPU";
    }
    return "Unknown";
}

std::string GpuDetector::getGpuInfoString() const {
    std::string result = "Backend: " + getBackendName();
    
    if (selectedBackend_ == GpuBackend::DirectML && !selectedGpu_.name.empty()) {
        float vramGB = static_cast<float>(selectedGpu_.dedicatedVideoMemory) / (1024.0f * 1024.0f * 1024.0f);
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << vramGB;
        result += " (" + selectedGpu_.name + ", " + oss.str() + " GB)";
    }
    
    return result;
}

std::string GpuDetector::getAccelerationReport() const {
    std::ostringstream report;
    
    report << "=== 加速状态报告 ===\n";
    report << "推理后端: " << getBackendName() << "\n";
    
    if (selectedBackend_ == GpuBackend::DirectML) {
        report << "GPU 设备: " << selectedGpu_.name << "\n";
        
        float vramGB = static_cast<float>(selectedGpu_.dedicatedVideoMemory) / (1024.0f * 1024.0f * 1024.0f);
        report << "显存大小: " << std::fixed << std::setprecision(1) << vramGB << " GB\n";
        
        // 厂商名称
        std::string vendorName;
        switch (selectedGpu_.vendorId) {
            case 0x10DE: vendorName = "NVIDIA"; break;
            case 0x1002: vendorName = "AMD"; break;
            case 0x8086: vendorName = "Intel"; break;
            case 0x1414: vendorName = "Microsoft"; break;
            default: vendorName = "Unknown"; break;
        }
        report << "GPU 厂商: " << vendorName << "\n";
    } else {
        report << "使用 CPU 推理\n";
    }
    
    if (!gpuDevices_.empty()) {
        report << "\n检测到的 GPU 列表:\n";
        for (size_t i = 0; i < gpuDevices_.size(); ++i) {
            const auto& gpu = gpuDevices_[i];
            float vramGB = static_cast<float>(gpu.dedicatedVideoMemory) / (1024.0f * 1024.0f * 1024.0f);
            report << "  [" << i << "] " << gpu.name 
                   << " (" << std::fixed << std::setprecision(1) << vramGB << " GB)"
                   << (gpu.isIntegrated ? " [集成]" : " [独立]") << "\n";
        }
    }
    
    return report.str();
}

size_t GpuDetector::getRecommendedGpuMemoryLimit() const {
    if (selectedBackend_ != GpuBackend::DirectML) {
        return 0;
    }
    
    size_t vramBytes = selectedGpu_.dedicatedVideoMemory;
    size_t sharedBytes = selectedGpu_.sharedSystemMemory;
    
    size_t effectiveMemory = vramBytes;
    
    if (selectedGpu_.isIntegrated) {
        if (vramBytes < 256 * 1024 * 1024) {
            effectiveMemory = sharedBytes / 4;
        } else {
            effectiveMemory = vramBytes + sharedBytes / 8;
        }
    }
    
    if (effectiveMemory == 0) {
        return 512 * 1024 * 1024;
    }
    
    size_t limit60Percent = static_cast<size_t>(effectiveMemory * 0.6);
    
    const size_t minLimit = 512 * 1024 * 1024;
    const size_t maxLimit = 8ULL * 1024 * 1024 * 1024;
    
    if (limit60Percent < minLimit) {
        return minLimit;
    }
    if (limit60Percent > maxLimit) {
        return maxLimit;
    }
    
    return limit60Percent;
}

} // namespace OpenTune
