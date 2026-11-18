#include "RealtimeAudit.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
    #include <malloc.h>
#endif

namespace
{
thread_local std::atomic<uint32_t>* spinLockWaitSignal = nullptr;

struct ThreadState
{
    uint32_t scopeDepth = 0;
    uint32_t cppAllocatorCallDepth = 0;
    OpenTune::RealtimeAudit::Counters counters{};
    OpenTune::RealtimeAudit::Record records[OpenTune::RealtimeAudit::maxRecordsPerScope]{};
    uint16_t recordCount = 0;
    uint32_t droppedRecords = 0;
};

thread_local ThreadState state{};

void clearState() noexcept
{
    state.counters = {};
    state.recordCount = 0;
    state.droppedRecords = 0;
}

void record(OpenTune::RealtimeAudit::Event event, OpenTune::RealtimeAudit::Api api) noexcept
{
    using namespace OpenTune::RealtimeAudit;
    if (state.scopeDepth == 0)
        return;

    switch (event)
    {
        case Event::Allocation:   ++state.counters.allocations; break;
        case Event::Deallocation: ++state.counters.deallocations; break;
        case Event::LockAttempt:  ++state.counters.lockAttempts; break;
        case Event::LockWait:     ++state.counters.lockWaits; break;
        case Event::FileIO:       ++state.counters.fileIO; break;
    }

    if (state.recordCount < maxRecordsPerScope)
        state.records[state.recordCount++] = { event, api };
    else
        ++state.droppedRecords;
}

OpenTune::RealtimeAudit::Result snapshot(const char* label,
                                         const OpenTune::RealtimeAudit::Counters& baselineCounters,
                                         uint16_t firstRecord,
                                         uint32_t baselineDroppedRecords) noexcept
{
    using namespace OpenTune::RealtimeAudit;
    Result result;
    result.label = label;
    result.counters.allocations = state.counters.allocations - baselineCounters.allocations;
    result.counters.deallocations = state.counters.deallocations - baselineCounters.deallocations;
    result.counters.lockAttempts = state.counters.lockAttempts - baselineCounters.lockAttempts;
    result.counters.lockWaits = state.counters.lockWaits - baselineCounters.lockWaits;
    result.counters.fileIO = state.counters.fileIO - baselineCounters.fileIO;
    result.recordCount = static_cast<uint16_t>(state.recordCount - firstRecord);
    result.droppedRecords = state.droppedRecords - baselineDroppedRecords;
    for (uint16_t i = 0; i < result.recordCount; ++i)
        result.records[i] = state.records[firstRecord + i];
    return result;
}

void* allocateCpp(size_t size) noexcept
{
    OpenTune::RealtimeAudit::recordAllocation(OpenTune::RealtimeAudit::Api::CppNew);
    OpenTune::RealtimeAudit::beginCppAllocatorCall();
    void* memory = std::malloc(size == 0 ? 1 : size);
    OpenTune::RealtimeAudit::endCppAllocatorCall();
    return memory;
}

#if defined(_WIN32)
void* allocateAlignedCpp(size_t size, size_t alignment) noexcept
{
    OpenTune::RealtimeAudit::recordAllocation(OpenTune::RealtimeAudit::Api::CppNew);
    OpenTune::RealtimeAudit::beginCppAllocatorCall();
    void* memory = _aligned_malloc(size == 0 ? 1 : size, alignment);
    OpenTune::RealtimeAudit::endCppAllocatorCall();
    return memory;
}
#endif

void deallocateCpp(void* memory) noexcept
{
    if (memory == nullptr)
        return;
    OpenTune::RealtimeAudit::recordDeallocation(OpenTune::RealtimeAudit::Api::CppDelete);
    OpenTune::RealtimeAudit::beginCppAllocatorCall();
    std::free(memory);
    OpenTune::RealtimeAudit::endCppAllocatorCall();
}

#if defined(_WIN32)
void deallocateAlignedCpp(void* memory) noexcept
{
    if (memory == nullptr)
        return;
    OpenTune::RealtimeAudit::recordDeallocation(OpenTune::RealtimeAudit::Api::CppDelete);
    OpenTune::RealtimeAudit::beginCppAllocatorCall();
    _aligned_free(memory);
    OpenTune::RealtimeAudit::endCppAllocatorCall();
}
#endif
}

namespace OpenTune::RealtimeAudit
{
Scope::Scope(const char* label) noexcept
{
    result_.label = label;
    if (state.scopeDepth++ == 0)
        clearState();
    baselineCounters_ = state.counters;
    firstRecord_ = state.recordCount;
    baselineDroppedRecords_ = state.droppedRecords;
}

Scope::~Scope()
{
    finish();
}

Result Scope::finish() noexcept
{
    if (active_)
    {
        result_ = snapshot(result_.label, baselineCounters_, firstRecord_, baselineDroppedRecords_);
        --state.scopeDepth;
        active_ = false;
    }
    return result_;
}

void recordAllocation(Api api) noexcept
{
    record(Event::Allocation, api);
}

void recordDeallocation(Api api) noexcept
{
    record(Event::Deallocation, api);
}

void recordLockAttempt(Api api) noexcept
{
    record(Event::LockAttempt, api);
}

void recordLockWait(Api api) noexcept
{
    record(Event::LockWait, api);
}

void recordFileIO(Api api) noexcept
{
    record(Event::FileIO, api);
}

void beginCppAllocatorCall() noexcept
{
    ++state.cppAllocatorCallDepth;
}

void endCppAllocatorCall() noexcept
{
    --state.cppAllocatorCallDepth;
}

bool isCppAllocatorCall() noexcept
{
    return state.cppAllocatorCallDepth != 0;
}

void recordSpinLockAttempt() noexcept
{
    recordLockAttempt(Api::JuceSpinLock);
}

void recordSpinLockWait() noexcept
{
    recordLockWait(Api::JuceSpinLock);
    if (auto* signal = spinLockWaitSignal)
        signal->store(1, std::memory_order_release);
}
}

