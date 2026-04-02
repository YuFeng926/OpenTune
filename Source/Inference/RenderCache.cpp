#include "RenderCache.h"
#include "Utils/AppLogger.h"
#include <algorithm>

namespace OpenTune {

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

RenderCache::RenderCache() = default;

RenderCache::~RenderCache() {
    clear();
}

bool RenderCache::addChunk(double startSeconds, double endSeconds, std::vector<float>&& audio, uint64_t targetRevision) {
    if (audio.empty() || targetRevision == 0 || endSeconds <= startSeconds) {
        AppLogger::log("RenderCache::addChunk REJECT early-check"
            " start=" + juce::String(startSeconds, 3)
            + " end=" + juce::String(endSeconds, 3)
            + " audioEmpty=" + juce::String(audio.empty() ? 1 : 0)
            + " targetRev=" + juce::String(static_cast<juce::int64>(targetRevision)));
        return false;
    }

    const juce::SpinLock::ScopedLockType guard(lock_);
    auto& chunk = chunks_[startSeconds];
    chunk.startSeconds = startSeconds;
    chunk.endSeconds = endSeconds;

    if (chunk.desiredRevision == 0) {
        chunk.desiredRevision = targetRevision;
    }

    if (targetRevision != chunk.desiredRevision) {
        AppLogger::log("RenderCache::addChunk REJECT revision-mismatch"
            " start=" + juce::String(startSeconds, 3)
            + " end=" + juce::String(endSeconds, 3)
            + " targetRev=" + juce::String(static_cast<juce::int64>(targetRevision))
            + " desiredRev=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision))
            + " publishedRev=" + juce::String(static_cast<juce::int64>(chunk.publishedRevision)));
        return false;
    }

    const size_t oldBytes = chunk.audio.size() * sizeof(float);
    if (oldBytes > 0) {
        totalMemoryUsage_ -= oldBytes;
        globalCacheCurrentBytes().fetch_sub(oldBytes, std::memory_order_relaxed);
    }

    size_t resampledBytes = 0;
    for (const auto& [rate, data] : chunk.resampledAudio) {
        juce::ignoreUnused(rate);
        resampledBytes += data.size() * sizeof(float);
    }
    if (resampledBytes > 0) {
        totalMemoryUsage_ -= resampledBytes;
        globalCacheCurrentBytes().fetch_sub(resampledBytes, std::memory_order_relaxed);
    }
    chunk.resampledAudio.clear();

    chunk.audio = std::move(audio);
    chunk.publishedRevision = targetRevision;

    const size_t chunkBytes = chunk.audio.size() * sizeof(float);
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
            const size_t evictBytes = it->second.audio.size() * sizeof(float);
            size_t evictResampledBytes = 0;
            for (const auto& [rate, data] : it->second.resampledAudio) {
                juce::ignoreUnused(rate);
                evictResampledBytes += data.size() * sizeof(float);
            }
            const size_t totalEvictBytes = evictBytes + evictResampledBytes;
            if (totalEvictBytes == 0) {
                continue;
            }
            totalMemoryUsage_ -= totalEvictBytes;
            globalCacheCurrentBytes().fetch_sub(totalEvictBytes, std::memory_order_relaxed);
            it->second.audio.clear();
            it->second.resampledAudio.clear();
            break;
        }
    }

    return true;
}

