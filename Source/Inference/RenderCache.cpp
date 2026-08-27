#include "RenderCache.h"
#include "../DSP/ResamplingManager.h"
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
        pc.audio = chunk.audio;
        pc.publishedRevision = chunk.publishedRevision;
        snapshot->chunks.push_back(pc);
    }
    auto oldSnapshot = std::atomic_load(&publishedSnapshot_);
    std::atomic_store(&publishedSnapshot_, std::shared_ptr<const PublishedRenderSnapshot>(std::move(snapshot)));
    if (oldSnapshot != nullptr) {
        retiredSnapshots_.push_back(std::move(oldSnapshot));
    }
    pruneRetiredSnapshotsLocked();
}

void RenderCache::pruneRetiredSnapshotsLocked() const {
    auto end = std::remove_if(retiredSnapshots_.begin(), retiredSnapshots_.end(),
        [](const std::shared_ptr<const PublishedRenderSnapshot>& snapshot) {
            return snapshot == nullptr || snapshot.use_count() == 1;
        });
    retiredSnapshots_.erase(end, retiredSnapshots_.end());
}

// ============================================================================
// rebuildPrepared — non-audio thread, serialized by preparedBuildMutex_
// ============================================================================

void RenderCache::rebuildPrepared()
{
    std::lock_guard<std::mutex> lg(preparedBuildMutex_);

    double targetSr = preparedSampleRate_;
    auto canonicalSnap = std::atomic_load(&publishedSnapshot_);
    auto oldPrep = std::atomic_load(&preparedSnapshot_);

    // 增量复用仅当目标率与旧 prepared 快照一致时成立；换率必须全量重建。
    const bool rateUnchanged =
        oldPrep != nullptr && std::abs(oldPrep->sampleRate - targetSr) < 1.0;
    std::map<int64_t, const PublishedPreparedChunk*> previous;
    if (rateUnchanged) {
        for (const auto& pc : oldPrep->chunks)
            previous.emplace(pc.startSample, &pc);
    }

    // Build prepared snapshot: empty canonical → publish empty prepared with target rate
    auto prepSnap = std::make_shared<PublishedPreparedSnapshot>();
    prepSnap->sampleRate = targetSr;

    if (targetSr > 0.0 && canonicalSnap && !canonicalSnap->chunks.empty()) {
        prepSnap->chunks.reserve(canonicalSnap->chunks.size());
        for (const auto& chunk : canonicalSnap->chunks) {
            if (!chunk.audio || chunk.audio->empty()) continue;

            const int64_t prepStart = TimeCoordinate::sampleRateProject(
                chunk.startSample, kSampleRate, targetSr);
            const int64_t prepEnd = TimeCoordinate::sampleRateProject(
                chunk.endSampleExclusive, kSampleRate, targetSr);
            const bool aliasCanonical = std::abs(targetSr - kSampleRate) < 1.0;

            PublishedPreparedChunk pc;
            pc.startSample = prepStart;
            pc.endSampleExclusive = prepEnd;

            // 增量复用：canonical 内容未变（同 span、同 revision）时直接共享旧
            // prepared PCM，跳过 r8brain 重采样。条件从严：任何不匹配都重采样，
            // 宁可重做不可复用过期音频。
            auto prevIt = previous.find(prepStart);
            if (!aliasCanonical
                && prevIt != previous.end()
                && prevIt->second->endSampleExclusive == prepEnd
                && prevIt->second->sourceRevision == chunk.publishedRevision)
            {
                pc.audio = prevIt->second->audio;
                pc.sourceRevision = prevIt->second->sourceRevision;
                prepSnap->chunks.push_back(std::move(pc));
                continue;
            }

            if (aliasCanonical) {
                pc.audio = chunk.audio;
                pc.sourceRevision = chunk.publishedRevision;
            } else {
                const int outputLength = static_cast<int>(prepEnd - prepStart);
                if (outputLength <= 0) continue;

                std::vector<float> resampled = preparedResampler_.resampleExactLength(
                    chunk.audio->data(),
                    chunk.audio->size(),
                    static_cast<int>(kSampleRate),
                    static_cast<int>(targetSr),
                    outputLength);

                pc.audio = std::make_shared<const std::vector<float>>(std::move(resampled));
                pc.sourceRevision = chunk.publishedRevision;
            }

            prepSnap->chunks.push_back(std::move(pc));
        }
    }

    // Compute prepared memory for non-alias (r8brain) chunks.
    // 44.1kHz alias chunks share canonical data already tracked by global canonical bytes.
    size_t newPreparedBytes = 0;
    if (targetSr > 0.0 && std::abs(targetSr - kSampleRate) >= 1.0) {
        for (const auto& chunk : prepSnap->chunks) {
            if (chunk.audio) {
                newPreparedBytes += chunk.audio->size() * sizeof(float);
            }
        }
    }

    // Atomically replace old prepared bytes with new in global counters.
    const size_t oldPrepared = preparedMemoryUsage_;
    if (oldPrepared > 0) {
        globalCacheCurrentBytes().fetch_sub(oldPrepared, std::memory_order_relaxed);
    }
    size_t newCurrent = globalCacheCurrentBytes().fetch_add(newPreparedBytes, std::memory_order_relaxed) + newPreparedBytes;
    size_t peak = globalCachePeakBytes().load(std::memory_order_relaxed);
    while (newCurrent > peak && !globalCachePeakBytes().compare_exchange_weak(peak, newCurrent, std::memory_order_relaxed)) {}
    preparedMemoryUsage_ = newPreparedBytes;

    auto oldSnap = std::atomic_exchange(&preparedSnapshot_,
        std::shared_ptr<const PublishedPreparedSnapshot>(std::move(prepSnap)));
    if (oldSnap) retiredPreparedSnapshots_.push_back(std::move(oldSnap));

    // Prune retired prepared snapshots
    retiredPreparedSnapshots_.erase(
        std::remove_if(retiredPreparedSnapshots_.begin(), retiredPreparedSnapshots_.end(),
            [](const auto& p) { return p.use_count() <= 1; }),
        retiredPreparedSnapshots_.end());
}

