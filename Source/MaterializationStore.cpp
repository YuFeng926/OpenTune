#include "MaterializationStore.h"

#include <algorithm>
#include <cmath>

#include "Utils/TimeCoordinate.h"
#include "Utils/ChannelLayoutLogger.h"

namespace OpenTune {

namespace {

bool findPreferredHopAlignedBoundarySample(const SilentGap& gap,
                                           int hopSize,
                                           int64_t& outSample)
{
    outSample = 0;
    if (!gap.isValid() || hopSize <= 0) {
        return false;
    }

    const int64_t firstAlignedSample = ((gap.startSample + hopSize - 1) / hopSize) * hopSize;
    const int64_t lastAlignedSample = ((gap.endSampleExclusive - 1) / hopSize) * hopSize;
    if (firstAlignedSample > lastAlignedSample) {
        return false;
    }

    const int64_t midpointSample = gap.midpointSample();
    const int64_t lowerAlignedSample = (midpointSample / hopSize) * hopSize;
    const int64_t upperAlignedSample = lowerAlignedSample + hopSize;

    int64_t preferredSample = lowerAlignedSample;
    if (upperAlignedSample <= lastAlignedSample
        && (midpointSample - lowerAlignedSample) >= (upperAlignedSample - midpointSample)) {
        preferredSample = upperAlignedSample;
    }

    outSample = juce::jlimit(firstAlignedSample, lastAlignedSample, preferredSample);
    return true;
}

} // namespace

MaterializationStore::MaterializationStore() = default;
MaterializationStore::~MaterializationStore() = default;

uint64_t MaterializationStore::createMaterialization(CreateMaterializationRequest request,
                                                     uint64_t forcedMaterializationId)
{
    if (request.sourceId == 0
        || request.audioBuffer == nullptr
        || request.audioBuffer->getNumChannels() <= 0
        || request.audioBuffer->getNumSamples() <= 0) {
        return 0;
    }
    // Channel-layout-policy invariant: storage MUST have 1 or 2 channels. Anything
    // outside that range indicates a caller bypassed prepareImport — fail loudly.
    const int requestChannels = request.audioBuffer->getNumChannels();
    if (requestChannels < 1 || requestChannels > 2) {
        jassertfalse;
        ChannelLayoutLog::logMaterializationReject(requestChannels);
        return 0;
    }

    MaterializationEntry materialization;
    materialization.materializationId = forcedMaterializationId != 0
        ? forcedMaterializationId
        : nextMaterializationId_.fetch_add(1, std::memory_order_relaxed);
    materialization.sourceId = request.sourceId;
    materialization.lineageParentMaterializationId = request.lineageParentMaterializationId;
    materialization.sourceWindow = request.sourceWindow;
    materialization.renderRevision = request.renderRevision;
    materialization.notesRevision = 1;
    materialization.audioBuffer = std::move(request.audioBuffer);
    materialization.pitchCurve = std::move(request.pitchCurve);
    materialization.originalF0State = request.originalF0State;
    materialization.detectedKey = request.detectedKey;
    materialization.renderCache = request.renderCache != nullptr ? std::move(request.renderCache)
                                                                  : std::make_shared<RenderCache>();
    materialization.notes = std::move(request.notes);
    materialization.silentGaps = std::move(request.silentGaps);

    const juce::ScopedWriteLock writeLock(lock_);
    const uint64_t materializationId = materialization.materializationId;
    if (forcedMaterializationId != 0) {
        nextMaterializationId_.store(juce::jmax(nextMaterializationId_.load(std::memory_order_relaxed), forcedMaterializationId + 1),
                                     std::memory_order_relaxed);
    }
    materializations_.emplace(materializationId, std::move(materialization));
    ChannelLayoutLog::logMaterializationCreate(static_cast<juce::int64>(materializationId),
                                                requestChannels);
    return materializationId;
}

void MaterializationStore::clear()
{
    {
        const juce::ScopedWriteLock writeLock(lock_);
        materializations_.clear();
        nextMaterializationId_.store(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> qLock(renderQueueMutex_);
        pendingRenderQueue_.clear();
    }
}

bool MaterializationStore::deleteMaterialization(uint64_t materializationId)
{
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedWriteLock writeLock(lock_);
    return materializations_.erase(materializationId) > 0;
}

bool MaterializationStore::containsMaterialization(uint64_t materializationId) const
{
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    return it != materializations_.end() && !it->second.isRetired_;
}

bool MaterializationStore::hasMaterializationForSource(uint64_t sourceId) const
{
    if (sourceId == 0) {
        return false;
    }

    const juce::ScopedReadLock readLock(lock_);
    const auto existing = std::find_if(materializations_.begin(),
                                       materializations_.end(),
                                       [sourceId](const auto& entry)
                                       {
                                           return entry.second.sourceId == sourceId && !entry.second.isRetired_;
                                       });
    return existing != materializations_.end();
}

bool MaterializationStore::hasMaterializationForSourceAnyState(uint64_t sourceId) const
{
    if (sourceId == 0) {
        return false;
    }

    const juce::ScopedReadLock readLock(lock_);
    const auto existing = std::find_if(materializations_.begin(),
                                       materializations_.end(),
                                       [sourceId](const auto& entry)
                                       {
                                           return entry.second.sourceId == sourceId;
                                       });
    return existing != materializations_.end();
}

bool MaterializationStore::retireMaterialization(uint64_t id)
{
    if (id == 0) return false;
    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(id);
    if (it == materializations_.end() || it->second.isRetired_) return false;
    it->second.isRetired_ = true;
    if (it->second.renderCache) {
        it->second.renderCache->clear();
    }
    return true;
}

bool MaterializationStore::reviveMaterialization(uint64_t id)
{
    if (id == 0) return false;
    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(id);
    if (it == materializations_.end() || !it->second.isRetired_) return false;
    it->second.isRetired_ = false;
    return true;
}

bool MaterializationStore::isRetired(uint64_t id) const
{
    if (id == 0) return false;
    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(id);
    return it != materializations_.end() && it->second.isRetired_;
}

bool MaterializationStore::physicallyDeleteIfReclaimable(uint64_t id)
{
    if (id == 0) return false;
    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(id);
    if (it == materializations_.end() || !it->second.isRetired_) return false;
    materializations_.erase(it);
    return true;
}

std::vector<uint64_t> MaterializationStore::getRetiredIds() const
{
    std::vector<uint64_t> ids;
    const juce::ScopedReadLock readLock(lock_);
    for (const auto& entry : materializations_) {
        if (entry.second.isRetired_) {
            ids.push_back(entry.first);
        }
    }
    return ids;
}

uint64_t MaterializationStore::getSourceIdAnyState(uint64_t id) const
{
    if (id == 0) return 0;
    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(id);
    return it != materializations_.end() ? it->second.sourceId : 0;
}

bool MaterializationStore::getAudioBuffer(uint64_t materializationId,
                                          std::shared_ptr<const juce::AudioBuffer<float>>& out) const
{
    out.reset();
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end() || it->second.isRetired_) {
        return false;
    }

    out = it->second.audioBuffer;
    return out != nullptr;
}

bool MaterializationStore::getPlaybackReadSource(uint64_t materializationId, PlaybackReadSource& out) const
{
    out = PlaybackReadSource{};
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end() || it->second.isRetired_) {
        return false;
    }

