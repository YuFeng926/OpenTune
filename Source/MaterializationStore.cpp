#include "MaterializationStore.h"

#include <algorithm>
#include <cmath>

#include "Utils/TimeCoordinate.h"
#include "Utils/ChannelLayoutLogger.h"

namespace OpenTune {

// ============================================================
// Helpers (private)
// ============================================================

ContentKey MaterializationStore::contentKeyForMaterializationId(uint64_t id) noexcept
{
    return ContentKey{DomainKind::StandaloneClip, id, 0};
}

bool MaterializationStore::hasRuntimeServices() const noexcept
{
    return contentRenderService_ != nullptr;
}

MaterializationStore::PlaybackReadSource MaterializationStore::makePlaybackReadSourceForEntry(
    ContentKey key, const MaterializationEntry& entry) const
{
    PlaybackReadSource source;
    source.contentKey = key;
    source.renderCache = hasRuntimeServices()
        ? contentRenderService_->getRenderCache(key)
        : nullptr;
    source.audioBuffer = entry.audioBuffer;
    source.timeStretchCache = hasRuntimeServices()
        ? &contentRenderService_->getTimeStretchCache()
        : nullptr;
    source.renderRevision = entry.renderRevision;
    source.pitchRevision = entry.pitchShiftRevision;
    source.pitchShiftRevision = entry.pitchShiftRevision;
    source.timeGridRevision = entry.timeGridRevision;
    source.pitchShiftSettings = entry.pitchShiftSettings;
    source.timeGridIsIdentity = (entry.timeGrid == nullptr) || entry.timeGrid->isIdentity();
    return source;
}

void MaterializationStore::publishPlaybackSourceForEntry(uint64_t id, const MaterializationEntry& entry)
{
    if (!hasRuntimeServices() || entry.isRetired_)
        return;
    const auto key = contentKeyForMaterializationId(id);
    contentRenderService_->publishPlaybackSource(key, makePlaybackReadSourceForEntry(key, entry));
}

void MaterializationStore::removeRuntimeForMaterialization(uint64_t id)
{
    if (!hasRuntimeServices())
        return;
    const auto key = contentKeyForMaterializationId(id);
    contentRenderService_->removePlaybackSource(key);
    contentRenderService_->removeRenderCache(key);
    contentRenderService_->removeStretcher(key);
    contentRenderService_->getTimeStretchCache().invalidate(key);
}

// ============================================================
// Construction
// ============================================================

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
    materialization.notes = std::move(request.notes);
    materialization.silentGaps = std::move(request.silentGaps);

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
    auto [it, _] = materializations_.emplace(materializationId, std::move(materialization));

    // Phase 0.7: RenderCache 由 CRS 管理
    if (hasRuntimeServices())
    {
        const auto key = contentKeyForMaterializationId(materializationId);
        auto rc = request.renderCache != nullptr ? request.renderCache
                                                  : std::make_shared<RenderCache>();
        contentRenderService_->renderCaches().put(key, rc);
        publishPlaybackSourceForEntry(materializationId, it->second);
    }

    ChannelLayoutLog::logMaterializationCreate(static_cast<juce::int64>(materializationId),
                                                requestChannels);
    return materializationId;
}

void MaterializationStore::clear()
{
    {
        const juce::ScopedWriteLock writeLock(lock_);
        for (const auto& [id, entry] : materializations_)
        {
            if (!entry.isRetired_)
                removeRuntimeForMaterialization(id);
        }
        materializations_.clear();
        nextMaterializationId_.store(1, std::memory_order_relaxed);
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
        removeRuntimeForMaterialization(materializationId);
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

    // Phase 0.7: invalidate RenderCache + remove runtime
    if (hasRuntimeServices())
    {
        const auto key = contentKeyForMaterializationId(id);
        contentRenderService_->renderCaches().invalidate(key);
    }
    removeRuntimeForMaterialization(id);
    return true;
}

bool MaterializationStore::reviveMaterialization(uint64_t id)
{
    if (id == 0) return false;
    const juce::ScopedWriteLock writeLock(lock_);
    const auto it = materializations_.find(id);
    if (it == materializations_.end() || !it->second.isRetired_) return false;
    it->second.isRetired_ = false;
    publishPlaybackSourceForEntry(id, it->second);
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
    removeRuntimeForMaterialization(id);
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
    if (materializationId == 0 || !hasRuntimeServices())
        return false;
    return contentRenderService_->getPlaybackReadSource(contentKeyForMaterializationId(materializationId), out);
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
    out.notes = it->second.notes;
    out.notesRevision = it->second.notesRevision;
    out.silentGaps = it->second.silentGaps;
    out.renderRevision = it->second.renderRevision;
    out.timeGrid = it->second.timeGrid;
    out.timeGridRevision = it->second.timeGridRevision;
    out.pitchShiftSettings = it->second.pitchShiftSettings;
    out.pitchShiftRevision = it->second.pitchShiftRevision;

    // Phase 0.7: RenderCache fetched from CRS, not entry
    out.renderCache = hasRuntimeServices()
        ? contentRenderService_->getRenderCache(contentKeyForMaterializationId(materializationId))
        : nullptr;
    return true;
}

bool MaterializationStore::getRenderCache(uint64_t materializationId, std::shared_ptr<RenderCache>& out) const
{
    out.reset();
    if (materializationId == 0) {
        return false;
    }

    {
        const juce::ScopedReadLock readLock(lock_);
        const auto it = materializations_.find(materializationId);
        if (it == materializations_.end()) {
            return false;
        }
    }

    if (hasRuntimeServices())
        out = contentRenderService_->getRenderCache(contentKeyForMaterializationId(materializationId));
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

    const bool timeGridChanged = (it->second.timeGridRevision == 0
                                  || it->second.timeGrid != timeGridAfter);

    it->second.notes = std::move(notesAfter);
    ++it->second.notesRevision;
    it->second.pitchCurve = std::move(pitchCurveAfter);
    it->second.originalF0State = !it->second.pitchCurve->getSnapshot()->getOriginalF0().empty()
        ? OriginalF0State::Ready
        : OriginalF0State::NotRequested;
    it->second.timeGrid = std::move(timeGridAfter);
    ++it->second.timeGridRevision;

    publishPlaybackSourceForEntry(materializationId, it->second);

    if (timeGridChanged && hasRuntimeServices())
    {
        const auto key = contentKeyForMaterializationId(materializationId);
        contentRenderService_->getTimeStretchCache().invalidate(key);
    }

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
// TimeGrid accessors
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

    publishPlaybackSourceForEntry(materializationId, it->second);

    if (hasRuntimeServices())
    {
        const auto key = contentKeyForMaterializationId(materializationId);
        contentRenderService_->getTimeStretchCache().invalidate(key);
    }

    return true;
}

// ============================================================================
// Pitch Shift
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

    if (it->second.pitchShiftSettings == settings) return true;

    it->second.pitchShiftSettings = settings;
    ++it->second.pitchShiftRevision;

    // Phase 0.7: invalidate caches via CRS
    if (hasRuntimeServices())
    {
        const auto key = contentKeyForMaterializationId(materializationId);
        contentRenderService_->renderCaches().invalidate(key);
        contentRenderService_->getTimeStretchCache().invalidate(key);
    }

    publishPlaybackSourceForEntry(materializationId, it->second);
    return true;
}

