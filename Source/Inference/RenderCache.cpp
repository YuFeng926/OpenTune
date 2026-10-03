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

bool reserveRenderCacheBytes(size_t bytes) noexcept
{
    // This counter covers only canonical/prepared PCM.  Other caches use the
    // legacy aggregate counter but do not consume this budget.
    auto& current = RenderCache::renderCacheCurrentBytes();
    const size_t limit = RenderCache::renderCachePcmLimitBytes().load(std::memory_order_acquire);
    size_t observed = current.load(std::memory_order_relaxed);
    for (;;) {
        if (bytes > limit || observed > limit - bytes)
            return false;
        if (current.compare_exchange_weak(observed, observed + bytes,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed))
            return true;
    }
}

void releaseRenderCacheBytes(size_t bytes) noexcept
{
    RenderCache::renderCacheCurrentBytes().fetch_sub(bytes, std::memory_order_acq_rel);
}

void recordRenderCachePeak(size_t current) noexcept
{
    auto& peak = RenderCache::renderCachePeakBytes();
    size_t observed = peak.load(std::memory_order_relaxed);
    while (current > observed
           && !peak.compare_exchange_weak(observed, current,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {}
}

std::shared_ptr<const std::vector<float>> makeTrackedPcm(
    std::vector<float>&& audio, size_t preReservedBytes = 0)
{
    if (audio.empty()) {
        if (preReservedBytes > 0)
            releaseRenderCacheBytes(preReservedBytes);
        return {};
    }

    // The allocation owner, rather than each snapshot, owns the accounting.
    // Canonical/prepared aliases therefore consume the budget only once, while
    // retired snapshots keep the allocation charged until their last reference.
    const size_t requestedBytes = audio.capacity() * sizeof(float);
    size_t reservedBytes = preReservedBytes;
    if (reservedBytes == 0) {
        if (!reserveRenderCacheBytes(requestedBytes))
            return {};
        reservedBytes = requestedBytes;
    } else if (requestedBytes > reservedBytes) {
        const size_t extraBytes = requestedBytes - reservedBytes;
        if (!reserveRenderCacheBytes(extraBytes)) {
            releaseRenderCacheBytes(reservedBytes);
            return {};
        }
        reservedBytes += extraBytes;
    } else if (requestedBytes < reservedBytes) {
        releaseRenderCacheBytes(reservedBytes - requestedBytes);
        reservedBytes = requestedBytes;
    }

    try {
        auto* raw = new std::vector<float>(std::move(audio));
        const size_t actualBytes = raw->capacity() * sizeof(float);

        if (actualBytes > requestedBytes) {
            const size_t extraBytes = actualBytes - requestedBytes;
            if (!reserveRenderCacheBytes(extraBytes)) {
                delete raw;
                releaseRenderCacheBytes(reservedBytes);
                return {};
            }
            reservedBytes += extraBytes;
        } else if (actualBytes < requestedBytes) {
            releaseRenderCacheBytes(requestedBytes - actualBytes);
            reservedBytes = actualBytes;
        }

        recordRenderCachePeak(RenderCache::renderCacheCurrentBytes().load(std::memory_order_relaxed));

        try {
            return std::shared_ptr<const std::vector<float>>(
                raw, [actualBytes](const std::vector<float>* value) {
                    delete value;
                    releaseRenderCacheBytes(actualBytes);
                });
        } catch (...) {
            delete raw;
            releaseRenderCacheBytes(actualBytes);
            reservedBytes = 0;
            throw;
        }
    } catch (...) {
        releaseRenderCacheBytes(reservedBytes);
        throw;
    }
}

} // namespace

std::atomic<size_t>& RenderCache::renderCachePcmLimitBytes() {
    static std::atomic<size_t> value{kDefaultRenderCachePcmLimitBytes};
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

std::atomic<size_t>& RenderCache::renderCacheCurrentBytes() {
    static std::atomic<size_t> value{0};
    return value;
}

std::atomic<size_t>& RenderCache::renderCachePeakBytes() {
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

void RenderCache::publishLocked(
    std::vector<std::shared_ptr<const PublishedRenderSnapshot>>& releases) {
    pruneRetiredSnapshotsLocked(releases);

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
        pc.contentRevision = chunk.lastRequestedContentRevision;
        snapshot->chunks.push_back(pc);
    }
    auto oldSnapshot = std::atomic_load(&publishedSnapshot_);
    std::atomic_store(&publishedSnapshot_, std::shared_ptr<const PublishedRenderSnapshot>(std::move(snapshot)));
    if (oldSnapshot != nullptr) {
        retiredSnapshots_.push_back(std::move(oldSnapshot));
    }
    pruneRetiredSnapshotsLocked(releases);
}

void RenderCache::pruneRetiredSnapshotsLocked(
    std::vector<std::shared_ptr<const PublishedRenderSnapshot>>& releases) const {
    auto end = std::remove_if(retiredSnapshots_.begin(), retiredSnapshots_.end(),
        [&releases](std::shared_ptr<const PublishedRenderSnapshot>& snapshot) {
            if (snapshot == nullptr)
                return true;
            if (snapshot.use_count() == 1) {
                releases.push_back(std::move(snapshot));
                return true;
            }
            return false;
        });
    retiredSnapshots_.erase(end, retiredSnapshots_.end());
}

// ============================================================================
// rebuildPrepared — non-audio thread, serialized by preparedBuildMutex_
// ============================================================================

bool RenderCache::rebuildPrepared(double requestedSampleRate)
{
    std::vector<std::shared_ptr<const PublishedPreparedSnapshot>> releases;
    std::lock_guard<std::mutex> lg(preparedBuildMutex_);

    const double targetSr = requestedSampleRate > 0.0
        ? requestedSampleRate : preparedSampleRate_;
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

            // 增量复用：canonical 内容未变（同 span、同 contentRevision）时直接共享
            // 旧 prepared PCM，跳过 r8brain 重采样。条件从严：任何不匹配都重采样，
            // 宁可重做不可复用过期音频。
            auto prevIt = previous.find(prepStart);
            if (!aliasCanonical
                && prevIt != previous.end()
                && prevIt->second->endSampleExclusive == prepEnd
                && prevIt->second->sourceRevision == chunk.contentRevision)
            {
                pc.audio = prevIt->second->audio;
                pc.sourceRevision = prevIt->second->sourceRevision;
                prepSnap->chunks.push_back(std::move(pc));
                continue;
            }

            if (aliasCanonical) {
                pc.audio = chunk.audio;
                pc.sourceRevision = chunk.contentRevision;
            } else {
                const int outputLength = static_cast<int>(prepEnd - prepStart);
                if (outputLength <= 0) continue;

                const size_t reservedBytes = static_cast<size_t>(outputLength) * sizeof(float);
                if (!reserveRenderCacheBytes(reservedBytes))
                    return false;

                std::vector<float> resampled;
                try {
                    resampled = preparedResampler_.resampleExactLength(
                        chunk.audio->data(),
                        chunk.audio->size(),
                        static_cast<int>(kSampleRate),
                        static_cast<int>(targetSr),
                        outputLength);
                } catch (...) {
                    releaseRenderCacheBytes(reservedBytes);
                    throw;
                }

                pc.audio = makeTrackedPcm(std::move(resampled), reservedBytes);
                if (!pc.audio)
                    return false;
                pc.sourceRevision = chunk.contentRevision;
            }

            prepSnap->chunks.push_back(std::move(pc));
        }
    }

    auto oldSnap = std::atomic_exchange(&preparedSnapshot_,
        std::shared_ptr<const PublishedPreparedSnapshot>(std::move(prepSnap)));
    if (oldSnap) retiredPreparedSnapshots_.push_back(std::move(oldSnap));

    // Prune retired prepared snapshots
    retiredPreparedSnapshots_.erase(
        std::remove_if(retiredPreparedSnapshots_.begin(), retiredPreparedSnapshots_.end(),
            [&releases](std::shared_ptr<const PublishedPreparedSnapshot>& p) {
                if (p.use_count() <= 1) {
                    releases.push_back(std::move(p));
                    return true;
                }
                return false;
            }),
        retiredPreparedSnapshots_.end());

    if (requestedSampleRate > 0.0)
        preparedSampleRate_ = targetSr;
    return true;
}

