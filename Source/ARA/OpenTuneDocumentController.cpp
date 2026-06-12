#include "OpenTuneDocumentController.h"

#include "OpenTuneEditorView.h"
#include "OpenTunePlaybackRenderer.h"

#include "../MaterializationStore.h"
#include "../Inference/F0InferenceService.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/TimeCoordinate.h"
#include "../Utils/SilentGapDetector.h"
#include "../Utils/PitchCurve.h"
#include "../Inference/RenderCache.h"
#include "../Utils/SourceWindow.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <utility>

namespace OpenTune {

namespace {

constexpr int kContentPayloadArchiveMagic = 0x4F544143;
constexpr int kContentPayloadArchiveVersion = 1;
constexpr int kMaxContentPayloadRecords = 4096;

} // namespace

OpenTuneDocumentController::OpenTuneDocumentController(const ARA::PlugIn::PlugInEntry* entry,
                                                       const ARA::ARADocumentControllerHostInstance* instance)
    : ARADocumentControllerSpecialisation(entry, instance)
    , resamplingManager_(std::make_shared<ResamplingManager>())
    , reclaimAsyncUpdater_([this] { runContentReclaimSweep(); })
{
}

OpenTuneDocumentController::~OpenTuneDocumentController()
{
    // 撤销服务租约，防止异步 F0 completion 写回已析构的 DC
    if (asyncLeaseToken_)
        asyncLeaseToken_->store(false, std::memory_order_release);

    playbackRenderers_.clear();
    // juce::AsyncUpdater in ReclaimAsyncUpdater auto-cancels pending updates on destruction.
}

void OpenTuneDocumentController::attachProcessorServices(ProcessorServices services)
{
    jassert(services.owner != nullptr);
    serviceOwner_ = services.owner;
    f0Service_ = std::move(services.f0Service);
    scheduleAsyncWork_ = std::move(services.scheduleAsyncWork);
    onReclaimNeeded_ = std::move(services.requestReclaimSweep);
    contentRenderService_ = services.contentRenderService;
    asyncLeaseToken_ = std::make_shared<std::atomic<bool>>(true);
}

void OpenTuneDocumentController::detachProcessorServices(const OpenTuneAudioProcessor* owner)
{
    if (serviceOwner_ != owner) return;

    // 撤销服务租约 — 通知所有后台 F0 work 释放
    if (asyncLeaseToken_)
        asyncLeaseToken_->store(false, std::memory_order_release);

    serviceOwner_ = nullptr;
    contentRenderService_ = nullptr;
    f0Service_.reset();
    scheduleAsyncWork_ = nullptr;
    onReclaimNeeded_ = nullptr;
}

void OpenTuneDocumentController::runContentReclaimSweep()
{
    // 清理每个 AudioModification 的 retired content records
    for (auto& mod : audioModifications_)
    {
        (void)mod;
    }

    // 清理 CRS derived artifacts（如果有）
    if (contentRenderService_)
    {
        // contentRenderService_->reclaimUnusedCaches(); // 如果有此 API
    }

    // 仍触发 onReclaimNeeded_ 通知 processor（processor 扫自己的 standalone arrangement）
    if (onReclaimNeeded_)
        onReclaimNeeded_();
}

void OpenTuneDocumentController::scheduleContentReclaim()
{
    reclaimAsyncUpdater_.triggerAsyncUpdate();
}

void OpenTuneDocumentController::getContentSnapshot(juce::XmlElement& dest) const
{
    // TODO: Step 6 实现 AudioModification.content 序列化
    // 暂时返回占位 XML
    dest.setAttribute("version", 2);
    // 后续 Step 6 将遍历 audioModifications_ 序列化每个 mod.content
    juce::ignoreUnused(dest);
}

void OpenTuneDocumentController::restoreContentPayloadInto(const juce::XmlElement& src)
{
    // TODO: Step 6 实现 AudioModification.content 恢复
    juce::ignoreUnused(src);
}

ContentRenderService* OpenTuneDocumentController::getContentRenderService() const noexcept
{
    return contentRenderService_;
}

bool OpenTuneDocumentController::PlaybackRegionProjection::isRenderable() const noexcept
{
    return contentKey.isValid()
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

int OpenTuneDocumentController::refreshAllAudioModifications()
{
    std::set<juce::String> uniqueModIds;
    for (const auto& region : playbackRegions_)
    {
        if (region.hasValidPlacement())
            uniqueModIds.insert(region.audioModificationPersistentId);
    }

    if (uniqueModIds.empty())
        return 0;

    int refreshedCount = 0;

    for (const auto& modId : uniqueModIds)
    {
        auto* modification = findAudioModification(modId);
        if (modification == nullptr)
            continue;

        if (birthMaterializationForModification(*modification))
            ++refreshedCount;
    }

    if (refreshedCount > 0)
        refreshRegisteredRenderers(publishModelChange());

    return refreshedCount;
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

    // 如果 modification 被 host 重新激活（undo 删除），尝试从 AudioModification 内部 revive
    const auto key = modification.contentKey();
    if (key.isValid() && modification.content.lifecycle <= ContentLifecycle::Empty)
    {
        if (modification.reviveContent(key))
        {
            if (modification.audioModification != nullptr)
                modification.audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
        }
    }

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willDestroyAudioModification(juce::ARAAudioModification* audioModification)
{
    auto* mod = findAudioModification(audioModification);
    if (mod == nullptr)
        return;

    // 1. Retire active content（留在 AudioModification 内部）
    if (mod->hasContentState() && mod->content.lifecycle == ContentLifecycle::Ready)
        mod->retireContent();

    // 2. 清理 CRS derived artifacts
    if (contentRenderService_)
    {
        auto key = mod->contentKey();
        contentRenderService_->removePlaybackSource(key);
        contentRenderService_->removeRenderCache(key);
        contentRenderService_->removeStretcher(key);
    }

    // 3. Detach host pointer — AudioModification 保留在 audioModifications_ 中作为 retired 记录
    mod->audioModification = nullptr;

    // 4. 删除关联的 PlaybackRegions
    const auto persistentId = mod->persistentId;
    playbackRegions_.erase(std::remove_if(playbackRegions_.begin(), playbackRegions_.end(),
                                          [&persistentId](const PlaybackRegion& region)
                                          {
                                              return persistentId.isNotEmpty()
                                                  && region.audioModificationPersistentId == persistentId;
                                          }),
                           playbackRegions_.end());
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers(publishModelChange());

    if (onReclaimNeeded_)
        onReclaimNeeded_();
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

    if (onReclaimNeeded_)
        onReclaimNeeded_();
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

    if (onReclaimNeeded_)
        onReclaimNeeded_();
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

    if (onReclaimNeeded_)
        onReclaimNeeded_();
}

namespace {
juce::String mapRestoredPersistentId(const juce::String& archivedPersistentId,
                                     const juce::ARARestoreObjectsFilter* filter)
{
    if (archivedPersistentId.isEmpty())
        return {};

    if (filter == nullptr)
        return archivedPersistentId;

    auto* audioModification = filter->getAudioModificationToRestoreStateWithID(
        archivedPersistentId.toRawUTF8());
    if (audioModification == nullptr)
        return {};

    const auto& restoredPersistentId = audioModification->getPersistentID();
    return restoredPersistentId.empty() ? juce::String() : juce::String::fromUTF8(restoredPersistentId.c_str());
}
} // namespace

bool OpenTuneDocumentController::doRestoreObjectsFromStream(juce::ARAInputStream& input,
                                                             const juce::ARARestoreObjectsFilter* filter)
{
    const int magic = input.readInt();
    if (magic != kContentPayloadArchiveMagic)
        return false;

    const int version = input.readInt();
    if (version != kContentPayloadArchiveVersion)
        return false;

    const int bindingCount = input.readInt();
    if (bindingCount < 0 || bindingCount > kMaxContentPayloadRecords)
        return false;

    // Restore retired content records directly into matching AudioModification
    for (int i = 0; i < bindingCount; ++i)
    {
        const auto archivedPersistentId = input.readString();
        juce::ignoreUnused(archivedPersistentId);
        const auto restoredPersistentId = mapRestoredPersistentId(archivedPersistentId, filter);
        juce::ignoreUnused(restoredPersistentId);

        // Read retired content record count
        const int retiredCount = input.readInt();

        // Find matching modification and push retired records into it
        auto* targetMod = findAudioModification(restoredPersistentId);
        for (int r = 0; r < retiredCount; ++r)
        {
            RetiredContentRecord record;
            record.key.domainKind = static_cast<DomainKind>(input.readInt());
            record.key.objectId = static_cast<uint64_t>(input.readInt64());
            record.content.lifecycle = ContentLifecycle::Retired;
            if (targetMod != nullptr)
                targetMod->retiredContentRecords.push_back(std::move(record));
        }
    }

    // 将已恢复的退休记录匹配到现存 modification 上
    for (auto& mod : audioModifications_)
    {
        const auto key = mod.contentKey();
        if (!key.isValid() || mod.audioModification == nullptr)
            continue;
        if (mod.reviveContent(key))
            mod.audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
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

    if (bindings.size() > static_cast<size_t>(kMaxContentPayloadRecords))
        return false;

    bool ok = output.writeInt(kContentPayloadArchiveMagic);
    ok = output.writeInt(kContentPayloadArchiveVersion) && ok;
    ok = output.writeInt(static_cast<int>(bindings.size())) && ok;

    for (const auto* modification : bindings)
    {
        ok = output.writeString(modification->persistentId) && ok;

        // Write retired content records count + payload
        const auto retiredCount = static_cast<int>(modification->retiredContentRecords.size());
        ok = output.writeInt(retiredCount) && ok;
        for (const auto& record : modification->retiredContentRecords)
        {
            ok = output.writeInt(static_cast<int>(record.key.domainKind)) && ok;
            ok = output.writeInt64(static_cast<juce::int64>(record.key.objectId)) && ok;
        }
    }

    return ok;
}

juce::ARAPlaybackRenderer* OpenTuneDocumentController::doCreatePlaybackRenderer()
{
    auto* renderer = new OpenTunePlaybackRenderer(getDocumentController(), this);
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

AudioModification* OpenTuneDocumentController::findAudioModificationByContentKey(const ContentKey& key)
{
    for (auto& mod : audioModifications_)
        if (mod.contentKey() == key)
            return &mod;
    return nullptr;
}

const AudioModification* OpenTuneDocumentController::findAudioModificationByContentKey(const ContentKey& key) const
{
    for (const auto& mod : audioModifications_)
        if (mod.contentKey() == key)
            return &mod;
    return nullptr;
}

AudioModification& OpenTuneDocumentController::ensureAudioModification(juce::ARAAudioModification* audioModification)
{
    // 1. 先按 pointer 查找活跃 modification
    if (auto* existing = findAudioModification(audioModification))
        return *existing;

    // 2. 按 persistentID 查找 retired modification
    if (audioModification != nullptr)
    {
        const juce::String persistentId = juce::String(audioModification->getPersistentID());
        if (persistentId.isNotEmpty())
        {
            auto* retired = findAudioModification(persistentId);
            if (retired != nullptr && retired->audioModification == nullptr)
            {
                // Revive retired modification
                retired->audioModification = audioModification;
                if (retired->content.lifecycle == ContentLifecycle::Retired)
                    retired->reviveContent(retired->contentKey());
                if (auto* source = findAudioSource(audioModification->getAudioSource()))
                    retired->attachSource(*source);
                return *retired;
            }
        }
    }

    // 3. 创建新 AudioModification
    AudioModification modification;
    modification.updateIdentity(audioModification);
    if (audioModification != nullptr)
        if (auto* source = findAudioSource(audioModification->getAudioSource()))
            modification.attachSource(*source);
    audioModifications_.push_back(std::move(modification));
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
    projection.contentRevision = modification->content.contentRevision;
    projection.materializationDurationSeconds = modification->materializationDurationSeconds;
    projection.contentKey = modification->contentKey();

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

bool OpenTuneDocumentController::refreshPlaybackReadSource(ContentKey key)
{
    auto* modification = findAudioModificationByContentKey(key);
    return modification != nullptr && publishPlaybackReadSourceForModification(*modification);
}

bool OpenTuneDocumentController::publishPlaybackReadSourceForModification(
    AudioModification& modification,
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer)
{
    if (contentRenderService_ == nullptr)
        return false;

    const auto key = modification.contentKey();
    PlaybackReadSource existingSource;
    if (audioBuffer == nullptr
        && contentRenderService_->getPlaybackReadSource(key, existingSource))
    {
        audioBuffer = existingSource.audioBuffer;
    }

    if (audioBuffer == nullptr)
        return false;

    PlaybackReadSource readSource;
    readSource.contentKey = key;
    readSource.renderCache = contentRenderService_->getOrCreateRenderCache(key);
    readSource.audioBuffer = std::move(audioBuffer);
    readSource.timeStretchCache = &contentRenderService_->getTimeStretchCache();
    readSource.renderRevision = modification.content.contentRevision;
    readSource.pitchRevision = modification.content.editable.pitchRevision;
    readSource.pitchShiftRevision = modification.content.editable.pitchShiftRevision;
    readSource.timeGridRevision = modification.content.editable.timeGridRevision;
    readSource.pitchShiftSettings = modification.content.editable.pitchShiftSettings;
    readSource.timeGridIsIdentity = modification.content.editable.timeGrid == nullptr
        || modification.content.editable.timeGrid->isIdentity();

    contentRenderService_->publishPlaybackSource(key, readSource);
    return true;
}

bool OpenTuneDocumentController::birthMaterializationForModification(AudioModification& modification)
{
    if (modification.persistentId.isEmpty() || modification.sourcePersistentId.isEmpty())
        return false;

    auto* source = findAudioSource(modification.sourcePersistentId);
    if (source == nullptr || !source->canReadSamples())
    {
        modification.birthState = AudioModificationBirthState::WaitingForSource;
        return false;
    }

    modification.birthState = AudioModificationBirthState::Rendering;
    ++modification.birthRevision;

    // 1. Determine source ID and window
    const auto sourceId = modification.sourceId != 0
        ? modification.sourceId
        : static_cast<uint64_t>(std::hash<std::string>{}(source->getIdentity().persistentId.toStdString()));

    auto readerLease = source->shareReaderLease();
    if (readerLease == nullptr || source->getShape().numChannels <= 0
        || source->getShape().numSamples <= 0 || source->getShape().sourceSampleRate <= 0.0)
    {
        modification.birthState = AudioModificationBirthState::Failed;
        return false;
    }

    const auto sourceWindow = modification.contentWindow.sourceId != 0
        ? modification.contentWindow
        : SourceWindow{sourceId, 0.0, source->getShape().durationSeconds()};

    const double sourceSampleRate = source->getShape().sourceSampleRate;
    const int64_t numSamples = source->getShape().numSamples;
    const int numChannels = source->getShape().numChannels;

    // 2. Read ARA source window
    const int64_t sourceStartSample = static_cast<int64_t>(
        std::round(sourceWindow.sourceStartSeconds * sourceSampleRate));
    const int64_t sourceEndSample = static_cast<int64_t>(
        std::round(sourceWindow.sourceEndSeconds * sourceSampleRate));
    const int64_t windowSamples = std::max<int64_t>(0,
        std::min<int64_t>(sourceEndSample, numSamples)
        - std::max<int64_t>(0, sourceStartSample));

    if (windowSamples <= 0)
    {
        modification.birthState = AudioModificationBirthState::Failed;
        return false;
    }

    juce::AudioBuffer<float> playableAccum(numChannels, static_cast<int>(windowSamples));
    playableAccum.clear();

    constexpr int64_t kChunkSamples = 32768;
    {
        int64_t readOffset = sourceStartSample;
        int64_t accumWriteOffset = 0;
        int64_t remaining = windowSamples;

        while (remaining > 0)
        {
            const int64_t chunkSamples = std::min(kChunkSamples, remaining);
            const int accumOffset = static_cast<int>(accumWriteOffset);

            std::vector<void*> channelPointers(static_cast<size_t>(numChannels));
            for (int ch = 0; ch < numChannels; ++ch)
                channelPointers[static_cast<size_t>(ch)] = playableAccum.getWritePointer(ch, accumOffset);

            if (!readerLease->readAudioSamples(readOffset,
                                                 static_cast<int>(chunkSamples),
                                                 channelPointers.data()))
            {
                modification.birthState = AudioModificationBirthState::Failed;
                return false;
            }

            readOffset += chunkSamples;
            accumWriteOffset += chunkSamples;
            remaining -= chunkSamples;
        }
    }

    // 3. Extract ch0 for async F0
    std::vector<float> channel0Data(static_cast<size_t>(playableAccum.getNumSamples()));
    {
        const float* ch0Read = playableAccum.getReadPointer(0);
        std::copy(ch0Read, ch0Read + playableAccum.getNumSamples(), channel0Data.begin());
    }

    // 4. Resample audio to 44.1kHz and populate AudioModification.content
    juce::AudioBuffer<float> storedBuffer;
    const double targetSampleRate = TimeCoordinate::kRenderSampleRate;
    if (std::abs(sourceSampleRate - targetSampleRate) > 1.0)
    {
        const int storedLen = juce::jmax(1,
            static_cast<int>(TimeCoordinate::secondsToSamples(
                TimeCoordinate::samplesToSeconds(playableAccum.getNumSamples(), sourceSampleRate),
                targetSampleRate)));

        storedBuffer.setSize(numChannels, storedLen);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto resampledData = resamplingManager_->upsampleForHost(
                playableAccum.getReadPointer(ch),
                playableAccum.getNumSamples(),
                static_cast<int>(sourceSampleRate),
                static_cast<int>(targetSampleRate));
            const int toCopy = juce::jmin(storedLen, static_cast<int>(resampledData.size()));
            storedBuffer.copyFrom(ch, 0, resampledData.data(), toCopy);
        }
    }
    else
    {
        storedBuffer = std::move(playableAccum);
    }

    // 6. Detect silent gaps and build content state
    auto silentGaps = SilentGapDetector::detectAllGapsAdaptive(storedBuffer);
    auto storedAudioBuffer = std::make_shared<const juce::AudioBuffer<float>>(std::move(storedBuffer));

    // Fill AudioModification.content (new content root)
    modification.content.sourceWindow = SourceWindow{sourceId,
                                                      sourceWindow.sourceStartSeconds,
                                                      sourceWindow.sourceEndSeconds};
    modification.content.lifecycle = ContentLifecycle::Loading;
    modification.content.analysis.silentGaps = std::move(silentGaps);
    modification.content.analysis.originalF0State = OriginalF0State::NotRequested;

    const double materializationDurationSeconds =
        TimeCoordinate::samplesToSeconds(storedAudioBuffer->getNumSamples(), targetSampleRate);

    // 7. Publish to CRS if available (owns render cache + audio buffer)
    publishPlaybackReadSourceForModification(modification, storedAudioBuffer);

    // 8. Set modification fields and notify ARA host
    modification.sourceId = sourceId;
    modification.contentWindow = SourceWindow{sourceId,
                                               sourceWindow.sourceStartSeconds,
                                               sourceWindow.sourceEndSeconds};
    modification.materializationDurationSeconds = materializationDurationSeconds;
    modification.birthState = AudioModificationBirthState::Ready;
    modification.content.lifecycle = ContentLifecycle::Ready;
    if (modification.audioModification != nullptr)
        modification.audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);

    // 9. Schedule async F0 extraction via CRS
    if (contentRenderService_ != nullptr)
        scheduleAsyncF0Extraction(modification.contentKey().objectId, std::move(channel0Data), sourceSampleRate);

    return true;
}

void OpenTuneDocumentController::scheduleAsyncF0Extraction(
    uint64_t contentObjectId,
    std::vector<float> channel0Data,
    double sourceSampleRate)
{
    if (!scheduleAsyncWork_)
        return;

    auto crs = contentRenderService_;
    auto f0Svc = f0Service_;
    const ContentKey key{DomainKind::ARAAudioModification, contentObjectId, 0};

    scheduleAsyncWork_([this, crs, f0Svc, key, leaseToken = asyncLeaseToken_,
                        data = std::move(channel0Data),
                        sourceSampleRate]()
    {
        if (leaseToken && !leaseToken->load(std::memory_order_acquire))
            return;

        if (f0Svc == nullptr || data.empty())
            return;

        auto extraction = f0Svc->extractF0(data.data(), data.size(),
                                            static_cast<int>(sourceSampleRate));

        if (!extraction.ok() || extraction.value().empty())
        {
            f0Svc->releaseImmediately();
            return;
        }

        const int hopSize = f0Svc->getF0HopSize();
        const int f0SampleRate = f0Svc->getF0SampleRate();

        auto pitchCurve = std::make_shared<PitchCurve>();
        pitchCurve->setHopSize(hopSize);
        pitchCurve->setSampleRate(static_cast<double>(f0SampleRate));
        pitchCurve->setOriginalF0(extraction.value());

        // Compute energy from channel0Data
        {
            std::vector<float> energy(extraction.value().size(), 0.0f);
            const double f0SecondsPerFrame = static_cast<double>(hopSize)
                / static_cast<double>(juce::jmax(1, f0SampleRate));
            const int halfRmsWindowSamples = juce::jmax(1,
                static_cast<int>(std::round(sourceSampleRate * 0.010)));
            for (size_t i = 0; i < extraction.value().size(); ++i)
            {
                if (!std::isfinite(extraction.value()[i]) || extraction.value()[i] <= 0.0f)
                    continue;
                const int centerSample = juce::jlimit(0, static_cast<int>(data.size()) - 1,
                    static_cast<int>(std::round(static_cast<double>(i) * f0SecondsPerFrame * sourceSampleRate)));
                const int startSample = juce::jmax(0, centerSample - halfRmsWindowSamples);
                const int endSampleExclusive = juce::jmin(static_cast<int>(data.size()),
                    centerSample + halfRmsWindowSamples);
                if (endSampleExclusive <= startSample) continue;
                double squareSum = 0.0;
                for (int s = startSample; s < endSampleExclusive; ++s)
                {
                    const float v = data[static_cast<size_t>(s)];
                    squareSum += static_cast<double>(v) * static_cast<double>(v);
                }
                const double meanSquare = squareSum / static_cast<double>(endSampleExclusive - startSample);
                energy[i] = juce::jlimit(0.0f, 1.0f, static_cast<float>(std::sqrt(meanSquare)));
            }
            pitchCurve->setOriginalEnergy(energy);
        }

        f0Svc->releaseImmediately();

        // Commit results to AudioModification content on the message thread
        juce::MessageManager::callAsync([this, crs, key, leaseToken,
                                          pc = std::move(pitchCurve)]() mutable
        {
            if (leaseToken && !leaseToken->load(std::memory_order_acquire))
                return;

            if (crs == nullptr)
                return;

            // Wire F0 analysis into AudioModification content state
            if (auto* mod = findAudioModificationByContentKey(key))
            {
                mod->applyF0Analysis(std::move(pc));
                if (mod->audioModification != nullptr)
                    mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
            }
        });
    });
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

// RestoredMaterializationBinding removed — ARA archive now uses ContentKey + content payload.
// Legacy archive records are skipped during doRestoreObjectsFromStream.

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

// -----------------------------------------------------------------------
// 编辑器只读内容访问器实现
// -----------------------------------------------------------------------

std::shared_ptr<const juce::AudioBuffer<float>> OpenTuneDocumentController::readAudioBuffer(ContentKey key) const
{
    PlaybackReadSource crsSrc;
    if (contentRenderService_ != nullptr && contentRenderService_->getPlaybackReadSource(key, crsSrc))
        return crsSrc.audioBuffer;
    return nullptr;
}

std::shared_ptr<PitchCurve> OpenTuneDocumentController::readPitchCurve(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return nullptr;
    return mod->content.analysis.pitchCurve;
}

OriginalF0State OpenTuneDocumentController::readOriginalF0State(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return OriginalF0State::NotRequested;
    return mod->content.analysis.originalF0State;
}

DetectedKey OpenTuneDocumentController::readDetectedKey(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return {};
    return mod->content.analysis.detectedKey;
}

std::vector<Note> OpenTuneDocumentController::readNotes(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return {};
    return mod->content.editable.notes;
}

uint64_t OpenTuneDocumentController::readNotesRevision(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return 0;
    return mod->content.editable.notesRevision;
}

std::shared_ptr<const TimeGridSnapshot> OpenTuneDocumentController::readTimeGrid(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return nullptr;
    return mod->content.editable.timeGrid;
}

uint64_t OpenTuneDocumentController::readTimeGridRevision(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return 0;
    return mod->content.editable.timeGridRevision;
}

PitchShiftSettings OpenTuneDocumentController::readPitchShift(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return {};
    return mod->content.editable.pitchShiftSettings;
}

RenderCache::ChunkStats OpenTuneDocumentController::readChunkStats(ContentKey key) const
{
    PlaybackReadSource crsSrc;
    if (contentRenderService_ != nullptr
        && contentRenderService_->getPlaybackReadSource(key, crsSrc)
        && crsSrc.renderCache != nullptr)
    {
        return crsSrc.renderCache->getChunkStats();
    }
    return {};
}

bool OpenTuneDocumentController::readChunkBoundaries(ContentKey key, std::vector<double>& outSeconds) const
{
    outSeconds.clear();

    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr)
        return false;

    PlaybackReadSource crsSrc;
    if (contentRenderService_ == nullptr || !contentRenderService_->getPlaybackReadSource(key, crsSrc))
        return false;
    if (crsSrc.audioBuffer == nullptr)
        return false;

    const int64_t sampleCount = static_cast<int64_t>(crsSrc.audioBuffer->getNumSamples());
    const auto& silentGaps = mod->content.analysis.silentGaps;
    constexpr int hopSize = 512; // DC 没有 vocoderDomain_，使用默认值

    auto boundaries = RenderChunkPlanner::buildChunkBoundariesFromSilentGaps(
        sampleCount, silentGaps, hopSize);

    outSeconds.reserve(boundaries.size());
    for (auto sample : boundaries)
        outSeconds.push_back(static_cast<double>(sample) / TimeCoordinate::kRenderSampleRate);

    return true;
}

uint64_t OpenTuneDocumentController::readContentRevision(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return 0;
    return mod->content.contentRevision;
}

double OpenTuneDocumentController::readMaterializationDuration(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return 0.0;
    return mod->materializationDurationSeconds;
}

uint64_t OpenTuneDocumentController::readSourceId(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return 0;
    return mod->sourceId;
}

bool OpenTuneDocumentController::hasContent(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    return mod != nullptr && mod->content.lifecycle == ContentLifecycle::Ready;
}

std::shared_ptr<const EditableContentSnapshot>
OpenTuneDocumentController::readContentSnapshot(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (!mod) return nullptr;
    return mod->snapshotContent();
}

} // namespace OpenTune

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<OpenTune::OpenTuneDocumentController>();
}
