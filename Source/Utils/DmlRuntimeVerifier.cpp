#include "DmlRuntimeVerifier.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectML.h>
#include <wrl/client.h>

#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <cctype>

namespace OpenTune {

namespace {

using Microsoft::WRL::ComPtr;

struct WinVersionInfo {
    DWORD major = 0;
    DWORD minor = 0;
    DWORD build = 0;
};

std::wstring toWide(const std::string& s)
{
    if (s.empty()) {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (length <= 0) {
        return {};
    }

    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], length);
    if (!out.empty() && out.back() == L'\0') {
        out.pop_back();
    }
    return out;
}

std::string toUtf8(const std::wstring& ws)
{
    if (ws.empty()) {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }

    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &out[0], length, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

std::wstring getModuleDirectory()
{
    std::wstring modulePath;
    modulePath.resize(512);

    while (true) {
        const DWORD copied = GetModuleFileNameW(nullptr, &modulePath[0], static_cast<DWORD>(modulePath.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < modulePath.size() - 1) {
            modulePath.resize(copied);
            break;
        }
        modulePath.resize(modulePath.size() * 2);
        if (modulePath.size() > 32768) {
            return {};
        }
    }

    const auto pos = modulePath.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return {};
    }
    modulePath.resize(pos);
    return modulePath;
}

std::wstring joinPath(const std::wstring& left, const wchar_t* right)
{
    if (left.empty() || right == nullptr) {
        return {};
    }

    std::wstring out = left;
    if (out.back() != L'\\' && out.back() != L'/') {
        out.push_back(L'\\');
    }
    out.append(right);
    return out;
}

std::string formatHRESULT(HRESULT hr)
{
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
        << static_cast<uint32_t>(hr);
    return oss.str();
}

std::string describeHRESULT(HRESULT hr)
{
    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(
        flags,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr
    );

    if (length == 0 || buffer == nullptr) {
        return "HRESULT " + formatHRESULT(hr);
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }

    return toUtf8(message);
}

WinVersionInfo queryWindowsVersion()
{
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    WinVersionInfo result;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return result;
    }

    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtlGetVersion == nullptr) {
        return result;
    }

    RTL_OSVERSIONINFOW versionInfo = {};
    versionInfo.dwOSVersionInfoSize = sizeof(versionInfo);
    if (rtlGetVersion(&versionInfo) != 0) {
        return result;
    }

    result.major = versionInfo.dwMajorVersion;
    result.minor = versionInfo.dwMinorVersion;
    result.build = versionInfo.dwBuildNumber;
    return result;
}

std::vector<int> parseVersionNumbers(const std::string& version)
{
    std::vector<int> values;
    size_t i = 0;
    while (i < version.size()) {
        while (i < version.size() && !std::isdigit(static_cast<unsigned char>(version[i]))) {
            ++i;
        }

        if (i >= version.size()) {
            break;
        }

        int value = 0;
        while (i < version.size() && std::isdigit(static_cast<unsigned char>(version[i]))) {
            value = value * 10 + (version[i] - '0');
            ++i;
        }
        values.push_back(value);
    }

    return values;
}

bool isVersionAtLeast(const std::string& found, const std::string& required)
{
    auto a = parseVersionNumbers(found);
    auto b = parseVersionNumbers(required);
    const size_t maxSize = std::max(a.size(), b.size());
    a.resize(maxSize, 0);
    b.resize(maxSize, 0);

    for (size_t i = 0; i < maxSize; ++i) {
        if (a[i] < b[i]) return false;
        if (a[i] > b[i]) return true;
    }
    return true;
}

