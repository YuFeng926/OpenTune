#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#endif

#if defined(_MSC_VER)
#include <delayimp.h>
#include <cstring>
#include <string>
#include <vector>

namespace OpenTune {
namespace {

static std::wstring getEnvVar(const wchar_t* name)
{
    std::wstring value;
    value.resize(32768);
    const DWORD len = ::GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (len == 0 || len >= value.size()) {
        return {};
    }
    value.resize(len);
    return value;
}

static std::wstring getModuleDirectory()
{
    HMODULE moduleHandle = nullptr;
    const BOOL ok = ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&getModuleDirectory),
        &moduleHandle
    );

    if (ok == 0 || moduleHandle == nullptr) {
        return {};
    }

    std::wstring path;
    path.resize(32768);
    const DWORD len = ::GetModuleFileNameW(moduleHandle, path.data(), static_cast<DWORD>(path.size()));
    if (len == 0 || len >= path.size()) {
        return {};
    }
    path.resize(len);

    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return {};
    }
    path.resize(pos);
    return path;
}

static std::wstring joinPath(const std::wstring& a, const wchar_t* b)
{
    if (a.empty()) {
        return {};
    }
    std::wstring out = a;
    if (!out.empty() && out.back() != L'\\' && out.back() != L'/') {
        out.push_back(L'\\');
    }
    out.append(b);
    return out;
}

static HMODULE tryLoad(const std::wstring& path)
{
    if (path.empty()) {
        return nullptr;
    }
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return nullptr;
    }
    return ::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

static HMODULE loadOnnxRuntimeFromCandidates()
{
    const std::wstring programW6432 = getEnvVar(L"ProgramW6432");
    const std::wstring programFiles = !programW6432.empty() ? programW6432 : getEnvVar(L"ProgramFiles");
    const std::wstring programData = getEnvVar(L"ProgramData");
    const std::wstring moduleDir = getModuleDirectory();

    std::vector<std::wstring> candidates;
    
    // 优先搜索模块所在目录（Standalone应用最常见的部署方式）
    // 这是最常用的情况：DLL与exe在同一目录
    if (!moduleDir.empty()) {
        candidates.push_back(joinPath(moduleDir, L"onnxruntime.dll"));
    }
    
    // 然后搜索系统安装路径（用于共享安装场景）
    if (!programFiles.empty()) {
        candidates.push_back(joinPath(joinPath(programFiles, L"OpenTune"), L"onnxruntime.dll"));
    }
    if (!programData.empty()) {
        candidates.push_back(joinPath(joinPath(programData, L"OpenTune"), L"onnxruntime.dll"));
    }

    for (const auto& p : candidates) {
        if (HMODULE h = tryLoad(p)) {
            return h;
        }
    }
    return nullptr;
}

static FARPROC WINAPI onnxRuntimeDelayLoadHook(unsigned dliNotify, PDelayLoadInfo pdli)
{
    if (dliNotify != dliNotePreLoadLibrary || pdli == nullptr || pdli->szDll == nullptr) {
        return nullptr;
    }

    if (_stricmp(pdli->szDll, "onnxruntime.dll") != 0) {
        return nullptr;
    }

    if (::GetModuleHandleW(L"onnxruntime.dll") != nullptr) {
        return nullptr;
    }

    const HMODULE h = loadOnnxRuntimeFromCandidates();
    if (h == nullptr) {
        return nullptr;
    }

    return reinterpret_cast<FARPROC>(h);
}

} // namespace
} // namespace OpenTune

extern "C" const PfnDliHook __pfnDliNotifyHook2 = OpenTune::onnxRuntimeDelayLoadHook;
#endif
