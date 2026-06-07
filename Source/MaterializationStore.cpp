#include "MaterializationStore.h"

#include <algorithm>
#include <cmath>

#include "Utils/TimeCoordinate.h"
#include "Utils/ChannelLayoutLogger.h"
#include "Inference/SoundTouchStretcher.h"   // §5.5 — needed for ~unique_ptr<ST> + lazy construction

namespace OpenTune {

namespace {

constexpr double kMaxRenderChunkDurationSeconds = 15.0;
constexpr int64_t kMaxRenderChunkSamples = static_cast<int64_t>(
    kMaxRenderChunkDurationSeconds * TimeCoordinate::kRenderSampleRate);

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

    // §3.6/3.7: Auto-seed identity TimeGrid if request didn't provide one.
    // The duration matches audioBuffer length at kRenderSampleRate.
    if (request.timeGrid != nullptr) {
        materialization.timeGrid = std::move(request.timeGrid);
    } else {
        const double durationSec = TimeCoordinate::samplesToSeconds(
            materialization.audioBuffer->getNumSamples(),
            TimeCoordinate::kRenderSampleRate);
        materialization.timeGrid = TimeGridSnapshot::makeIdentity(durationSec);
    }
    materialization.timeGridRevision = 1;

    const juce::ScopedWriteLock writeLock(lock_);
    const uint64_t materializationId = materialization.materializationId;
    if (forcedMaterializationId != 0) {
        nextMaterializationId_.store(juce::jmax(nextMaterializationId_.load(std::memory_order_relaxed), forcedMaterializationId + 1),
                                     std::memory_order_relaxed);
    }
    materializations_.emplace(materializationId, std::move(materialization));
    rebuildPlaybackSourceCache();
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
        rebuildPlaybackSourceCache();
    }
    if (contentRenderService_ != nullptr)
        contentRenderService_->drainRenderWorker();
}

bool MaterializationStore::deleteMaterialization(uint64_t materializationId)
{
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedWriteLock writeLock(lock_);
    const bool erased = materializations_.erase(materializationId) > 0;
    if (erased) {
        rebuildPlaybackSourceCache();
    }
    return erased;
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
    rebuildPlaybackSourceCache();
    return true;
}