// ============================================================================
// prepareForPlaybackSampleRate — 设置目标率并触发非音频线程 rebuild
// ============================================================================

void RenderCache::prepareForPlaybackSampleRate(double targetSr) {
    if (targetSr <= 0.0) return;
    {
        std::lock_guard<std::mutex> lg(preparedBuildMutex_);
        preparedSampleRate_ = targetSr;
    }
    rebuildPrepared();
}

// ============================================================================
// overlayPreparedAudio — 音频线程直接从 prepared snapshot replace（非叠加）
// ============================================================================

void RenderCache::overlayPreparedAudio(juce::AudioBuffer<float>& destination,
                                       int destStartSample,
                                       int numSamples,
                                       int64_t readStartSample,
                                       int targetSampleRate) const {
    if (targetSampleRate <= 0 || numSamples <= 0) return;

    const int destinationChannels = destination.getNumChannels();
    const int destinationSamples = destination.getNumSamples();
    if (destinationChannels <= 0 || destinationSamples <= 0) return;
    if (destStartSample < 0 || destStartSample >= destinationSamples) return;

    const int writableSamples = std::min(numSamples, destinationSamples - destStartSample);
    if (writableSamples <= 0) return;

    auto snap = std::atomic_load(&preparedSnapshot_);
    if (!snap || snap->chunks.empty()) return;

    // Prepared rate must match target rate
    if (std::abs(snap->sampleRate - static_cast<double>(targetSampleRate)) > 1.0) return;

    const int64_t requestStart = readStartSample;
    const int64_t requestEnd = requestStart + writableSamples;

    auto it = std::upper_bound(snap->chunks.begin(), snap->chunks.end(), requestStart,
        [](int64_t sample, const PublishedPreparedChunk& chunk) {
            return sample < chunk.startSample;
        });
    if (it != snap->chunks.begin()) --it;

    for (; it != snap->chunks.end(); ++it) {
        const auto& chunk = *it;
        if (chunk.endSampleExclusive <= requestStart) continue;
        if (chunk.startSample >= requestEnd) break;
        if (!chunk.audio || chunk.audio->empty()) continue;

        const int64_t overlapStart = std::max(requestStart, chunk.startSample);
        const int64_t overlapEnd = std::min(requestEnd, chunk.endSampleExclusive);
        if (overlapEnd <= overlapStart) continue;

        const int chunkSrcOffset = static_cast<int>(overlapStart - chunk.startSample);
        const int copyLen = static_cast<int>(overlapEnd - overlapStart);
        const int destOffset = destStartSample + static_cast<int>(overlapStart - requestStart);

        if (chunkSrcOffset < 0 || copyLen <= 0) continue;
        if (chunkSrcOffset + copyLen > static_cast<int>(chunk.audio->size())) continue;

        const float* srcData = chunk.audio->data() + chunkSrcOffset;
        for (int ch = 0; ch < destinationChannels; ++ch) {
            float* dst = destination.getWritePointer(ch);
            for (int i = 0; i < copyLen; ++i) {
                dst[destOffset + i] = srcData[i];
            }
        }
    }
}