// ============================================================================
// Stretcher (delegates to StretcherPool via CRS)
// ============================================================================

SoundTouchStretcher* MaterializationStore::getOpenTuneStretcher(uint64_t materializationId,
                                                                   double sampleRate,
                                                                   int channels)
{
    if (materializationId == 0 || sampleRate <= 0.0 || channels <= 0) return nullptr;

    {
        const juce::ScopedReadLock readLock(lock_);
        const auto it = materializations_.find(materializationId);
        if (it == materializations_.end() || it->second.isRetired_) return nullptr;
    }

    return hasRuntimeServices()
        ? contentRenderService_->getStretcher(contentKeyForMaterializationId(materializationId), sampleRate, channels)
        : nullptr;
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
    it->second.referenceFeatures.reset();

    // Phase 0.7: invalidate caches via CRS
    if (hasRuntimeServices())
    {
        const auto key = contentKeyForMaterializationId(materializationId);
        contentRenderService_->renderCaches().invalidate(key);
        contentRenderService_->getTimeStretchCache().invalidate(key);
    }

    publishPlaybackSourceForEntry(materializationId, it->second);
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
    newEntry.notes = std::move(request.notes);
    newEntry.silentGaps = std::move(request.silentGaps);

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
    auto [it, _] = materializations_.emplace(newId, std::move(newEntry));

    // Phase 0.7: cleanup old, register new in CRS
    removeRuntimeForMaterialization(oldId);
    if (hasRuntimeServices())
    {
        const auto newKey = contentKeyForMaterializationId(newId);
        auto rc = request.renderCache != nullptr ? request.renderCache
                                                  : std::make_shared<RenderCache>();
        contentRenderService_->renderCaches().put(newKey, rc);
        publishPlaybackSourceForEntry(newId, it->second);
    }

    return newId;
}


bool MaterializationStore::enqueuePartialRender(uint64_t materializationId,
                                                double relStartSeconds,
                                                double relEndSeconds,
                                                int /*hopSize*/)
{
    if (contentRenderService_ == nullptr) return false;
    if (materializationId == 0 || relEndSeconds <= relStartSeconds) return false;

    RenderJob job;
    job.contentKey = contentKeyForMaterializationId(materializationId);
    job.startSeconds = relStartSeconds;
    job.endSeconds = relEndSeconds;

    {
        const juce::ScopedReadLock readLock(lock_);
        const auto it = materializations_.find(materializationId);
        if (it != materializations_.end() && !it->second.isRetired_) {
            job.pitchCurve = it->second.pitchCurve;
            job.audioBuffer = it->second.audioBuffer;
            // Phase 0.7: RenderCache from CRS, revisions from entry
            job.renderCache = contentRenderService_->getOrCreateRenderCache(job.contentKey);
            job.targetRevision = it->second.renderRevision;
            job.renderRevision = it->second.renderRevision;
            job.pitchRevision = it->second.pitchShiftRevision;
            job.pitchShiftRevision = it->second.pitchShiftRevision;
            job.timeGridRevision = it->second.timeGridRevision;
        }
    }

    contentRenderService_->enqueueRender(std::move(job));
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

std::vector<int64_t> MaterializationStore::buildChunkBoundariesFromSilentGaps(
    int64_t materializationSampleCount,
    const std::vector<SilentGap>& silentGaps,
    int hopSize)
{
    return RenderChunkPlanner::buildChunkBoundariesFromSilentGaps(materializationSampleCount, silentGaps, hopSize);
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
// Render Worker delegation
// ============================================================================

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

TimeStretchCache& MaterializationStore::getTimeStretchCache() noexcept
{
    jassert(contentRenderService_ != nullptr);
    return contentRenderService_->getTimeStretchCache();
}

const TimeStretchCache& MaterializationStore::getTimeStretchCache() const noexcept
{
    jassert(contentRenderService_ != nullptr);
    return contentRenderService_->getTimeStretchCache();
}

} // namespace OpenTune
