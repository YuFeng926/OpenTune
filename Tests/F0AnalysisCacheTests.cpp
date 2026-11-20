#include "../Source/Services/F0AnalysisCache.h"

#include <juce_cryptography/juce_cryptography.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

using Cache = OpenTune::F0AnalysisCache;

int failures = 0;
int checks = 0;

void check(bool condition, const char* name)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", name);
    }
}

juce::File makeTestDirectory()
{
    return juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("OpenTuneF0AnalysisCacheTests-" + juce::Uuid().toString());
}

Cache::KeyConfig makeConfig()
{
    Cache::KeyConfig config;
    config.algorithmVersion = 3;
    config.resamplingAlgorithmVersion = 5;
    config.f0AlgorithmVersion = 7;
    config.energyAlgorithmVersion = 11;
    config.modelType = 1;
    config.modelWeightDigest.fill(0x31);
    config.confidence = 0.006f;
    config.f0Min = 32.7f;
    config.f0Max = 1975.5f;
    config.backend = "DirectML";
    config.sourceSampleRate = 44100.0;
    config.sourceChannelCount = 2;
    config.channelPolicyVersion = Cache::kChannelZeroPolicyVersion;
    config.windowSampleCount = 8;
    return config;
}

Cache::Record makeRecord(float first = 0.0f)
{
    Cache::Record record;
    record.hopSize = 160;
    record.f0SampleRate = 16000.0;
    record.originalF0 = {first, 220.0f, 0.0f, 440.0f};
    record.energy = {0.0f, 0.25f, 0.0f, 0.5f};
    return record;
}

std::vector<uint8_t> readBytes(const juce::File& file)
{
    juce::FileInputStream stream(file);
    if (!stream.openedOk())
        return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(stream.getTotalLength()));
    if (stream.read(bytes.data(), static_cast<int>(bytes.size())) != static_cast<int>(bytes.size()))
        return {};
    return bytes;
}

bool writeBytes(const juce::File& file, const std::vector<uint8_t>& bytes)
{
    file.deleteFile();
    juce::FileOutputStream stream(file);
    if (!stream.openedOk() || !stream.write(bytes.data(), bytes.size()))
        return false;
    stream.flush();
    return stream.getStatus().wasOk()
        && static_cast<size_t>(file.getSize()) == bytes.size();
}

void replaceChecksum(std::vector<uint8_t>& bytes)
{
    const juce::SHA256 hash(bytes.data(), bytes.size() - 32);
    const auto digest = hash.getRawData();
    std::memcpy(bytes.data() + bytes.size() - 32, digest.getData(), 32);
}

void testKeySensitivity()
{
    const std::array<float, 8> pcm{0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f};
    const auto config = makeConfig();
    const auto key = Cache::buildKey(config, pcm.data(), pcm.size());
    check(Cache::buildKey(config, Cache::digestPcm(pcm.data(), pcm.size())) == key,
          "precomputed PCM digest shares key");
    check(Cache::buildKey(config, pcm.data(), pcm.size()) == key, "identical PCM and config share key");

    auto changedPcm = pcm;
    changedPcm[3] += 0.01f;
    check(Cache::buildKey(config, changedPcm.data(), changedPcm.size()) != key, "single PCM sample changes key");

    const auto changed = [&config, &pcm, &key](auto mutate, const char* name) {
        auto variant = config;
        mutate(variant);
        check(Cache::buildKey(variant, pcm.data(), pcm.size()) != key, name);
    };
    changed([](auto& value) { value.schemaVersion++; }, "schema changes key");
    changed([](auto& value) { value.algorithmVersion++; }, "algorithm version changes key");
    changed([](auto& value) { value.resamplingAlgorithmVersion++; }, "resampling version changes key");
    changed([](auto& value) { value.f0AlgorithmVersion++; }, "FCPE version changes key");
    changed([](auto& value) { value.energyAlgorithmVersion++; }, "energy version changes key");
    changed([](auto& value) { value.sourceSampleRate = 48000.0; }, "source sample rate changes key");
    changed([](auto& value) { value.sourceChannelCount++; }, "source channel count changes key");
    changed([](auto& value) { value.channelPolicyVersion++; }, "channel policy changes key");
    changed([](auto& value) { value.windowSampleCount++; }, "window size changes key");
    changed([](auto& value) { value.modelWeightDigest[4]++; }, "model digest changes key");
    changed([](auto& value) { value.modelType++; }, "model type changes key");
    changed([](auto& value) { value.confidence += 0.001f; }, "confidence changes key");
    changed([](auto& value) { value.f0Min += 1.0f; }, "F0 minimum changes key");
    changed([](auto& value) { value.f0Max += 1.0f; }, "F0 maximum changes key");
    changed([](auto& value) { value.backend = "CPU"; }, "backend changes key");
}

