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
    } else if (chunk.status == Chunk::Status::Running) {
        // 取消当前运行中的渲染：清零 runningRevision 使 stale completion 被忽略，
        // 重新入 Pending 让 Worker 用最新 desiredRevision 重新调度。
        chunk.runningRevision = 0;
        chunk.status = Chunk::Status::Pending;
        pendingChunks_.insert(projectedStartSeconds);
        AppLogger::log("RenderCache::requestRenderPending -> cancel Running, requeue Pending"
            " newDesired=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision)));
    } else {
        AppLogger::log("RenderCache::requestRenderPending -> status unchanged (Pending, already queued)");
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

RenderCache::ChunkRenderResult RenderCache::completeChunkRenderWithAudio(int64_t startSample,
                                                                           int64_t endSampleExclusive,
                                                                           std::vector<float>&& audio,
                                                                           uint64_t revision) {
    // 输入验证：调用方应该保证这些条件。违反属于 bug，jassert 捕获。
    // 返回 InvalidInput 让调用方走 failure 收口，而不是伪装成 Stale。
    if (audio.empty() || revision == 0 || endSampleExclusive <= startSample) {
        jassert(audio.empty() == false && revision != 0 && endSampleExclusive > startSample);
        AppLogger::log("RenderCache::completeChunkRenderWithAudio INVALID_INPUT"
            " startSample=" + juce::String(startSample)
            + " audioEmpty=" + juce::String(audio.empty() ? 1 : 0)
            + " revision=" + juce::String(static_cast<juce::int64>(revision)));
        return ChunkRenderResult::InvalidInput;
    }

    const int64_t expectedSamples = endSampleExclusive - startSample;
    if (expectedSamples != static_cast<int64_t>(audio.size())) {
        jassert(expectedSamples == static_cast<int64_t>(audio.size()));
        AppLogger::log("RenderCache::completeChunkRenderWithAudio SAMPLE_SPAN_MISMATCH"
            " expected=" + juce::String(expectedSamples)
            + " actual=" + juce::String(static_cast<int64_t>(audio.size())));
        return ChunkRenderResult::InvalidInput;
    }

    const double startSeconds = projectRenderSeconds(startSample);
    auto immutableAudio = std::make_shared<const std::vector<float>>(std::move(audio));

    const juce::SpinLock::ScopedLockType guard(lock_);
    auto it = chunks_.find(startSeconds);
    if (it == chunks_.end()) {
        // Chunk 不存在：调用方 bug（完成了一个从未提交的 chunk）
        jassertfalse;
        AppLogger::log("RenderCache::completeChunkRenderWithAudio NOT_FOUND start=" + juce::String(startSeconds, 3));
        return ChunkRenderResult::InvalidInput;
    }

    auto& chunk = it->second;

    // 唯一 stale 检测：runningRevision 是 completion token
    if (chunk.runningRevision != revision) {
        AppLogger::log("RenderCache::completeChunkRenderWithAudio STALE runningRevision="
            + juce::String(static_cast<juce::int64>(chunk.runningRevision))
            + " != completionRev=" + juce::String(static_cast<juce::int64>(revision)));
        return ChunkRenderResult::Stale;
    }

    // 释放旧 audio
    const size_t oldBytes = (chunk.audio ? chunk.audio->size() : 0) * sizeof(float);
    if (oldBytes > 0) {
        totalMemoryUsage_ -= oldBytes;
        globalCacheCurrentBytes().fetch_sub(oldBytes, std::memory_order_relaxed);
    }

    // 写入新 audio
    chunk.audio = immutableAudio;
    chunk.publishedRevision = revision;
    chunk.status = Chunk::Status::Idle;
    chunk.runningRevision = 0;

    const size_t chunkBytes = chunk.audio->size() * sizeof(float);
    totalMemoryUsage_ += chunkBytes;

    const size_t newCurrent = globalCacheCurrentBytes().fetch_add(chunkBytes, std::memory_order_relaxed) + chunkBytes;

    size_t peak = globalCachePeakBytes().load(std::memory_order_relaxed);
    while (newCurrent > peak && !globalCachePeakBytes().compare_exchange_weak(peak, newCurrent, std::memory_order_relaxed)) {}

    // LRU 驱逐
    const size_t limit = globalCacheLimitBytes().load(std::memory_order_relaxed);
    Chunk* currentChunk = &chunk;
    if (newCurrent > limit && !chunks_.empty()) {
        for (auto evictIt = chunks_.begin(); evictIt != chunks_.end(); ++evictIt) {
            if (&evictIt->second == currentChunk) continue;
            const size_t evictBytes = (evictIt->second.audio ? evictIt->second.audio->size() : 0) * sizeof(float);
            if (evictBytes == 0) continue;
            totalMemoryUsage_ -= evictBytes;
            globalCacheCurrentBytes().fetch_sub(evictBytes, std::memory_order_relaxed);
            evictIt->second.audio.reset();
            evictIt->second.publishedRevision = 0;
            break;
        }
    }

    publishLocked();

    AppLogger::log("RenderCache::completeChunkRenderWithAudio PUBLISHED start=" + juce::String(startSeconds, 3)
        + " revision=" + juce::String(static_cast<juce::int64>(revision)));
    return ChunkRenderResult::Published;
}

void RenderCache::completeChunkRenderFailure(double startSeconds, uint64_t revision) {
    const juce::SpinLock::ScopedLockType guard(lock_);
    auto it = chunks_.find(startSeconds);
    if (it == chunks_.end()) {
        AppLogger::log("RenderCache::completeChunkRenderFailure NOT_FOUND start=" + juce::String(startSeconds, 3));
        return;
    }

    auto& chunk = it->second;

    if (chunk.runningRevision != revision) {
        AppLogger::log("RenderCache::completeChunkRenderFailure STALE runningRevision="
            + juce::String(static_cast<juce::int64>(chunk.runningRevision))
            + " != completionRev=" + juce::String(static_cast<juce::int64>(revision))
            + " -> ignore");
        return;
    }

    chunk.status = Chunk::Status::Idle;
    chunk.runningRevision = 0;

    AppLogger::log("RenderCache::completeChunkRenderFailure start=" + juce::String(startSeconds, 3)
        + " revision=" + juce::String(static_cast<juce::int64>(revision))
        + " desired=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision)));
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

    // Blank 是 Running completion 的一种，只处理 Running 状态
    if (chunk.status != Chunk::Status::Running) {
        return;
    }

    // 使用 runningRevision token 做 stale 检测
    if (chunk.runningRevision != revision) {
        AppLogger::log("RenderCache::markChunkAsBlank STALE runningRevision="
            + juce::String(static_cast<juce::int64>(chunk.runningRevision))
            + " != revision=" + juce::String(static_cast<juce::int64>(revision)));
        return;
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
