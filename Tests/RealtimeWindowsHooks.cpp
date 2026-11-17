#include "RealtimeAudit.h"

#if defined(_WIN32)

#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <malloc.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <new>
#include <utility>

namespace
{
using OpenTune::RealtimeAudit::Api;
using OpenTune::RealtimeAudit::recordAllocation;
using OpenTune::RealtimeAudit::recordDeallocation;
using OpenTune::RealtimeAudit::recordFileIO;
using OpenTune::RealtimeAudit::recordLockAttempt;
using OpenTune::RealtimeAudit::recordLockWait;
using OpenTune::RealtimeAuditTest::WindowsHookReport;
using OpenTune::RealtimeAuditTest::apiName;

constexpr size_t apiCount = static_cast<size_t>(Api::Count);
constexpr size_t hookSlotCount = 16;

struct OriginalTable
{
    void* addresses[apiCount][hookSlotCount]{};
};

std::atomic<const OriginalTable*> activeOriginals{};

void recordAllocatorAllocation(Api api) noexcept
{
    if (! OpenTune::RealtimeAudit::isCppAllocatorCall())
        recordAllocation(api);
}

void recordAllocatorDeallocation(Api api) noexcept
{
    if (! OpenTune::RealtimeAudit::isCppAllocatorCall())
        recordDeallocation(api);
}

template <size_t Slot, typename Function>
__forceinline Function original(Api api) noexcept
{
    const auto* routes = activeOriginals.load(std::memory_order_acquire);
    return reinterpret_cast<Function>(routes->addresses[static_cast<size_t>(api)][Slot]);
}

template <size_t Slot>
void* __cdecl hookMalloc(size_t size)
{
    recordAllocatorAllocation(Api::CrtMalloc);
    return original<Slot, void* (__cdecl*)(size_t)>(Api::CrtMalloc)(size);
}

template <size_t Slot>
void* __cdecl hookCalloc(size_t count, size_t size)
{
    recordAllocatorAllocation(Api::CrtCalloc);
    return original<Slot, void* (__cdecl*)(size_t, size_t)>(Api::CrtCalloc)(count, size);
}

template <size_t Slot>
void* __cdecl hookRealloc(void* memory, size_t size)
{
    recordAllocatorAllocation(Api::CrtRealloc);
    void* result = original<Slot, void* (__cdecl*)(void*, size_t)>(Api::CrtRealloc)(memory, size);
    if (memory != nullptr && (result != nullptr || size == 0))
        recordAllocatorDeallocation(Api::CrtRealloc);
    return result;
}

template <size_t Slot>
void __cdecl hookFree(void* memory)
{
    if (memory != nullptr)
        recordAllocatorDeallocation(Api::CrtFree);
    original<Slot, void (__cdecl*)(void*)>(Api::CrtFree)(memory);
}

template <size_t Slot>
void* __cdecl hookAlignedMalloc(size_t size, size_t alignment)
{
    recordAllocatorAllocation(Api::CrtAlignedMalloc);
    return original<Slot, void* (__cdecl*)(size_t, size_t)>(Api::CrtAlignedMalloc)(size, alignment);
}

template <size_t Slot>
void* __cdecl hookAlignedRealloc(void* memory, size_t size, size_t alignment)
{
    recordAllocatorAllocation(Api::CrtAlignedRealloc);
    void* result = original<Slot, void* (__cdecl*)(void*, size_t, size_t)>(Api::CrtAlignedRealloc)(
        memory, size, alignment);
    if (memory != nullptr && (result != nullptr || size == 0))
        recordAllocatorDeallocation(Api::CrtAlignedRealloc);
    return result;
}

template <size_t Slot>
void __cdecl hookAlignedFree(void* memory)
{
    if (memory != nullptr)
        recordAllocatorDeallocation(Api::CrtAlignedFree);
    original<Slot, void (__cdecl*)(void*)>(Api::CrtAlignedFree)(memory);
}

template <size_t Slot>
void* __cdecl hookAlignedOffsetMalloc(size_t size, size_t alignment, size_t offset)
{
    recordAllocatorAllocation(Api::CrtAlignedOffsetMalloc);
    return original<Slot, void* (__cdecl*)(size_t, size_t, size_t)>(Api::CrtAlignedOffsetMalloc)(size, alignment, offset);
}

template <size_t Slot>
void* __cdecl hookAlignedOffsetRealloc(void* memory, size_t size, size_t alignment, size_t offset)
{
    recordAllocatorAllocation(Api::CrtAlignedOffsetRealloc);
    void* result = original<Slot, void* (__cdecl*)(void*, size_t, size_t, size_t)>(Api::CrtAlignedOffsetRealloc)(
        memory, size, alignment, offset);
    if (memory != nullptr && (result != nullptr || size == 0))
        recordAllocatorDeallocation(Api::CrtAlignedOffsetRealloc);
    return result;
}

template <size_t Slot>
FILE* __cdecl hookFopen(const char* path, const char* mode)
{
    recordFileIO(Api::CrtFopen);
    return original<Slot, FILE* (__cdecl*)(const char*, const char*)>(Api::CrtFopen)(path, mode);
}

template <size_t Slot>
FILE* __cdecl hookWFopen(const wchar_t* path, const wchar_t* mode)
{
    recordFileIO(Api::CrtWFopen);
    return original<Slot, FILE* (__cdecl*)(const wchar_t*, const wchar_t*)>(Api::CrtWFopen)(path, mode);
}

template <size_t Slot>
size_t __cdecl hookFread(void* data, size_t size, size_t count, FILE* file)
{
    recordFileIO(Api::CrtFread);
    return original<Slot, size_t (__cdecl*)(void*, size_t, size_t, FILE*)>(Api::CrtFread)(data, size, count, file);
}

template <size_t Slot>
size_t __cdecl hookFwrite(const void* data, size_t size, size_t count, FILE* file)
{
    recordFileIO(Api::CrtFwrite);
    return original<Slot, size_t (__cdecl*)(const void*, size_t, size_t, FILE*)>(Api::CrtFwrite)(data, size, count, file);
}

template <size_t Slot>
int __cdecl hookFclose(FILE* file)
{
    recordFileIO(Api::CrtFclose);
    return original<Slot, int (__cdecl*)(FILE*)>(Api::CrtFclose)(file);
}

template <size_t Slot>
int __cdecl hookFflush(FILE* file)
{
    recordFileIO(Api::CrtFflush);
    return original<Slot, int (__cdecl*)(FILE*)>(Api::CrtFflush)(file);
}

template <size_t Slot>
int __cdecl hookFseek(FILE* file, long offset, int origin)
{
    recordFileIO(Api::CrtFseek);
    return original<Slot, int (__cdecl*)(FILE*, long, int)>(Api::CrtFseek)(file, offset, origin);
}

template <size_t Slot>
int __cdecl hookFseek64(FILE* file, __int64 offset, int origin)
{
    recordFileIO(Api::CrtFseek64);
    return original<Slot, int (__cdecl*)(FILE*, __int64, int)>(Api::CrtFseek64)(file, offset, origin);
}

template <size_t Slot>
LPVOID WINAPI hookHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size)
{
    recordAllocatorAllocation(Api::WinHeapAlloc);
    return original<Slot, LPVOID (WINAPI*)(HANDLE, DWORD, SIZE_T)>(Api::WinHeapAlloc)(heap, flags, size);
}

