#include "ReaperHostBridge.h"

#include <cstdint>

namespace OpenTune {

namespace {

#if JUCE_WINDOWS
// Minimal COM-style view of the two VST3 interfaces involved, so no REAPER SDK or VST3 SDK
// header is needed. On Windows VST3 interfaces use the COM layout: FUnknown is
// queryInterface / addRef / release, IReaperHostApplication adds getReaperApi first
// (reaper_vst3_interfaces.h in the REAPER SDK).
struct FUnknownAbi
{
    virtual std::int32_t __stdcall queryInterface(const char* iid, void** object) = 0;
    virtual std::uint32_t __stdcall addRef() = 0;
    virtual std::uint32_t __stdcall release() = 0;
};

struct ReaperHostApplicationAbi : FUnknownAbi
{
    virtual void* __stdcall getReaperApi(const char* functionName) = 0;
};

// IID 79655E36-77EE-4267-A573-FEF74912C27C in the COM byte order VST3 uses on Windows:
// the first three groups little-endian, the rest as written.
constexpr char kReaperHostApplicationIid[16] = {
    '\x36', '\x5E', '\x65', '\x79', '\xEE', '\x77', '\x67', '\x42',
    '\xA5', '\x73', '\xFE', '\xF7', '\x49', '\x12', '\xC2', '\x7C'
};
#endif

} // namespace

void ReaperHostBridge::setIHostApplication(Steinberg::FUnknown* host)
{
    getCursorPosition_ = nullptr;
    getPlayPosition_ = nullptr;
    getPlayState_ = nullptr;
#if JUCE_WINDOWS
    if (host == nullptr)
        return;

    void* found = nullptr;
    auto* unknown = reinterpret_cast<FUnknownAbi*>(host);
    if (unknown->queryInterface(kReaperHostApplicationIid, &found) != 0 || found == nullptr)
        return; // not REAPER

    auto* reaper = static_cast<ReaperHostApplicationAbi*>(found);
    // REAPER API functions are plain C functions that live as long as REAPER,
    // so the interface itself can be released right away.
    getCursorPosition_ = reinterpret_cast<double (*)()>(reaper->getReaperApi("GetCursorPosition"));
    getPlayPosition_ = reinterpret_cast<double (*)()>(reaper->getReaperApi("GetPlayPosition"));
    getPlayState_ = reinterpret_cast<int (*)()>(reaper->getReaperApi("GetPlayState"));
    reaper->release();
#else
    juce::ignoreUnused(host);
#endif
}

std::optional<ReaperHostBridge::Transport> ReaperHostBridge::getTransport() const
{
    if (getCursorPosition_ == nullptr || getPlayPosition_ == nullptr || getPlayState_ == nullptr)
        return std::nullopt;
    Transport transport;
    transport.playState = getPlayState_();
    transport.editCursor = getCursorPosition_();
    transport.playPosition = getPlayPosition_();
    return transport;
}

} // namespace OpenTune