    out.renderCache = it->second.renderCache;
    out.audioBuffer = it->second.audioBuffer;
    return out.canRead();
}

bool MaterializationStore::getSnapshot(uint64_t materializationId, MaterializationSnapshot& out) const
{
    out = MaterializationSnapshot{};
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end() || it->second.isRetired_) {
        return false;
    }

    out.materializationId = it->second.materializationId;
    out.sourceId = it->second.sourceId;
    out.lineageParentMaterializationId = it->second.lineageParentMaterializationId;
    out.sourceWindow = it->second.sourceWindow;
    out.audioBuffer = it->second.audioBuffer;
    out.pitchCurve = it->second.pitchCurve;
    out.originalF0State = it->second.originalF0State;
    out.detectedKey = it->second.detectedKey;
    out.renderCache = it->second.renderCache;
    out.notes = it->second.notes;
    out.notesRevision = it->second.notesRevision;
    out.silentGaps = it->second.silentGaps;
    out.renderRevision = it->second.renderRevision;
    return true;
}

bool MaterializationStore::getRenderCache(uint64_t materializationId, std::shared_ptr<RenderCache>& out) const
{
    out.reset();
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end()) {
        return false;
    }

    out = it->second.renderCache;
    return out != nullptr;
}

bool MaterializationStore::getPitchCurve(uint64_t materializationId, std::shared_ptr<PitchCurve>& out) const
{
    out.reset();
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end()) {
        return false;
    }

    out = it->second.pitchCurve;
    return out != nullptr;
}