template <size_t Slot>
LPVOID WINAPI hookHeapReAlloc(HANDLE heap, DWORD flags, LPVOID memory, SIZE_T size)
{
    recordAllocatorAllocation(Api::WinHeapRealloc);
    LPVOID result = original<Slot, LPVOID (WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T)>(Api::WinHeapRealloc)(
        heap, flags, memory, size);
    if (memory != nullptr && result != nullptr)
        recordAllocatorDeallocation(Api::WinHeapRealloc);
    return result;
}

template <size_t Slot>
BOOL WINAPI hookHeapFree(HANDLE heap, DWORD flags, LPVOID memory)
{
    if (memory != nullptr)
        recordAllocatorDeallocation(Api::WinHeapFree);
    return original<Slot, BOOL (WINAPI*)(HANDLE, DWORD, LPVOID)>(Api::WinHeapFree)(heap, flags, memory);
}

template <size_t Slot>
void WINAPI hookEnterCriticalSection(LPCRITICAL_SECTION section)
{
    recordLockAttempt(Api::WinEnterCriticalSection);
    recordLockWait(Api::WinEnterCriticalSection);
    original<Slot, void (WINAPI*)(LPCRITICAL_SECTION)>(Api::WinEnterCriticalSection)(section);
}

template <size_t Slot>
BOOL WINAPI hookTryEnterCriticalSection(LPCRITICAL_SECTION section)
{
    recordLockAttempt(Api::WinTryEnterCriticalSection);
    return original<Slot, BOOL (WINAPI*)(LPCRITICAL_SECTION)>(Api::WinTryEnterCriticalSection)(section);
}