bool MaterializationStore::reviveMaterialization(uint64_t id)
{
    if (id == 0) return false;
    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(id);
    if (it == materializations_.end() || !it->second.isRetired_) return false;
    it->second.isRetired_ = false;
    rebuildPlaybackSourceCache();
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
    rebuildPlaybackSourceCache();
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

void MaterializationStore::rebuildPlaybackSourceCache()
{
    // Caller MUST hold write lock on lock_.
    auto cache = std::make_shared<std::map<uint64_t, PlaybackReadSource>>();
    for (auto& [id, entry] : materializations_)
    {
        if (entry.isRetired_)
            continue;

        PlaybackReadSource src;
        src.renderCache = entry.renderCache;
        src.audioBuffer = entry.audioBuffer;
        src.materializationId = id;
        src.timeGridRevision = static_cast<uint32_t>(entry.timeGridRevision);
        src.pitchRevision = static_cast<uint32_t>(entry.pitchShiftRevision);
        src.pitchShiftSettings = entry.pitchShiftSettings;
        src.timeGridIsIdentity = (entry.timeGrid == nullptr) || entry.timeGrid->isIdentity();
        cache->emplace(id, std::move(src));
    }
    std::atomic_store(&playbackSourceCache_,
                      std::shared_ptr<const std::map<uint64_t, PlaybackReadSource>>(std::move(cache)));
}

bool MaterializationStore::getPlaybackReadSource(uint64_t materializationId, PlaybackReadSource& out) const
{
    out = PlaybackReadSource{};
    if (materializationId == 0)
        return false;

    // When CRS is attached, delegate to ContentRenderService
    // using a ContentKey built from the materializationId.
    if (contentRenderService_ != nullptr)
    {
        ContentRenderService::PlaybackReadSource crsSource;
        ContentKey ck;
        ck.objectId = materializationId;
        if (contentRenderService_->getPlaybackReadSource(ck, crsSource))
        {
            // Map back to MaterializationStore::PlaybackReadSource for compat
            out.renderCache = crsSource.renderCache;
            out.audioBuffer = crsSource.audioBuffer;
            out.timeStretchCache = crsSource.timeStretchCache;
            out.materializationId = materializationId;
            out.pitchRevision = crsSource.pitchRevision;
            out.timeGridRevision = crsSource.timeGridRevision;
            out.pitchShiftSettings = crsSource.pitchShiftSettings;
            out.timeGridIsIdentity = crsSource.timeGridIsIdentity;
            return out.canRead();
        }
        return false;
    }

    // Lock-free read: atomic_load immutable snapshot, zero blocking on audio thread.
    auto snap = std::atomic_load(&playbackSourceCache_);
    if (!snap)
        return false;

    auto it = snap->find(materializationId);
    if (it == snap->end())
        return false;

    out = it->second;
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
    out.timeGrid = it->second.timeGrid;
    out.timeGridRevision = it->second.timeGridRevision;
    out.pitchShiftSettings = it->second.pitchShiftSettings;
    out.pitchShiftRevision = it->second.pitchShiftRevision;
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
    // Reference AUTO feature cache is derived from source audio + original F0.
    it->second.referenceFeatures.reset();
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
    // Notes alone do not invalidate reference features, but pitch-curve/original-F0
    // changes do.
    it->second.referenceFeatures.reset();
    return true;
}

bool MaterializationStore::commitReferenceAlignmentPatch(
    uint64_t materializationId,
    std::vector<Note> notesAfter,
    std::shared_ptr<PitchCurve> pitchCurveAfter,
    std::shared_ptr<const TimeGridSnapshot> timeGridAfter)
{
    if (materializationId == 0 || pitchCurveAfter == nullptr || timeGridAfter == nullptr) {
        return false;
    }

    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end() || it->second.isRetired_) {
        return false;
    }

    it->second.notes = std::move(notesAfter);
    ++it->second.notesRevision;
    it->second.pitchCurve = std::move(pitchCurveAfter);
    it->second.originalF0State = !it->second.pitchCurve->getSnapshot()->getOriginalF0().empty()
        ? OriginalF0State::Ready
        : OriginalF0State::NotRequested;
    it->second.timeGrid = std::move(timeGridAfter);
    ++it->second.timeGridRevision;

    rebuildPlaybackSourceCache();
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

// ============================================================================
// vocal-time-stretch §3.6 — TimeGrid accessors
// ============================================================================

bool MaterializationStore::getTimeGrid(uint64_t materializationId,
                                       std::shared_ptr<const TimeGridSnapshot>& outSnapshot) const
{
    outSnapshot.reset();
    if (materializationId == 0) return false;

    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end() || it->second.isRetired_) return false;

    outSnapshot = it->second.timeGrid;
    return outSnapshot != nullptr;
}

uint64_t MaterializationStore::getTimeGridRevision(uint64_t materializationId) const
{
    if (materializationId == 0) return 0;
    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end() || it->second.isRetired_) return 0;
    return it->second.timeGridRevision;
}

bool MaterializationStore::setTimeGrid(uint64_t materializationId,
                                       std::shared_ptr<const TimeGridSnapshot> snapshot,
                                       int64_t /*affectedSrcStartFrame*/,
                                       int64_t /*affectedSrcEndFrame*/)
{
    if (materializationId == 0 || snapshot == nullptr) return false;

    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end() || it->second.isRetired_) return false;

    it->second.timeGrid = std::move(snapshot);
    ++it->second.timeGridRevision;

    rebuildPlaybackSourceCache();
    getTimeStretchCache().invalidate(materializationId);
    return true;
}

// ============================================================================
// Pitch Shift — clip-level render modifier
// ============================================================================

