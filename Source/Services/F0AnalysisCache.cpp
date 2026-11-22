#include "F0AnalysisCache.h"
#include "../Utils/AppLogger.h"
#include "../Utils/AppPreferences.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include <juce_cryptography/juce_cryptography.h>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <fcntl.h>
 #include <sys/file.h>
 #include <unistd.h>
#endif

namespace OpenTune {
namespace {

// "OTF0" identifies OpenTune's original-F0 record format.
constexpr uint8_t kMagic[4] = {'O', 'T', 'F', '0'};
constexpr size_t kHeaderSize = 56;
constexpr size_t kDigestSize = 32;
constexpr uint64_t kSerializedFixedBytes = kHeaderSize + kDigestSize;
constexpr uint64_t kMaximumRecordBytes = F0AnalysisCache::kDefaultCapacityBytes;
constexpr char kUsage[] = "OpenTune ARA original F0";
constexpr char kExtension[] = ".otf0";
constexpr char kTemporaryExtension[] = ".otf0.tmp";
constexpr char kLockFileName[] = ".cache.lock";
constexpr char kHexDigits[] = "0123456789abcdef";

bool isTemporaryCacheFile(const juce::File& file)
{
    return file.getFileName().contains(kTemporaryExtension);
}

void appendUint32(std::vector<uint8_t>& bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void appendUint64(std::vector<uint8_t>& bytes, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i)
        bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void appendInt32(std::vector<uint8_t>& bytes, int32_t value)
{
    appendUint32(bytes, static_cast<uint32_t>(value));
}

void appendFloat(std::vector<uint8_t>& bytes, float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendUint32(bytes, bits);
}

void appendDouble(std::vector<uint8_t>& bytes, double value)
{
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendUint64(bytes, bits);
}

void appendString(std::vector<uint8_t>& bytes, const std::string& value)
{
    appendUint32(bytes, static_cast<uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

uint32_t readUint32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0])
        | (static_cast<uint32_t>(bytes[1]) << 8)
        | (static_cast<uint32_t>(bytes[2]) << 16)
        | (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t readUint64(const uint8_t* bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    return value;
}

float readFloat(const uint8_t* bytes)
{
    const uint32_t bits = readUint32(bytes);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double readDouble(const uint8_t* bytes)
{
    const uint64_t bits = readUint64(bytes);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

F0AnalysisCache::Digest digestOf(const void* bytes, size_t size)
{
    const juce::SHA256 hash(bytes, size);
    F0AnalysisCache::Digest digest{};
    const auto raw = hash.getRawData();
    std::memcpy(digest.data(), raw.getData(), digest.size());
    return digest;
}

bool validRecord(const F0AnalysisCache::Record& record)
{
    if (record.hopSize <= 0 || !std::isfinite(record.f0SampleRate)
        || record.f0SampleRate < 1.0
        || record.f0SampleRate > static_cast<double>(std::numeric_limits<int>::max())
        || record.originalF0.empty() || record.originalF0.size() != record.energy.size()
        || record.originalF0.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        return false;

    return std::all_of(record.originalF0.begin(), record.originalF0.end(), [](float value) {
               return std::isfinite(value);
           })
        && std::all_of(record.energy.begin(), record.energy.end(), [](float value) {
               return std::isfinite(value);
           });
}

juce::String keyPrefix(const F0AnalysisCache::Digest& key)
{
    return juce::String(F0AnalysisCache::keyToHex(key).substr(0, 16));
}

void logCorrupt(const F0AnalysisCache::Digest& key)
{
    AppLogger::log("ARA-F0 cache_corrupt key=" + keyPrefix(key));
}

std::vector<uint8_t> serialize(const F0AnalysisCache::Digest& key,
                               const F0AnalysisCache::Record& record)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(kHeaderSize + record.originalF0.size() * sizeof(float) * 2 + kDigestSize);
    bytes.insert(bytes.end(), std::begin(kMagic), std::end(kMagic));
    appendInt32(bytes, F0AnalysisCache::kSchemaVersion);
    bytes.insert(bytes.end(), key.begin(), key.end());
    appendInt32(bytes, record.hopSize);
    appendDouble(bytes, record.f0SampleRate);
    appendInt32(bytes, static_cast<int32_t>(record.originalF0.size()));
    for (const auto value : record.originalF0)
        appendFloat(bytes, value);
    for (const auto value : record.energy)
        appendFloat(bytes, value);
    const auto checksum = digestOf(bytes.data(), bytes.size());
    bytes.insert(bytes.end(), checksum.begin(), checksum.end());
    return bytes;
}

void removeAbandonedTemporariesLocked(const juce::File& directory,
                                      const juce::File& publicationTemporary = {})
{
    // The directory lock covers every writer from temp creation through rename.
    const auto children = directory.findChildFiles(juce::File::findFiles, false,
        juce::String("*") + kTemporaryExtension + "*");
    for (const auto& file : children)
        if (file != publicationTemporary && isTemporaryCacheFile(file))
            file.deleteFile();
}

void enforceCapacityLocked(const juce::File& directory, uint64_t maxBytes)
{
    struct Entry
    {
        juce::File file;
        uint64_t size;
        juce::Time modified;
    };

    const auto children = directory.findChildFiles(juce::File::findFiles, false,
        juce::String("*") + kExtension);
    const auto isRecord = [](const juce::File& file) {
        return file.getFileName().endsWith(kExtension) && !isTemporaryCacheFile(file);
    };
    uint64_t totalBytes = 0;
    for (const auto& file : children)
        if (isRecord(file))
            totalBytes += static_cast<uint64_t>(file.getSize());

    if (totalBytes <= maxBytes)
        return;

    std::vector<Entry> records;
    for (const auto& file : children)
        if (isRecord(file))
            records.push_back({file, static_cast<uint64_t>(file.getSize()), file.getLastModificationTime()});

    std::sort(records.begin(), records.end(), [](const Entry& left, const Entry& right) {
        return left.modified < right.modified;
    });
    for (const auto& record : records)
    {
        if (totalBytes <= maxBytes)
            break;
        if (record.file.deleteFile())
            totalBytes -= record.size;
    }
}

class ScopedDirectoryLock
{
public:
    explicit ScopedDirectoryLock(const juce::File& directory)
#if JUCE_WINDOWS
        : lock_(directory.getChildFile(kLockFileName).getFullPathName())
#endif
    {
#if JUCE_WINDOWS
        locked_ = lock_.enter(0);
#else
        const auto path = directory.getChildFile(kLockFileName).getFullPathName().toStdString();
        descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
        if (descriptor_ >= 0 && ::flock(descriptor_, LOCK_EX | LOCK_NB) == 0)
            locked_ = true;
        else if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
#endif
    }

    ~ScopedDirectoryLock()
    {
#if JUCE_WINDOWS
        if (locked_)
            lock_.exit();
#else
        if (descriptor_ >= 0) {
            if (locked_)
                ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
#endif
    }

    bool isLocked() const noexcept { return locked_; }

private:
#if JUCE_WINDOWS
    juce::InterProcessLock lock_;
#else
    int descriptor_{-1};
#endif
    bool locked_{false};
};

} // namespace

F0AnalysisCache::Digest F0AnalysisCache::digestPcm(const float* pcm, size_t sampleCount)
{
    return digestOf(pcm, sampleCount * sizeof(float));
}

F0AnalysisCache::Digest F0AnalysisCache::buildKey(const KeyConfig& config,
                                                   const float* pcm,
                                                   size_t sampleCount)
{
    return buildKey(config, digestPcm(pcm, sampleCount));
}

F0AnalysisCache::Digest F0AnalysisCache::buildKey(const KeyConfig& config,
                                                   const Digest& pcmDigest)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(160 + config.backend.size());
    appendString(bytes, kUsage);
    appendInt32(bytes, config.schemaVersion);
    appendInt32(bytes, config.algorithmVersion);
    appendInt32(bytes, config.resamplingAlgorithmVersion);
    appendInt32(bytes, config.f0AlgorithmVersion);
    appendInt32(bytes, config.energyAlgorithmVersion);
    appendInt32(bytes, config.modelType);
    bytes.insert(bytes.end(), config.modelWeightDigest.begin(), config.modelWeightDigest.end());
    appendFloat(bytes, config.confidence);
    appendFloat(bytes, config.f0Min);
    appendFloat(bytes, config.f0Max);
    appendString(bytes, config.backend);
    appendDouble(bytes, config.sourceSampleRate);
    appendInt32(bytes, config.sourceChannelCount);
    appendInt32(bytes, config.channelPolicyVersion);
    appendUint64(bytes, config.windowSampleCount);
    bytes.insert(bytes.end(), pcmDigest.begin(), pcmDigest.end());
    return digestOf(bytes.data(), bytes.size());
}

std::string F0AnalysisCache::keyToHex(const Digest& key)
{
    std::string text;
    text.reserve(key.size() * 2);
    for (const auto byte : key)
    {
        text.push_back(kHexDigits[byte >> 4]);
        text.push_back(kHexDigits[byte & 0x0f]);
    }
    return text;
}

juce::File F0AnalysisCache::cacheDirectory()
{
    return AppPreferences::getStorageDirectory()
        .getChildFile("Cache")
        .getChildFile("ARA-F0");
}

juce::File F0AnalysisCache::fileForKey(const juce::File& directory, const Digest& key)
{
    return directory.getChildFile(keyToHex(key) + kExtension);
}

std::optional<F0AnalysisCache::Record> F0AnalysisCache::tryLoad(const juce::File& directory,
                                                                 const Digest& key)
{
    const auto file = fileForKey(directory, key);
    if (!file.existsAsFile())
        return std::nullopt;

    const int64_t fileSize = file.getSize();
    if (fileSize < static_cast<int64_t>(kHeaderSize + kDigestSize)
        || static_cast<uint64_t>(fileSize) > kMaximumRecordBytes)
    {
        logCorrupt(key);
        return std::nullopt;
    }

    juce::FileInputStream stream(file);
    if (!stream.openedOk())
    {
        logCorrupt(key);
        return std::nullopt;
    }

    uint8_t header[kHeaderSize]{};
    if (stream.read(header, static_cast<int>(sizeof(header))) != static_cast<int>(sizeof(header)))
    {
        logCorrupt(key);
        return std::nullopt;
    }

    const int32_t frameCount = static_cast<int32_t>(readUint32(header + 52));
    if (frameCount <= 0)
    {
        logCorrupt(key);
        return std::nullopt;
    }
    const uint64_t expectedSize = kHeaderSize
        + static_cast<uint64_t>(frameCount) * sizeof(float) * 2
        + kDigestSize;
    if (expectedSize != static_cast<uint64_t>(fileSize))
    {
        logCorrupt(key);
        return std::nullopt;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
    std::memcpy(bytes.data(), header, sizeof(header));
    const auto remaining = bytes.size() - sizeof(header);
    if (stream.read(bytes.data() + sizeof(header), static_cast<int>(remaining)) != static_cast<int>(remaining))
    {
        logCorrupt(key);
        return std::nullopt;
    }

    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0
        || static_cast<int32_t>(readUint32(bytes.data() + 4)) != kSchemaVersion
        || !std::equal(key.begin(), key.end(), bytes.begin() + 8))
    {
        logCorrupt(key);
        return std::nullopt;
    }

    const auto payloadSize = bytes.size() - kDigestSize;
    const auto checksum = digestOf(bytes.data(), payloadSize);
    if (!std::equal(checksum.begin(), checksum.end(), bytes.begin() + static_cast<std::ptrdiff_t>(payloadSize)))
    {
        logCorrupt(key);
        return std::nullopt;
    }

    Record record;
    record.hopSize = static_cast<int32_t>(readUint32(bytes.data() + 40));
    record.f0SampleRate = readDouble(bytes.data() + 44);
    if (record.hopSize <= 0 || !std::isfinite(record.f0SampleRate)
        || record.f0SampleRate < 1.0
        || record.f0SampleRate > static_cast<double>(std::numeric_limits<int>::max()))
    {
        logCorrupt(key);
        return std::nullopt;
    }

    record.originalF0.resize(static_cast<size_t>(frameCount));
    record.energy.resize(static_cast<size_t>(frameCount));
    size_t offset = kHeaderSize;
    for (auto& value : record.originalF0)
    {
        value = readFloat(bytes.data() + offset);
        offset += sizeof(float);
    }
    for (auto& value : record.energy)
    {
        value = readFloat(bytes.data() + offset);
        offset += sizeof(float);
    }
    if (!validRecord(record))
    {
        logCorrupt(key);
        return std::nullopt;
    }
    return record;
}

bool F0AnalysisCache::publish(const juce::File& directory,
                              const Digest& key,
                              const Record& record)
{
    if (!validRecord(record))
    {
        AppLogger::log("ARA-F0 cache_publish_failed key=" + keyPrefix(key) + " reason=invalid_record");
        return false;
    }
    const auto serializedBytes = kSerializedFixedBytes
        + static_cast<uint64_t>(record.originalF0.size()) * sizeof(float) * 2;
    if (serializedBytes > kMaximumRecordBytes)
    {
        AppLogger::log("ARA-F0 cache_publish_failed key=" + keyPrefix(key) + " reason=oversized");
        return false;
    }

    directory.createDirectory();
    ScopedDirectoryLock scopedLock(directory);
    if (!scopedLock.isLocked())
    {
        AppLogger::log("ARA-F0 cache_publish_failed key=" + keyPrefix(key) + " reason=lock");
        return false;
    }

    if (tryLoad(directory, key).has_value())
    {
        AppLogger::log("ARA-F0 cache_publish key=" + keyPrefix(key) + " preserved=1");
        removeAbandonedTemporariesLocked(directory);
        enforceCapacityLocked(directory, kDefaultCapacityBytes);
        return true;
    }

    const auto bytes = serialize(key, record);
    const auto destination = fileForKey(directory, key);
    const auto temporary = directory.getNonexistentChildFile(keyToHex(key), kTemporaryExtension, false);
    bool written = false;
    {
        juce::FileOutputStream stream(temporary);
        if (stream.openedOk())
        {
            written = stream.write(bytes.data(), bytes.size());
            stream.flush();
            written = written && stream.getStatus().wasOk();
        }
    }
    if (!written)
    {
        temporary.deleteFile();
        AppLogger::log("ARA-F0 cache_publish_failed key=" + keyPrefix(key) + " reason=write");
        return false;
    }

    removeAbandonedTemporariesLocked(directory, temporary);

    bool moved = false;
#if JUCE_WINDOWS
    const auto temporaryPath = temporary.getFullPathName();
    const auto destinationPath = destination.getFullPathName();
    moved = MoveFileExW(temporaryPath.toWideCharPointer(), destinationPath.toWideCharPointer(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary.getFullPathName().toStdString(),
                            destination.getFullPathName().toStdString(), error);
    moved = !error;
#endif
    if (!moved)
    {
        temporary.deleteFile();
        AppLogger::log("ARA-F0 cache_publish_failed key=" + keyPrefix(key) + " reason=rename");
        return false;
    }

    enforceCapacityLocked(directory, kDefaultCapacityBytes);
    AppLogger::log("ARA-F0 cache_publish key=" + keyPrefix(key) + " frames="
        + juce::String(static_cast<int>(record.originalF0.size())));
    return true;
}

void F0AnalysisCache::touch(const juce::File& directory, const Digest& key)
{
    const auto file = fileForKey(directory, key);
#if JUCE_WINDOWS
    const auto path = file.getFullPathName();
    HANDLE handle = CreateFileW(path.toWideCharPointer(), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    FILETIME currentTime{};
    GetSystemTimeAsFileTime(&currentTime);
    SetFileTime(handle, nullptr, nullptr, &currentTime);
    CloseHandle(handle);
#else
    if (file.existsAsFile())
        file.setLastModificationTime(juce::Time::getCurrentTime());
#endif
}

void F0AnalysisCache::enforceCapacity(const juce::File& directory, uint64_t maxBytes)
{
    directory.createDirectory();
    ScopedDirectoryLock scopedLock(directory);
    if (!scopedLock.isLocked())
        return;
    removeAbandonedTemporariesLocked(directory);
    enforceCapacityLocked(directory, maxBytes);
}

} // namespace OpenTune
