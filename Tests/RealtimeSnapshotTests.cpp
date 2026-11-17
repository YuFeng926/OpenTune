#include "../Source/Utils/RealtimeSnapshot.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

namespace {

bool testRetirementCapacityAndPendingCoalesce()
{
    OpenTune::RealtimeSnapshot<int, 2> snapshot;
    snapshot.publish(std::make_shared<const int>(0));
    auto held = snapshot.read();

    for (int value = 1; value <= 8; ++value) {
        snapshot.publish(std::make_shared<const int>(value));
        if (snapshot.retiredCount() > 2)
            return false;
    }

    if (!held || *held != 0 || snapshot.retiredCount() != 2
        || !snapshot.hasReaders() || !snapshot.hasPending()
        || snapshot.latestOwner() == nullptr || *snapshot.latestOwner() != 8)
        return false;

    held = {};
    if (!snapshot.collect() || snapshot.hasPending() || snapshot.retiredCount() != 0)
        return false;

    auto current = snapshot.read();
    return current && *current == 8;
}

struct DestructionRecord
{
    explicit DestructionRecord(int value, std::array<std::thread::id, 4>& threads)
        : id(value), destroyedOn(threads)
    {
    }

    ~DestructionRecord() { destroyedOn[static_cast<std::size_t>(id)] = std::this_thread::get_id(); }

    int id;
    std::array<std::thread::id, 4>& destroyedOn;
};

bool testDestructionRunsOnControlThread()
{
    std::array<std::thread::id, 4> destroyedOn {};
    const auto controlThread = std::this_thread::get_id();
    OpenTune::RealtimeSnapshot<DestructionRecord, 1> snapshot;
    snapshot.publish(std::make_shared<DestructionRecord>(0, destroyedOn));

    std::mutex gateMutex;
    std::condition_variable gate;
    bool readerReady = false;
    bool readerSawInitial = false;
    bool releaseReader = false;
    std::thread reader([&] {
        auto held = snapshot.read();
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            readerSawInitial = held && held->id == 0;
            readerReady = true;
        }
        gate.notify_one();

        std::unique_lock<std::mutex> lock(gateMutex);
        gate.wait(lock, [&] { return releaseReader; });
    });
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        gate.wait(lock, [&] { return readerReady; });
    }

    snapshot.publish(std::make_shared<DestructionRecord>(1, destroyedOn));
    snapshot.publish(std::make_shared<DestructionRecord>(2, destroyedOn));
    snapshot.publish(std::make_shared<DestructionRecord>(3, destroyedOn));
    const bool pendingDestroyedOnControl = destroyedOn[2] == controlThread;

    {
        std::lock_guard<std::mutex> lock(gateMutex);
        releaseReader = true;
    }
    gate.notify_one();
    reader.join();

    const bool collected = snapshot.collect();
    if (!collected)
        return false;
    snapshot.clear();

    return readerSawInitial
        && pendingDestroyedOnControl
        && destroyedOn[0] == controlThread
        && destroyedOn[1] == controlThread
        && destroyedOn[2] == controlThread
        && destroyedOn[3] == controlThread;
}

bool testReaderExitCollectPublishesLatest()
{
    OpenTune::RealtimeSnapshot<int, 1> snapshot;
    snapshot.publish(std::make_shared<const int>(10));
    auto held = snapshot.read();

    snapshot.publish(std::make_shared<const int>(20));
    snapshot.publish(std::make_shared<const int>(30));
    if (!held || *held != 10 || !snapshot.hasPending())
        return false;

    held = {};
    if (!snapshot.collect() || snapshot.hasPending())
        return false;

    auto current = snapshot.read();
    return current && *current == 30 && *snapshot.latestOwner() == 30;
}

bool testClearRetainsVersionForActiveReader()
{
    OpenTune::RealtimeSnapshot<int> snapshot;
    snapshot.publish(std::make_shared<const int>(42));
    auto held = snapshot.read();

    snapshot.clear();
    if (!held || *held != 42 || snapshot.latestOwner() != nullptr
        || snapshot.retiredCount() != 1)
        return false;

    held = {};
    if (!snapshot.collect())
        return false;
    auto empty = snapshot.read();
    return !empty && snapshot.latestOwner() == nullptr;
}

bool testConcurrentReadersAndContinuousPublication()
{
    struct Payload
    {
        Payload(uint64_t value, std::atomic<bool>& wrongThread)
            : controlThread(std::this_thread::get_id()), destroyedOffControl(wrongThread)
        {
            samples.fill(value);
        }

        ~Payload()
        {
            if (std::this_thread::get_id() != controlThread)
                destroyedOffControl.store(true, std::memory_order_relaxed);
        }

        std::array<uint64_t, 64> samples{};
        std::thread::id controlThread;
        std::atomic<bool>& destroyedOffControl;
    };

    std::atomic<bool> destroyedOffControl{false};
    OpenTune::RealtimeSnapshot<Payload> snapshot;
    snapshot.publish(std::make_shared<Payload>(0, destroyedOffControl));
    std::atomic<int> ready{0};
    std::atomic<bool> finished{false};
    std::array<bool, 2> intact{true, true};
    std::array<std::thread, 2> readers;
    for (size_t index = 0; index < readers.size(); ++index)
        readers[index] = std::thread([&, index] {
            ready.fetch_add(1, std::memory_order_release);
            do
            {
                const auto borrowed = snapshot.read();
                if (!borrowed)
                    intact[index] = false;
                else
                    for (const auto value : borrowed->samples)
                        intact[index] = intact[index] && value == borrowed->samples.front();
            }
            while (!finished.load(std::memory_order_acquire));
        });

    while (ready.load(std::memory_order_acquire) != static_cast<int>(readers.size()))
        std::this_thread::yield();
    bool bounded = true;
    constexpr uint64_t finalValue = 8192;
    for (uint64_t value = 1; value <= finalValue; ++value)
    {
        snapshot.publish(std::make_shared<Payload>(value, destroyedOffControl));
        snapshot.collect();
        bounded = bounded && snapshot.retiredCount() <= 4;
    }
    finished.store(true, std::memory_order_release);
    for (auto& reader : readers)
        reader.join();

    const bool collected = snapshot.collect();
    const bool latest = snapshot.latestOwner()->samples.front() == finalValue;
    snapshot.clear();
    return collected && bounded && latest && intact[0] && intact[1]
        && !destroyedOffControl.load(std::memory_order_relaxed);
}

} // namespace

int main()
{
    if (!testRetirementCapacityAndPendingCoalesce()) {
        std::fputs("FAIL: bounded retirement and pending coalesce\n", stderr);
        return 1;
    }
    if (!testDestructionRunsOnControlThread()) {
        std::fputs("FAIL: retired owners are destroyed on the control thread\n", stderr);
        return 1;
    }
    if (!testReaderExitCollectPublishesLatest()) {
        std::fputs("FAIL: collect publishes the latest pending state after readers exit\n", stderr);
        return 1;
    }
    if (!testClearRetainsVersionForActiveReader()) {
        std::fputs("FAIL: clear retains the old version for active readers\n", stderr);
        return 1;
    }
    if (!testConcurrentReadersAndContinuousPublication()) {
        std::fputs("FAIL: continuous publication preserves borrowed data and control-thread reclamation\n", stderr);
        return 1;
    }
    std::puts("All RealtimeSnapshot tests passed.");
    return 0;
}
