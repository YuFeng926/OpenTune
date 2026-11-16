#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <optional>

namespace OpenTune {

// REAPER gives VST3 plugins its own API (IReaperHostApplication). OpenTune uses it to know
// where the user is in the song: while the transport is stopped or paused REAPER does not
// report the position to the plugin, so keyboard paste, arrows and Shift+X would act at
// the place where playback last ran. In other hosts nothing is found and nothing changes.
class ReaperHostBridge final : public juce::VST3ClientExtensions
{
public:
    struct Transport
    {
        int playState = 0;          // GetPlayState: bit 1 playing, 2 paused, 4 recording
        double editCursor = 0.0;    // project seconds
        double playPosition = 0.0;  // project seconds, the audible position
        bool isRunning() const noexcept { return (playState & (1 | 4)) != 0; }
        bool isPaused() const noexcept { return !isRunning() && (playState & 2) != 0; }
    };

    void setIHostApplication(Steinberg::FUnknown* host) override;

    // Empty in other hosts.
    std::optional<Transport> getTransport() const;

private:
    double (*getCursorPosition_)() = nullptr;
    double (*getPlayPosition_)() = nullptr;
    int (*getPlayState_)() = nullptr;
};

} // namespace OpenTune
