#include "AccelerationDetector.h"
#include "AppLogger.h"
#include <juce_core/juce_core.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <Windows.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#pragma comment(lib, "dxgi.lib")
#include <dml_provider_factory.h>
#endif

#include <sstream>
#include <iomanip>
#include <onnxruntime_cxx_api.h>
#include <algorithm>

namespace OpenTune {

AccelerationDetector& AccelerationDetector::getInstance() {
    static auto* instance = new AccelerationDetector;
    return *instance;
}

#ifdef _WIN32
bool AccelerationDetector::enumerateGpuDevices(std::vector<GpuDeviceInfo>& gpuDevices) {
    IDXGIFactory1* pFactory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory);
    
    if (FAILED(hr) || pFactory == nullptr) {
        AppLogger::warn("[AccelerationDetector] Failed to create DXGI factory");
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
        info.adapterIndex = i;
        
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
            gpuDevices.push_back(info);
            
            // 格式化显存大小
            float vramGB = static_cast<float>(info.dedicatedVideoMemory) / (1024.0f * 1024.0f * 1024.0f);
            AppLogger::debug("[AccelerationDetector] Found GPU: " + juce::String(info.name) 
                      + " (" + juce::String(vramGB, 1) + " GB VRAM)");
        }
        
        pAdapter->Release();
        i++;
    }
    
    pFactory->Release();
    
    return !gpuDevices.empty();
}
#else
bool AccelerationDetector::enumerateGpuDevices(std::vector<GpuDeviceInfo>& gpuDevices) {
    juce::ignoreUnused(gpuDevices);
    return false;
}
#endif

bool AccelerationDetector::detectDirectML(GpuDeviceInfo& selectedGpu, int& adapterIndex) {
#ifdef _WIN32
    std::vector<GpuDeviceInfo> gpuDevices;
    if (!enumerateGpuDevices(gpuDevices)) {
        AppLogger::warn("[AccelerationDetector] No DirectX 12 compatible GPU found");
        return false;
    }

    // 选择最佳 GPU（独显优先，VRAM 降序）— 仅用于日志/UI 和 adapterIndex
    std::sort(gpuDevices.begin(), gpuDevices.end(), [](const GpuDeviceInfo& a, const GpuDeviceInfo& b) {
        if (a.isIntegrated != b.isIntegrated) return !a.isIntegrated;
        return a.dedicatedVideoMemory > b.dedicatedVideoMemory;
    });
    selectedGpu = gpuDevices[0];
    adapterIndex = static_cast<int>(selectedGpu.adapterIndex);

    // 用 ORT API 判断 DML EP 是否编译进当前 OpenTuneOnnxRuntime_1_24_4.dll
    Ort::InitApi();
    auto& api = Ort::GetApi();
    const OrtDmlApi* dmlApi = nullptr;
    OrtStatus* status = api.GetExecutionProviderApi(
        "DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dmlApi));

    if (status != nullptr) {
        std::string msg = api.GetErrorMessage(status);
        api.ReleaseStatus(status);
        AppLogger::warn("[AccelerationDetector] DML EP not available in this ORT build: " + juce::String(msg));
        return false;
    }

    if (dmlApi == nullptr) {
        AppLogger::warn("[AccelerationDetector] DML EP API pointer is null");
        return false;
    }

    float vramGB = static_cast<float>(selectedGpu.dedicatedVideoMemory) / (1024.0f * 1024.0f * 1024.0f);
    AppLogger::info("[AccelerationDetector] DML EP available, selected GPU: " + juce::String(selectedGpu.name)
                      + " (" + juce::String(vramGB, 1) + " GB VRAM, adapterIndex="
                      + juce::String(static_cast<int>(selectedGpu.adapterIndex)) + ")");
    return true;
#else
    return false;
#endif
}

bool AccelerationDetector::detectCoreML() {
#if defined(__APPLE__)
    // CoreML 是 macOS 系统框架，在所有 macOS 版本上都可用
    // 实际 EP 兼容性（算子支持等）由 ModelFactory::createSessionOptions 的 try/catch 处理
    AppLogger::info("[AccelerationDetector] CoreML available (macOS system framework)");
    return true;
#else
    AppLogger::debug("[AccelerationDetector] CoreML only available on macOS");
    return false;
#endif
}

void AccelerationDetector::detect(bool forceCpu) {
    if (!forceCpu && selection_.load(std::memory_order_acquire) != kUndetected)
        return;

    const auto selection = detectSelection(forceCpu);
    const auto encoded = encodeSelection(selection);

    if (forceCpu) {
        selection_.store(encoded, std::memory_order_release);
        return;
    }

    uint64_t expected = kUndetected;
    selection_.compare_exchange_strong(expected, encoded,
                                       std::memory_order_release,
                                       std::memory_order_acquire);
}

void AccelerationDetector::resetAndDetect(bool forceCpu) {
    const auto selection = detectSelection(forceCpu);
    selection_.store(encodeSelection(selection), std::memory_order_release);
}

AccelerationDetector::BackendSelection AccelerationDetector::detectSelection(bool forceCpu) {
    BackendSelection selection;

    AppLogger::debug("[AccelerationDetector] Detecting acceleration capabilities...");

    if (forceCpu) {
        AppLogger::info("[AccelerationDetector] CPU forced by user preference");
        AppLogger::info("[AccelerationDetector] Selected backend: CPU");
        return selection;
    }

#if defined(__APPLE__)
    if (detectCoreML()) {
        selection.backend = AccelBackend::CoreML;
    } else {
        AppLogger::info("[AccelerationDetector] No acceleration available, using CPU");
    }
#elif defined(_WIN32)
    GpuDeviceInfo selectedGpu;
    if (detectDirectML(selectedGpu, selection.dmlAdapterIndex)) {
        selection.backend = AccelBackend::DirectML;
    } else {
        AppLogger::info("[AccelerationDetector] No GPU acceleration available, using CPU");
    }
#else
    AppLogger::info("[AccelerationDetector] No acceleration available, using CPU");
#endif

    AppLogger::info("[AccelerationDetector] Selected backend: "
        + juce::String(backendName(selection.backend)));
    return selection;
}

void AccelerationDetector::overrideBackend(AccelBackend backend) {
    auto selection = getSelection();
    selection.backend = backend;
    if (backend != AccelBackend::DirectML)
        selection.dmlAdapterIndex = 0;
    selection_.store(encodeSelection(selection), std::memory_order_release);
}

AccelerationDetector::BackendSelection AccelerationDetector::getSelection() const noexcept {
    return decodeSelection(selection_.load(std::memory_order_acquire));
}

const char* AccelerationDetector::backendName(AccelBackend backend) noexcept {
    switch (backend) {
        case AccelBackend::CoreML: return "CoreML";
        case AccelBackend::DirectML: return "DirectML";
        case AccelBackend::CPU: return "CPU";
    }
    return "Unknown";
}

uint64_t AccelerationDetector::encodeSelection(BackendSelection selection) noexcept {
    return (static_cast<uint64_t>(selection.backend) << 32)
        | static_cast<uint32_t>(selection.dmlAdapterIndex);
}

AccelerationDetector::BackendSelection AccelerationDetector::decodeSelection(uint64_t encoded) noexcept {
    if (encoded == kUndetected)
        return {};

    BackendSelection selection;
    selection.backend = static_cast<AccelBackend>((encoded >> 32) & 0xffu);
    selection.dmlAdapterIndex = static_cast<int>(static_cast<uint32_t>(encoded));
    return selection;
}

} // namespace OpenTune