bool MaterializationStore::setPitchCurve(uint64_t materializationId, std::shared_ptr<PitchCurve> curve)
{
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end()) {
        return false;
    }

    it->second.pitchCurve = std::move(curve);
    it->second.originalF0State = (it->second.pitchCurve != nullptr && !it->second.pitchCurve->getSnapshot()->getOriginalF0().empty())
        ? OriginalF0State::Ready
        : OriginalF0State::NotRequested;
    return true;
}

bool MaterializationStore::commitNotesAndPitchCurve(uint64_t materializationId,
                                                    std::vector<Note> notes,
                                                    std::shared_ptr<PitchCurve> curve)
{
    if (materializationId == 0 || curve == nullptr) {
        return false;
    }

    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end()) {
        return false;
    }

    it->second.notes = std::move(notes);
    ++it->second.notesRevision;
    it->second.pitchCurve = std::move(curve);
    it->second.originalF0State = !it->second.pitchCurve->getSnapshot()->getOriginalF0().empty()
        ? OriginalF0State::Ready
        : OriginalF0State::NotRequested;
    return true;
}

OriginalF0State MaterializationStore::getOriginalF0State(uint64_t materializationId) const
{
    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    return it != materializations_.end() ? it->second.originalF0State : OriginalF0State::NotRequested;
}

bool MaterializationStore::setOriginalF0State(uint64_t materializationId, OriginalF0State state)
{
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end()) {
        return false;
    }

    it->second.originalF0State = state;
    return true;
}

DetectedKey MaterializationStore::getDetectedKey(uint64_t materializationId) const
{
    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    return it != materializations_.end() ? it->second.detectedKey : DetectedKey{};
}

bool MaterializationStore::setDetectedKey(uint64_t materializationId, const DetectedKey& key)
{
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end()) {
        return false;
    }

    it->second.detectedKey = key;
    return true;
}

std::vector<Note> MaterializationStore::getNotes(uint64_t materializationId) const
{
    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    return it != materializations_.end() ? it->second.notes : std::vector<Note>{};
}

bool MaterializationStore::getNotesSnapshot(uint64_t materializationId, MaterializationNotesSnapshot& out) const
{
    out = MaterializationNotesSnapshot{};
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end()) {
        return false;
    }

    out.notes = it->second.notes;
    out.notesRevision = it->second.notesRevision;
    return true;
}

bool MaterializationStore::setNotes(uint64_t materializationId, std::vector<Note> notes)
{
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end()) {
        return false;
    }

    it->second.notes = std::move(notes);
    ++it->second.notesRevision;
    return true;
}

bool MaterializationStore::setSilentGaps(uint64_t materializationId, std::vector<SilentGap> silentGaps)
{
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end()) {
        return false;
    }

    it->second.silentGaps = std::move(silentGaps);
    return true;
}

bool MaterializationStore::replaceAudio(uint64_t materializationId,
                                        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
                                        std::vector<SilentGap> silentGaps)
{
    if (materializationId == 0
        || audioBuffer == nullptr
        || audioBuffer->getNumChannels() <= 0
        || audioBuffer->getNumSamples() <= 0) {
        return false;
    }

    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end()) {
        return false;
    }

    it->second.audioBuffer = std::move(audioBuffer);
    it->second.silentGaps = std::move(silentGaps);
    it->second.detectedKey = DetectedKey{};
    it->second.originalF0State = OriginalF0State::NotRequested;
    // sourceWindow 不改变：replaceAudio 语义 = 换 audio buffer，lineage 不变
    return true;
}