template <size_t Slot>
void WINAPI hookAcquireSRWExclusive(PSRWLOCK lock)
{
    recordLockAttempt(Api::WinAcquireSRWExclusive);
    recordLockWait(Api::WinAcquireSRWExclusive);
    original<Slot, void (WINAPI*)(PSRWLOCK)>(Api::WinAcquireSRWExclusive)(lock);
}

template <size_t Slot>
void WINAPI hookAcquireSRWShared(PSRWLOCK lock)
{
    recordLockAttempt(Api::WinAcquireSRWShared);
    recordLockWait(Api::WinAcquireSRWShared);
    original<Slot, void (WINAPI*)(PSRWLOCK)>(Api::WinAcquireSRWShared)(lock);
}

template <size_t Slot>
BOOLEAN WINAPI hookTryAcquireSRWExclusive(PSRWLOCK lock)
{
    recordLockAttempt(Api::WinTryAcquireSRWExclusive);
    return original<Slot, BOOLEAN (WINAPI*)(PSRWLOCK)>(Api::WinTryAcquireSRWExclusive)(lock);
}

template <size_t Slot>
BOOLEAN WINAPI hookTryAcquireSRWShared(PSRWLOCK lock)
{
    recordLockAttempt(Api::WinTryAcquireSRWShared);
    return original<Slot, BOOLEAN (WINAPI*)(PSRWLOCK)>(Api::WinTryAcquireSRWShared)(lock);
}

template <size_t Slot>
DWORD WINAPI hookWaitForSingleObject(HANDLE handle, DWORD timeout)
{
    recordLockWait(Api::WinWaitForSingleObject);
    return original<Slot, DWORD (WINAPI*)(HANDLE, DWORD)>(Api::WinWaitForSingleObject)(handle, timeout);
}

template <size_t Slot>
DWORD WINAPI hookWaitForSingleObjectEx(HANDLE handle, DWORD timeout, BOOL alertable)
{
    recordLockWait(Api::WinWaitForSingleObjectEx);
    return original<Slot, DWORD (WINAPI*)(HANDLE, DWORD, BOOL)>(Api::WinWaitForSingleObjectEx)(handle, timeout, alertable);
}

template <size_t Slot>
DWORD WINAPI hookWaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD timeout)
{
    recordLockWait(Api::WinWaitForMultipleObjects);
    return original<Slot, DWORD (WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD)>(Api::WinWaitForMultipleObjects)(
        count, handles, waitAll, timeout);
}

template <size_t Slot>
DWORD WINAPI hookWaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, BOOL waitAll,
                                          DWORD timeout, BOOL alertable)
{
    recordLockWait(Api::WinWaitForMultipleObjectsEx);
    return original<Slot, DWORD (WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD, BOOL)>(Api::WinWaitForMultipleObjectsEx)(
        count, handles, waitAll, timeout, alertable);
}

template <size_t Slot>
BOOL WINAPI hookSleepConditionVariableCS(PCONDITION_VARIABLE condition, PCRITICAL_SECTION lock, DWORD timeout)
{
    recordLockWait(Api::WinSleepConditionVariableCS);
    return original<Slot, BOOL (WINAPI*)(PCONDITION_VARIABLE, PCRITICAL_SECTION, DWORD)>(Api::WinSleepConditionVariableCS)(
        condition, lock, timeout);
}

template <size_t Slot>
BOOL WINAPI hookSleepConditionVariableSRW(PCONDITION_VARIABLE condition, PSRWLOCK lock,
                                          DWORD timeout, ULONG flags)
{
    recordLockWait(Api::WinSleepConditionVariableSRW);
    return original<Slot, BOOL (WINAPI*)(PCONDITION_VARIABLE, PSRWLOCK, DWORD, ULONG)>(Api::WinSleepConditionVariableSRW)(
        condition, lock, timeout, flags);
}

template <size_t Slot>
BOOL WINAPI hookWaitOnAddress(volatile VOID* address, PVOID compare, SIZE_T size, DWORD timeout)
{
    recordLockWait(Api::WinWaitOnAddress);
    return original<Slot, BOOL (WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD)>(Api::WinWaitOnAddress)(
        address, compare, size, timeout);
}

template <size_t Slot>
void WINAPI hookSleep(DWORD milliseconds)
{
    recordLockWait(Api::WinSleep);
    original<Slot, void (WINAPI*)(DWORD)>(Api::WinSleep)(milliseconds);
}

template <size_t Slot>
DWORD WINAPI hookSleepEx(DWORD milliseconds, BOOL alertable)
{
    recordLockWait(Api::WinSleepEx);
    return original<Slot, DWORD (WINAPI*)(DWORD, BOOL)>(Api::WinSleepEx)(milliseconds, alertable);
}