PitchShiftSettings MaterializationStore::getPitchShiftSettings(uint64_t materializationId) const
{
    if (materializationId == 0) return PitchShiftSettings::identity();
    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end() || it->second.isRetired_) return PitchShiftSettings::identity();
    return it->second.pitchShiftSettings;
}

uint64_t MaterializationStore::getPitchShiftRevision(uint64_t materializationId) const
{
    if (materializationId == 0) return 0;
    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end() || it->second.isRetired_) return 0;
    return it->second.pitchShiftRevision;
}

bool MaterializationStore::setPitchShiftSettings(uint64_t materializationId, const PitchShiftSettings& settings)
{
    if (materializationId == 0) return false;

    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end() || it->second.isRetired_) return false;

    if (it->second.pitchShiftSettings == settings) return true;  // no-op

    it->second.pitchShiftSettings = settings;
    ++it->second.pitchShiftRevision;

    // Invalidate Stage 1 RenderCache — all chunks need re-render with new F0 offset
    if (it->second.renderCache != nullptr) {
        it->second.renderCache->clear();
    }

    rebuildPlaybackSourceCache();
    return true;
}

// ============================================================================
// vocal-time-stretch §5.5 — lazy SoundTouchStretcher accessor
// (replaces the archived phase-vocoder stretcher; see swap-time-stretch-to-soundtouch change)
// ============================================================================

SoundTouchStretcher* MaterializationStore::getOpenTuneStretcher(uint64_t materializationId,
                                                                   double sampleRate,
                                                                   int channels)
{
    if (materializationId == 0 || sampleRate <= 0.0 || channels <= 0) return nullptr;

    // Fast path: read lock + check existing
    {
        const juce::ScopedReadLock readLock(lock_);
        const auto it = materializations_.find(materializationId);
        if (it == materializations_.end() || it->second.isRetired_) return nullptr;
        if (it->second.stretcher != nullptr) return it->second.stretcher.get();
    }

    // Slow path: write lock + lazy construct
    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end() || it->second.isRetired_) return nullptr;
    if (it->second.stretcher == nullptr) {
        it->second.stretcher = std::make_unique<SoundTouchStretcher>(sampleRate, channels);
    }
    return it->second.stretcher.get();
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
    it->second.referenceFeatures.reset();  // audio buffer changed -> feature cache stale
    // sourceWindow 不改变：replaceAudio 语义 = 换 audio buffer，lineage 不变
    rebuildPlaybackSourceCache();
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

    // §3.6: carry TimeGrid forward; auto-seed identity if absent.
    if (request.timeGrid != nullptr) {
        newEntry.timeGrid = std::move(request.timeGrid);
    } else {
        const double durationSec = TimeCoordinate::samplesToSeconds(
            newEntry.audioBuffer->getNumSamples(),
            TimeCoordinate::kRenderSampleRate);
        newEntry.timeGrid = TimeGridSnapshot::makeIdentity(durationSec);
    }
    newEntry.timeGridRevision = 1;

    const juce::ScopedWriteLock writeLock(lock_);
    jassert(materializations_.find(oldId) != materializations_.end());
    if (materializations_.find(oldId) == materializations_.end()) {
        return 0;
    }

    const uint64_t newId = newEntry.materializationId;
    materializations_.erase(oldId);
    materializations_.emplace(newId, std::move(newEntry));
    rebuildPlaybackSourceCache();
    return newId;
}


bool MaterializationStore::enqueuePartialRender(uint64_t materializationId,
                                                double relStartSeconds,
                                                double relEndSeconds,
                                                int /*hopSize*/)
{
    if (contentRenderService_ == nullptr) return false;
    if (materializationId == 0 || relEndSeconds <= relStartSeconds) return false;

    ContentRenderService::PendingRenderJob job;
    job.contentKey = ContentKey{DomainKind::StandaloneClip, materializationId, 0};
    job.startSeconds = relStartSeconds;
    job.endSeconds = relEndSeconds;

    // 从 store snapshot 提取 pitchCurve，确保 processChunkRenderJob 非空
    {
        const juce::ScopedReadLock readLock(lock_);
        const auto it = materializations_.find(materializationId);
        if (it != materializations_.end() && !it->second.isRetired_) {
            job.pitchCurve = it->second.pitchCurve;
            job.renderCache = it->second.renderCache;
            job.audioBuffer = it->second.audioBuffer;
        }
    }

    contentRenderService_->enqueueRender(std::move(job));
    return true;
}

