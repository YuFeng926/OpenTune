#include "SourceStore.h"

namespace OpenTune {

SourceStore::SourceStore() = default;
SourceStore::~SourceStore() = default;

uint64_t SourceStore::createSource(CreateSourceRequest request, uint64_t forcedSourceId)
{
    if (forcedSourceId != 0) {
        const juce::ScopedWriteLock writeLock(lock_);
        const auto existing = sources_.find(forcedSourceId);
        if (existing != sources_.end()) {
            return forcedSourceId;
        }
    }

    if (request.audioBuffer == nullptr
        || request.audioBuffer->getNumChannels() <= 0
        || request.audioBuffer->getNumSamples() <= 0) {
        return 0;
    }

    SourceEntry source;
    source.sourceId = forcedSourceId != 0 ? forcedSourceId : nextSourceId_.fetch_add(1, std::memory_order_relaxed);
    source.displayName = std::move(request.displayName);
    source.audioBuffer = std::move(request.audioBuffer);
    source.sampleRate = request.sampleRate;
    source.numChannels = source.audioBuffer->getNumChannels();
    source.numSamples = source.audioBuffer->getNumSamples();
    const uint64_t sourceId = source.sourceId;

    const juce::ScopedWriteLock writeLock(lock_);
    if (forcedSourceId != 0) {
        nextSourceId_.store(juce::jmax(nextSourceId_.load(std::memory_order_relaxed), forcedSourceId + 1),
                            std::memory_order_relaxed);
    }
    sources_.emplace(sourceId, std::move(source));
    return sourceId;
}

void SourceStore::clear()
{
    const juce::ScopedWriteLock writeLock(lock_);
    sources_.clear();
    nextSourceId_.store(1, std::memory_order_relaxed);
}

bool SourceStore::deleteSource(uint64_t sourceId)
{
    if (sourceId == 0) {
        return false;
    }

    const juce::ScopedWriteLock writeLock(lock_);
    return sources_.erase(sourceId) > 0;
}

bool SourceStore::containsSource(uint64_t sourceId) const
{
    if (sourceId == 0) {
        return false;
    }

    const juce::ScopedReadLock readLock(lock_);
    const auto it = sources_.find(sourceId);
    return it != sources_.end() && !it->second.isRetired_;
}

bool SourceStore::retireSource(uint64_t id)
{
    if (id == 0) return false;
    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = sources_.find(id);
    if (it == sources_.end() || it->second.isRetired_) return false;
    it->second.isRetired_ = true;
    return true;
}

bool SourceStore::reviveSource(uint64_t id)
{
    if (id == 0) return false;
    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = sources_.find(id);
    if (it == sources_.end() || !it->second.isRetired_) return false;
    it->second.isRetired_ = false;
    return true;
}

bool SourceStore::isRetired(uint64_t id) const
{
    if (id == 0) return false;
    const juce::ScopedReadLock readLock(lock_);
    const auto it = sources_.find(id);
    return it != sources_.end() && it->second.isRetired_;
}

bool SourceStore::physicallyDeleteIfReclaimable(uint64_t id)
{
    if (id == 0) return false;
    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = sources_.find(id);
    if (it == sources_.end() || !it->second.isRetired_) return false;
    sources_.erase(it);
    return true;
}

std::vector<uint64_t> SourceStore::getRetiredSourceIds() const
{
    std::vector<uint64_t> ids;
    const juce::ScopedReadLock readLock(lock_);
    ids.reserve(sources_.size());
    for (const auto& kv : sources_) {
        if (kv.second.isRetired_) {
            ids.push_back(kv.first);
        }
    }
    return ids;
}

bool SourceStore::getSnapshot(uint64_t sourceId, SourceSnapshot& out) const
{
    out = SourceSnapshot{};
    if (sourceId == 0) {
        return false;
    }

    const juce::ScopedReadLock readLock(lock_);
    const auto it = sources_.find(sourceId);
    if (it == sources_.end()) {
        return false;
    }

    out.sourceId = it->second.sourceId;
    out.displayName = it->second.displayName;
    out.audioBuffer = it->second.audioBuffer;
    out.sampleRate = it->second.sampleRate;
    out.numChannels = it->second.numChannels;
    out.numSamples = it->second.numSamples;
    return true;
}

bool SourceStore::getAudioBuffer(uint64_t sourceId, std::shared_ptr<const juce::AudioBuffer<float>>& out) const
{
    out.reset();
    if (sourceId == 0) {
        return false;
    }

    const juce::ScopedReadLock readLock(lock_);
    const auto it = sources_.find(sourceId);
    if (it == sources_.end()) {
        return false;
    }

    out = it->second.audioBuffer;
    return out != nullptr;
}

} // namespace OpenTune
