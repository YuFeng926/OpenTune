#include "../Source/Utils/PlayHeadState.h"

#include <cstdio>
#include <array>
#include <thread>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

juce::Optional<juce::AudioPlayHead::PositionInfo> makePosition(double seconds, bool playing)
{
    juce::AudioPlayHead::PositionInfo position;
    position.setTimeInSeconds(seconds);
    position.setIsPlaying(playing);
    return juce::Optional<juce::AudioPlayHead::PositionInfo>(position);
}

void testConcurrentDuplicateObservationCommitsOnce()
{
    OpenTune::PlayHeadState state;
    std::atomic<bool> start{false};
    std::array<std::thread, 8> observers;
    std::array<bool, 8> committed{};
    for (size_t i = 0; i < observers.size(); ++i)
        observers[i] = std::thread([&, i] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            committed[i] = state.update(makePosition(42.0, false)).timeChanged;
        });
    start.store(true, std::memory_order_release);
    for (auto& observer : observers)
        observer.join();
    int commits = 0;
    for (bool value : committed)
        commits += value ? 1 : 0;
    expect(commits == 1, "concurrent duplicate times have one committing observer");
    expect(state.presentationEpoch.load() == 1, "duplicate observers advance one epoch");
    expect(!state.update(juce::nullopt).timeChanged,
           "absent transport information preserves the observed state");
}

} // namespace

int main()
{
    testConcurrentDuplicateObservationCommitsOnce();
    return failures == 0 ? 0 : 1;
}