bool RenderCache::addResampledChunk(double startSeconds, double endSeconds, int targetSampleRate, 
                                    std::vector<float>&& resampledAudio, uint64_t targetRevision) {
    if (resampledAudio.empty() || targetRevision == 0 || endSeconds <= startSeconds || targetSampleRate <= 0) {
        return false;
    }

    const juce::SpinLock::ScopedLockType guard(lock_);
    auto it = chunks_.find(startSeconds);
    if (it == chunks_.end()) {
        return false;
    }

    auto& chunk = it->second;
    if (chunk.endSeconds != endSeconds) {
        return false;
    }

    if (chunk.publishedRevision != targetRevision) {
        return false;
    }

    const size_t oldBytes = chunk.resampledAudio[targetSampleRate].size() * sizeof(float);
    if (oldBytes > 0) {
        totalMemoryUsage_ -= oldBytes;
        globalCacheCurrentBytes().fetch_sub(oldBytes, std::memory_order_relaxed);
    }

    const size_t newBytes = resampledAudio.size() * sizeof(float);
    chunk.resampledAudio[targetSampleRate] = std::move(resampledAudio);
    totalMemoryUsage_ += newBytes;
    globalCacheCurrentBytes().fetch_add(newBytes, std::memory_order_relaxed);

    return true;
}

int RenderCache::readAtTimeForRate(float* dest, int numSamples, double timeSeconds, 
                                   int targetSampleRate, bool nonBlocking) {
    auto readWithLock = [&]() -> int {
        auto it = chunks_.upper_bound(timeSeconds);
        if (it == chunks_.begin()) {
            return 0;
        }

        --it;
        const auto& chunk = it->second;
        if (timeSeconds < chunk.startSeconds || timeSeconds >= chunk.endSeconds) {
            return 0;
        }

        if (chunk.publishedRevision != chunk.desiredRevision) {
            return 0;
        }

        const double sampleRate = static_cast<double>(targetSampleRate);
        const double readPos = TimeCoordinate::secondsToSamplesExact(timeSeconds - chunk.startSeconds, sampleRate);
        if (readPos < 0.0) {
            return 0;
        }

        const float* src = nullptr;
        int64_t chunkSize64 = 0;

        if (targetSampleRate == static_cast<int>(kSampleRate)) {
            if (chunk.audio.empty()) {
                return 0;
            }
            src = chunk.audio.data();
            chunkSize64 = static_cast<int64_t>(chunk.audio.size());
        } else {
            auto resampledIt = chunk.resampledAudio.find(targetSampleRate);
            if (resampledIt == chunk.resampledAudio.end() || resampledIt->second.empty()) {
                return 0;
            }
            src = resampledIt->second.data();
            chunkSize64 = static_cast<int64_t>(resampledIt->second.size());
        }

        if (readPos >= static_cast<double>(chunkSize64)) {
            return 0;
        }

        const int64_t startIndex = static_cast<int64_t>(readPos);
        const int64_t availableSamples = chunkSize64 - startIndex;
        const int64_t requestedSamples = std::max<int64_t>(0, numSamples);
        const int samplesToRead = static_cast<int>(std::min(availableSamples, requestedSamples));

        if (samplesToRead <= 0) {
            return 0;
        }

        const double baseFraction = readPos - static_cast<double>(startIndex);
        if (baseFraction == 0.0) {
            std::copy(src + startIndex, src + startIndex + samplesToRead, dest);
            return samplesToRead;
        }

        for (int i = 0; i < samplesToRead; ++i) {
            const int64_t idx0 = startIndex + i;
            const int64_t idx1 = std::min<int64_t>(idx0 + 1, chunkSize64 - 1);
            const float s0 = src[idx0];
            const float s1 = src[idx1];
            dest[i] = static_cast<float>(s0 + (s1 - s0) * baseFraction);
        }
        return samplesToRead;
    };

    if (nonBlocking) {
        juce::SpinLock::ScopedTryLockType guard(lock_);
        if (!guard.isLocked()) {
            return 0;
        }
        return readWithLock();
    }

    const juce::SpinLock::ScopedLockType guard(lock_);
    return readWithLock();
}

