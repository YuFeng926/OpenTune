#pragma once

#include <cstddef>
#include <cstdint>

namespace OpenTune::RealtimeAudit
{
enum class Api : uint16_t
{
    CppNew,
    CppDelete,
    CrtMalloc,
    CrtCalloc,
    CrtRealloc,
    CrtFree,
    CrtAlignedMalloc,
    CrtAlignedRealloc,
    CrtAlignedFree,
    CrtAlignedOffsetMalloc,
    CrtAlignedOffsetRealloc,
    CrtFopen,
    CrtWFopen,
    CrtFread,
    CrtFwrite,
    CrtFclose,
    CrtFflush,
    CrtFseek,
    CrtFseek64,
    WinHeapAlloc,
    WinHeapRealloc,
    WinHeapFree,
    WinEnterCriticalSection,
    WinTryEnterCriticalSection,
    WinAcquireSRWExclusive,
    WinAcquireSRWShared,
    WinTryAcquireSRWExclusive,
    WinTryAcquireSRWShared,
    WinWaitForSingleObject,
    WinWaitForSingleObjectEx,
    WinWaitForMultipleObjects,
    WinWaitForMultipleObjectsEx,
    WinSleepConditionVariableCS,
    WinSleepConditionVariableSRW,
    WinWaitOnAddress,
    WinSleep,
    WinSleepEx,
    WinSwitchToThread,
    WinCreateFileA,
    WinCreateFileW,
    WinReadFile,
    WinReadFileEx,
    WinWriteFile,
    WinWriteFileEx,
    WinSetFilePointerEx,
    WinFlushFileBuffers,
    WinGetFileSizeEx,
    WinGetFileAttributesExW,
    WinFindFirstFileW,
    WinDeleteFileW,
    WinMoveFileExW,
    WinCreateFileMappingW,
    WinMapViewOfFile,
    JuceSpinLock,
    Count
};

enum class Event : uint8_t
{
    Allocation,
    Deallocation,
    LockAttempt,
    LockWait,
    FileIO
};

struct Counters
{
    uint32_t allocations = 0;
    uint32_t deallocations = 0;
    uint32_t lockAttempts = 0;
    uint32_t lockWaits = 0;
    uint32_t fileIO = 0;
};

struct Record
{
    Event event{};
    Api api{};
};

inline constexpr size_t maxRecordsPerScope = 64;

struct Result
{
    const char* label = nullptr;
    Counters counters{};
    Record records[maxRecordsPerScope]{};
    uint16_t recordCount = 0;
    uint32_t droppedRecords = 0;
};

class Scope final
{
public:
    explicit Scope(const char* label) noexcept;
    ~Scope();

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

    Result finish() noexcept;

private:
    Result result_{};
    Counters baselineCounters_{};
    uint16_t firstRecord_ = 0;
    uint32_t baselineDroppedRecords_ = 0;
    bool active_ = true;
};

void recordAllocation(Api api) noexcept;
void recordDeallocation(Api api) noexcept;
void recordLockAttempt(Api api) noexcept;
void recordLockWait(Api api) noexcept;
void recordFileIO(Api api) noexcept;
void beginCppAllocatorCall() noexcept;
void endCppAllocatorCall() noexcept;
bool isCppAllocatorCall() noexcept;

void recordSpinLockAttempt() noexcept;
void recordSpinLockWait() noexcept;
}

#if defined(OPENTUNE_REALTIME_AUDIT)
    #define OPENTUNE_REALTIME_AUDIT_SCOPE \
        ::OpenTune::RealtimeAudit::Scope opentuneRealtimeAuditScope{ "vst3_process" }
    #define OPENTUNE_REALTIME_AUDIT_SPINLOCK() \
        ::OpenTune::RealtimeAudit::recordSpinLockAttempt()
    #define OPENTUNE_REALTIME_AUDIT_SPIN_WAIT() \
        ::OpenTune::RealtimeAudit::recordSpinLockWait()
#endif