void testRoundTripAndCorruption(const juce::File& directory)
{
    directory.createDirectory();
    const std::array<float, 8> pcm{0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    const auto config = makeConfig();
    const auto key = Cache::buildKey(config, pcm.data(), pcm.size());
    const auto record = makeRecord();
    check(Cache::publish(directory, key, record), "publish record");
    const auto loaded = Cache::tryLoad(directory, key);
    check(loaded.has_value(), "load record");
    check(loaded && loaded->hopSize == record.hopSize
          && loaded->f0SampleRate == record.f0SampleRate
          && loaded->originalF0 == record.originalF0
          && loaded->energy == record.energy,
          "record round trip including legal zero F0 frames");

    auto corruptCase = [&directory, &pcm, &config, &record](
        const char* name, const std::function<void(std::vector<uint8_t>&)>& mutate) {
        auto variant = config;
        variant.algorithmVersion += checks;
        const auto badKey = Cache::buildKey(variant, pcm.data(), pcm.size());
        check(Cache::publish(directory, badKey, record), "publish corruption-test seed");
        check(Cache::tryLoad(directory, badKey).has_value(), "valid corruption-test seed loads");
        auto bytes = readBytes(Cache::fileForKey(directory, badKey));
        const auto originalSize = bytes.size();
        mutate(bytes);
        check(writeBytes(Cache::fileForKey(directory, badKey), bytes), "write corrupted record");
        check(static_cast<size_t>(Cache::fileForKey(directory, badKey).getSize()) == bytes.size(),
              "corrupted record has expected physical length");
        if (std::string(name) != "truncated record rejected")
            check(bytes.size() == originalSize, "same-length corruption preserves file length");
        check(!Cache::tryLoad(directory, badKey).has_value(), name);
    };
    corruptCase("truncated record rejected", [](auto& bytes) {
        bytes.resize(bytes.size() - 1);
    });
    corruptCase("altered content rejected", [](auto& bytes) {
        bytes[56] ^= 0x01;
    });
    corruptCase("wrong schema rejected", [](auto& bytes) {
        bytes[4] = 2;
        replaceChecksum(bytes);
    });

    auto otherConfig = config;
    otherConfig.algorithmVersion += 10000;
    const auto otherKey = Cache::buildKey(otherConfig, pcm.data(), pcm.size());
    check(Cache::fileForKey(directory, key).copyFileTo(Cache::fileForKey(directory, otherKey)),
          "copy record under a different key");
    check(!Cache::tryLoad(directory, otherKey).has_value(), "wrong embedded key rejected");
}

void testFirstPublisherAndLru(const juce::File& directory)
{
    directory.createDirectory();
    const std::array<float, 8> pcm{1, 2, 3, 4, 5, 6, 7, 8};
    const auto config = makeConfig();
    const auto key = Cache::buildKey(config, pcm.data(), pcm.size());
    auto missingConfig = config;
    missingConfig.modelType += 100000;
    const auto missingKey = Cache::buildKey(missingConfig, pcm.data(), pcm.size());
    Cache::touch(directory, missingKey);
    check(!Cache::fileForKey(directory, missingKey).existsAsFile(),
          "touch does not recreate a missing cache record");
    const auto first = makeRecord(0.0f);
    const auto second = makeRecord(111.0f);
    check(Cache::publish(directory, key, first), "publish first result");
    check(Cache::publish(directory, key, second), "duplicate complete entry is accepted");
    const auto loaded = Cache::tryLoad(directory, key);
    check(loaded && loaded->originalF0 == first.originalF0, "first publisher is preserved");

    const auto fileA = Cache::fileForKey(directory, key);
    const auto timeA = juce::Time::fromISO8601("2000-01-01T00:00:00Z");
    fileA.setLastModificationTime(timeA);
    Cache::touch(directory, key);
    check(fileA.getLastModificationTime() > timeA, "touch advances file mtime");

    const auto strayTemp = directory.getChildFile("orphan.otf0.tmp.1.1.1");
    strayTemp.replaceWithText("temporary");
    strayTemp.setLastModificationTime(timeA);
    const auto activeTemp = directory.getChildFile("active.otf0.tmp.2.2.2");
    activeTemp.replaceWithText("in progress");
    auto keyBConfig = config;
    keyBConfig.modelType++;
    const auto keyB = Cache::buildKey(keyBConfig, pcm.data(), pcm.size());
    auto keyCConfig = config;
    keyCConfig.modelType += 2;
    const auto keyC = Cache::buildKey(keyCConfig, pcm.data(), pcm.size());
    Cache::publish(directory, keyB, first);
    Cache::publish(directory, keyC, first);

    const auto timeB = juce::Time::fromISO8601("2001-01-01T00:00:00Z");
    const auto timeC = juce::Time::fromISO8601("2002-01-01T00:00:00Z");
    Cache::fileForKey(directory, keyB).setLastModificationTime(timeB);
    Cache::fileForKey(directory, keyC).setLastModificationTime(timeC);
    const auto recordBytes = static_cast<uint64_t>(fileA.getSize());
    Cache::enforceCapacity(directory, recordBytes * 2);
    check(Cache::fileForKey(directory, key).existsAsFile(),
          "recently touched record retained by LRU");
    check(!Cache::fileForKey(directory, keyB).existsAsFile(), "least-recently-used record evicted");
    check(Cache::fileForKey(directory, keyC).existsAsFile(), "most recent record retained by LRU");
    check(!strayTemp.existsAsFile(), "capacity event removes stale temporary files");
    check(activeTemp.existsAsFile(), "capacity event preserves active temporary files");
}

#if !JUCE_WINDOWS
void testConcurrentPublish(const juce::File& directory)
{
    const std::array<float, 8> pcm{0.2f, -0.1f, 0.4f, -0.3f, 0.6f, -0.5f, 0.8f, -0.7f};
    const auto key = Cache::buildKey(makeConfig(), pcm.data(), pcm.size());
    const auto record = makeRecord();
    std::atomic<bool> start{false};
    bool results[2]{};
    const auto publish = [&](int index) {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        results[index] = Cache::publish(directory, key, record);
    };

    std::thread first(publish, 0);
    std::thread second(publish, 1);
    start.store(true, std::memory_order_release);
    first.join();
    second.join();

    check(results[0] || results[1], "concurrent same-key publish succeeds at least once");
    check(Cache::tryLoad(directory, key).has_value(), "concurrent publish leaves a complete record");
    int completeRecordCount = 0;
    bool hasTemporaryFile = false;
    for (const auto& file : directory.findChildFiles(juce::File::findFiles, false)) {
        if (file.getFileName().endsWith(".otf0"))
            ++completeRecordCount;
        if (file.getFileName().contains(".otf0.tmp"))
            hasTemporaryFile = true;
    }
    check(completeRecordCount == 1, "concurrent publish leaves exactly one complete record");
    check(!hasTemporaryFile, "concurrent publish leaves no temporary files");
}
#endif

} // namespace

int main()
{
    testKeySensitivity();
    const auto roundTripDirectory = makeTestDirectory();
    testRoundTripAndCorruption(roundTripDirectory);
    roundTripDirectory.deleteRecursively();

    const auto capacityDirectory = makeTestDirectory();
    testFirstPublisherAndLru(capacityDirectory);
    capacityDirectory.deleteRecursively();

#if !JUCE_WINDOWS
    const auto concurrentDirectory = makeTestDirectory();
    testConcurrentPublish(concurrentDirectory);
    concurrentDirectory.deleteRecursively();
#endif

    std::printf("F0AnalysisCache: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