// ============================================================================
// overlayCanonicalAudio — 非实时 canonical replace（仅供 Stage2/export）
// ============================================================================

void RenderCache::overlayCanonicalAudio(juce::AudioBuffer<float>& destination,
                                         int destStartSample,
                                         int numSamples,
                                         int64_t readStartSample) const {
    if (numSamples <= 0) return;

    const int destinationChannels = destination.getNumChannels();
    const int destinationSamples = destination.getNumSamples();
    if (destinationChannels <= 0 || destinationSamples <= 0) return;
    if (destStartSample < 0 || destStartSample >= destinationSamples) return;

    const int writableSamples = std::min(numSamples, destinationSamples - destStartSample);
    if (writableSamples <= 0) return;

    auto canonicalSnap = std::atomic_load(&publishedSnapshot_);
    if (!canonicalSnap || canonicalSnap->chunks.empty()) return;

    const int64_t requestStart = readStartSample;
    const int64_t requestEnd = requestStart + writableSamples;

    auto it = std::upper_bound(canonicalSnap->chunks.begin(), canonicalSnap->chunks.end(), requestStart,
        [](int64_t sample, const PublishedChunk& chunk) {
            return sample < chunk.startSample;
        });
    if (it != canonicalSnap->chunks.begin()) --it;

    for (; it != canonicalSnap->chunks.end(); ++it) {
        const auto& chunk = *it;
        if (chunk.endSampleExclusive <= requestStart) continue;
        if (chunk.startSample >= requestEnd) break;
        if (!chunk.audio || chunk.audio->empty()) continue;

        const int64_t overlapStart = std::max(requestStart, chunk.startSample);
        const int64_t overlapEnd = std::min(requestEnd, chunk.endSampleExclusive);
        if (overlapEnd <= overlapStart) continue;

        const int chunkSrcOffset = static_cast<int>(overlapStart - chunk.startSample);
        const int copyLen = static_cast<int>(overlapEnd - overlapStart);
        const int destOffset = destStartSample + static_cast<int>(overlapStart - requestStart);

        if (chunkSrcOffset < 0 || copyLen <= 0) continue;
        if (chunkSrcOffset + copyLen > static_cast<int>(chunk.audio->size())) continue;

        const float* srcData = chunk.audio->data() + chunkSrcOffset;
        for (int ch = 0; ch < destinationChannels; ++ch) {
            float* dst = destination.getWritePointer(ch);
            for (int i = 0; i < copyLen; ++i) {
                dst[destOffset + i] = srcData[i];
            }
        }
    }
}

void RenderCache::clear() {
    {
        const juce::SpinLock::ScopedLockType guard(lock_);
        for (const auto& [key, chunk] : chunks_) {
            juce::ignoreUnused(key);
            const size_t chunkBytes = (chunk.audio ? chunk.audio->size() : 0) * sizeof(float);
            globalCacheCurrentBytes().fetch_sub(chunkBytes, std::memory_order_relaxed);
        }
        chunks_.clear();
        pendingChunks_.clear();
        publishLocked();
    }

    {
        std::lock_guard<std::mutex> lg(preparedBuildMutex_);

        if (preparedMemoryUsage_ > 0) {
            globalCacheCurrentBytes().fetch_sub(preparedMemoryUsage_, std::memory_order_relaxed);
            preparedMemoryUsage_ = 0;
        }

        auto oldSnap = std::atomic_exchange(&preparedSnapshot_,
            std::shared_ptr<const PublishedPreparedSnapshot>());
        if (oldSnap) retiredPreparedSnapshots_.push_back(std::move(oldSnap));
        retiredPreparedSnapshots_.erase(
            std::remove_if(retiredPreparedSnapshots_.begin(), retiredPreparedSnapshots_.end(),
                [](const auto& p) { return p.use_count() <= 1; }),
            retiredPreparedSnapshots_.end());
    }
}

// ---------------------------------------------------------------------------
// 调度状态管理实现
// ---------------------------------------------------------------------------