void RenderCache::clearResampledCache() {
    const juce::SpinLock::ScopedLockType guard(lock_);
    for (auto& [key, chunk] : chunks_) {
        juce::ignoreUnused(key);
        size_t resampledBytes = 0;
        for (const auto& [rate, data] : chunk.resampledAudio) {
            juce::ignoreUnused(rate);
            resampledBytes += data.size() * sizeof(float);
        }
        if (resampledBytes > 0) {
            totalMemoryUsage_ -= resampledBytes;
            globalCacheCurrentBytes().fetch_sub(resampledBytes, std::memory_order_relaxed);
            chunk.resampledAudio.clear();
        }
    }
}

void RenderCache::clear() {
    const juce::SpinLock::ScopedLockType guard(lock_);
    for (const auto& [key, chunk] : chunks_) {
        juce::ignoreUnused(key);
        const size_t chunkBytes = chunk.audio.size() * sizeof(float);
        globalCacheCurrentBytes().fetch_sub(chunkBytes, std::memory_order_relaxed);
        for (const auto& [rate, data] : chunk.resampledAudio) {
            juce::ignoreUnused(rate);
            const size_t resampledBytes = data.size() * sizeof(float);
            globalCacheCurrentBytes().fetch_sub(resampledBytes, std::memory_order_relaxed);
        }
    }
    chunks_.clear();
    totalMemoryUsage_ = 0;
}

size_t RenderCache::getTotalMemoryUsage() const {
    const juce::SpinLock::ScopedLockType guard(lock_);
    return totalMemoryUsage_;
}

bool RenderCache::isRevisionPublished(double startSeconds, double endSeconds, uint64_t revision) const {
    if (revision == 0 || endSeconds <= startSeconds) {
        return false;
    }

    const juce::SpinLock::ScopedLockType guard(lock_);
    auto it = chunks_.find(startSeconds);
    if (it == chunks_.end()) {
        return false;
    }

    const auto& chunk = it->second;
    if (chunk.endSeconds != endSeconds) {
        return false;
    }
    return chunk.publishedRevision == revision && chunk.desiredRevision == revision && !chunk.audio.empty();
}

void RenderCache::setMemoryLimit(size_t bytes) {
    globalCacheLimitBytes().store(std::max<size_t>(1, bytes), std::memory_order_relaxed);
}

RenderCache::GlobalMemoryStats RenderCache::getGlobalMemoryStats() {
    GlobalMemoryStats stats;
    stats.cacheLimitBytes = globalCacheLimitBytes().load(std::memory_order_relaxed);
    stats.cacheCurrentBytes = globalCacheCurrentBytes().load(std::memory_order_relaxed);
    stats.cachePeakBytes = globalCachePeakBytes().load(std::memory_order_relaxed);
    return stats;
}

// ---------------------------------------------------------------------------
// 调度状态管理实现
// ---------------------------------------------------------------------------

