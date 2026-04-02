#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace OpenTune {

struct DmlDiagnosticIssue {
    std::string stage;
    long hresult = 0;
    std::string detail;
    std::string remediation;
};

struct DmlDiagnosticReport {
    bool ok = false;
    std::string adapterName;
    uint32_t adapterIndex = 0;
    uint32_t windowsBuild = 0;
    std::string directMLDllPath;
    std::string directMLVersion;
    std::vector<DmlDiagnosticIssue> issues;

    std::string toMultilineString() const;
};

class DmlRuntimeVerifier {
public:
    static constexpr uint32_t kMinimumWindowsBuild = 19044;
    static constexpr const char* kRequiredDirectMLVersion = "1.15.4";

    static DmlDiagnosticReport verify(uint32_t adapterIndex, const std::string& adapterName);
};

} // namespace OpenTune