std::string queryFileVersion(const std::wstring& filePath)
{
    DWORD handle = 0;
    const DWORD infoSize = GetFileVersionInfoSizeW(filePath.c_str(), &handle);
    if (infoSize == 0) {
        return {};
    }

    std::vector<std::uint8_t> buffer(infoSize);
    if (!GetFileVersionInfoW(filePath.c_str(), handle, infoSize, buffer.data())) {
        return {};
    }

    VS_FIXEDFILEINFO* fixedInfo = nullptr;
    UINT fixedInfoLength = 0;
    if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&fixedInfo), &fixedInfoLength) ||
        fixedInfo == nullptr || fixedInfoLength == 0) {
        return {};
    }

    std::ostringstream oss;
    oss << HIWORD(fixedInfo->dwFileVersionMS) << "."
        << LOWORD(fixedInfo->dwFileVersionMS) << "."
        << HIWORD(fixedInfo->dwFileVersionLS) << "."
        << LOWORD(fixedInfo->dwFileVersionLS);
    return oss.str();
}

void addIssue(
    DmlDiagnosticReport& report,
    const char* stage,
    HRESULT hr,
    const std::string& detail,
    const std::string& remediation)
{
    DmlDiagnosticIssue issue;
    issue.stage = stage;
    issue.hresult = static_cast<long>(hr);
    issue.detail = detail;
    issue.remediation = remediation;
    report.issues.push_back(std::move(issue));
}

} // namespace

DmlDiagnosticReport DmlRuntimeVerifier::verify(uint32_t adapterIndex, const std::string& adapterName)
{
    DmlDiagnosticReport report;
    report.ok = false;
    report.adapterIndex = adapterIndex;
    report.adapterName = adapterName;

    const WinVersionInfo version = queryWindowsVersion();
    report.windowsBuild = version.build;
    if (version.build < kMinimumWindowsBuild) {
        addIssue(
            report,
            "os_build",
            HRESULT_FROM_WIN32(ERROR_OLD_WIN_VERSION),
            "Windows build is " + std::to_string(version.build) + ", required >= " + std::to_string(kMinimumWindowsBuild),
            "Upgrade Windows to 10 21H2 (build 19044) or newer."
        );
        return report;
    }

    const std::wstring moduleDir = getModuleDirectory();
    const std::wstring directMLPath = joinPath(moduleDir, L"DirectML.dll");
    const std::wstring d3d12CorePath = joinPath(moduleDir, L"D3D12\\D3D12Core.dll");

    report.directMLDllPath = toUtf8(directMLPath);

    const DWORD directMLAttrs = GetFileAttributesW(directMLPath.c_str());
    if (directMLAttrs == INVALID_FILE_ATTRIBUTES || (directMLAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        addIssue(
            report,
            "directml_runtime",
            HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND),
            "DirectML.dll not found in application directory.",
            "Deploy Microsoft.AI.DirectML runtime and place DirectML.dll beside OpenTune.exe."
        );
        return report;
    }

    report.directMLVersion = queryFileVersion(directMLPath);
    if (report.directMLVersion.empty()) {
        addIssue(
            report,
            "directml_runtime",
            HRESULT_FROM_WIN32(ERROR_FILE_INVALID),
            "Unable to read DirectML.dll version.",
            "Redeploy Microsoft.AI.DirectML package and ensure DirectML.dll is not corrupted."
        );
        return report;
    }

    if (!isVersionAtLeast(report.directMLVersion, kRequiredDirectMLVersion)) {
        addIssue(
            report,
            "directml_runtime",
            E_FAIL,
            "DirectML.dll version is " + report.directMLVersion + ", required >= " + std::string(kRequiredDirectMLVersion),
            "Update deployed Microsoft.AI.DirectML runtime to version 1.15.4 or newer."
        );
        return report;
    }

    const DWORD d3d12CoreAttrs = GetFileAttributesW(d3d12CorePath.c_str());
    if (d3d12CoreAttrs == INVALID_FILE_ATTRIBUTES || (d3d12CoreAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        addIssue(
            report,
            "agility_runtime",
            HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND),
            "D3D12Core.dll not found under .\\D3D12.",
            "Deploy DirectX Agility SDK and copy D3D12Core.dll to the D3D12 runtime folder."
        );
        return report;
    }

    ComPtr<IDXGIFactory6> dxgiFactory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(dxgiFactory.GetAddressOf()));
    if (FAILED(hr)) {
        addIssue(
            report,
            "dxgi_factory",
            hr,
            "CreateDXGIFactory2 failed: " + describeHRESULT(hr),
            "Ensure DirectX runtime is available and GPU drivers are installed correctly."
        );
        return report;
    }

    ComPtr<IDXGIAdapter1> adapter;
    hr = dxgiFactory->EnumAdapters1(adapterIndex, adapter.GetAddressOf());
    if (FAILED(hr)) {
        addIssue(
            report,
            "dxgi_adapter",
            hr,
            "EnumAdapters1 failed for adapterIndex=" + std::to_string(adapterIndex),
            "Validate selected adapter index and verify GPU enumeration order."
        );
        return report;
    }

    ComPtr<ID3D12Device> d3d12Device;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(d3d12Device.GetAddressOf()));
    if (FAILED(hr)) {
        addIssue(
            report,
            "d3d12_device",
            hr,
            "D3D12CreateDevice failed: " + describeHRESULT(hr),
            "Install a recent GPU driver and ensure D3D12 is supported on the selected adapter."
        );
        return report;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;

    ComPtr<ID3D12CommandQueue> commandQueue;
    hr = d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(commandQueue.GetAddressOf()));
    if (FAILED(hr)) {
        addIssue(
            report,
            "d3d12_queue",
            hr,
            "CreateCommandQueue failed: " + describeHRESULT(hr),
            "Verify driver compatibility with D3D12 command queues used by ONNX Runtime DirectML EP."
        );
        return report;
    }

    ComPtr<IDMLDevice> dmlDevice;
    hr = DMLCreateDevice1(
        d3d12Device.Get(),
        DML_CREATE_DEVICE_FLAG_NONE,
        DML_FEATURE_LEVEL_5_0,
        IID_PPV_ARGS(dmlDevice.GetAddressOf())
    );
    if (FAILED(hr)) {
        addIssue(
            report,
            "dml_feature_level",
            hr,
            "DMLCreateDevice1(DML_FEATURE_LEVEL_5_0) failed: " + describeHRESULT(hr),
            "Update DirectML runtime and GPU driver to versions that support DML feature level 5_0."
        );
        return report;
    }

    report.ok = true;
    return report;
}