template <size_t Slot>
BOOL WINAPI hookSwitchToThread()
{
    recordLockWait(Api::WinSwitchToThread);
    return original<Slot, BOOL (WINAPI*)()>(Api::WinSwitchToThread)();
}

template <size_t Slot>
HANDLE WINAPI hookCreateFileA(LPCSTR path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES attributes,
                              DWORD creation, DWORD flags, HANDLE templateFile)
{
    recordFileIO(Api::WinCreateFileA);
    return original<Slot, HANDLE (WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE)>(
        Api::WinCreateFileA)(path, access, sharing, attributes, creation, flags, templateFile);
}

template <size_t Slot>
HANDLE WINAPI hookCreateFileW(LPCWSTR path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES attributes,
                              DWORD creation, DWORD flags, HANDLE templateFile)
{
    recordFileIO(Api::WinCreateFileW);
    return original<Slot, HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE)>(
        Api::WinCreateFileW)(path, access, sharing, attributes, creation, flags, templateFile);
}

template <size_t Slot>
BOOL WINAPI hookReadFile(HANDLE file, LPVOID data, DWORD bytes, LPDWORD readBytes, LPOVERLAPPED overlapped)
{
    recordFileIO(Api::WinReadFile);
    return original<Slot, BOOL (WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED)>(Api::WinReadFile)(
        file, data, bytes, readBytes, overlapped);
}

template <size_t Slot>
BOOL WINAPI hookReadFileEx(HANDLE file, LPVOID data, DWORD bytes, LPOVERLAPPED overlapped,
                           LPOVERLAPPED_COMPLETION_ROUTINE completion)
{
    recordFileIO(Api::WinReadFileEx);
    return original<Slot, BOOL (WINAPI*)(HANDLE, LPVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE)>(
        Api::WinReadFileEx)(file, data, bytes, overlapped, completion);
}

template <size_t Slot>
BOOL WINAPI hookWriteFile(HANDLE file, LPCVOID data, DWORD bytes, LPDWORD writtenBytes, LPOVERLAPPED overlapped)
{
    recordFileIO(Api::WinWriteFile);
    return original<Slot, BOOL (WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED)>(Api::WinWriteFile)(
        file, data, bytes, writtenBytes, overlapped);
}

template <size_t Slot>
BOOL WINAPI hookWriteFileEx(HANDLE file, LPCVOID data, DWORD bytes, LPOVERLAPPED overlapped,
                            LPOVERLAPPED_COMPLETION_ROUTINE completion)
{
    recordFileIO(Api::WinWriteFileEx);
    return original<Slot, BOOL (WINAPI*)(HANDLE, LPCVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE)>(
        Api::WinWriteFileEx)(file, data, bytes, overlapped, completion);
}

template <size_t Slot>
BOOL WINAPI hookSetFilePointerEx(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER position, DWORD method)
{
    recordFileIO(Api::WinSetFilePointerEx);
    return original<Slot, BOOL (WINAPI*)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD)>(Api::WinSetFilePointerEx)(
        file, distance, position, method);
}

template <size_t Slot>
BOOL WINAPI hookFlushFileBuffers(HANDLE file)
{
    recordFileIO(Api::WinFlushFileBuffers);
    return original<Slot, BOOL (WINAPI*)(HANDLE)>(Api::WinFlushFileBuffers)(file);
}

template <size_t Slot>
BOOL WINAPI hookGetFileSizeEx(HANDLE file, PLARGE_INTEGER size)
{
    recordFileIO(Api::WinGetFileSizeEx);
    return original<Slot, BOOL (WINAPI*)(HANDLE, PLARGE_INTEGER)>(Api::WinGetFileSizeEx)(file, size);
}

template <size_t Slot>
BOOL WINAPI hookGetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info)
{
    recordFileIO(Api::WinGetFileAttributesExW);
    return original<Slot, BOOL (WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID)>(Api::WinGetFileAttributesExW)(
        path, level, info);
}

template <size_t Slot>
HANDLE WINAPI hookFindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data)
{
    recordFileIO(Api::WinFindFirstFileW);
    return original<Slot, HANDLE (WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW)>(Api::WinFindFirstFileW)(path, data);
}

template <size_t Slot>
BOOL WINAPI hookDeleteFileW(LPCWSTR path)
{
    recordFileIO(Api::WinDeleteFileW);
    return original<Slot, BOOL (WINAPI*)(LPCWSTR)>(Api::WinDeleteFileW)(path);
}

template <size_t Slot>
BOOL WINAPI hookMoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    recordFileIO(Api::WinMoveFileExW);
    return original<Slot, BOOL (WINAPI*)(LPCWSTR, LPCWSTR, DWORD)>(Api::WinMoveFileExW)(from, to, flags);
}

