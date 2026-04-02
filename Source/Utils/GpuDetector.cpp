#include "GpuDetector.h"
#include "AppLogger.h"
#include "DmlRuntimeVerifier.h"
#include <juce_core/juce_core.h>

#ifdef _WIN32
#include <Windows.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#pragma comment(lib, "dxgi.lib")
#endif

#include <sstream>
#include <iomanip>
#include <algorithm>

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
        info.adapterIndex = i;
        
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
    dmlDiagnosticReport_ = DmlDiagnosticReport{};

    // 首先枚举 GPU 设备
    if (!enumerateGpuDevices()) {
        AppLogger::warn("[GpuDetector] No DirectX 12 compatible GPU found");
        return false;
    }
    
    std::sort(gpuDevices_.begin(), gpuDevices_.end(), [](const GpuDeviceInfo& a, const GpuDeviceInfo& b) {
        if (a.isIntegrated != b.isIntegrated) {
            return !a.isIntegrated;
        }
        return a.dedicatedVideoMemory > b.dedicatedVideoMemory;
    });

    for (const auto& candidate : gpuDevices_) {
        juce::String candidateMsg = "[GpuDetector] Probing GPU for DirectML: " + juce::String(candidate.name)
            + " (adapterIndex=" + juce::String(static_cast<int>(candidate.adapterIndex)) + ")";
        AppLogger::info(candidateMsg);

        const DmlDiagnosticReport report = DmlRuntimeVerifier::verify(candidate.adapterIndex, candidate.name);
        if (report.ok) {
            selectedGpu_ = candidate;
            directMLDeviceId_ = static_cast<int>(candidate.adapterIndex);
            dmlDiagnosticReport_ = report;
            directMLAvailable_ = true;

            AppLogger::info("[GpuDetector] Selected GPU for DirectML: " + juce::String(selectedGpu_.name)
                + " (adapterIndex=" + juce::String(static_cast<int>(selectedGpu_.adapterIndex)) + ")");
            AppLogger::info("[GpuDetector] DirectML runtime verification passed: Windows build="
                + juce::String(static_cast<int>(dmlDiagnosticReport_.windowsBuild))
                + " DirectML=" + juce::String(dmlDiagnosticReport_.directMLVersion));
            AppLogger::info("[GpuDetector] DirectML available and verified");
            return true;
        }

        AppLogger::warn("[GpuDetector] DirectML runtime verification failed for adapterIndex="
            + juce::String(static_cast<int>(candidate.adapterIndex))
            + " gpu=" + juce::String(candidate.name));
        for (const auto& issue : report.issues) {
            juce::String line = "[GpuDetector] stage=" + juce::String(issue.stage)
                + " hr=0x" + juce::String::toHexString(static_cast<juce::int64>(static_cast<uint32_t>(issue.hresult))).paddedLeft('0', 8)
                + " detail=" + juce::String(issue.detail)
                + " remediation=" + juce::String(issue.remediation);
            AppLogger::warn(line.toStdString());
        }

        dmlDiagnosticReport_ = report;
    }

    directMLAvailable_ = false;
    return false;
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

} // namespace OpenTune
