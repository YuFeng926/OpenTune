#pragma once

#include "../Source/Utils/RealtimeAuditScope.h"

#include <array>
#include <cstdint>

namespace OpenTune::RealtimeAuditTest
{
using OpenTune::RealtimeAudit::Api;
using OpenTune::RealtimeAudit::Result;

struct WindowsHookReport
{
    bool supported = false;
    uint32_t scannedModules = 0;
    uint32_t patchedImports = 0;
    std::array<uint32_t, static_cast<size_t>(Api::Count)> importsByApi{};
};

WindowsHookReport installWindowsHooks() noexcept;
const char* apiName(Api api) noexcept;
const char* eventName(OpenTune::RealtimeAudit::Event event) noexcept;
bool isClean(const Result& result) noexcept;
void reportResult(const Result& result) noexcept;
void reportHookCoverage(const WindowsHookReport& report) noexcept;
}