uint64_t MaterializationStore::replaceMaterializationWithNewLineage(uint64_t oldId,
                                                                      CreateMaterializationRequest request)
{
    jassert(oldId != 0);
    if (oldId == 0
        || request.audioBuffer == nullptr
        || request.audioBuffer->getNumChannels() <= 0
        || request.audioBuffer->getNumSamples() <= 0) {
        return 0;
    }
    const int newRequestChannels = request.audioBuffer->getNumChannels();
    if (newRequestChannels < 1 || newRequestChannels > 2) {
        jassertfalse;
        ChannelLayoutLog::logMaterializationReject(newRequestChannels);
        return 0;
    }

    MaterializationEntry newEntry;
    newEntry.materializationId = nextMaterializationId_.fetch_add(1, std::memory_order_relaxed);
    newEntry.sourceId = request.sourceId;
    newEntry.lineageParentMaterializationId = request.lineageParentMaterializationId;
    newEntry.sourceWindow = request.sourceWindow;
    newEntry.renderRevision = request.renderRevision;
    newEntry.notesRevision = 1;
    newEntry.audioBuffer = std::move(request.audioBuffer);
    newEntry.pitchCurve = std::move(request.pitchCurve);
    newEntry.originalF0State = request.originalF0State;
    newEntry.detectedKey = request.detectedKey;
    newEntry.renderCache = request.renderCache != nullptr ? std::move(request.renderCache)
                                                          : std::make_shared<RenderCache>();
    newEntry.notes = std::move(request.notes);
    newEntry.silentGaps = std::move(request.silentGaps);

    const juce::ScopedWriteLock writeLock(lock_);
    jassert(materializations_.find(oldId) != materializations_.end());
    if (materializations_.find(oldId) == materializations_.end()) {
        return 0;
    }

    const uint64_t newId = newEntry.materializationId;
    materializations_.erase(oldId);
    materializations_.emplace(newId, std::move(newEntry));
    return newId;
}

void MaterializationStore::prepareAllCrossoverMixers(double sampleRate, int maxBlockSize)
{
    const juce::ScopedReadLock readLock(lock_);
    for (const auto& entry : materializations_) {
        if (entry.second.renderCache != nullptr) {
            entry.second.renderCache->prepareCrossoverMixer(sampleRate, maxBlockSize, 2);
        }
    }
}

bool MaterializationStore::enqueuePartialRender(uint64_t materializationId,
                                                double relStartSeconds,
                                                double relEndSeconds,
                                                int hopSize)
{
    if (materializationId == 0 || relEndSeconds <= relStartSeconds || hopSize <= 0) {
        return false;
    }

    // 第一步：在 ReadLock 内完成 RenderCache 操作，收集待入队 entry
    std::vector<PendingRenderEntry> entriesToQueue;

    {
        const juce::ScopedReadLock readLock(lock_);
        const auto it = materializations_.find(materializationId);
        if (it == materializations_.end() || it->second.audioBuffer == nullptr || it->second.renderCache == nullptr) {
            return false;
        }

        const auto chunkBoundaries = buildChunkBoundariesFromSilentGaps(it->second.audioBuffer->getNumSamples(),
                                                                         it->second.silentGaps,
                                                                         hopSize);
        const int64_t requestStartSample = TimeCoordinate::secondsToSamplesFloor(relStartSeconds, TimeCoordinate::kRenderSampleRate);
        const int64_t requestEndSampleExclusive = TimeCoordinate::secondsToSamplesCeil(relEndSeconds, TimeCoordinate::kRenderSampleRate);

        if (chunkBoundaries.size() < 2) {
            it->second.renderCache->requestRenderPending(relStartSeconds,
                                                          relEndSeconds,
                                                          requestStartSample,
                                                          requestEndSampleExclusive);
            entriesToQueue.push_back({materializationId, relStartSeconds, relEndSeconds, requestStartSample, requestEndSampleExclusive});
        } else {
            for (size_t i = 0; i + 1 < chunkBoundaries.size(); ++i) {
                const int64_t chunkStartSample = chunkBoundaries[i];
                const int64_t chunkEndSampleExclusive = chunkBoundaries[i + 1];
                const int64_t overlapStart = std::max(requestStartSample, chunkStartSample);
                const int64_t overlapEnd = std::min(requestEndSampleExclusive, chunkEndSampleExclusive);
                if (overlapEnd <= overlapStart) {
                    continue;
                }

                const double chunkStartSeconds = TimeCoordinate::samplesToSeconds(chunkStartSample, TimeCoordinate::kRenderSampleRate);
                const double chunkEndSeconds = TimeCoordinate::samplesToSeconds(chunkEndSampleExclusive, TimeCoordinate::kRenderSampleRate);
                it->second.renderCache->requestRenderPending(chunkStartSeconds,
                                                              chunkEndSeconds,
                                                              chunkStartSample,
                                                              chunkEndSampleExclusive);
                entriesToQueue.push_back({materializationId, chunkStartSeconds, chunkEndSeconds, chunkStartSample, chunkEndSampleExclusive});
            }
        }
    }

    // 第二步：锁外批量入队（renderQueueMutex_ 不嵌套在 ReadLock 内）
    if (!entriesToQueue.empty()) {
        std::lock_guard<std::mutex> qLock(renderQueueMutex_);
        for (auto& e : entriesToQueue) {
            pendingRenderQueue_.push_back(std::move(e));
        }
    }

    return !entriesToQueue.empty();
}