void RenderCache::requestRenderPending(double startSeconds, double endSeconds) {
    if (endSeconds <= startSeconds) return;

    const juce::SpinLock::ScopedLockType guard(lock_);
    auto& chunk = chunks_[startSeconds];
    chunk.startSeconds = startSeconds;
    chunk.endSeconds = endSeconds;
    ++chunk.desiredRevision;

    AppLogger::log("RenderCache::requestRenderPending"
        " start=" + juce::String(startSeconds, 3)
        + " end=" + juce::String(endSeconds, 3)
        + " desired=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision))
        + " oldStatus=" + juce::String(static_cast<int>(chunk.status)));

    if (chunk.status == Chunk::Status::Idle || chunk.status == Chunk::Status::Blank) {
        chunk.status = Chunk::Status::Pending;
        pendingChunks_.insert(startSeconds);
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

    outJob.startSeconds = chunk.startSeconds;
    outJob.endSeconds = chunk.endSeconds;
    outJob.targetRevision = chunk.desiredRevision;

    AppLogger::log("RenderCache::getNextPendingJob start=" + juce::String(startSec, 3)
        + " end=" + juce::String(chunk.endSeconds, 3)
        + " revision=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision)));

    return true;
}

void RenderCache::completeChunkRender(double startSeconds, uint64_t revision, RenderCache::CompletionResult result) {
    const juce::SpinLock::ScopedLockType guard(lock_);
    auto it = chunks_.find(startSeconds);
    if (it == chunks_.end()) {
        AppLogger::log("RenderCache::completeChunkRender NOT_FOUND start=" + juce::String(startSeconds, 3));
        return;
    }

    auto& chunk = it->second;
    const char* resultText = "unknown";
    switch (result) {
        case CompletionResult::Succeeded: resultText = "Succeeded"; break;
        case CompletionResult::RetryableFailure: resultText = "RetryableFailure"; break;
        case CompletionResult::TerminalFailure: resultText = "TerminalFailure"; break;
    }
    AppLogger::log("RenderCache::completeChunkRender start=" + juce::String(startSeconds, 3)
        + " revision=" + juce::String(static_cast<juce::int64>(revision))
        + " desired=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision))
        + " result=" + juce::String(resultText)
        + " oldStatus=" + juce::String(static_cast<int>(chunk.status)));

    if (result == CompletionResult::RetryableFailure) {
        // 可重试失败：回到 Pending 等待重试（任务守恒）
        chunk.status = Chunk::Status::Pending;
        pendingChunks_.insert(startSeconds);
        return;
    }

    if (revision != chunk.desiredRevision) {
        // 版本过期：回到 Pending，重新渲染最新版本
        chunk.status = Chunk::Status::Pending;
        pendingChunks_.insert(startSeconds);
        AppLogger::log("RenderCache::completeChunkRender STALE revision=" + juce::String(static_cast<juce::int64>(revision))
            + " < desired=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision))
            + " -> requeue Pending");
        return;
    }

    if (result == CompletionResult::TerminalFailure) {
        // 终态失败：当前 revision 不可渲染，回到 Idle，等待后续编辑触发新 revision
        chunk.status = Chunk::Status::Idle;
    } else {
        // 版本匹配：成功完成，发布结果
        chunk.status = Chunk::Status::Idle;
        chunk.publishedRevision = revision;
        AppLogger::log("RenderCache::completeChunkRender SUCCESS revision=" + juce::String(static_cast<juce::int64>(revision)));
    }
}

int RenderCache::getPendingCount() const {
    const juce::SpinLock::ScopedLockType guard(lock_);
    return static_cast<int>(pendingChunks_.size());
}

RenderCache::ChunkStats RenderCache::getChunkStats() const {
    ChunkStats stats;
    const juce::SpinLock::ScopedLockType guard(lock_);
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

std::vector<RenderCache::PublishedChunk> RenderCache::getPublishedChunks() const {
    std::vector<PublishedChunk> result;
    const juce::SpinLock::ScopedLockType guard(lock_);
    for (const auto& [key, chunk] : chunks_) {
        juce::ignoreUnused(key);
        if (chunk.publishedRevision > 0 && 
            chunk.publishedRevision == chunk.desiredRevision && 
            !chunk.audio.empty()) {
            result.push_back({chunk.startSeconds, chunk.endSeconds, &chunk.audio});
        }
    }
    return result;
}

void RenderCache::markChunkAsBlank(double startSeconds) {
    const juce::SpinLock::ScopedLockType guard(lock_);
    auto it = chunks_.find(startSeconds);
    if (it == chunks_.end()) {
        return;
    }

    auto& chunk = it->second;
    
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
    
    AppLogger::log("RenderCache::markChunkAsBlank start=" + juce::String(startSeconds, 3)
        + " end=" + juce::String(chunk.endSeconds, 3)
        + " revision=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision)));
}

void RenderCache::clearAllPending() {
    const juce::SpinLock::ScopedLockType guard(lock_);
    for (double startSec : pendingChunks_) {
        auto it = chunks_.find(startSec);
        if (it != chunks_.end()) {
            it->second.status = Chunk::Status::Idle;
        }
    }
    pendingChunks_.clear();
}

} // namespace OpenTune
