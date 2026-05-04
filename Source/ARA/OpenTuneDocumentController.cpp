#include "OpenTuneDocumentController.h"

#include "OpenTunePlaybackRenderer.h"
#include "PluginProcessor.h"
#include "Utils/AppLogger.h"
#include "SourceStore.h"
#include "MaterializationStore.h"
#include "DSP/ResamplingManager.h"

#include <algorithm>

namespace OpenTune {

OpenTuneDocumentController::~OpenTuneDocumentController()
{
    // Detach the processor pointer so the hydration worker cannot call back into
    // a partially-destroyed processor while session_ is being torn down.
    session_->setProcessor(nullptr);
    processor_ = nullptr;
    // session_ unique_ptr destructor runs here: joins hydration worker safely.
}

OpenTuneDocumentController::OpenTuneDocumentController(const ARA::PlugIn::PlugInEntry* entry,
                                                       const ARA::ARADocumentControllerHostInstance* instance)
    : ARADocumentControllerSpecialisation(entry, instance)
    , session_(std::make_unique<VST3AraSession>())
    , sharedSourceStore_(std::make_shared<SourceStore>())
    , sharedMaterializationStore_(std::make_shared<MaterializationStore>())
    , sharedResamplingManager_(std::make_shared<ResamplingManager>())
{
    AppLogger::log("ARA: DocumentController created, session owned");
}

void OpenTuneDocumentController::setProcessor(OpenTuneAudioProcessor* processor)
{
    processor_ = processor;
    session_->setProcessor(processor);
}

OpenTuneDocumentController::SnapshotHandle OpenTuneDocumentController::loadSnapshot() const
{
    return session_->loadSnapshot();
}

void OpenTuneDocumentController::didUpdateMusicalContextProperties(juce::ARAMusicalContext* musicalContext)
{
    juce::ignoreUnused(musicalContext);
    AppLogger::log("ARA: didUpdateMusicalContextProperties called");
}

void OpenTuneDocumentController::willBeginEditing(juce::ARADocument* document)
{
    juce::ignoreUnused(document);
    session_->willBeginEditing();
}

void OpenTuneDocumentController::didEndEditing(juce::ARADocument* document)
{
    juce::ignoreUnused(document);
    session_->didEndEditing();
}

void OpenTuneDocumentController::didUpdatePlaybackRegionProperties(juce::ARAPlaybackRegion* playbackRegion)
{
    session_->didUpdatePlaybackRegionProperties(playbackRegion);
}

void OpenTuneDocumentController::willDestroyPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion)
{
    session_->willRemovePlaybackRegionFromAudioModification(playbackRegion);

    if (processor_ != nullptr)
        processor_->scheduleReclaimSweep();
}

void OpenTuneDocumentController::didAddPlaybackRegionToAudioModification(
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    session_->didAddPlaybackRegionToAudioModification(audioModification, playbackRegion);
}

void OpenTuneDocumentController::didUpdateAudioSourceProperties(juce::ARAAudioSource* audioSource)
{
    session_->didUpdateAudioSourceProperties(audioSource);
}

void OpenTuneDocumentController::doUpdateAudioSourceContent(juce::ARAAudioSource* audioSource,
                                                            juce::ARAContentUpdateScopes scopeFlags)
{
    session_->doUpdateAudioSourceContent(audioSource, scopeFlags);
}

void OpenTuneDocumentController::willEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                                                    bool enable)
{
    session_->willEnableAudioSourceSamplesAccess(audioSource, enable);
}

void OpenTuneDocumentController::didEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                                                   bool enable)
{
    session_->didEnableAudioSourceSamplesAccess(audioSource, enable);
}

void OpenTuneDocumentController::willRemovePlaybackRegionFromAudioModification(
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    juce::ignoreUnused(audioModification);
    session_->willRemovePlaybackRegionFromAudioModification(playbackRegion);

    if (processor_ != nullptr)
        processor_->scheduleReclaimSweep();
}

void OpenTuneDocumentController::willDestroyAudioSource(juce::ARAAudioSource* audioSource)
{
    session_->willDestroyAudioSource(audioSource);

    if (processor_ != nullptr)
        processor_->scheduleReclaimSweep();
}

bool OpenTuneDocumentController::doRestoreObjectsFromStream(juce::ARAInputStream& input,
                                                            const juce::ARARestoreObjectsFilter* filter)
{
    juce::ignoreUnused(input, filter);
    return true;
}

bool OpenTuneDocumentController::doStoreObjectsToStream(juce::ARAOutputStream& output,
                                                        const juce::ARAStoreObjectsFilter* filter)
{
    juce::ignoreUnused(output, filter);
    return true;
}

juce::ARAPlaybackRenderer* OpenTuneDocumentController::doCreatePlaybackRenderer()
{
    return new OpenTunePlaybackRenderer(getDocumentController());
}

bool OpenTuneDocumentController::requestSetPlaybackPosition(double timeInSeconds)
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
    {
        AppLogger::error("[ARA] DocumentController is null, cannot set playback position");
        return false;
    }

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
    {
        AppLogger::error("[ARA] PlaybackController not available");
        return false;
    }

    playbackController->requestSetPlaybackPosition(timeInSeconds);
    AppLogger::log(juce::String("[ARA] requestSetPlaybackPosition -> ") + juce::String(timeInSeconds, 3) + "s");
    return true;
}

bool OpenTuneDocumentController::requestStartPlayback()
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
    {
        AppLogger::error("[ARA] DocumentController is null, cannot start playback");
        return false;
    }

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
    {
        AppLogger::error("[ARA] PlaybackController not available, cannot start playback");
        return false;
    }

    playbackController->requestStartPlayback();
    AppLogger::log("[ARA] requestStartPlayback forwarded to host");
    return true;
}

bool OpenTuneDocumentController::requestStopPlayback()
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
    {
        AppLogger::error("[ARA] DocumentController is null, cannot stop playback");
        return false;
    }

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
    {
        AppLogger::error("[ARA] PlaybackController not available, cannot stop playback");
        return false;
    }

    playbackController->requestStopPlayback();
    AppLogger::log("[ARA] requestStopPlayback forwarded to host");
    return true;
}

} // namespace OpenTune

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<OpenTune::OpenTuneDocumentController>();
}