namespace OpenTune::RealtimeAuditTest
{
void setSpinLockWaitSignal(std::atomic<uint32_t>* signal) noexcept
{
    spinLockWaitSignal = signal;
}

const char* apiName(Api api) noexcept
{
    static constexpr const char* names[] = {
        "operator new", "operator delete", "malloc", "calloc", "realloc", "free",
        "_aligned_malloc", "_aligned_realloc", "_aligned_free", "_aligned_offset_malloc",
        "_aligned_offset_realloc", "fopen", "_wfopen", "fread", "fwrite", "fclose",
        "fflush", "fseek", "_fseeki64", "HeapAlloc", "HeapReAlloc", "HeapFree",
        "EnterCriticalSection", "TryEnterCriticalSection", "AcquireSRWLockExclusive",
        "AcquireSRWLockShared", "TryAcquireSRWLockExclusive", "TryAcquireSRWLockShared",
        "WaitForSingleObject", "WaitForSingleObjectEx", "WaitForMultipleObjects",
        "WaitForMultipleObjectsEx", "SleepConditionVariableCS", "SleepConditionVariableSRW",
        "WaitOnAddress", "Sleep", "SleepEx", "SwitchToThread", "CreateFileA", "CreateFileW",
        "ReadFile", "ReadFileEx", "WriteFile", "WriteFileEx", "SetFilePointerEx",
        "FlushFileBuffers", "GetFileSizeEx", "GetFileAttributesExW", "FindFirstFileW",
        "DeleteFileW", "MoveFileExW", "CreateFileMappingW", "MapViewOfFile", "JUCE SpinLock"
    };
    const auto index = static_cast<size_t>(api);
    return index < static_cast<size_t>(Api::Count) ? names[index] : "unknown";
}

const char* eventName(OpenTune::RealtimeAudit::Event event) noexcept
{
    using OpenTune::RealtimeAudit::Event;
    switch (event)
    {
        case Event::Allocation: return "allocation";
        case Event::Deallocation: return "deallocation";
        case Event::LockAttempt: return "lock_attempt";
        case Event::LockWait: return "lock_wait";
        case Event::FileIO: return "file_io";
    }
    return "unknown";
}

bool isClean(const Result& result) noexcept
{
    const auto& c = result.counters;
    return c.allocations == 0 && c.deallocations == 0
        && c.lockAttempts == 0 && c.lockWaits == 0 && c.fileIO == 0;
}

void reportResult(const Result& result) noexcept
{
    const auto& c = result.counters;
    std::fprintf(stdout,
        "{\"event\":\"realtime_audit\",\"scope\":\"%s\",\"allocations\":%u,"
        "\"deallocations\":%u,\"lockAttempts\":%u,\"lockWaits\":%u,\"fileIO\":%u,"
        "\"droppedRecords\":%u}\n",
        result.label != nullptr ? result.label : "unnamed", c.allocations, c.deallocations,
        c.lockAttempts, c.lockWaits, c.fileIO, result.droppedRecords);

    for (uint16_t i = 0; i < result.recordCount; ++i)
        std::fprintf(stdout, "  %s: %s\n", eventName(result.records[i].event),
                     apiName(result.records[i].api));
}

void reportHookCoverage(const WindowsHookReport& report) noexcept
{
    std::fprintf(stdout,
        "{\"event\":\"realtime_audit_hooks\",\"supported\":%s,"
        "\"scannedModules\":%u,\"patchedImports\":%u}\n",
        report.supported ? "true" : "false", report.scannedModules, report.patchedImports);
    for (size_t i = 0; i < report.importsByApi.size(); ++i)
        if (report.importsByApi[i] != 0)
            std::fprintf(stdout, "  %s: %u IAT import(s)\n",
                         apiName(static_cast<Api>(i)), report.importsByApi[i]);
}
}

void* operator new(std::size_t size)
{
    if (auto* memory = allocateCpp(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    return allocateCpp(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
{
    return ::operator new(size, tag);
}

void operator delete(void* memory) noexcept
{
    deallocateCpp(memory);
}

void operator delete[](void* memory) noexcept
{
    deallocateCpp(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    deallocateCpp(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    deallocateCpp(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept
{
    deallocateCpp(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept
{
    deallocateCpp(memory);
}

#if defined(_WIN32)
void* operator new(std::size_t size, std::align_val_t alignment)
{
    if (auto* memory = allocateAlignedCpp(size, static_cast<size_t>(alignment)))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return allocateAlignedCpp(size, static_cast<size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& tag) noexcept
{
    return ::operator new(size, alignment, tag);
}

void operator delete(void* memory, std::align_val_t) noexcept
{
    deallocateAlignedCpp(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept
{
    deallocateAlignedCpp(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
    deallocateAlignedCpp(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
    deallocateAlignedCpp(memory);
}

void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
    deallocateAlignedCpp(memory);
}

void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
    deallocateAlignedCpp(memory);
}
#endif
