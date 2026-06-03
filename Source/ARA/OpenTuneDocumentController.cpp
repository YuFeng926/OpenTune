#include "OpenTuneDocumentController.h"

#include "OpenTuneEditorView.h"
#include "OpenTunePlaybackRenderer.h"
#include "PluginProcessor.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace OpenTune {

namespace {

struct ArchivedMaterializationBindingRecord
{
    juce::String audioModificationPersistentId;
    SourceWindow contentWindow;
    uint64_t sourceId{0};
    uint64_t materializationId{0};
    uint64_t materializationRevision{0};
    double materializationDurationSeconds{0.0};

    bool isValid() const noexcept
    {
        return audioModificationPersistentId.isNotEmpty()
            && sourceId != 0
            && materializationId != 0
            && contentWindow.isValid()
            && materializationDurationSeconds > 0.0;
    }
};

constexpr int kMaterializationBindingArchiveMagic = 0x4F544142;
constexpr int kMaterializationBindingArchiveVersion = 1;
constexpr int kMaxMaterializationBindingRecords = 4096;

juce::String mapRestoredAudioModificationPersistentId(const juce::String& archivedPersistentId,
                                                      const juce::ARARestoreObjectsFilter* filter)
{
    if (archivedPersistentId.isEmpty())
        return {};

    if (filter == nullptr)
        return archivedPersistentId;

    auto* audioModification = filter->getAudioModificationToRestoreStateWithID(archivedPersistentId.toRawUTF8());
    if (audioModification == nullptr)
        return {};

    const auto& restoredPersistentId = audioModification->getPersistentID();
    return restoredPersistentId.empty() ? juce::String() : juce::String::fromUTF8(restoredPersistentId.c_str());
}

ArchivedMaterializationBindingRecord readRestoredMaterializationBindingRecord(juce::ARAInputStream& input,
                                                                              juce::String persistentId)
{
    ArchivedMaterializationBindingRecord record;
    record.audioModificationPersistentId = std::move(persistentId);
    record.sourceId = static_cast<uint64_t>(input.readInt64());
    record.materializationId = static_cast<uint64_t>(input.readInt64());
    record.contentWindow.sourceId = static_cast<uint64_t>(input.readInt64());
    record.contentWindow.sourceStartSeconds = input.readDouble();
    record.contentWindow.sourceEndSeconds = input.readDouble();
    record.materializationRevision = static_cast<uint64_t>(input.readInt64());
    record.materializationDurationSeconds = input.readDouble();
    return record;
}

void skipRestoredMaterializationBindingRecord(juce::ARAInputStream& input)
{
    const auto ignoredSourceId = input.readInt64();
    const auto ignoredMaterializationId = input.readInt64();
    const auto ignoredWindowSourceId = input.readInt64();
    const auto ignoredWindowStart = input.readDouble();
    const auto ignoredWindowEnd = input.readDouble();
    const auto ignoredRevision = input.readInt64();
    const auto ignoredDuration = input.readDouble();
    juce::ignoreUnused(ignoredSourceId,
                       ignoredMaterializationId,
                       ignoredWindowSourceId,
                       ignoredWindowStart,
                       ignoredWindowEnd,
                       ignoredRevision,
                       ignoredDuration);
}

} // namespace

OpenTuneDocumentController::OpenTuneDocumentController(const ARA::PlugIn::PlugInEntry* entry,
                                                       const ARA::ARADocumentControllerHostInstance* instance)
    : ARADocumentControllerSpecialisation(entry, instance)
{
}

OpenTuneDocumentController::~OpenTuneDocumentController()
{
    processor_ = nullptr;
    playbackRenderers_.clear();
}

void OpenTuneDocumentController::setProcessor(OpenTuneAudioProcessor* processor)
{
    processor_ = processor;
    refreshRegisteredRenderers(publishModelChange());
}

bool OpenTuneDocumentController::PlaybackRegionProjection::isRenderable() const noexcept
{
    return materializationId != 0
        && materializationDurationSeconds > 0.0
        && durationInPlaybackTime > 0.0
        && durationInModificationTime > 0.0;
}