bool MaterializationStore::hasPendingRenderJobs() const
{
    std::lock_guard<std::mutex> qLock(renderQueueMutex_);
    return !pendingRenderQueue_.empty();
}

bool MaterializationStore::pullNextPendingRenderJob(PendingRenderJob& out)
{
    out = PendingRenderJob{};

    // 第一步：从 queue 取出 entry
    PendingRenderEntry entry;
    {
        std::lock_guard<std::mutex> qLock(renderQueueMutex_);
        if (pendingRenderQueue_.empty()) return false;
        entry = pendingRenderQueue_.front();
        pendingRenderQueue_.pop_front();
    }

    // 第二步：ReadLock 查找 materialization 并拉取 RenderCache pending job
    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(entry.materializationId);
    if (it == materializations_.end() || it->second.isRetired_) return false;

    auto& mat = it->second;
    if (!mat.renderCache) return false;

    RenderCache::PendingJob pendingJob;
    if (!mat.renderCache->getNextPendingJob(pendingJob)) return false;

    out.materializationId = mat.materializationId;
    out.renderCache = mat.renderCache;
    out.audioBuffer = mat.audioBuffer;
    out.pitchCurve = mat.pitchCurve;
    out.silentGaps = mat.silentGaps;
    out.startSeconds = pendingJob.startSeconds;
    out.endSeconds = pendingJob.endSeconds;
    out.startSample = pendingJob.startSample;
    out.endSampleExclusive = pendingJob.endSampleExclusive;
    out.targetRevision = pendingJob.targetRevision;
    return true;
}

double MaterializationStore::getMaterializationAudioDurationById(uint64_t materializationId) const noexcept
{
    if (materializationId == 0) return 0.0;
    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end() || it->second.isRetired_) return 0.0;
    if (it->second.audioBuffer == nullptr) return 0.0;
    return TimeCoordinate::samplesToSeconds(it->second.audioBuffer->getNumSamples(),
                                            TimeCoordinate::kRenderSampleRate);
}

uint64_t MaterializationStore::findMaterializationBySourceWindow(uint64_t sourceId, const SourceWindow& window) const
{
    if (sourceId == 0) return 0;
    const juce::ScopedReadLock readLock(lock_);
    for (const auto& [id, entry] : materializations_) {
        if (entry.isRetired_) continue;
        if (entry.sourceId == sourceId
            && std::abs(entry.sourceWindow.sourceStartSeconds - window.sourceStartSeconds) < 0.001
            && std::abs(entry.sourceWindow.sourceEndSeconds - window.sourceEndSeconds) < 0.001) {
            return id;
        }
    }
    return 0;
}

std::vector<int64_t> MaterializationStore::buildChunkBoundariesFromSilentGaps(int64_t materializationSampleCount,
                                                                               const std::vector<SilentGap>& silentGaps,
                                                                               int hopSize)
{
    std::vector<int64_t> boundaries;
    if (materializationSampleCount <= 0 || hopSize <= 0) {
        return boundaries;
    }

    boundaries.reserve(silentGaps.size() + 2);
    boundaries.push_back(0);

    for (const auto& gap : silentGaps) {
        int64_t splitSample = 0;
        if (!findPreferredHopAlignedBoundarySample(gap, hopSize, splitSample)) {
            continue;
        }

        if (splitSample <= boundaries.back() || splitSample >= materializationSampleCount) {
            continue;
        }

        boundaries.push_back(splitSample);
    }

    if (boundaries.back() != materializationSampleCount) {
        boundaries.push_back(materializationSampleCount);
    }

    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    return boundaries;
}

} // namespace OpenTune