template <size_t Slot>
HANDLE WINAPI hookCreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES attributes, DWORD protection,
                                     DWORD maxHigh, DWORD maxLow, LPCWSTR name)
{
    recordFileIO(Api::WinCreateFileMappingW);
    return original<Slot, HANDLE (WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR)>(
        Api::WinCreateFileMappingW)(file, attributes, protection, maxHigh, maxLow, name);
}

template <size_t Slot>
LPVOID WINAPI hookMapViewOfFile(HANDLE mapping, DWORD access, DWORD offsetHigh, DWORD offsetLow, SIZE_T bytes)
{
    recordFileIO(Api::WinMapViewOfFile);
    return original<Slot, LPVOID (WINAPI*)(HANDLE, DWORD, DWORD, DWORD, SIZE_T)>(Api::WinMapViewOfFile)(
        mapping, access, offsetHigh, offsetLow, bytes);
}

struct HookSpec
{
    const char* importName;
    Api api;
    const std::array<void*, hookSlotCount>* replacements;
};

template <typename Function>
void* addressOf(Function function) noexcept
{
    return reinterpret_cast<void*>(function);
}

#define DEFINE_HOOK_ARRAY(name) \
    template <size_t... Slots> \
    std::array<void*, sizeof...(Slots)> make##name##HookArray(std::index_sequence<Slots...>) noexcept \
    { \
        return { addressOf(&name<Slots>)... }; \
    } \
    const auto name##Hooks = make##name##HookArray(std::make_index_sequence<hookSlotCount>{})

DEFINE_HOOK_ARRAY(hookMalloc);
DEFINE_HOOK_ARRAY(hookCalloc);
DEFINE_HOOK_ARRAY(hookRealloc);
DEFINE_HOOK_ARRAY(hookFree);
DEFINE_HOOK_ARRAY(hookAlignedMalloc);
DEFINE_HOOK_ARRAY(hookAlignedRealloc);
DEFINE_HOOK_ARRAY(hookAlignedFree);
DEFINE_HOOK_ARRAY(hookAlignedOffsetMalloc);
DEFINE_HOOK_ARRAY(hookAlignedOffsetRealloc);
DEFINE_HOOK_ARRAY(hookFopen);
DEFINE_HOOK_ARRAY(hookWFopen);
DEFINE_HOOK_ARRAY(hookFread);
DEFINE_HOOK_ARRAY(hookFwrite);
DEFINE_HOOK_ARRAY(hookFclose);
DEFINE_HOOK_ARRAY(hookFflush);
DEFINE_HOOK_ARRAY(hookFseek);
DEFINE_HOOK_ARRAY(hookFseek64);
DEFINE_HOOK_ARRAY(hookHeapAlloc);
DEFINE_HOOK_ARRAY(hookHeapReAlloc);
DEFINE_HOOK_ARRAY(hookHeapFree);
DEFINE_HOOK_ARRAY(hookEnterCriticalSection);
DEFINE_HOOK_ARRAY(hookTryEnterCriticalSection);
DEFINE_HOOK_ARRAY(hookAcquireSRWExclusive);
DEFINE_HOOK_ARRAY(hookAcquireSRWShared);
DEFINE_HOOK_ARRAY(hookTryAcquireSRWExclusive);
DEFINE_HOOK_ARRAY(hookTryAcquireSRWShared);
DEFINE_HOOK_ARRAY(hookWaitForSingleObject);
DEFINE_HOOK_ARRAY(hookWaitForSingleObjectEx);
DEFINE_HOOK_ARRAY(hookWaitForMultipleObjects);
DEFINE_HOOK_ARRAY(hookWaitForMultipleObjectsEx);
DEFINE_HOOK_ARRAY(hookSleepConditionVariableCS);
DEFINE_HOOK_ARRAY(hookSleepConditionVariableSRW);
DEFINE_HOOK_ARRAY(hookWaitOnAddress);
DEFINE_HOOK_ARRAY(hookSleep);
DEFINE_HOOK_ARRAY(hookSleepEx);
DEFINE_HOOK_ARRAY(hookSwitchToThread);
DEFINE_HOOK_ARRAY(hookCreateFileA);
DEFINE_HOOK_ARRAY(hookCreateFileW);
DEFINE_HOOK_ARRAY(hookReadFile);
DEFINE_HOOK_ARRAY(hookReadFileEx);
DEFINE_HOOK_ARRAY(hookWriteFile);
DEFINE_HOOK_ARRAY(hookWriteFileEx);
DEFINE_HOOK_ARRAY(hookSetFilePointerEx);
DEFINE_HOOK_ARRAY(hookFlushFileBuffers);
DEFINE_HOOK_ARRAY(hookGetFileSizeEx);
DEFINE_HOOK_ARRAY(hookGetFileAttributesExW);
DEFINE_HOOK_ARRAY(hookFindFirstFileW);
DEFINE_HOOK_ARRAY(hookDeleteFileW);
DEFINE_HOOK_ARRAY(hookMoveFileExW);
DEFINE_HOOK_ARRAY(hookCreateFileMappingW);
DEFINE_HOOK_ARRAY(hookMapViewOfFile);