std::vector<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::getPlaybackRegionProjections() const
{
    return buildProjections();
}

std::vector<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::getPlaybackRegionProjectionsFor(
    const std::vector<juce::ARAPlaybackRegion*>& playbackRegions) const
{
    std::vector<PlaybackRegionProjection> projections;
    projections.reserve(playbackRegions.size());

    for (auto* playbackRegion : playbackRegions)
    {
        const auto* region = findPlaybackRegion(playbackRegion);
        if (region != nullptr && region->hasValidPlacement())
            projections.push_back(makeProjection(*region));
    }

    return projections;
}

std::vector<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::getEditorSelectionPlaybackRegionProjections() const
{
    return getPlaybackRegionProjectionsFor(editorSelectionPlaybackRegions_);
}

std::optional<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::getFocusedEditorPlaybackRegionProjection() const
{
    const auto projections = getEditorSelectionPlaybackRegionProjections();
    if (projections.empty())
        return std::nullopt;

    return projections.front();
}

bool OpenTuneDocumentController::referencesMaterialization(uint64_t materializationId) const
{
    if (materializationId == 0)
        return false;

    for (const auto& region : playbackRegions_)
    {
        if (!region.hasValidPlacement())
            continue;

        const auto* modification = findAudioModification(region.audioModificationPersistentId);
        if (modification != nullptr && modification->materializationId == materializationId)
            return true;
    }

    return false;
}

bool OpenTuneDocumentController::requestBirthForFocusedEditorPlaybackRegion()
{
    const auto focusedRegion = getFocusedEditorPlaybackRegionProjection();
    if (!focusedRegion.has_value() || focusedRegion->audioModificationPersistentId.isEmpty())
        return false;

    auto* region = findPlaybackRegion(focusedRegion->playbackRegion);
    if (region == nullptr)
        return false;

    const bool born = birthMaterializationForRegion(*region);
    refreshRegisteredRenderers(publishModelChange());
    return born;
}

void OpenTuneDocumentController::setEditorViewSelectionPlaybackRegions(
    std::vector<juce::ARAPlaybackRegion*> playbackRegions)
{
    editorSelectionPlaybackRegions_ = std::move(playbackRegions);
    reconcileEditorSelectionPlaybackRegions();
}

void OpenTuneDocumentController::registerPlaybackRenderer(OpenTunePlaybackRenderer& renderer)
{
    if (std::find(playbackRenderers_.begin(), playbackRenderers_.end(), &renderer) == playbackRenderers_.end())
        playbackRenderers_.push_back(&renderer);
}

void OpenTuneDocumentController::unregisterPlaybackRenderer(OpenTunePlaybackRenderer& renderer)
{
    playbackRenderers_.erase(std::remove(playbackRenderers_.begin(), playbackRenderers_.end(), &renderer),
                             playbackRenderers_.end());
}

void OpenTuneDocumentController::didUpdateMusicalContextProperties(juce::ARAMusicalContext* musicalContext)
{
    juce::ignoreUnused(musicalContext);
}

void OpenTuneDocumentController::willBeginEditing(juce::ARADocument* document)
{
    juce::ignoreUnused(document);
}