RenderCache::ReconcileResult RenderCache::reconcileFullPlanAndRequest(
    const std::vector<PlannedChunk>& fullPlan,
    int64_t requestStartSample,
    int64_t requestEndSampleExclusive,
    uint64_t contentRevision)
{
    jassert(!fullPlan.empty());
    jassert(requestEndSampleExclusive > requestStartSample);
    jassert(contentRevision != 0);

    ReconcileResult result;
    bool geometryChanged = false;

    {
        const juce::SpinLock::ScopedLockType guard(lock_);

        std::map<double, Chunk> newChunks;
        std::set<double> newPendingChunks;

        const auto reclaimChunkBytes = [](const Chunk& chunk) {
            const size_t bytes = (chunk.audio ? chunk.audio->size() : 0) * sizeof(float);
            if (bytes > 0)
                globalCacheCurrentBytes().fetch_sub(bytes, std::memory_order_relaxed);
        };

        auto oldIt = chunks_.begin();

        for (std::size_t i = 0; i < fullPlan.size(); ++i)
        {
            const PlannedChunk& planned = fullPlan[i];
            jassert(planned.endSampleExclusive > planned.startSample);
            if (i > 0)
                jassert(planned.startSample >= fullPlan[i - 1].endSampleExclusive);

            const double startSeconds = projectRenderSeconds(planned.startSample);
            const int64_t endSampleExclusive = planned.endSampleExclusive;

            while (oldIt != chunks_.end() && oldIt->first < startSeconds)
            {
                reclaimChunkBytes(oldIt->second);
                geometryChanged = true;
                ++oldIt;
            }

            Chunk newChunk;
            newChunk.startSeconds = startSeconds;
            newChunk.startSample = planned.startSample;
            newChunk.endSampleExclusive = endSampleExclusive;

            const bool sameSpan = oldIt != chunks_.end()
                && oldIt->first == startSeconds
                && oldIt->second.endSampleExclusive == endSampleExclusive;

            const bool intersectsRequest = planned.startSample < requestEndSampleExclusive
                && endSampleExclusive > requestStartSample;

            if (sameSpan)
            {
                newChunk = oldIt->second;
                ++oldIt;

                if (intersectsRequest)
                {
                    const bool contentUnchanged = contentRevision == newChunk.lastRequestedContentRevision;
                    const bool alreadySettled = (newChunk.status == Chunk::Status::Idle
                            && newChunk.publishedRevision > 0
                            && newChunk.publishedRevision == newChunk.desiredRevision)
                        || newChunk.status == Chunk::Status::Blank;

                    // Same geometry + same contentRevision → no-op for
                    // Pending, Running, Blank, and successful Idle.
                    // Only Failed allows explicit same-version retry.
                    if (contentUnchanged && alreadySettled)
                    {
                        // no-op
                    }
                    else if (contentUnchanged && newChunk.status == Chunk::Status::Pending)
                    {
                        // Already queued for same revision: no-op
                    }
                    else if (contentUnchanged && newChunk.status == Chunk::Status::Running)
                    {
                        // Already executing for same revision: no-op
                    }
                    else if (contentUnchanged && newChunk.status == Chunk::Status::Failed)
                    {
                        // Same revision retry: re-queue
                        ++newChunk.desiredRevision;
                        newChunk.status = Chunk::Status::Pending;
                        newChunk.runningRevision = 0;
                        newChunk.publishedRevision = 0;
                        newChunk.audio = nullptr;
                        newPendingChunks.insert(startSeconds);
                        ++result.workerTokenCount;
                        result.stateChanged = true;
                    }
                    else
                    {
                        ++newChunk.desiredRevision;
                        newChunk.lastRequestedContentRevision = contentRevision;
                        if (newChunk.status == Chunk::Status::Pending)
                        {
                            newPendingChunks.insert(startSeconds);
                        }
                        else
                        {
                            newChunk.status = Chunk::Status::Pending;
                            newChunk.runningRevision = 0;
                            newPendingChunks.insert(startSeconds);
                            ++result.workerTokenCount;
                        }
                        result.stateChanged = true;
                    }
                }
                else if (newChunk.status == Chunk::Status::Pending)
                {
                    newPendingChunks.insert(startSeconds);
                }
            }
            else
            {
                geometryChanged = true;

                if (oldIt != chunks_.end() && oldIt->first == startSeconds)
                {
                    reclaimChunkBytes(oldIt->second);
                    newChunk.desiredRevision = oldIt->second.desiredRevision + 1;
                    ++oldIt;
                }
                else
                {
                    newChunk.desiredRevision = 1;
                }

                newChunk.status = Chunk::Status::Pending;
                newChunk.runningRevision = 0;
                newChunk.publishedRevision = 0;
                newChunk.audio = nullptr;
                newChunk.lastRequestedContentRevision = contentRevision;
                newPendingChunks.insert(startSeconds);
                ++result.workerTokenCount;
                result.stateChanged = true;
            }

            newChunks.emplace(startSeconds, std::move(newChunk));
        }

        for (; oldIt != chunks_.end(); ++oldIt)
        {
            reclaimChunkBytes(oldIt->second);
            geometryChanged = true;
            result.stateChanged = true;
        }

        chunks_ = std::move(newChunks);
        pendingChunks_ = std::move(newPendingChunks);

        if (geometryChanged)
            publishLocked();
    }

    if (geometryChanged)
        rebuildPrepared();

    AppLogger::log("RenderCache::reconcileFullPlanAndRequest"
        " fullPlan=" + juce::String(static_cast<juce::int64>(fullPlan.size()))
        + " requestStart=" + juce::String(requestStartSample)
        + " requestEnd=" + juce::String(requestEndSampleExclusive)
        + " jobTokens=" + juce::String(static_cast<juce::int64>(result.workerTokenCount))
        + " stateChanged=" + juce::String(result.stateChanged ? 1 : 0)
        + " geometryChanged=" + juce::String(geometryChanged ? 1 : 0));

    return result;
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
    outJob.startSample = chunk.startSample;
    outJob.endSampleExclusive = chunk.endSampleExclusive;
    outJob.targetRevision = chunk.desiredRevision;

    AppLogger::log("RenderCache::getNextPendingJob start=" + juce::String(startSec, 3)
        + " startSample=" + juce::String(chunk.startSample)
        + " endSampleExclusive=" + juce::String(chunk.endSampleExclusive)
        + " revision=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision)));

    return true;
}

RenderCache::ChunkRenderResult RenderCache::completeChunkRenderWithAudio(int64_t startSample,
                                                                           int64_t endSampleExclusive,
                                                                           std::vector<float>&& audio,
                                                                           uint64_t revision) {
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

    {
        const juce::SpinLock::ScopedLockType guard(lock_);
        auto it = chunks_.find(startSeconds);
        if (it == chunks_.end())
            return ChunkRenderResult::Stale;

        auto& chunk = it->second;

        // 身份检查：span 不符视为过期完成（chunk 已被重新规划）
        if (chunk.startSample != startSample || chunk.endSampleExclusive != endSampleExclusive)
            return ChunkRenderResult::Stale;

        if (chunk.runningRevision != revision)
            return ChunkRenderResult::Stale;

        const size_t oldBytes = (chunk.audio ? chunk.audio->size() : 0) * sizeof(float);
        if (oldBytes > 0) {
            globalCacheCurrentBytes().fetch_sub(oldBytes, std::memory_order_relaxed);
        }

        chunk.audio = immutableAudio;
        chunk.publishedRevision = revision;
        chunk.status = Chunk::Status::Idle;
        chunk.runningRevision = 0;

        const size_t chunkBytes = chunk.audio->size() * sizeof(float);

        const size_t newCurrent = globalCacheCurrentBytes().fetch_add(chunkBytes, std::memory_order_relaxed) + chunkBytes;

        size_t peak = globalCachePeakBytes().load(std::memory_order_relaxed);
        while (newCurrent > peak && !globalCachePeakBytes().compare_exchange_weak(peak, newCurrent, std::memory_order_relaxed)) {}

        // Diagnostic: memory stats only. No inter-chunk eviction — the RenderCache
        // contract requires complete canonical PCM resident for all chunks.
        (void)globalCacheLimitBytes().load(std::memory_order_relaxed);

        publishLocked();
    } // SpinLock released

    // Rebuild prepared outside SpinLock — rebuildPrepared locks preparedBuildMutex_ internally
    rebuildPrepared();

    AppLogger::log("RenderCache::completeChunkRenderWithAudio PUBLISHED start=" + juce::String(startSeconds, 3)
        + " revision=" + juce::String(static_cast<juce::int64>(revision)));
    return ChunkRenderResult::Published;
}

void RenderCache::completeChunkRenderFailure(double startSeconds, uint64_t revision) {
    bool hadAudio = false;
    {
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

        hadAudio = chunk.audio != nullptr && !chunk.audio->empty();
        if (hadAudio)
        {
            const size_t bytes = chunk.audio->size() * sizeof(float);
            globalCacheCurrentBytes().fetch_sub(bytes, std::memory_order_relaxed);
            chunk.audio.reset();
        }
        chunk.publishedRevision = 0;
        chunk.status = Chunk::Status::Failed;
        chunk.runningRevision = 0;
        publishLocked();

        AppLogger::log("RenderCache::completeChunkRenderFailure start=" + juce::String(startSeconds, 3)
            + " revision=" + juce::String(static_cast<juce::int64>(revision))
            + " desired=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision)));
    }

    if (hadAudio)
        rebuildPrepared();
}

bool RenderCache::requeueRunningChunk(double startSeconds, uint64_t runningRevision) {
    const juce::SpinLock::ScopedLockType guard(lock_);
    auto it = chunks_.find(startSeconds);
    if (it == chunks_.end())
        return false;

    auto& chunk = it->second;
    if (chunk.status != Chunk::Status::Running || chunk.runningRevision != runningRevision)
        return false;

    // 仅 Running→Pending 状态回退：不 bump desired、不改几何、不发布快照。
    // worker 下轮经 getNextPendingJob 重拉 span/revision。
    chunk.status = Chunk::Status::Pending;
    chunk.runningRevision = 0;
    pendingChunks_.insert(startSeconds);

    AppLogger::log("RenderCache::requeueRunningChunk start=" + juce::String(startSeconds, 3)
        + " revision=" + juce::String(static_cast<juce::int64>(runningRevision)));
    return true;
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
            case Chunk::Status::Failed: ++stats.failed; break;
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
            case Chunk::Status::Failed: ++snapshot.chunkStats.failed; break;
        }
    }
    return snapshot;
}