#undef DEFINE_HOOK_ARRAY

const HookSpec hookSpecs[] = {
    { "malloc", Api::CrtMalloc, &hookMallocHooks },
    { "calloc", Api::CrtCalloc, &hookCallocHooks },
    { "realloc", Api::CrtRealloc, &hookReallocHooks },
    { "free", Api::CrtFree, &hookFreeHooks },
    { "_aligned_malloc", Api::CrtAlignedMalloc, &hookAlignedMallocHooks },
    { "_aligned_realloc", Api::CrtAlignedRealloc, &hookAlignedReallocHooks },
    { "_aligned_free", Api::CrtAlignedFree, &hookAlignedFreeHooks },
    { "_aligned_offset_malloc", Api::CrtAlignedOffsetMalloc, &hookAlignedOffsetMallocHooks },
    { "_aligned_offset_realloc", Api::CrtAlignedOffsetRealloc, &hookAlignedOffsetReallocHooks },
    { "fopen", Api::CrtFopen, &hookFopenHooks },
    { "_wfopen", Api::CrtWFopen, &hookWFopenHooks },
    { "fread", Api::CrtFread, &hookFreadHooks },
    { "fwrite", Api::CrtFwrite, &hookFwriteHooks },
    { "fclose", Api::CrtFclose, &hookFcloseHooks },
    { "fflush", Api::CrtFflush, &hookFflushHooks },
    { "fseek", Api::CrtFseek, &hookFseekHooks },
    { "_fseeki64", Api::CrtFseek64, &hookFseek64Hooks },
    { "HeapAlloc", Api::WinHeapAlloc, &hookHeapAllocHooks },
    { "HeapReAlloc", Api::WinHeapRealloc, &hookHeapReAllocHooks },
    { "HeapFree", Api::WinHeapFree, &hookHeapFreeHooks },
    { "EnterCriticalSection", Api::WinEnterCriticalSection, &hookEnterCriticalSectionHooks },
    { "TryEnterCriticalSection", Api::WinTryEnterCriticalSection, &hookTryEnterCriticalSectionHooks },
    { "AcquireSRWLockExclusive", Api::WinAcquireSRWExclusive, &hookAcquireSRWExclusiveHooks },
    { "AcquireSRWLockShared", Api::WinAcquireSRWShared, &hookAcquireSRWSharedHooks },
    { "TryAcquireSRWLockExclusive", Api::WinTryAcquireSRWExclusive, &hookTryAcquireSRWExclusiveHooks },
    { "TryAcquireSRWLockShared", Api::WinTryAcquireSRWShared, &hookTryAcquireSRWSharedHooks },
    { "WaitForSingleObject", Api::WinWaitForSingleObject, &hookWaitForSingleObjectHooks },
    { "WaitForSingleObjectEx", Api::WinWaitForSingleObjectEx, &hookWaitForSingleObjectExHooks },
    { "WaitForMultipleObjects", Api::WinWaitForMultipleObjects, &hookWaitForMultipleObjectsHooks },
    { "WaitForMultipleObjectsEx", Api::WinWaitForMultipleObjectsEx, &hookWaitForMultipleObjectsExHooks },
    { "SleepConditionVariableCS", Api::WinSleepConditionVariableCS, &hookSleepConditionVariableCSHooks },
    { "SleepConditionVariableSRW", Api::WinSleepConditionVariableSRW, &hookSleepConditionVariableSRWHooks },
    { "WaitOnAddress", Api::WinWaitOnAddress, &hookWaitOnAddressHooks },
    { "Sleep", Api::WinSleep, &hookSleepHooks },
    { "SleepEx", Api::WinSleepEx, &hookSleepExHooks },
    { "SwitchToThread", Api::WinSwitchToThread, &hookSwitchToThreadHooks },
    { "CreateFileA", Api::WinCreateFileA, &hookCreateFileAHooks },
    { "CreateFileW", Api::WinCreateFileW, &hookCreateFileWHooks },
    { "ReadFile", Api::WinReadFile, &hookReadFileHooks },
    { "ReadFileEx", Api::WinReadFileEx, &hookReadFileExHooks },
    { "WriteFile", Api::WinWriteFile, &hookWriteFileHooks },
    { "WriteFileEx", Api::WinWriteFileEx, &hookWriteFileExHooks },
    { "SetFilePointerEx", Api::WinSetFilePointerEx, &hookSetFilePointerExHooks },
    { "FlushFileBuffers", Api::WinFlushFileBuffers, &hookFlushFileBuffersHooks },
    { "GetFileSizeEx", Api::WinGetFileSizeEx, &hookGetFileSizeExHooks },
    { "GetFileAttributesExW", Api::WinGetFileAttributesExW, &hookGetFileAttributesExWHooks },
    { "FindFirstFileW", Api::WinFindFirstFileW, &hookFindFirstFileWHooks },
    { "DeleteFileW", Api::WinDeleteFileW, &hookDeleteFileWHooks },
    { "MoveFileExW", Api::WinMoveFileExW, &hookMoveFileExWHooks },
    { "CreateFileMappingW", Api::WinCreateFileMappingW, &hookCreateFileMappingWHooks },
    { "MapViewOfFile", Api::WinMapViewOfFile, &hookMapViewOfFileHooks }
};

