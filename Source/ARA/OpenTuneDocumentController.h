#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

#include "VST3AraSession.h"

namespace OpenTune {

class OpenTuneAudioProcessor;
class SourceStore;
class MaterializationStore;
class ResamplingManager;

class OpenTuneDocumentController : public juce::ARADocumentControllerSpecialisation
{
public:
    using RegionIdentity = VST3AraSession::RegionIdentity;
    using AppliedMaterializationProjection = VST3AraSession::AppliedMaterializationProjection;
    using SourceSlot = VST3AraSession::SourceSlot;
    using RegionSlot = VST3AraSession::RegionSlot;
    using PublishedRegionView = VST3AraSession::PublishedRegionView;
    using PublishedSnapshot = VST3AraSession::PublishedSnapshot;
    using SnapshotHandle = VST3AraSession::SnapshotHandle;

    OpenTuneDocumentController(const ARA::PlugIn::PlugInEntry* entry,
                               const ARA::ARADocumentControllerHostInstance* instance);

    ~OpenTuneDocumentController() override;

    void setProcessor(OpenTuneAudioProcessor* processor);
    OpenTuneAudioProcessor* getProcessor() const { return processor_; }

    VST3AraSession* getSession() noexcept { return session_.get(); }
    const VST3AraSession* getSession() const noexcept { return session_.get(); }

    std::shared_ptr<SourceStore> getSharedSourceStore() const { return sharedSourceStore_; }
    std::shared_ptr<MaterializationStore> getSharedMaterializationStore() const { return sharedMaterializationStore_; }
    std::shared_ptr<ResamplingManager> getSharedResamplingManager() const { return sharedResamplingManager_; }

    SnapshotHandle loadSnapshot() const;

    void didUpdateMusicalContextProperties(juce::ARAMusicalContext* musicalContext) override;
    void willBeginEditing(juce::ARADocument* document) override;
    void didEndEditing(juce::ARADocument* document) override;

    void didUpdatePlaybackRegionProperties(juce::ARAPlaybackRegion* playbackRegion) override;
    void willDestroyPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion) override;
    void didAddPlaybackRegionToAudioModification(juce::ARAAudioModification* audioModification,
                                                 juce::ARAPlaybackRegion* playbackRegion) override;

    void didUpdateAudioSourceProperties(juce::ARAAudioSource* audioSource) override;
    void doUpdateAudioSourceContent(juce::ARAAudioSource* audioSource,
                                    juce::ARAContentUpdateScopes scopeFlags) override;
    void willEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                            bool enable) override;
    void didEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                           bool enable) override;
    void willRemovePlaybackRegionFromAudioModification(juce::ARAAudioModification* audioModification,
                                                       juce::ARAPlaybackRegion* playbackRegion) override;

    void willDestroyAudioSource(juce::ARAAudioSource* audioSource) override;

    bool requestSetPlaybackPosition(double timeInSeconds);
    bool requestStartPlayback();
    bool requestStopPlayback();

protected:
    bool doRestoreObjectsFromStream(juce::ARAInputStream& input,
                                    const juce::ARARestoreObjectsFilter* filter) override;
    bool doStoreObjectsToStream(juce::ARAOutputStream& output,
                                const juce::ARAStoreObjectsFilter* filter) override;

    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() override;

private:
    std::shared_ptr<SourceStore> sharedSourceStore_;
    std::shared_ptr<MaterializationStore> sharedMaterializationStore_;
    std::shared_ptr<ResamplingManager> sharedResamplingManager_;

    std::unique_ptr<VST3AraSession> session_;
    OpenTuneAudioProcessor* processor_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneDocumentController)
};

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();

} // namespace OpenTune