bool RenderCache::isCanonicalSettled() const
{
    const juce::SpinLock::ScopedLockType guard(lock_);
    if (chunks_.empty())
        return false;

    for (const auto& [key, chunk] : chunks_)
    {
        juce::ignoreUnused(key);
        if (chunk.status == Chunk::Status::Pending || chunk.status == Chunk::Status::Running)
            return false;

        if (chunk.status == Chunk::Status::Blank)
            continue;

        if (chunk.status == Chunk::Status::Failed)
            return false;

        if (chunk.status != Chunk::Status::Idle
            || chunk.audio == nullptr
            || chunk.audio->empty()
            || chunk.publishedRevision == 0
            || chunk.publishedRevision != chunk.desiredRevision)
        {
            return false;
        }
    }
    return true;
}

void RenderCache::markChunkAsBlank(double startSeconds, uint64_t revision) {
    int64_t endSample = 0;
    {
        const juce::SpinLock::ScopedLockType guard(lock_);
        auto it = chunks_.find(startSeconds);
        if (it == chunks_.end()) {
            return;
        }

        auto& chunk = it->second;

        if (chunk.status != Chunk::Status::Running) {
            return;
        }

        if (chunk.runningRevision != revision) {
            AppLogger::log("RenderCache::markChunkAsBlank STALE runningRevision="
                + juce::String(static_cast<juce::int64>(chunk.runningRevision))
                + " != revision=" + juce::String(static_cast<juce::int64>(revision)));
            return;
        }

        chunk.status = Chunk::Status::Blank;
        chunk.runningRevision = 0;

        if (chunk.audio != nullptr && !chunk.audio->empty()) {
            const size_t evictBytes = (chunk.audio ? chunk.audio->size() : 0) * sizeof(float);
            globalCacheCurrentBytes().fetch_sub(evictBytes, std::memory_order_relaxed);
            chunk.audio.reset();
        }
        chunk.publishedRevision = 0;
        endSample = chunk.endSampleExclusive;
        publishLocked();
    } // SpinLock released

    // Rebuild prepared outside SpinLock — rebuildPrepared locks preparedBuildMutex_ internally
    rebuildPrepared();

    AppLogger::log("RenderCache::markChunkAsBlank start=" + juce::String(startSeconds, 3)
        + " endSampleExclusive=" + juce::String(endSample)
        + " revision=" + juce::String(static_cast<juce::int64>(revision)));
}

} // namespace OpenTune