bool isCrtModule(const wchar_t* name) noexcept
{
    return _wcsicmp(name, L"ucrtbase.dll") == 0
        || _wcsicmp(name, L"msvcrt.dll") == 0
        || _wcsnicmp(name, L"msvcp", 5) == 0
        || _wcsnicmp(name, L"vcruntime", 9) == 0
        || _wcsnicmp(name, L"api-ms-win-crt-", 14) == 0;
}

bool isOutsideSystemDirectory(const wchar_t* modulePath, const wchar_t* systemDirectory,
                              size_t systemDirectoryLength) noexcept
{
    if (systemDirectoryLength == 0 || _wcsnicmp(modulePath, systemDirectory, systemDirectoryLength) != 0)
        return true;
    const wchar_t next = modulePath[systemDirectoryLength];
    return next != L'\\' && next != L'/';
}

bool isSelectedModule(const MODULEENTRY32W& module, const wchar_t* systemDirectory,
                      size_t systemDirectoryLength) noexcept
{
    const bool inSystemDirectory = systemDirectoryLength != 0
        && ! isOutsideSystemDirectory(module.szExePath, systemDirectory, systemDirectoryLength);
    return ! inSystemDirectory || isCrtModule(module.szModule);
}

const HookSpec* findHookSpec(const char* importName) noexcept
{
    for (const auto& spec : hookSpecs)
        if (std::strcmp(importName, spec.importName) == 0)
            return &spec;
    return nullptr;
}

enum class HookScanPass
{
    Preflight,
    Apply
};

void reportProtectionFailure(const char* operation, Api api, const wchar_t* module,
                             void* slot, void* address, DWORD error,
                             uint32_t patchedBeforeFailure) noexcept
{
    std::fprintf(stderr,
        "Windows IAT hook %s failed: api=%s module=%ls slot=%p address=%p "
        "error=%lu patched_before_failure=%u\n",
        operation, apiName(api), module, slot, address, error, patchedBeforeFailure);
}

size_t findOriginalSlot(const OriginalTable& table, Api api, void* address) noexcept
{
    const auto& slots = table.addresses[static_cast<size_t>(api)];
    for (size_t slot = 0; slot < hookSlotCount; ++slot)
        if (slots[slot] == address)
            return slot;
    return hookSlotCount;
}

size_t findHookSlot(const HookSpec& spec, void* address) noexcept
{
    for (size_t slot = 0; slot < hookSlotCount; ++slot)
        if ((*spec.replacements)[slot] == address)
            return slot;
    return hookSlotCount;
}

bool registerOriginal(OriginalTable& table, Api api, void* address, size_t& slot,
                      bool& routesChanged) noexcept
{
    auto& slots = table.addresses[static_cast<size_t>(api)];
    for (size_t i = 0; i < hookSlotCount; ++i)
        if (slots[i] == address)
        {
            slot = i;
            return true;
        }

    for (size_t i = 0; i < hookSlotCount; ++i)
        if (slots[i] == nullptr)
        {
            slots[i] = address;
            slot = i;
            routesChanged = true;
            return true;
        }

    return false;
}