bool MaterializationStore::hasPendingRenderJobs() const
{
    if (contentRenderService_ != nullptr)
        return contentRenderService_->hasPendingJobs();
    return false;
}

bool MaterializationStore::pullNextPendingRenderJob(PendingRenderJob& out)
{
    out = PendingRenderJob{};
    if (contentRenderService_ == nullptr)
        return false;

    ContentRenderService::PendingRenderJob crsJob;
    if (!contentRenderService_->pullNextPendingRenderJob(crsJob))
        return false;

    out.materializationId = crsJob.contentKey.objectId;
    out.renderCache = crsJob.renderCache;
    out.audioBuffer = crsJob.audioBuffer;
    out.pitchCurve = crsJob.pitchCurve;
    out.silentGaps = crsJob.silentGaps;
    out.startSeconds = crsJob.startSeconds;
    out.endSeconds = crsJob.endSeconds;
    out.startSample = crsJob.startSample;
    out.endSampleExclusive = crsJob.endSampleExclusive;
    out.targetRevision = crsJob.targetRevision;
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

    std::vector<int64_t> anchorBoundaries;
    anchorBoundaries.reserve(silentGaps.size() + 2);
    anchorBoundaries.push_back(0);

    for (const auto& gap : silentGaps) {
        int64_t splitSample = 0;
        if (!findPreferredHopAlignedBoundarySample(gap, hopSize, splitSample)) {
            continue;
        }

        if (splitSample <= anchorBoundaries.back() || splitSample >= materializationSampleCount) {
            continue;
        }

        anchorBoundaries.push_back(splitSample);
    }

    if (anchorBoundaries.back() != materializationSampleCount) {
        anchorBoundaries.push_back(materializationSampleCount);
    }

    std::sort(anchorBoundaries.begin(), anchorBoundaries.end());
    anchorBoundaries.erase(std::unique(anchorBoundaries.begin(), anchorBoundaries.end()), anchorBoundaries.end());

    boundaries.reserve(anchorBoundaries.size() + static_cast<size_t>(materializationSampleCount / kMaxRenderChunkSamples) + 1);
    boundaries.push_back(anchorBoundaries.front());

    for (size_t i = 0; i + 1 < anchorBoundaries.size(); ++i) {
        const int64_t anchorStart = anchorBoundaries[i];
        const int64_t anchorEnd = anchorBoundaries[i + 1];

        int64_t chunkStart = anchorStart;
        while ((anchorEnd - chunkStart) > kMaxRenderChunkSamples) {
            int64_t splitSample = ((chunkStart + kMaxRenderChunkSamples) / hopSize) * hopSize;
            if (splitSample <= chunkStart) {
                splitSample = ((chunkStart / hopSize) + 1) * static_cast<int64_t>(hopSize);
            }

            if (splitSample >= anchorEnd) {
                break;
            }

            boundaries.push_back(splitSample);
            chunkStart = splitSample;
        }

        if (boundaries.back() != anchorEnd) {
            boundaries.push_back(anchorEnd);
        }
    }

    return boundaries;
}

std::vector<uint64_t> MaterializationStore::getAllActiveMaterializationIds() const
{
    std::vector<uint64_t> ids;
    const juce::ScopedReadLock readLock(lock_);
    ids.reserve(materializations_.size());
    for (const auto& entry : materializations_) {
        if (!entry.second.isRetired_) {
            ids.push_back(entry.first);
        }
    }
    return ids;
}