// ============================================================================
// prepareForPlaybackSampleRate — 设置目标率并触发非音频线程 rebuild
// ============================================================================

bool RenderCache::prepareForPlaybackSampleRate(double targetSr) {
    if (targetSr <= 0.0) return false;
    return rebuildPrepared(targetSr);
}

RenderCache::PreparedSnapshotRollback RenderCache::capturePreparedSnapshot() const
{
    std::lock_guard<std::mutex> lg(preparedBuildMutex_);
    const auto current = std::atomic_load(&preparedSnapshot_);
    return {std::shared_ptr<const void>(current),
            current != nullptr ? current->sampleRate : preparedSampleRate_, true};
}

bool RenderCache::restorePreparedSnapshot(PreparedSnapshotRollback&& rollback)
{
    if (!rollback.valid)
        return false;

    std::vector<std::shared_ptr<const PublishedPreparedSnapshot>> releases;
    std::lock_guard<std::mutex> lg(preparedBuildMutex_);
    std::shared_ptr<const PublishedPreparedSnapshot> restored;
    if (rollback.snapshot != nullptr) {
        restored = std::shared_ptr<const PublishedPreparedSnapshot>(
            rollback.snapshot,
            static_cast<const PublishedPreparedSnapshot*>(rollback.snapshot.get()));
    }
    auto old = std::atomic_exchange(&preparedSnapshot_, restored);
    if (old)
        retiredPreparedSnapshots_.push_back(std::move(old));
    preparedSampleRate_ = rollback.sampleRate;

    if (restored != nullptr) {
        const auto retiredIt = std::find_if(
            retiredPreparedSnapshots_.begin(), retiredPreparedSnapshots_.end(),
            [&restored](const auto& snapshot) {
                return snapshot.get() == restored.get();
            });
        if (retiredIt != retiredPreparedSnapshots_.end())
            retiredPreparedSnapshots_.erase(retiredIt);
    }

    retiredPreparedSnapshots_.erase(
        std::remove_if(retiredPreparedSnapshots_.begin(), retiredPreparedSnapshots_.end(),
            [&releases](std::shared_ptr<const PublishedPreparedSnapshot>& snapshot) {
                if (snapshot.use_count() <= 1) {
                    releases.push_back(std::move(snapshot));
                    return true;
                }
                return false;
            }),
        retiredPreparedSnapshots_.end());
    rollback.snapshot.reset();
    rollback.valid = false;
    return true;
}