bool scanModule(HMODULE module, const wchar_t* modulePath, HookScanPass pass,
                OriginalTable& candidate, bool& routesChanged,
                WindowsHookReport& report) noexcept
{
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return true;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return true;

    const auto& importDirectory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDirectory.VirtualAddress == 0)
        return true;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDirectory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor)
    {
        if (descriptor->OriginalFirstThunk == 0 || descriptor->FirstThunk == 0)
            continue;

        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;

            const auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            const HookSpec* spec = findHookSpec(reinterpret_cast<const char*>(import->Name));
            if (spec == nullptr)
                continue;

            const Api api = spec->api;
            const auto index = static_cast<size_t>(api);
            void* current = reinterpret_cast<void*>(slots->u1.Function);
            if (findHookSlot(*spec, current) != hookSlotCount)
                continue;

            if (pass == HookScanPass::Preflight)
            {
                size_t originalSlot = hookSlotCount;
                if (! registerOriginal(candidate, api, current, originalSlot, routesChanged))
                {
                    std::fprintf(stderr,
                        "Windows IAT hook slot capacity exceeded: api=%s capacity=%zu "
                        "module=%ls address=%p\n",
                        apiName(api), hookSlotCount, modulePath, current);
                    return false;
                }
                continue;
            }

            const size_t originalSlot = findOriginalSlot(candidate, api, current);
            if (originalSlot == hookSlotCount)
            {
                std::fprintf(stderr,
                    "Windows IAT hook changed after preflight: api=%s module=%ls "
                    "address=%p slot=%p\n",
                    apiName(api), modulePath, current,
                    reinterpret_cast<void*>(&slots->u1.Function));
                return false;
            }

            void* replacement = (*spec->replacements)[originalSlot];
            DWORD oldProtection = 0;
            if (! VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &oldProtection))
            {
                const DWORD error = GetLastError();
                reportProtectionFailure("VirtualProtect(write)", api, modulePath,
                                        reinterpret_cast<void*>(&slots->u1.Function), current,
                                        error, report.patchedImports);
                return false;
            }

            slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            DWORD ignoredProtection = 0;
            if (! VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), oldProtection,
                                 &ignoredProtection))
            {
                const DWORD error = GetLastError();
                reportProtectionFailure("VirtualProtect(restore)", api, modulePath,
                                        reinterpret_cast<void*>(&slots->u1.Function), replacement,
                                        error, report.patchedImports + 1);
                return false;
            }

            ++report.importsByApi[index];
            ++report.patchedImports;
        }
    }

    return true;
}

bool scanSelectedModules(HANDLE snapshot, const wchar_t* systemDirectory,
                         size_t systemDirectoryLength, HookScanPass pass,
                         OriginalTable& candidate, bool& routesChanged,
                         WindowsHookReport& report) noexcept
{
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (! Module32FirstW(snapshot, &module))
        return false;

    do
    {
        if (isSelectedModule(module, systemDirectory, systemDirectoryLength))
        {
            if (pass == HookScanPass::Preflight)
                ++report.scannedModules;

            if (! scanModule(module.hModule, module.szExePath, pass, candidate,
                             routesChanged, report))
                return false;
        }
    }
    while (Module32NextW(snapshot, &module));

    return true;
}
}

namespace OpenTune::RealtimeAuditTest
{
WindowsHookReport installWindowsHooks() noexcept
{
    WindowsHookReport report;

    wchar_t systemDirectory[MAX_PATH]{};
    const UINT systemLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    const size_t systemDirectoryLength = systemLength < MAX_PATH ? systemLength : 0;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return report;

    const OriginalTable* active = activeOriginals.load(std::memory_order_acquire);
    OriginalTable candidate{};
    if (active != nullptr)
        candidate = *active;
    bool routesChanged = false;

    if (! scanSelectedModules(snapshot, systemDirectory, systemDirectoryLength,
                              HookScanPass::Preflight, candidate, routesChanged, report))
    {
        CloseHandle(snapshot);
        return report;
    }

    if (active == nullptr || routesChanged)
    {
        const auto* published = new (std::nothrow) OriginalTable(candidate);
        if (published == nullptr)
        {
            CloseHandle(snapshot);
            return report;
        }
        activeOriginals.store(published, std::memory_order_release);
    }

    if (! scanSelectedModules(snapshot, systemDirectory, systemDirectoryLength,
                              HookScanPass::Apply, candidate, routesChanged, report))
    {
        report.patchedImports = 0;
        report.importsByApi = {};
        CloseHandle(snapshot);
        return report;
    }

    CloseHandle(snapshot);
    report.supported = true;
    return report;
}
}

#else

namespace OpenTune::RealtimeAuditTest
{
WindowsHookReport installWindowsHooks() noexcept
{
    return {};
}
}

#endif