std::vector<MaterializationStore::MaterializationSnapshot> MaterializationStore::getAllActiveMaterializationSnapshots() const
{
    const auto ids = getAllActiveMaterializationIds();
    std::vector<MaterializationSnapshot> snapshots;
    snapshots.reserve(ids.size());
    for (auto id : ids) {
        MaterializationSnapshot snap;
        if (getSnapshot(id, snap)) {
            snapshots.push_back(snap);
        }
    }
    return snapshots;
}

int MaterializationStore::getTotalCount() const
{
    const juce::ScopedReadLock readLock(lock_);
    return static_cast<int>(materializations_.size());
}

// ============================================================================
// Reference feature cache API
// ============================================================================

bool MaterializationStore::setReferenceFeatures(uint64_t materializationId, const ReferenceFeatureSet& features)
{
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end() || it->second.isRetired_) {
        return false;
    }

    const int previousRevision = it->second.referenceFeatures.analysisRevision;
    const int nextRevision = juce::jmax(previousRevision + 1,
                                        features.analysisRevision > 0 ? features.analysisRevision : 1);
    it->second.referenceFeatures = features;
    it->second.referenceFeatures.analysisRevision = nextRevision;
    return true;
}

bool MaterializationStore::getReferenceFeatures(uint64_t materializationId, ReferenceFeatureSet& out) const
{
    out = ReferenceFeatureSet{};
    if (materializationId == 0) {
        return false;
    }

    const juce::ScopedReadLock readLock(lock_);
    const auto it = materializations_.find(materializationId);
    if (it == materializations_.end() || it->second.isRetired_) {
        return false;
    }

    out = it->second.referenceFeatures;
    return out.status != ReferenceFeatureStatus::NotRequested || out.analysisRevision > 0;
}

// ============================================================================
// Render Worker — delegated to ContentRenderService
// ============================================================================

void MaterializationStore::setRenderJobCallback(std::function<void(PendingRenderJob&)> cb)
{
    if (contentRenderService_ == nullptr)
        return;

    ContentRenderService::ExecutionLease lease;
    lease.renderJobCallback = [cb = std::move(cb)](ContentRenderService::PendingRenderJob& crsJob)
    {
        MaterializationStore::PendingRenderJob oldJob;
        oldJob.materializationId = crsJob.contentKey.objectId;
        oldJob.renderCache = crsJob.renderCache;
        oldJob.audioBuffer = crsJob.audioBuffer;
        oldJob.pitchCurve = crsJob.pitchCurve;
        oldJob.silentGaps = crsJob.silentGaps;
        oldJob.startSeconds = crsJob.startSeconds;
        oldJob.endSeconds = crsJob.endSeconds;
        oldJob.startSample = crsJob.startSample;
        oldJob.endSampleExclusive = crsJob.endSampleExclusive;
        oldJob.targetRevision = crsJob.targetRevision;
        cb(oldJob);
    };
    lease.leaseOwner = this;
    contentRenderService_->attachExecutionLease(std::move(lease));
}

void MaterializationStore::notifyRenderWorker()
{
    if (contentRenderService_ != nullptr)
        contentRenderService_->notifyRenderWorker();
}

void MaterializationStore::pauseRenderWorker()
{
    if (contentRenderService_ != nullptr)
        contentRenderService_->pauseRenderWorker();
}

void MaterializationStore::resumeRenderWorker()
{
    if (contentRenderService_ != nullptr)
        contentRenderService_->resumeRenderWorker();
}

void MaterializationStore::drainRenderWorker()
{
    if (contentRenderService_ != nullptr)
        contentRenderService_->drainRenderWorker();
}

TimeStretchCache& MaterializationStore::getTimeStretchCache() noexcept
{
    if (contentRenderService_ != nullptr)
        return contentRenderService_->getTimeStretchCache();
    static TimeStretchCache fallback;
    return fallback;
}

const TimeStretchCache& MaterializationStore::getTimeStretchCache() const noexcept
{
    if (contentRenderService_ != nullptr)
        return contentRenderService_->getTimeStretchCache();
    static TimeStretchCache fallback;
    return fallback;
}

} // namespace OpenTune
