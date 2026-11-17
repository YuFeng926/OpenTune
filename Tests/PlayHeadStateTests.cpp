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

juce::Optional<juce::AudioPlayHead::PositionInfo> makePosition(
    double seconds, bool playing, bool looping = false)
{
    juce::AudioPlayHead::PositionInfo position;
    position.setTimeInSeconds(seconds);
    position.setIsPlaying(playing);
    position.setIsLooping(looping);
    return juce::Optional<juce::AudioPlayHead::PositionInfo>(position);
}

void testRepeatedObservationIsIdempotent()
{
    OpenTune::PlayHeadState state;
    const auto first = state.update(makePosition(3.0, false));
    expect(first.timeChanged, "first canonical time observation commits");
    expect(first.epoch == 1, "first time commit advances one generation");

    const auto repeated = state.update(makePosition(3.0, false));
    expect(!repeated.timeChanged, "repeated time observation is unchanged");
    expect(repeated.epoch == first.epoch, "repeated observation does not advance generation");
}

void testPlayStateChangeAdvancesGeneration()
{
    OpenTune::PlayHeadState state;
    const auto stopped = state.update(makePosition(3.0, false));
    const auto playing = state.update(makePosition(3.0, true));

    expect(!playing.timeChanged, "play-state change does not mark time changed");
    expect(playing.epoch == stopped.epoch + 1, "play-state change advances generation");
    expect(state.isPlaying.load(std::memory_order_relaxed), "playing state is published");

    const auto repeated = state.update(makePosition(3.0, true, true));
    expect(!repeated.timeChanged, "same time while playing remains unchanged");
    expect(repeated.epoch == playing.epoch, "loop observation does not advance generation");
}

void testJumpCommitsNewCanonicalTime()
{
    OpenTune::PlayHeadState state;
    const auto initial = state.update(makePosition(3.0, true));
    const auto jumped = state.update(makePosition(17.25, true));

    expect(jumped.timeChanged, "transport jump commits the new canonical time");
    expect(jumped.epoch == initial.epoch + 1, "transport jump advances generation once");
    expect(state.timeInSeconds.load(std::memory_order_relaxed) == 17.25,
           "transport jump stores the requested time");
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
    testRepeatedObservationIsIdempotent();
    testPlayStateChangeAdvancesGeneration();
    testJumpCommitsNewCanonicalTime();
    testConcurrentDuplicateObservationCommitsOnce();
    return failures == 0 ? 0 : 1;
}