std::string DmlDiagnosticReport::toMultilineString() const
{
    std::ostringstream oss;
    oss << "adapter=" << adapterName
        << " index=" << adapterIndex
        << " windowsBuild=" << windowsBuild
        << " directMLVersion=" << (directMLVersion.empty() ? "<unknown>" : directMLVersion)
        << " directMLPath=" << directMLDllPath;

    for (const auto& issue : issues) {
        oss << "\n"
            << "stage=" << issue.stage
            << " hr=" << formatHRESULT(static_cast<HRESULT>(issue.hresult))
            << " detail=" << issue.detail
            << " remediation=" << issue.remediation;
    }

    return oss.str();
}

} // namespace OpenTune

#else

namespace OpenTune {

DmlDiagnosticReport DmlRuntimeVerifier::verify(uint32_t adapterIndex, const std::string& adapterName)
{
    DmlDiagnosticReport report;
    report.adapterIndex = adapterIndex;
    report.adapterName = adapterName;
    report.ok = false;
    DmlDiagnosticIssue issue;
    issue.stage = "platform";
    issue.hresult = 0;
    issue.detail = "DirectML verification is only supported on Windows.";
    issue.remediation = "Run this build on Windows with DirectML runtime deployment.";
    report.issues.push_back(std::move(issue));
    return report;
}

std::string DmlDiagnosticReport::toMultilineString() const
{
    return "DirectML verification unavailable on this platform";
}

} // namespace OpenTune

#endif
