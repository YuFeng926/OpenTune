#if JUCE_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include <windows.h>

static std::wstring getModuleDirectoryFromAddress(const void* address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address),
                            &module))
    {
        return {};
    }

    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(module, modulePath, static_cast<DWORD>(std::size(modulePath))) == 0)
        return {};

    std::wstring path(modulePath);
    const auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return {};

    path.resize(pos);
    return path;
}

static void addSelfDirectoryToDllSearchPath()
{
    const std::wstring moduleDir = getModuleDirectoryFromAddress(reinterpret_cast<const void*>(&addSelfDirectoryToDllSearchPath));
    if (moduleDir.empty())
        return;

    using SetDefaultDllDirectoriesFn = BOOL (WINAPI*)(DWORD);
    auto setDefaultDllDirectories = reinterpret_cast<SetDefaultDllDirectoriesFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetDefaultDllDirectories")
    );

    if (setDefaultDllDirectories != nullptr)
        setDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);

    using AddDllDirectoryFn = DLL_DIRECTORY_COOKIE (WINAPI*)(PCWSTR);
    auto addDllDirectory = reinterpret_cast<AddDllDirectoryFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "AddDllDirectory")
    );

    if (addDllDirectory != nullptr)
        addDllDirectory(moduleDir.c_str());
}

struct WindowsDllSearchPathInitializer
{
    WindowsDllSearchPathInitializer()
    {
        addSelfDirectoryToDllSearchPath();
    }
};

static WindowsDllSearchPathInitializer dllSearchPathInitializer;

#endif