void OpenTuneDocumentController::didEndEditing(juce::ARADocument* document)
{
    juce::ignoreUnused(document);
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::didUpdateAudioModificationProperties(juce::ARAAudioModification* audioModification)
{
    auto& modification = ensureAudioModification(audioModification);
    modification.updateIdentity(audioModification);
    if (auto* source = findAudioSource(audioModification != nullptr ? audioModification->getAudioSource() : nullptr))
        modification.attachSource(*source);
    applyPendingRestoredBinding(modification);

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willDestroyAudioModification(juce::ARAAudioModification* audioModification)
{
    const auto* record = findAudioModification(audioModification);
    const auto persistentId = record != nullptr ? record->persistentId : juce::String();
    audioModifications_.erase(std::remove_if(audioModifications_.begin(), audioModifications_.end(),
                                             [audioModification](const AudioModification& record)
                                             {
                                                 return record.audioModification == audioModification;
                                             }),
                              audioModifications_.end());
    playbackRegions_.erase(std::remove_if(playbackRegions_.begin(), playbackRegions_.end(),
                                          [&persistentId](const PlaybackRegion& region)
                                          {
                                              return persistentId.isNotEmpty()
                                                  && region.audioModificationPersistentId == persistentId;
                                          }),
                           playbackRegions_.end());
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers(publishModelChange());

    if (processor_ != nullptr)
        processor_->scheduleReclaimSweep();
}

void OpenTuneDocumentController::didUpdatePlaybackRegionProperties(juce::ARAPlaybackRegion* playbackRegion)
{
    auto& region = ensurePlaybackRegion(playbackRegion);
    region.updateFrom(playbackRegion);
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willDestroyPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion)
{
    removePlaybackRegion(playbackRegion);
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers(publishModelChange());

    if (processor_ != nullptr)
        processor_->scheduleReclaimSweep();
}

void OpenTuneDocumentController::didAddPlaybackRegionToAudioModification(
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    auto& modification = ensureAudioModification(audioModification);
    auto& region = ensurePlaybackRegion(playbackRegion);
    region.updateFrom(playbackRegion);
    if (region.audioModificationPersistentId.isEmpty())
        region.audioModificationPersistentId = modification.persistentId;
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::didUpdateAudioSourceProperties(juce::ARAAudioSource* audioSource)
{
    auto& source = ensureAudioSource(audioSource);
    source.updateFrom(audioSource);
    for (auto& modification : audioModifications_)
        if (modification.audioModification != nullptr && modification.audioModification->getAudioSource() == audioSource)
            modification.attachSource(source);

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::doUpdateAudioSourceContent(juce::ARAAudioSource* audioSource,
                                                            juce::ARAContentUpdateScopes scopeFlags)
{
    juce::ignoreUnused(scopeFlags);

    if (auto* source = findAudioSource(audioSource))
    {
        for (auto& modification : audioModifications_)
        {
            if (modification.sourcePersistentId == source->getIdentity().persistentId)
                modification.resetContent();
        }
    }

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                                                    bool enable)
{
    ensureAudioSource(audioSource).setSampleAccessEnabled(enable);
}

void OpenTuneDocumentController::didEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                                                   bool enable)
{
    auto& source = ensureAudioSource(audioSource);
    source.setSampleAccessEnabled(enable);
    if (enable)
        source.createReaderLease();

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willRemovePlaybackRegionFromAudioModification(
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    juce::ignoreUnused(audioModification);
    removePlaybackRegion(playbackRegion);
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers(publishModelChange());

    if (processor_ != nullptr)
        processor_->scheduleReclaimSweep();
}

void OpenTuneDocumentController::willDestroyAudioSource(juce::ARAAudioSource* audioSource)
{
    AudioSource* source = findAudioSource(audioSource);
    const auto persistentId = source != nullptr ? source->getIdentity().persistentId : juce::String();
    audioSources_.erase(std::remove_if(audioSources_.begin(), audioSources_.end(),
                                       [audioSource](const AudioSource& source)
                                       {
                                           return source.matches(audioSource);
                                       }),
                        audioSources_.end());
    if (persistentId.isNotEmpty())
    {
        for (auto& modification : audioModifications_)
        {
            if (modification.sourcePersistentId == persistentId)
                modification.resetContent();
        }
    }

    refreshRegisteredRenderers(publishModelChange());

    if (processor_ != nullptr)
        processor_->scheduleReclaimSweep();
}

bool OpenTuneDocumentController::doRestoreObjectsFromStream(juce::ARAInputStream& input,
                                                            const juce::ARARestoreObjectsFilter* filter)
{
    const int magic = input.readInt();
    if (magic != kMaterializationBindingArchiveMagic)
        return false;

    const int version = input.readInt();
    if (version != kMaterializationBindingArchiveVersion)
        return false;

    const int bindingCount = input.readInt();
    if (bindingCount < 0 || bindingCount > kMaxMaterializationBindingRecords)
        return false;

    for (int i = 0; i < bindingCount; ++i)
    {
        const auto archivedPersistentId = input.readString();
        const auto restoredPersistentId = mapRestoredAudioModificationPersistentId(archivedPersistentId, filter);
        if (restoredPersistentId.isEmpty())
        {
            skipRestoredMaterializationBindingRecord(input);
            continue;
        }

        const auto record = readRestoredMaterializationBindingRecord(input, restoredPersistentId);
        if (!record.isValid())
            return false;

        RestoredMaterializationBinding binding;
        binding.audioModificationPersistentId = record.audioModificationPersistentId;
        binding.sourceWindow = record.contentWindow;
        binding.sourceId = record.sourceId;
        binding.materializationId = record.materializationId;
        binding.materializationRevision = record.materializationRevision;
        binding.materializationDurationSeconds = record.materializationDurationSeconds;

        if (auto* modification = findAudioModification(binding.audioModificationPersistentId))
            applyRestoredBinding(*modification, binding);
        else
            rememberPendingRestoredBinding(std::move(binding));
    }

    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::doStoreObjectsToStream(juce::ARAOutputStream& output,
                                                        const juce::ARAStoreObjectsFilter* filter)
{
    std::vector<const AudioModification*> bindings;
    bindings.reserve(audioModifications_.size());

    if (filter == nullptr)
    {
        for (const auto& modification : audioModifications_)
            if (modification.persistentId.isNotEmpty() && modification.isRenderable())
                bindings.push_back(&modification);
    }
    else
    {
        const auto& modsToStore = filter->getAudioModificationsToStore();
        for (const auto& modification : audioModifications_)
        {
            if (modification.persistentId.isEmpty() || !modification.isRenderable())
                continue;

            const auto* araMod = modification.audioModification;
            if (araMod == nullptr)
                continue;

            const auto* basePtr = static_cast<const ARA::PlugIn::AudioModification*>(araMod);
            if (std::find(modsToStore.begin(), modsToStore.end(), basePtr) != modsToStore.end())
                bindings.push_back(&modification);
        }
    }

    if (bindings.size() > static_cast<size_t>(kMaxMaterializationBindingRecords))
        return false;

    bool ok = output.writeInt(kMaterializationBindingArchiveMagic);
    ok = output.writeInt(kMaterializationBindingArchiveVersion) && ok;
    ok = output.writeInt(static_cast<int>(bindings.size())) && ok;

    for (const auto* modification : bindings)
    {
        ok = output.writeString(modification->persistentId) && ok;
        ok = output.writeInt64(static_cast<juce::int64>(modification->sourceId)) && ok;
        ok = output.writeInt64(static_cast<juce::int64>(modification->materializationId)) && ok;
        ok = output.writeInt64(static_cast<juce::int64>(modification->contentWindow.sourceId)) && ok;
        ok = output.writeDouble(modification->contentWindow.sourceStartSeconds) && ok;
        ok = output.writeDouble(modification->contentWindow.sourceEndSeconds) && ok;
        ok = output.writeInt64(static_cast<juce::int64>(modification->materializationRevision)) && ok;
        ok = output.writeDouble(modification->materializationDurationSeconds) && ok;
    }

    return ok;
}

juce::ARAPlaybackRenderer* OpenTuneDocumentController::doCreatePlaybackRenderer()
{
    auto* renderer = new OpenTunePlaybackRenderer(getDocumentController());
    registerPlaybackRenderer(*renderer);
    renderer->refreshRenderPlanFromDocument();
    return renderer;
}

juce::ARAEditorView* OpenTuneDocumentController::doCreateEditorView()
{
    return new OpenTuneEditorView(getDocumentController(), *this);
}

AudioSource* OpenTuneDocumentController::findAudioSource(juce::ARAAudioSource* audioSource)
{
    const auto it = std::find_if(audioSources_.begin(), audioSources_.end(),
                                 [audioSource](const AudioSource& source)
                                 {
                                     return source.matches(audioSource);
                                 });
    return it != audioSources_.end() ? &*it : nullptr;
}

const AudioSource* OpenTuneDocumentController::findAudioSource(const juce::String& persistentId) const
{
    const auto it = std::find_if(audioSources_.begin(), audioSources_.end(),
                                 [&persistentId](const AudioSource& source)
                                 {
                                     return source.getIdentity().persistentId == persistentId;
                                 });
    return it != audioSources_.end() ? &*it : nullptr;
}

AudioSource& OpenTuneDocumentController::ensureAudioSource(juce::ARAAudioSource* audioSource)
{
    if (auto* existing = findAudioSource(audioSource))
        return *existing;

    AudioSource source;
    source.updateFrom(audioSource);
    audioSources_.push_back(std::move(source));
    return audioSources_.back();
}

AudioModification* OpenTuneDocumentController::findAudioModification(const juce::String& persistentId)
{
    const auto it = std::find_if(audioModifications_.begin(), audioModifications_.end(),
                                 [&persistentId](const AudioModification& modification)
                                 {
                                     return modification.persistentId == persistentId;
                                 });
    return it != audioModifications_.end() ? &*it : nullptr;
}

const AudioModification* OpenTuneDocumentController::findAudioModification(const juce::String& persistentId) const
{
    const auto it = std::find_if(audioModifications_.begin(), audioModifications_.end(),
                                 [&persistentId](const AudioModification& modification)
                                 {
                                     return modification.persistentId == persistentId;
                                 });
    return it != audioModifications_.end() ? &*it : nullptr;
}

AudioModification* OpenTuneDocumentController::findAudioModification(juce::ARAAudioModification* audioModification)
{
    const auto it = std::find_if(audioModifications_.begin(), audioModifications_.end(),
                                 [audioModification](const AudioModification& modification)
                                 {
                                     return modification.audioModification == audioModification;
                                 });
    return it != audioModifications_.end() ? &*it : nullptr;
}

AudioModification& OpenTuneDocumentController::ensureAudioModification(juce::ARAAudioModification* audioModification)
{
    if (auto* existing = findAudioModification(audioModification))
    {
        applyPendingRestoredBinding(*existing);
        return *existing;
    }

    AudioModification modification;
    modification.updateIdentity(audioModification);
    if (audioModification != nullptr)
        if (auto* source = findAudioSource(audioModification->getAudioSource()))
            modification.attachSource(*source);
    audioModifications_.push_back(std::move(modification));
    applyPendingRestoredBinding(audioModifications_.back());
    return audioModifications_.back();
}

PlaybackRegion* OpenTuneDocumentController::findPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion)
{
    const auto it = std::find_if(playbackRegions_.begin(), playbackRegions_.end(),
                                 [playbackRegion](const PlaybackRegion& region)
                                 {
                                     return region.playbackRegion == playbackRegion;
                                 });
    return it != playbackRegions_.end() ? &*it : nullptr;
}

const PlaybackRegion* OpenTuneDocumentController::findPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion) const
{
    const auto it = std::find_if(playbackRegions_.begin(), playbackRegions_.end(),
                                 [playbackRegion](const PlaybackRegion& region)
                                 {
                                     return region.playbackRegion == playbackRegion;
                                 });
    return it != playbackRegions_.end() ? &*it : nullptr;
}

PlaybackRegion& OpenTuneDocumentController::ensurePlaybackRegion(juce::ARAPlaybackRegion* playbackRegion)
{
    if (auto* existing = findPlaybackRegion(playbackRegion))
        return *existing;

    PlaybackRegion region;
    region.updateFrom(playbackRegion);
    playbackRegions_.push_back(std::move(region));
    return playbackRegions_.back();
}

OpenTuneDocumentController::PlaybackRegionProjection
OpenTuneDocumentController::makeProjection(const PlaybackRegion& placement) const
{
    PlaybackRegionProjection projection;
    projection.playbackRegion = placement.playbackRegion;
    projection.audioModificationPersistentId = placement.audioModificationPersistentId;
    projection.placementRevision = placement.placementRevision;
    projection.startInPlaybackTime = placement.startInPlaybackTime;
    projection.startInModificationTime = placement.startInModificationTime;
    projection.durationInPlaybackTime = placement.durationInPlaybackTime;
    projection.durationInModificationTime = placement.durationInModificationTime;
    projection.timestretchEnabled = placement.timestretchEnabled;
    projection.timestretchReflectingTempo = placement.timestretchReflectingTempo;
    projection.contentBasedFadeAtHead = placement.contentBasedFadeAtHead;
    projection.contentBasedFadeAtTail = placement.contentBasedFadeAtTail;

    const auto* modification = findAudioModification(placement.audioModificationPersistentId);
    if (modification == nullptr)
        return projection;

    projection.contentWindow = modification->contentWindow;
    projection.sourceId = modification->sourceId;
    projection.materializationId = modification->materializationId;
    projection.materializationRevision = modification->materializationRevision;
    projection.contentRevision = modification->contentRevision;
    projection.materializationDurationSeconds = modification->materializationDurationSeconds;

    const auto* source = findAudioSource(modification->sourcePersistentId);
    if (source != nullptr)
    {
        projection.sampleRate = source->getShape().sourceSampleRate;
        projection.numChannels = source->getShape().numChannels;
    }

    return projection;
}

std::vector<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::buildProjections() const
{
    std::vector<PlaybackRegionProjection> projections;
    projections.reserve(playbackRegions_.size());

    for (const auto& region : playbackRegions_)
    {
        if (region.hasValidPlacement())
            projections.push_back(makeProjection(region));
    }

    return projections;
}

std::vector<OpenTunePlaybackRenderer*> OpenTuneDocumentController::publishModelChange()
{
    return playbackRenderers_;
}

void OpenTuneDocumentController::refreshRegisteredRenderers(const std::vector<OpenTunePlaybackRenderer*>& renderers)
{
    for (auto* renderer : renderers)
        if (renderer != nullptr)
            renderer->refreshRenderPlanFromDocument();
}

void OpenTuneDocumentController::reconcileEditorSelectionPlaybackRegions()
{
    editorSelectionPlaybackRegions_.erase(
        std::remove_if(editorSelectionPlaybackRegions_.begin(),
                       editorSelectionPlaybackRegions_.end(),
                       [this](juce::ARAPlaybackRegion* playbackRegion)
                       {
                           return findPlaybackRegion(playbackRegion) == nullptr;
                       }),
        editorSelectionPlaybackRegions_.end());
}

bool OpenTuneDocumentController::birthMaterializationForRegion(PlaybackRegion& region)
{
    auto* modification = findAudioModification(region.audioModificationPersistentId);
    if (modification == nullptr || modification->persistentId.isEmpty() || modification->sourcePersistentId.isEmpty())
        return false;

    auto* source = findAudioSource(modification->sourcePersistentId);
    if (source == nullptr || !source->canReadSamples())
    {
        modification->birthState = AudioModificationBirthState::WaitingForSource;
        return false;
    }

    if (processor_ == nullptr)
    {
        modification->birthState = AudioModificationBirthState::Failed;
        return false;
    }

    modification->birthState = AudioModificationBirthState::Rendering;
    ++modification->birthRevision;

    OpenTuneAudioProcessor::AraOriginalF0BirthRequest request;
    request.audioSource = source->getAraAudioSource();
    request.sourceId = modification->sourceId != 0
        ? modification->sourceId
        : static_cast<uint64_t>(std::hash<std::string>{}(source->getIdentity().persistentId.toStdString()));
    request.readerLease = source->shareReaderLease();
    request.numChannels = source->getShape().numChannels;
    request.numSamples = source->getShape().numSamples;
    request.sourceSampleRate = source->getShape().sourceSampleRate;
    request.sourceWindow = modification->contentWindow.sourceId != 0
        ? modification->contentWindow
        : SourceWindow{request.sourceId, 0.0, source->getShape().durationSeconds()};
    request.sourceWindow.sourceId = request.sourceId;
    request.playbackStartSeconds = region.startInPlaybackTime;

    const auto result = processor_->birthAraMaterializationWithOriginalF0(std::move(request));
    if (!result.has_value())
    {
        modification->birthState = AudioModificationBirthState::Failed;
        return false;
    }

    modification->sourceId = result->sourceId;
    modification->contentWindow.sourceId = result->sourceId;
    modification->materializationId = result->materializationId;
    modification->materializationRevision = result->materializationRevision;
    modification->materializationDurationSeconds = result->materializationDurationSeconds;
    modification->birthState = AudioModificationBirthState::Ready;
    return true;
}

bool OpenTuneDocumentController::removePlaybackRegion(juce::ARAPlaybackRegion* playbackRegion)
{
    const auto oldSize = playbackRegions_.size();
    playbackRegions_.erase(std::remove_if(playbackRegions_.begin(), playbackRegions_.end(),
                                          [playbackRegion](const PlaybackRegion& region)
                                          {
                                              return region.playbackRegion == playbackRegion;
                                          }),
                           playbackRegions_.end());
    return playbackRegions_.size() != oldSize;
}

void OpenTuneDocumentController::applyRestoredBinding(AudioModification& modification,
                                                      const RestoredMaterializationBinding& binding) noexcept
{
    modification.sourceId = binding.sourceId;
    modification.contentWindow = binding.sourceWindow;
    modification.contentWindow.sourceId = binding.sourceWindow.sourceId != 0
        ? binding.sourceWindow.sourceId
        : binding.sourceId;
    modification.materializationId = binding.materializationId;
    modification.materializationRevision = binding.materializationRevision;
    modification.materializationDurationSeconds = binding.materializationDurationSeconds;
    modification.birthState = AudioModificationBirthState::Ready;
    ++modification.contentRevision;
}

bool OpenTuneDocumentController::applyPendingRestoredBinding(AudioModification& modification)
{
    if (modification.persistentId.isEmpty())
        return false;

    const auto it = std::find_if(pendingRestoredBindings_.begin(),
                                 pendingRestoredBindings_.end(),
                                 [&modification](const RestoredMaterializationBinding& binding)
                                 {
                                     return binding.audioModificationPersistentId == modification.persistentId;
                                 });
    if (it == pendingRestoredBindings_.end())
        return false;

    applyRestoredBinding(modification, *it);
    pendingRestoredBindings_.erase(it);
    return true;
}

void OpenTuneDocumentController::rememberPendingRestoredBinding(RestoredMaterializationBinding binding)
{
    if (binding.audioModificationPersistentId.isEmpty())
        return;

    const auto it = std::find_if(pendingRestoredBindings_.begin(),
                                 pendingRestoredBindings_.end(),
                                 [&binding](const RestoredMaterializationBinding& pending)
                                 {
                                     return pending.audioModificationPersistentId
                                         == binding.audioModificationPersistentId;
                                 });
    if (it != pendingRestoredBindings_.end())
    {
        *it = std::move(binding);
        return;
    }

    pendingRestoredBindings_.push_back(std::move(binding));
}

bool OpenTuneDocumentController::requestSetPlaybackPosition(double timeInSeconds)
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
        return false;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return false;

    playbackController->requestSetPlaybackPosition(timeInSeconds);
    return true;
}

bool OpenTuneDocumentController::requestStartPlayback()
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
        return false;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return false;

    playbackController->requestStartPlayback();
    return true;
}

bool OpenTuneDocumentController::requestStopPlayback()
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
        return false;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return false;

    playbackController->requestStopPlayback();
    return true;
}

} // namespace OpenTune

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<OpenTune::OpenTuneDocumentController>();
}