// ============================================================================
// overlayPreparedAudio — 音频线程直接从 prepared snapshot replace（非叠加）
// 只复制 sourceRevision == expectedContentRevision 的 chunk。
// ============================================================================

void RenderCache::overlayPreparedAudio(juce::AudioBuffer<float>& destination,
                                       int destStartSample,
                                       int numSamples,
                                       int64_t readStartSample,
                                       int targetSampleRate,
                                       uint64_t expectedContentRevision) const {
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
        if (chunk.sourceRevision != expectedContentRevision) continue;
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
    std::map<int64_t, Chunk> chunksToRelease;
    std::vector<std::shared_ptr<const PublishedRenderSnapshot>> snapshotReleases;
    {
        const juce::SpinLock::ScopedLockType guard(lock_);
        chunksToRelease = std::move(chunks_);
        pendingChunks_.clear();
        publishLocked(snapshotReleases);
    }

    std::vector<std::shared_ptr<const PublishedPreparedSnapshot>> preparedReleases;
    {
        std::lock_guard<std::mutex> lg(preparedBuildMutex_);

        auto oldSnap = std::atomic_exchange(&preparedSnapshot_,
            std::shared_ptr<const PublishedPreparedSnapshot>());
        if (oldSnap) retiredPreparedSnapshots_.push_back(std::move(oldSnap));
        retiredPreparedSnapshots_.erase(
            std::remove_if(retiredPreparedSnapshots_.begin(), retiredPreparedSnapshots_.end(),
                [&preparedReleases](std::shared_ptr<const PublishedPreparedSnapshot>& p) {
                    if (p.use_count() <= 1) {
                        preparedReleases.push_back(std::move(p));
                        return true;
                    }
                    return false;
                }),
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
    std::size_t pendingChunkCount = 0;
    std::map<int64_t, Chunk> oldChunksToRelease;
    std::vector<std::shared_ptr<const std::vector<float>>> audioReleases;

    {
        const juce::SpinLock::ScopedLockType guard(lock_);

        std::map<int64_t, Chunk> newChunks;
        std::set<int64_t> newPendingChunks;
        oldChunksToRelease = std::move(chunks_);

        auto oldIt = oldChunksToRelease.begin();

        for (std::size_t i = 0; i < fullPlan.size(); ++i)
        {
            const PlannedChunk& planned = fullPlan[i];
            jassert(planned.endSampleExclusive > planned.startSample);
            if (i > 0)
                jassert(planned.startSample >= fullPlan[i - 1].endSampleExclusive);

            const int64_t startSample = planned.startSample;
            const int64_t endSampleExclusive = planned.endSampleExclusive;

            while (oldIt != oldChunksToRelease.end() && oldIt->first < startSample)
            {
                geometryChanged = true;
                ++oldIt;
            }

            Chunk newChunk;
            newChunk.startSample = planned.startSample;
            newChunk.endSampleExclusive = endSampleExclusive;

            const bool sameSpan = oldIt != oldChunksToRelease.end()
                && oldIt->first == startSample
                && oldIt->second.endSampleExclusive == endSampleExclusive;

            const bool intersectsRequest = planned.startSample < requestEndSampleExclusive
                && endSampleExclusive > requestStartSample;

            if (sameSpan)
            {
                newChunk = oldIt->second;
                ++oldIt;

                const bool alreadySettled = (newChunk.status == Chunk::Status::Idle
                        && newChunk.publishedRevision > 0
                        && newChunk.publishedRevision == newChunk.desiredRevision)
                    || newChunk.status == Chunk::Status::Blank;

                // A local edit advances the content identity for the whole
                // published snapshot. Unaffected chunks remain valid audio,
                // including chunks that are still pending/running, because
                // their edit range is disjoint. Keep their eventual publish
                // label aligned with the snapshot carried by the queued job.
                if (!intersectsRequest)
                    newChunk.lastRequestedContentRevision = contentRevision;

                if (intersectsRequest)
                {
                    const bool contentUnchanged = contentRevision == newChunk.lastRequestedContentRevision;

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
                        audioReleases.push_back(std::move(newChunk.audio));
                        newPendingChunks.insert(planned.startSample);
                        result.stateChanged = true;
                    }
                    else
                    {
                        ++newChunk.desiredRevision;
                        newChunk.lastRequestedContentRevision = contentRevision;
                        if (newChunk.status == Chunk::Status::Pending)
                        {
                            newPendingChunks.insert(planned.startSample);
                        }
                        else
                        {
                            newChunk.status = Chunk::Status::Pending;
                            newChunk.runningRevision = 0;
                            newPendingChunks.insert(planned.startSample);
                        }
                        result.stateChanged = true;
                    }
                }
                else if (newChunk.status == Chunk::Status::Pending)
                {
                    newPendingChunks.insert(planned.startSample);
                }
            }
            else
            {
                geometryChanged = true;

                if (oldIt != oldChunksToRelease.end() && oldIt->first == startSample)
                {
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
                newPendingChunks.insert(planned.startSample);
                result.stateChanged = true;
            }

            newChunks.emplace(startSample, std::move(newChunk));
        }

        for (; oldIt != oldChunksToRelease.end(); ++oldIt)
        {
            geometryChanged = true;
            result.stateChanged = true;
        }

        chunks_ = std::move(newChunks);
        pendingChunks_ = std::move(newPendingChunks);
        pendingChunkCount = pendingChunks_.size();
        // Do NOT publish or rebuild on geometry change: the old snapshot (or empty
        // initial) stays visible to the audio thread until all chunks are settled.
        // completeChunkRenderWithAudio / markChunkAsBlank gate on isCanonicalSettled.
    }

    // Do NOT rebuild prepared on geometry change: completeChunkRenderWithAudio /
    // markChunkAsBlank will rebuild only when all chunks are canonical settled.

    AppLogger::log("RenderCache::reconcileFullPlanAndRequest"
        " fullPlan=" + juce::String(static_cast<juce::int64>(fullPlan.size()))
        + " requestStart=" + juce::String(requestStartSample)
        + " requestEnd=" + juce::String(requestEndSampleExclusive)
        + " pendingChunks=" + juce::String(static_cast<juce::int64>(pendingChunkCount))
        + " stateChanged=" + juce::String(result.stateChanged ? 1 : 0)
        + " geometryChanged=" + juce::String(geometryChanged ? 1 : 0));

    return result;
}

bool RenderCache::claimPendingJob(const RenderCache::PendingJob& expectedJob) {
    const juce::SpinLock::ScopedLockType guard(lock_);
    auto pendingIt = pendingChunks_.find(expectedJob.startSample);
    if (pendingIt == pendingChunks_.end()) {
        return false;
    }

    auto it = chunks_.find(expectedJob.startSample);
    if (it == chunks_.end()) {
        pendingChunks_.erase(pendingIt);
        return false;
    }

    auto& chunk = it->second;
    if (chunk.startSample != expectedJob.startSample
        || chunk.endSampleExclusive != expectedJob.endSampleExclusive
        || chunk.desiredRevision != expectedJob.targetRevision
        || chunk.status != Chunk::Status::Pending) {
        return false;
    }

    pendingChunks_.erase(pendingIt);

    chunk.status = Chunk::Status::Running;
    chunk.runningRevision = chunk.desiredRevision;

    AppLogger::log("RenderCache::claimPendingJob start=" + juce::String(projectRenderSeconds(expectedJob.startSample), 3)
        + " startSample=" + juce::String(chunk.startSample)
        + " endSampleExclusive=" + juce::String(chunk.endSampleExclusive)
        + " revision=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision)));

    return true;
}

std::vector<RenderCache::PendingJob> RenderCache::getPendingJobs() const
{
    const juce::SpinLock::ScopedLockType guard(lock_);
    std::vector<PendingJob> jobs;
    jobs.reserve(pendingChunks_.size());
    for (const auto startSample : pendingChunks_) {
        const auto it = chunks_.find(startSample);
        if (it != chunks_.end() && it->second.status == Chunk::Status::Pending)
            jobs.push_back({it->second.startSample, it->second.endSampleExclusive,
                            it->second.desiredRevision});
    }
    return jobs;
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

    std::shared_ptr<const std::vector<float>> oldAudio;
    {
        const juce::SpinLock::ScopedLockType guard(lock_);
        const auto it = chunks_.find(startSample);
        if (it == chunks_.end()
            || it->second.startSample != startSample
            || it->second.endSampleExclusive != endSampleExclusive
            || it->second.runningRevision != revision)
            return ChunkRenderResult::Stale;
    }

    auto immutableAudio = makeTrackedPcm(std::move(audio));
    if (!immutableAudio)
        return ChunkRenderResult::MemoryLimitExceeded;

    bool staleAfterAllocation = false;
    Chunk::Status previousStatus = Chunk::Status::Running;
    uint64_t previousRunningRevision = revision;
    uint64_t previousPublishedRevision = 0;
    {
        const juce::SpinLock::ScopedLockType guard(lock_);
        auto it = chunks_.find(startSample);
        if (it == chunks_.end())
            staleAfterAllocation = true;
        else
        {
            auto& chunk = it->second;

            // 身份检查：span 不符视为过期完成（chunk 已被重新规划）
            if (chunk.startSample != startSample || chunk.endSampleExclusive != endSampleExclusive
                || chunk.runningRevision != revision)
                staleAfterAllocation = true;
            else
            {
                previousStatus = chunk.status;
                previousRunningRevision = chunk.runningRevision;
                previousPublishedRevision = chunk.publishedRevision;
                oldAudio = std::move(chunk.audio);
                chunk.audio = std::move(immutableAudio);
                chunk.publishedRevision = revision;
                chunk.status = Chunk::Status::Idle;
                chunk.runningRevision = 0;
            }
        }

    } // SpinLock released

    if (staleAfterAllocation)
        return ChunkRenderResult::Stale;

    // Only publish and rebuild prepared when ALL chunks are canonical settled.
    // Partial chunk completion must not update the audio-thread-visible snapshot.
    bool shouldPublish = false;
    std::shared_ptr<const PublishedRenderSnapshot> previousSnapshot;
    std::vector<std::shared_ptr<const PublishedRenderSnapshot>> snapshotReleases;
    {
        const juce::SpinLock::ScopedLockType guard2(lock_);
        shouldPublish = isCanonicalSettledLocked_();
        if (shouldPublish) {
            previousSnapshot = std::atomic_load(&publishedSnapshot_);
            publishLocked(snapshotReleases);
        }
    }
    if (shouldPublish && !rebuildPrepared()) {
        std::shared_ptr<const std::vector<float>> failedAudio;
        {
            const juce::SpinLock::ScopedLockType guard3(lock_);
            auto it = chunks_.find(startSample);
            if (it != chunks_.end()
                && it->second.status == Chunk::Status::Idle
                && it->second.publishedRevision == revision) {
                failedAudio = std::move(it->second.audio);
                it->second.audio = std::move(oldAudio);
                it->second.status = previousStatus;
                it->second.runningRevision = previousRunningRevision;
                it->second.publishedRevision = previousPublishedRevision;
                std::atomic_store(&publishedSnapshot_, previousSnapshot);
            }
        }
        return ChunkRenderResult::MemoryLimitExceeded;
    }

    AppLogger::log("RenderCache::completeChunkRenderWithAudio "
        + juce::String(shouldPublish ? "PUBLISHED" : "SETTLED")
        + " start=" + juce::String(startSeconds, 3)
        + " revision=" + juce::String(static_cast<juce::int64>(revision)));
    return ChunkRenderResult::Published;
}

bool RenderCache::completeChunkRenderFailure(int64_t startSample, uint64_t revision) {
    const double startSeconds = projectRenderSeconds(startSample);
    std::shared_ptr<const std::vector<float>> oldAudio;
    {
        const juce::SpinLock::ScopedLockType guard(lock_);
        auto it = chunks_.find(startSample);
        if (it == chunks_.end()) {
            AppLogger::log("RenderCache::completeChunkRenderFailure NOT_FOUND start=" + juce::String(startSeconds, 3));
            return false;
        }

        auto& chunk = it->second;

        const bool runningMatch = chunk.status == Chunk::Status::Running
            && chunk.runningRevision == revision;
        const bool pendingMatch = chunk.status == Chunk::Status::Pending
            && chunk.desiredRevision == revision;
        if (!runningMatch && !pendingMatch) {
            AppLogger::log("RenderCache::completeChunkRenderFailure STALE runningRevision="
                + juce::String(static_cast<juce::int64>(chunk.runningRevision))
                + " != completionRev=" + juce::String(static_cast<juce::int64>(revision))
                + " -> ignore");
            return false;
        }

        oldAudio = std::move(chunk.audio);
        chunk.publishedRevision = 0;
        chunk.status = Chunk::Status::Failed;
        chunk.runningRevision = 0;
        pendingChunks_.erase(startSample);
        // Do NOT publish on failure: the old snapshot (or empty) remains visible
        // to the audio thread. Failure keeps this chunk unsettled.

        AppLogger::log("RenderCache::completeChunkRenderFailure start=" + juce::String(startSeconds, 3)
            + " revision=" + juce::String(static_cast<juce::int64>(revision))
            + " desired=" + juce::String(static_cast<juce::int64>(chunk.desiredRevision)));
    }

    // Failure must NOT trigger rebuildPrepared: the old snapshot stays visible.
    return true;
}

bool RenderCache::requeueRunningChunk(int64_t startSample, uint64_t runningRevision) {
    const juce::SpinLock::ScopedLockType guard(lock_);
    const double startSeconds = projectRenderSeconds(startSample);
    auto it = chunks_.find(startSample);
    if (it == chunks_.end())
        return false;

    auto& chunk = it->second;
    if (chunk.startSample != startSample
        || chunk.status != Chunk::Status::Running
        || chunk.runningRevision != runningRevision)
        return false;

    // 仅 Running→Pending 状态回退：不 bump desired、不改几何、不发布快照。
    // worker 下轮经 claimPendingJob 重拉 span/revision。
    chunk.status = Chunk::Status::Pending;
    chunk.runningRevision = 0;
    pendingChunks_.insert(startSample);

    AppLogger::log("RenderCache::requeueRunningChunk start=" + juce::String(startSeconds, 3)
        + " revision=" + juce::String(static_cast<juce::int64>(runningRevision)));
    return true;
}

RenderCache::ChunkStats RenderCache::getChunkStats() const {
    ChunkStats stats;
    std::vector<std::shared_ptr<const PublishedRenderSnapshot>> snapshotReleases;
    {
        const juce::SpinLock::ScopedLockType guard(lock_);
        pruneRetiredSnapshotsLocked(snapshotReleases);
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
    }
    return stats;
}

RenderCache::StateSnapshot RenderCache::getStateSnapshot() const {
    StateSnapshot snapshot;
    std::vector<std::shared_ptr<const PublishedRenderSnapshot>> snapshotReleases;
    {
        const juce::SpinLock::ScopedLockType guard(lock_);
        pruneRetiredSnapshotsLocked(snapshotReleases);
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
    }
    return snapshot;
}

bool RenderCache::isCanonicalSettledLocked_() const
{
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

bool RenderCache::isCanonicalSettled() const
{
    const juce::SpinLock::ScopedLockType guard(lock_);
    return isCanonicalSettledLocked_();
}

bool RenderCache::markChunkAsBlank(int64_t startSample, uint64_t revision) {
    const double startSeconds = projectRenderSeconds(startSample);
    int64_t endSample = 0;
    std::shared_ptr<const std::vector<float>> oldAudio;
    {
        const juce::SpinLock::ScopedLockType guard(lock_);
        auto it = chunks_.find(startSample);
        if (it == chunks_.end()) {
            return false;
        }

        auto& chunk = it->second;

        if (chunk.status != Chunk::Status::Running) {
            return false;
        }

        if (chunk.runningRevision != revision) {
            AppLogger::log("RenderCache::markChunkAsBlank STALE runningRevision="
                + juce::String(static_cast<juce::int64>(chunk.runningRevision))
                + " != revision=" + juce::String(static_cast<juce::int64>(revision)));
            return false;
        }

        chunk.status = Chunk::Status::Blank;
        chunk.runningRevision = 0;

        oldAudio = std::move(chunk.audio);
        chunk.publishedRevision = 0;
        endSample = chunk.endSampleExclusive;
    } // SpinLock released

    // Only publish and rebuild prepared when ALL chunks are canonical settled.
    // Partial chunk completion must not update the audio-thread-visible snapshot.
    bool shouldPublish = false;
    std::shared_ptr<const PublishedRenderSnapshot> previousSnapshot;
    std::vector<std::shared_ptr<const PublishedRenderSnapshot>> snapshotReleases;
    {
        const juce::SpinLock::ScopedLockType guard2(lock_);
        shouldPublish = isCanonicalSettledLocked_();
        if (shouldPublish) {
            previousSnapshot = std::atomic_load(&publishedSnapshot_);
            publishLocked(snapshotReleases);
        }
    }
    if (shouldPublish && !rebuildPrepared()) {
        const juce::SpinLock::ScopedLockType guard3(lock_);
        auto it = chunks_.find(startSample);
        if (it != chunks_.end() && it->second.status == Chunk::Status::Blank) {
            it->second.status = Chunk::Status::Running;
            it->second.runningRevision = revision;
            it->second.publishedRevision = 0;
            it->second.audio = std::move(oldAudio);
            std::atomic_store(&publishedSnapshot_, previousSnapshot);
        }
        return false;
    }

    AppLogger::log("RenderCache::markChunkAsBlank start=" + juce::String(startSeconds, 3)
        + " endSampleExclusive=" + juce::String(endSample)
        + " revision=" + juce::String(static_cast<juce::int64>(revision)));
    return true;
}

} // namespace OpenTune
