#include "RenderCache.h"
#include "Utils/AppLogger.h"
#include <algorithm>
#include <atomic>
#include <memory>

namespace OpenTune {

namespace {

double projectRenderSeconds(int64_t sample) {
    return TimeCoordinate::samplesToSeconds(sample, RenderCache::kSampleRate);
}

} // namespace

std::atomic<size_t>& RenderCache::globalCacheLimitBytes() {
    static std::atomic<size_t> value{kDefaultGlobalCacheLimitBytes};
    return value;
}

std::atomic<size_t>& RenderCache::globalCacheCurrentBytes() {
    static std::atomic<size_t> value{0};
    return value;
}

std::atomic<size_t>& RenderCache::globalCachePeakBytes() {
    static std::atomic<size_t> value{0};
    return value;
}

RenderCache::RenderCache() {
    std::shared_ptr<const PublishedRenderSnapshot> emptySnapshot = std::make_shared<PublishedRenderSnapshot>();
    std::atomic_store(&publishedSnapshot_, emptySnapshot);
}

RenderCache::~RenderCache() {
    clear();
}

void RenderCache::publishLocked() {
    // Caller MUST hold lock_.
    pruneRetiredSnapshotsLocked();

    auto snapshot = std::make_shared<PublishedRenderSnapshot>();
    snapshot->chunks.reserve(chunks_.size());
    for (const auto& [key, chunk] : chunks_) {
        juce::ignoreUnused(key);
        if (chunk.publishedRevision == 0 || chunk.audio == nullptr || chunk.audio->empty())
            continue;
        PublishedChunk pc;
        pc.startSample = chunk.startSample;
        pc.endSampleExclusive = chunk.endSampleExclusive;
        pc.startSeconds = chunk.startSeconds;
        pc.endSeconds = chunk.endSeconds;
        pc.publishedRevision = chunk.publishedRevision;
        pc.audio = chunk.audio;  // shared_ptr copy, refcount++
        snapshot->chunks.push_back(pc);
    }
    // chunks_ is keyed by project seconds derived from startSample, so map
    // iteration preserves startSample order for published chunks.
    auto oldSnapshot = std::atomic_load(&publishedSnapshot_);
    std::atomic_store(&publishedSnapshot_, std::shared_ptr<const PublishedRenderSnapshot>(std::move(snapshot)));
    if (oldSnapshot != nullptr) {
        retiredSnapshots_.push_back(std::move(oldSnapshot));
    }
    pruneRetiredSnapshotsLocked();
}

void RenderCache::pruneRetiredSnapshotsLocked() const {
    // Caller MUST hold lock_. If use_count()==1, only retiredSnapshots_ owns it,
    // so destruction happens here on the writer thread, never in processBlock.
    auto end = std::remove_if(retiredSnapshots_.begin(), retiredSnapshots_.end(),
        [](const std::shared_ptr<const PublishedRenderSnapshot>& snapshot) {
            return snapshot == nullptr || snapshot.use_count() == 1;
        });
    retiredSnapshots_.erase(end, retiredSnapshots_.end());
}

bool RenderCache::addChunk(int64_t startSample,
                           int64_t endSampleExclusive,
                           std::vector<float>&& audio,
                           uint64_t targetRevision) {
    if (audio.empty() || targetRevision == 0 || endSampleExclusive <= startSample) {
        AppLogger::log("RenderCache::addChunk REJECT early-check"
            " startSample=" + juce::String(startSample)
            + " endSampleExclusive=" + juce::String(endSampleExclusive)
            + " audioEmpty=" + juce::String(audio.empty() ? 1 : 0)
            + " targetRev=" + juce::String(static_cast<juce::int64>(targetRevision)));
        return false;
    }

    const int64_t expectedSamples = endSampleExclusive - startSample;
    if (expectedSamples != static_cast<int64_t>(audio.size())) {
        AppLogger::log("RenderCache::addChunk REJECT sample-span-mismatch"
            " startSample=" + juce::String(startSample)
            + " endSampleExclusive=" + juce::String(endSampleExclusive)
            + " expectedSamples=" + juce::String(expectedSamples)
            + " actualSamples=" + juce::String(static_cast<juce::int64>(audio.size()))
            + " targetRev=" + juce::String(static_cast<juce::int64>(targetRevision)));
        return false;
    }

    const double startSeconds = projectRenderSeconds(startSample);
    const double endSeconds = projectRenderSeconds(endSampleExclusive);

    auto immutableAudio = std::make_shared<const std::vector<float>>(std::move(audio));

    const juce::SpinLock::ScopedLockType guard(lock_);
    auto& chunk = chunks_[startSeconds];
    const bool hasStoredSampleSpan = chunk.endSampleExclusive > chunk.startSample;
    if (hasStoredSampleSpan
        && (chunk.startSample != startSample || chunk.endSampleExclusive != endSampleExclusive)) {
        AppLogger::log("RenderCache::addChunk REJECT sample-boundary-mismatch"
            " start=" + juce::String(startSeconds, 6)
            + " storedStartSample=" + juce::String(chunk.startSample)
            + " storedEndSampleExclusive=" + juce::String(chunk.endSampleExclusive)
            + " incomingStartSample=" + juce::String(startSample)
            + " incomingEndSampleExclusive=" + juce::String(endSampleExclusive));
        return false;
    }

    chunk.startSample = startSample;
    chunk.endSampleExclusive = endSampleExclusive;
    chunk.startSeconds = startSeconds;
    chunk.endSeconds = endSeconds;

    if (chunk.desiredRevision == 0) {
        chunk.desiredRevision = targetRevision;
    }

    if (targetRevision != chunk.desiredRevision) {
        AppLogger::log("RenderCache::addChunk REJECT revision-mismatch"
            " startSample=" + juce::String(startSample)
            + " endSampleExclusive=" + juce::String(endSampleExclusive)
            + " targetRev=" + juce::String(static_cast<juce::int64>(targetRevision))
            + " desiredRev=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision))
            + " publishedRev=" + juce::String(static_cast<juce::int64>(chunk.publishedRevision)));
        return false;
    }

    const size_t oldBytes = (chunk.audio ? chunk.audio->size() : 0) * sizeof(float);
    if (oldBytes > 0) {
        totalMemoryUsage_ -= oldBytes;
        globalCacheCurrentBytes().fetch_sub(oldBytes, std::memory_order_relaxed);
    }

    chunk.audio = immutableAudio;
    chunk.publishedRevision = targetRevision;

    const size_t chunkBytes = chunk.audio->size() * sizeof(float);
    totalMemoryUsage_ += chunkBytes;

    const size_t newCurrent = globalCacheCurrentBytes().fetch_add(chunkBytes, std::memory_order_relaxed) + chunkBytes;

    size_t peak = globalCachePeakBytes().load(std::memory_order_relaxed);
    while (newCurrent > peak && !globalCachePeakBytes().compare_exchange_weak(peak, newCurrent, std::memory_order_relaxed)) {}

    const size_t limit = globalCacheLimitBytes().load(std::memory_order_relaxed);
    // currentChunk 指向刚写入的 chunk，避免在驱逐时删除自身
    Chunk* currentChunk = &chunk;
    if (newCurrent > limit && !chunks_.empty()) {
        for (auto it = chunks_.begin(); it != chunks_.end(); ++it) {
            if (&it->second == currentChunk) {
                continue;
            }
            const size_t evictBytes = (it->second.audio ? it->second.audio->size() : 0) * sizeof(float);
            if (evictBytes == 0) {
                continue;
            }
            totalMemoryUsage_ -= evictBytes;
            globalCacheCurrentBytes().fetch_sub(evictBytes, std::memory_order_relaxed);
            it->second.audio.reset();
            it->second.publishedRevision = 0;
            break;
        }
    }

    publishLocked();
    return true;
}

void RenderCache::overlayPublishedAudioForRate(juce::AudioBuffer<float>& destination,
                                               int destStartSample,
                                               int numSamples,
                                               double timeSeconds,
                                               int targetSampleRate) const {
    if (targetSampleRate <= 0 || numSamples <= 0)
        return;

    const int destinationChannels = destination.getNumChannels();
    const int destinationSamples = destination.getNumSamples();
    if (destinationChannels <= 0 || destinationSamples <= 0)
        return;

    if (destStartSample < 0 || destStartSample >= destinationSamples)
        return;

    const int writableSamples = std::min(numSamples, destinationSamples - destStartSample);
    if (writableSamples <= 0)
        return;

    auto snapshot = std::atomic_load(&publishedSnapshot_);
    if (!snapshot || snapshot->chunks.empty())
        return;

    const double playbackSampleRate = static_cast<double>(targetSampleRate);
    const double requestEndSeconds = timeSeconds + static_cast<double>(writableSamples) / playbackSampleRate;
    const int64_t queryStartSample = TimeCoordinate::secondsToSamplesFloor(timeSeconds, kSampleRate);

    // Binary search for the first chunk with startSample > queryStartSample
    auto it = std::upper_bound(snapshot->chunks.begin(), snapshot->chunks.end(), queryStartSample,
        [](int64_t sample, const PublishedChunk& chunk) {
            return sample < chunk.startSample;
        });

    // If we're past the first chunk, step back to include chunks that start before queryStartSample
    if (it != snapshot->chunks.begin())
        --it;

    for (; it != snapshot->chunks.end(); ++it) {
        const auto& chunk = *it;
        const double chunkStartSeconds = chunk.startSeconds;
        const double chunkEndSeconds = chunk.endSeconds;

        if (chunkEndSeconds <= timeSeconds)
            continue;
        if (chunkStartSeconds >= requestEndSeconds)
            break;
        if (chunk.publishedRevision == 0)
            continue;
        if (chunk.audio == nullptr || chunk.audio->empty())
            continue;

        const int requestStartIndex = juce::jmax(
            0,
            static_cast<int>(std::floor(
                TimeCoordinate::secondsToSamplesExact(chunkStartSeconds - timeSeconds,
                                                      playbackSampleRate))));
        const int requestEndIndex = juce::jmin(
            writableSamples,
            static_cast<int>(std::ceil(
                TimeCoordinate::secondsToSamplesExact(chunkEndSeconds - timeSeconds,
                                                      playbackSampleRate))));
        if (requestEndIndex <= requestStartIndex)
            continue;

        const auto& audio = *chunk.audio;
        const float* source = audio.data();
        const int64_t sourceSize = static_cast<int64_t>(audio.size());
        if (source == nullptr || sourceSize <= 0)
            continue;

        for (int sample = requestStartIndex; sample < requestEndIndex; ++sample) {
            const double sampleTime = timeSeconds + static_cast<double>(sample) / playbackSampleRate;
            const double readPos = (sampleTime - chunkStartSeconds) * kSampleRate;
            if (readPos < 0.0 || readPos >= static_cast<double>(sourceSize))
                continue;

            const int64_t idx0 = static_cast<int64_t>(readPos);
            const int64_t idx1 = std::min<int64_t>(idx0 + 1, sourceSize - 1);
            const double fraction = readPos - static_cast<double>(idx0);
            const float value = static_cast<float>(source[idx0] + (source[idx1] - source[idx0]) * fraction);

            for (int channel = 0; channel < destinationChannels; ++channel)
                destination.setSample(channel, destStartSample + sample, value);
        }
    }
}

void RenderCache::clear() {
    const juce::SpinLock::ScopedLockType guard(lock_);
    for (const auto& [key, chunk] : chunks_) {
        juce::ignoreUnused(key);
        const size_t chunkBytes = (chunk.audio ? chunk.audio->size() : 0) * sizeof(float);
        globalCacheCurrentBytes().fetch_sub(chunkBytes, std::memory_order_relaxed);
    }
    chunks_.clear();
    totalMemoryUsage_ = 0;
    publishLocked();
}

// ---------------------------------------------------------------------------
// 调度状态管理实现
// ---------------------------------------------------------------------------

void RenderCache::requestRenderPending(double startSeconds,
                                       double endSeconds,
                                       int64_t startSample,
                                       int64_t endSampleExclusive) {
    juce::ignoreUnused(startSeconds, endSeconds);

    if (endSampleExclusive <= startSample) {
        return;
    }

    const double projectedStartSeconds = projectRenderSeconds(startSample);
    const double projectedEndSeconds = projectRenderSeconds(endSampleExclusive);

    const juce::SpinLock::ScopedLockType guard(lock_);
    auto& chunk = chunks_[projectedStartSeconds];
    chunk.startSeconds = projectedStartSeconds;
    chunk.endSeconds = projectedEndSeconds;
    chunk.startSample = startSample;
    chunk.endSampleExclusive = endSampleExclusive;
    ++chunk.desiredRevision;

    AppLogger::log("RenderCache::requestRenderPending"
        " start=" + juce::String(projectedStartSeconds, 3)
        + " end=" + juce::String(projectedEndSeconds, 3)
        + " startSample=" + juce::String(startSample)
        + " endSampleExclusive=" + juce::String(endSampleExclusive)
        + " desired=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision))
        + " oldStatus=" + juce::String(static_cast<int>(chunk.status)));

    if (chunk.status == Chunk::Status::Idle || chunk.status == Chunk::Status::Blank) {
        chunk.status = Chunk::Status::Pending;
        pendingChunks_.insert(projectedStartSeconds);
        AppLogger::log("RenderCache::requestRenderPending -> Pending");
    } else {
        AppLogger::log("RenderCache::requestRenderPending -> status unchanged (was not Idle/Blank)");
    }
}

bool RenderCache::getNextPendingJob(PendingJob& outJob) {
    const juce::SpinLock::ScopedLockType guard(lock_);
    if (pendingChunks_.empty()) {
        return false;
    }

    double startSec = *pendingChunks_.begin();
    pendingChunks_.erase(pendingChunks_.begin());

    auto it = chunks_.find(startSec);
    if (it == chunks_.end()) {
        return false;
    }

    auto& chunk = it->second;
    if (chunk.status != Chunk::Status::Pending) {
        return false;
    }

    chunk.status = Chunk::Status::Running;

    chunk.runningRevision = chunk.desiredRevision;

    outJob.startSeconds = chunk.startSeconds;
    outJob.endSeconds = chunk.endSeconds;
    outJob.startSample = chunk.startSample;
    outJob.endSampleExclusive = chunk.endSampleExclusive;
    outJob.targetRevision = chunk.desiredRevision;

    AppLogger::log("RenderCache::getNextPendingJob start=" + juce::String(startSec, 3)
        + " end=" + juce::String(chunk.endSeconds, 3)
        + " startSample=" + juce::String(chunk.startSample)
        + " endSampleExclusive=" + juce::String(chunk.endSampleExclusive)
        + " revision=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision)));

    return true;
}

bool RenderCache::completeChunkRender(double startSeconds, uint64_t revision, RenderCache::CompletionResult result) {
    const juce::SpinLock::ScopedLockType guard(lock_);
    auto it = chunks_.find(startSeconds);
    if (it == chunks_.end()) {
        AppLogger::log("RenderCache::completeChunkRender NOT_FOUND start=" + juce::String(startSeconds, 3));
        return false;
    }

    auto& chunk = it->second;

    if (chunk.runningRevision != revision) {
        AppLogger::log("RenderCache::completeChunkRender STALE runningRevision="
            + juce::String(static_cast<juce::int64>(chunk.runningRevision))
            + " != completionRev=" + juce::String(static_cast<juce::int64>(revision))
            + " -> ignore");
        return false;
    }

    const char* resultText = "unknown";
    switch (result) {
        case CompletionResult::Succeeded: resultText = "Succeeded"; break;
        case CompletionResult::TerminalFailure: resultText = "TerminalFailure"; break;
    }
    AppLogger::log("RenderCache::completeChunkRender start=" + juce::String(startSeconds, 3)
        + " revision=" + juce::String(static_cast<juce::int64>(revision))
        + " desired=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision))
        + " result=" + juce::String(resultText)
        + " oldStatus=" + juce::String(static_cast<int>(chunk.status)));

    if (revision != chunk.desiredRevision) {
        chunk.status = Chunk::Status::Pending;
        chunk.runningRevision = 0;
        pendingChunks_.insert(startSeconds);
        AppLogger::log("RenderCache::completeChunkRender STALE revision=" + juce::String(static_cast<juce::int64>(revision))
            + " < desired=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision))
            + " -> requeue Pending");
        return false;
    }

    if (result == CompletionResult::TerminalFailure) {
        chunk.status = Chunk::Status::Idle;
        chunk.runningRevision = 0;
        return false;
    } else {
        chunk.status = Chunk::Status::Idle;
        chunk.runningRevision = 0;
        chunk.publishedRevision = revision;
        AppLogger::log("RenderCache::completeChunkRender SUCCESS revision=" + juce::String(static_cast<juce::int64>(revision)));
        return true;
    }
}

int RenderCache::getPendingCount() const {
    const juce::SpinLock::ScopedLockType guard(lock_);
    pruneRetiredSnapshotsLocked();
    return static_cast<int>(pendingChunks_.size());
}

RenderCache::ChunkStats RenderCache::getChunkStats() const {
    ChunkStats stats;
    const juce::SpinLock::ScopedLockType guard(lock_);
    pruneRetiredSnapshotsLocked();
    for (const auto& [key, chunk] : chunks_) {
        juce::ignoreUnused(key);
        switch (chunk.status) {
            case Chunk::Status::Idle: ++stats.idle; break;
            case Chunk::Status::Pending: ++stats.pending; break;
            case Chunk::Status::Running: ++stats.running; break;
            case Chunk::Status::Blank: ++stats.blank; break;
        }
    }
    return stats;
}

RenderCache::StateSnapshot RenderCache::getStateSnapshot() const {
    StateSnapshot snapshot;
    const juce::SpinLock::ScopedLockType guard(lock_);
    pruneRetiredSnapshotsLocked();
    for (const auto& [key, chunk] : chunks_) {
        juce::ignoreUnused(key);

        snapshot.hasPublishedAudio = snapshot.hasPublishedAudio
            || (chunk.publishedRevision > 0 && chunk.audio != nullptr && !chunk.audio->empty());
        snapshot.hasNonBlankChunks = snapshot.hasNonBlankChunks
            || chunk.status != Chunk::Status::Blank;

        switch (chunk.status) {
            case Chunk::Status::Idle: ++snapshot.chunkStats.idle; break;
            case Chunk::Status::Pending: ++snapshot.chunkStats.pending; break;
            case Chunk::Status::Running: ++snapshot.chunkStats.running; break;
            case Chunk::Status::Blank: ++snapshot.chunkStats.blank; break;
        }
    }
    return snapshot;
}

void RenderCache::markChunkAsBlank(double startSeconds, uint64_t revision) {
    const juce::SpinLock::ScopedLockType guard(lock_);
    auto it = chunks_.find(startSeconds);
    if (it == chunks_.end()) {
        return;
    }

    auto& chunk = it->second;

    if (revision != chunk.desiredRevision) {
        chunk.status = Chunk::Status::Pending;
        pendingChunks_.insert(startSeconds);
        AppLogger::log("RenderCache::markChunkAsBlank STALE start=" + juce::String(startSeconds, 3)
            + " revision=" + juce::String(static_cast<juce::int64>(revision))
            + " desired=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision))
            + " -> requeue Pending");
        return;
    }
    
    // 只处理 Pending 或 Running 状态
    // Pending: 还在 pendingChunks_ 中，需要移除
    // Running: 已被调度器拾取，不在 pendingChunks_ 中
    if (chunk.status != Chunk::Status::Pending && chunk.status != Chunk::Status::Running) {
        return;
    }
    
    if (chunk.status == Chunk::Status::Pending) {
        pendingChunks_.erase(startSeconds);
    }
    chunk.status = Chunk::Status::Blank;
    chunk.runningRevision = 0;

    // Blank = 无有效渲染结果，清理旧 published audio 防止 stale overlay
    if (chunk.audio != nullptr && !chunk.audio->empty()) {
        const size_t evictBytes = (chunk.audio ? chunk.audio->size() : 0) * sizeof(float);
        totalMemoryUsage_ -= evictBytes;
        globalCacheCurrentBytes().fetch_sub(evictBytes, std::memory_order_relaxed);
        chunk.audio.reset();
    }
    chunk.publishedRevision = 0;
    publishLocked();

    AppLogger::log("RenderCache::markChunkAsBlank start=" + juce::String(startSeconds, 3)
        + " end=" + juce::String(chunk.endSeconds, 3)
        + " revision=" + juce::String(static_cast<juce::int64>(revision)));
}

} // namespace OpenTune
