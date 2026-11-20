#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <ARA_Library/Dispatch/ARAHostDispatch.h>
#include "../Source/PluginProcessor.h"
#include "../Source/ARA/OpenTuneDocumentController.h"
#include "../Source/ARA/OpenTunePlaybackRenderer.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include "LifecycleTestTrace.h"
#include "LifecycleWatchdog.h"

extern const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();

namespace OpenTune
{
struct OpenTunePlaybackRendererTestAccess
{
    using Renderer = OpenTunePlaybackRenderer;

    template <typename Callback>
    static void withLifecycleRead(Renderer& renderer, Callback callback)
    {
        Renderer::LifecycleReadGuard lifecycle(renderer);
        RealtimeSnapshot<Renderer::RenderPlan>::Read plan;
        if (lifecycle)
            plan = renderer.currentPlan_.read();
        callback(static_cast<bool>(lifecycle) && static_cast<bool>(plan), plan.get());
    }

    static bool lifecycleClosing(const Renderer& renderer)
    {
        return (renderer.lifecycleGate_.load(std::memory_order_acquire)
                & Renderer::lifecycleClosingBit_) != 0;
    }

    static bool fillRetiredPlans(Renderer& renderer)
    {
        auto read = renderer.currentPlan_.read();
        if (!read || renderer.currentPlan_.latestOwner() == nullptr)
            return false;

        for (int index = 0; index < 4; ++index)
        {
            auto next = std::make_shared<Renderer::RenderPlan>(
                *renderer.currentPlan_.latestOwner());
            renderer.currentPlan_.publish(std::move(next));
        }

        return renderer.currentPlan_.retiredCount() == 4
            && !renderer.currentPlan_.hasPending();
    }

    static bool publishedPlanIsLatest(Renderer& renderer)
    {
        const auto& latest = renderer.currentPlan_.latestOwner();
        const auto published = renderer.currentPlan_.read();
        return latest != nullptr && published.get() == latest.get();
    }

    static void publishTopologyDuringRateChange(
        Renderer& renderer,
        double hostSampleRate,
        double planSampleRate,
        std::vector<juce::ARAPlaybackRegion*> playbackRegions)
    {
        renderer.hostSampleRate_.store(hostSampleRate, std::memory_order_release);
        renderer.pendingRenderPlanSampleRate_.store(hostSampleRate, std::memory_order_release);
        renderer.publishRenderPlanFor(std::move(playbackRegions), planSampleRate);
    }

    static bool latestPlanMatches(Renderer& renderer,
                                  double sampleRate,
                                  const std::vector<juce::ARAPlaybackRegion*>& playbackRegions)
    {
        const auto& latest = renderer.currentPlan_.latestOwner();
        return latest != nullptr
            && latest->targetSampleRate == sampleRate
            && latest->playbackRegions == playbackRegions;
    }

    static bool snapshotsReleased(Renderer& renderer)
    {
        const auto published = renderer.currentPlan_.read();
        return !published
            && renderer.currentPlan_.retiredCount() == 0
            && !renderer.currentPlan_.hasPending()
            && renderer.contentRenderService_ == nullptr
            && renderer.notePitchAudition_.load(std::memory_order_acquire) == nullptr;
    }
};

struct AuditionRealtimeTestAccess
{
    static int stagePitch(NotePitchAudition& audition,
                          uint64_t generation,
                          float value,
                          double sampleRate,
                          const void* targetRegion)
    {
        const int index = audition.claimBuildSlot();
        if (index < 0)
            return -1;

        audition.discardPendingSlots();
        auto& slot = audition.slots_[static_cast<size_t>(index)];
        slot.snippet.mono.assign(2048, value);
        slot.snippet.sampleRate = sampleRate;
        slot.snippet.targetRegion = targetRegion;
        slot.generation = generation;
        audition.controlGeneration_.store(generation, std::memory_order_release);
        slot.state.store(static_cast<uint8_t>(NotePitchAudition::SlotState::Pending),
                         std::memory_order_release);
        return index;
    }

    static uint64_t pitchStopGeneration(const NotePitchAudition& audition)
    {
        return audition.stopGeneration_.load(std::memory_order_acquire);
    }

    static void setActiveTargetRegion(NotePitchAudition& audition, const void* region)
    {
        audition.activeTargetRegion_.store(region, std::memory_order_release);
    }
};

struct PluginProcessorARALifecycleTestAccess
{
    static bool releaseOwnedNotePitchMixer(AuditionRoute& route,
                                           juce::ARAEditorRenderer* owner)
    {
        return OpenTuneAudioProcessor::releaseOwnedNotePitchMixer(
            route.notePitchMixer, route.notePitchAudition, owner);
    }
};
}

namespace
{
struct HostAudioAccess final : ARA::Host::AudioAccessControllerInterface
{
    static constexpr ARA::ARASamplePosition pcmSampleCount = 48000;

    ARA::ARAAudioReaderHostRef createAudioReaderForSource(ARA::ARAAudioSourceHostRef,
                                                           bool) noexcept override
    {
        return reinterpret_cast<ARA::ARAAudioReaderHostRef>(this);
    }

    bool readAudioSamples(ARA::ARAAudioReaderHostRef, ARA::ARASamplePosition samplePosition,
                          ARA::ARASampleCount samplesPerChannel, void* const buffers[]) noexcept override
    {
        const auto sampleCount = static_cast<ARA::ARASamplePosition>(samplesPerChannel);
        if (samplePosition < 0 || samplePosition + sampleCount > pcmSampleCount)
            return false;

        std::fill_n(static_cast<float*>(buffers[0]), static_cast<size_t>(samplesPerChannel), 0.25f);
        return true;
    }

    void destroyAudioReader(ARA::ARAAudioReaderHostRef) noexcept override {}
};

struct HostArchiving final : ARA::Host::ArchivingControllerInterface
{
    ARA::ARASize getArchiveSize(ARA::ARAArchiveReaderHostRef) noexcept override { return 0; }
    bool readBytesFromArchive(ARA::ARAArchiveReaderHostRef, ARA::ARASize,
                              ARA::ARASize, ARA::ARAByte[]) noexcept override { return false; }
    bool writeBytesToArchive(ARA::ARAArchiveWriterHostRef, ARA::ARASize,
                             ARA::ARASize, const ARA::ARAByte[]) noexcept override { return false; }
    void notifyDocumentArchivingProgress(float) noexcept override {}
    void notifyDocumentUnarchivingProgress(float) noexcept override {}
    ARA::ARAPersistentID getDocumentArchiveID(ARA::ARAArchiveReaderHostRef) noexcept override
    {
        return "ARALifecycleContractTests";
    }
};

bool check (bool value, const char* name)
{
    if (! value)
        std::fprintf (stderr, "{\"event\":\"ara_lifecycle_contract\",\"result\":\"failure\",\"reason\":\"%s\"}\n", name);
    return value;
}

bool runPlaybackRendererLifecycleRegression(ARA::ARADocumentControllerRef documentControllerRef,
                                            ARA::Host::DocumentController& hostDocumentController,
                                            ARA::ARAAudioSourceRef audioSource,
                                            ARA::ARAPlaybackRegionRef playbackRegionRef)
{
    constexpr double initialSampleRate = 48000.0;
    constexpr double switchedSampleRate = 44100.0;
    constexpr int blockSize = 256;
    bool passed = true;
    const auto expect = [&passed](bool condition, const char* name)
    {
        passed = check(condition, name) && passed;
        return condition;
    };

    OpenTune::OpenTuneAudioProcessor firstProcessor;
    OpenTune::OpenTuneAudioProcessor secondProcessor;
    const auto playbackRole = static_cast<ARA::ARAPlugInInstanceRoleFlags>(ARA::kARAPlaybackRendererRole);
    const auto* firstInstance = firstProcessor.bindToARA(documentControllerRef, playbackRole, playbackRole);
    const auto* secondInstance = secondProcessor.bindToARA(documentControllerRef, playbackRole, playbackRole);
    if (!expect(firstInstance != nullptr && secondInstance != nullptr,
                "renderer_plugin_binding_failed"))
        return false;

    auto* firstRenderer = firstProcessor.getPlaybackRenderer<OpenTune::OpenTunePlaybackRenderer>();
    auto* secondRenderer = secondProcessor.getPlaybackRenderer<OpenTune::OpenTunePlaybackRenderer>();
    auto* controller = firstProcessor.getDocumentController();
    if (!expect(firstRenderer != nullptr && secondRenderer != nullptr && controller != nullptr,
                "renderer_public_api_unavailable"))
        return false;
    if (!expect(secondProcessor.getDocumentController() == controller,
                "renderers_do_not_share_document_controller"))
        return false;

    if (!expect(firstInstance->playbackRendererInterface != nullptr
                    && secondInstance->playbackRendererInterface != nullptr,
                "renderer_host_interface_unavailable"))
        return false;

    firstInstance->playbackRendererInterface->addPlaybackRegion(
        firstInstance->playbackRendererRef, playbackRegionRef);
    secondInstance->playbackRendererInterface->addPlaybackRegion(
        secondInstance->playbackRendererRef, playbackRegionRef);

    const auto prepare = [=](OpenTune::OpenTunePlaybackRenderer& renderer, double sampleRate)
    {
        renderer.prepareToPlay(sampleRate, blockSize, 1,
                               juce::AudioProcessor::ProcessingPrecision::singlePrecision,
                               juce::ARARenderer::AlwaysNonRealtime::no);
    };

    std::thread firstPrepare([&] { prepare(*firstRenderer, initialSampleRate); });
    std::thread secondPrepare([&] { prepare(*secondRenderer, initialSampleRate); });
    firstPrepare.join();
    secondPrepare.join();
    controller->collectPlaybackSnapshots();

    hostDocumentController.enableAudioSourceSamplesAccess(audioSource, true);
    auto* playbackRegion = ARA::PlugIn::fromRef<juce::ARAPlaybackRegion>(playbackRegionRef);
    expect(playbackRegion != nullptr
               && controller->requestReadAudioForPlaybackRegion(playbackRegion) == 1,
           "fixed_pcm_playback_source_creation_failed");
    controller->collectPlaybackSnapshots();

    juce::AudioPlayHead::PositionInfo positionInfo;
    positionInfo.setTimeInSamples(0);
    positionInfo.setIsPlaying(true);
    const auto expectAudible = [&](OpenTune::OpenTunePlaybackRenderer& renderer,
                                   const char* name,
                                   bool checkFixedPcm)
    {
        juce::AudioBuffer<float> output(1, blockSize);
        const bool processed = renderer.processBlock(
            output, juce::AudioProcessor::Realtime::yes, positionInfo);
        const bool hasPcm = output.getMagnitude(0, 0, output.getNumSamples()) > 0.1f;
        const bool pcmMatches = !checkFixedPcm
            || std::abs(output.getSample(0, 0) - 0.25f) < 0.000001f;
        return expect(processed && hasPcm && pcmMatches, name);
    };
    const auto expectSilent = [&](OpenTune::OpenTunePlaybackRenderer& renderer, const char* name)
    {
        juce::AudioBuffer<float> output(1, blockSize);
        output.clear();
        const bool processed = renderer.processBlock(
            output, juce::AudioProcessor::Realtime::yes, positionInfo);
        return expect(processed && output.getMagnitude(0, 0, output.getNumSamples()) == 0.0f, name);
    };

    expectAudible(*firstRenderer, "same_rate_first_renderer_pcm_missing", true);
    expectAudible(*secondRenderer, "same_rate_second_renderer_pcm_missing", true);

    auto* route = controller->getAuditionRoute();
    auto& audition = controller->getNotePitchAudition();
    static int ownerTokenStorage = 0;
    static int replacementOwnerTokenStorage = 0;
    static const int teardownRegionToken = 0;
    auto* ownerToken = reinterpret_cast<juce::ARAEditorRenderer*>(&ownerTokenStorage);
    auto* replacementOwnerToken =
        reinterpret_cast<juce::ARAEditorRenderer*>(&replacementOwnerTokenStorage);
    route->notePitchMixer.store(ownerToken, std::memory_order_release);

    if (!expect(OpenTune::AuditionRealtimeTestAccess::stagePitch(
                    audition, 1, 0.4f, initialSampleRate, playbackRegion) >= 0,
                "note_pitch_target_voice_stage_failed"))
        return false;
    juce::AudioBuffer<float> auditionBlock(1, 1);
    auditionBlock.clear();
    audition.mixIntoBuffer(auditionBlock, auditionBlock.getNumSamples(), initialSampleRate);
    if (!expect(audition.activeTargetRegion() == playbackRegion,
                "note_pitch_target_region_not_published"))
        return false;
    expectSilent(*firstRenderer, "note_pitch_target_region_not_suppressed");

    const uint64_t stopGenerationBefore =
        OpenTune::AuditionRealtimeTestAccess::pitchStopGeneration(audition);
    firstProcessor.stopNotePitchAudition();
    expect(route->notePitchMixer.load(std::memory_order_acquire) == ownerToken,
           "ordinary_stop_released_the_editor_mixer_owner");
    expect(OpenTune::AuditionRealtimeTestAccess::pitchStopGeneration(audition)
               > stopGenerationBefore,
           "ordinary_stop_did_not_publish_a_stop_generation");

    juce::AudioBuffer<float> fadeBlock(1, 128);
    fadeBlock.clear();
    audition.mixIntoBuffer(fadeBlock, fadeBlock.getNumSamples(), initialSampleRate);
    expect(audition.isActive() && audition.activeTargetRegion() == playbackRegion,
           "stop_cleared_target_before_audio_fade_completed");
    expectSilent(*firstRenderer, "playback_resumed_during_note_pitch_fade");

    juce::AudioBuffer<float> fadeRemainder(1, 512);
    fadeRemainder.clear();
    audition.mixIntoBuffer(fadeRemainder, fadeRemainder.getNumSamples(), initialSampleRate);
    expect(!audition.isActive() && audition.activeTargetRegion() == nullptr,
           "note_pitch_fade_did_not_clear_its_target");
    expectAudible(*firstRenderer, "playback_did_not_resume_after_note_pitch_stop", true);

    OpenTune::AuditionRealtimeTestAccess::setActiveTargetRegion(
        audition, &teardownRegionToken);
    route->notePitchMixer.store(replacementOwnerToken, std::memory_order_release);
    const uint64_t beforeForeignRelease =
        OpenTune::AuditionRealtimeTestAccess::pitchStopGeneration(audition);
    expect(!OpenTune::PluginProcessorARALifecycleTestAccess::releaseOwnedNotePitchMixer(
               *route, ownerToken)
               && route->notePitchMixer.load(std::memory_order_acquire) == replacementOwnerToken
               && audition.activeTargetRegion() == &teardownRegionToken
               && OpenTune::AuditionRealtimeTestAccess::pitchStopGeneration(audition)
                   == beforeForeignRelease,
           "owner_teardown_cleared_a_new_renderer_route");

    route->notePitchMixer.store(ownerToken, std::memory_order_release);
    const uint64_t beforeOwnerRelease =
        OpenTune::AuditionRealtimeTestAccess::pitchStopGeneration(audition);
    expect(OpenTune::PluginProcessorARALifecycleTestAccess::releaseOwnedNotePitchMixer(
               *route, ownerToken)
               && route->notePitchMixer.load(std::memory_order_acquire) == nullptr
               && audition.activeTargetRegion() == nullptr
               && OpenTune::AuditionRealtimeTestAccess::pitchStopGeneration(audition)
                   > beforeOwnerRelease,
           "owner_teardown_did_not_clear_its_route_and_suppression_target");

    std::thread firstRelease([&] { firstRenderer->releaseResources(); });
    std::thread secondRelease([&] { secondRenderer->releaseResources(); });
    firstRelease.join();
    secondRelease.join();

    std::thread firstRateChange([&] { prepare(*firstRenderer, switchedSampleRate); });
    firstRateChange.join();
    expectSilent(*firstRenderer, "renderer_rate_change_was_not_silent_before_collect");

    // A second renderer can prepare before the first pending plan is projected.
    // Its request must not leave the first renderer with a permanently stale
    // prepared source view. The second request is coalesced by the rate change
    // below before that renderer's message-thread projection is consumed.
    std::thread secondReprepare([&] { prepare(*secondRenderer, initialSampleRate); });
    secondReprepare.join();
    firstRenderer->collectPlaybackSnapshots();
    expectAudible(*firstRenderer, "renderer_pcm_missing_after_rate_collect", false);

    std::thread secondRateChange([&] { prepare(*secondRenderer, switchedSampleRate); });
    secondRateChange.join();
    expectSilent(*secondRenderer, "second_renderer_rate_change_not_silent_before_collect");
    controller->collectPlaybackSnapshots();
    expectAudible(*secondRenderer, "second_renderer_pcm_missing_after_rate_collect", false);

    prepare(*firstRenderer, switchedSampleRate);
    expectAudible(*firstRenderer, "message_thread_prepare_did_not_publish_ready_plan", false);

    const std::vector<juce::ARAPlaybackRegion*> postAddTopology{playbackRegion};
    const std::vector<juce::ARAPlaybackRegion*> postRemoveTopology;
    OpenTune::OpenTunePlaybackRendererTestAccess::publishTopologyDuringRateChange(
        *firstRenderer, switchedSampleRate, switchedSampleRate, postRemoveTopology);
    expect(OpenTune::OpenTunePlaybackRendererTestAccess::latestPlanMatches(
               *firstRenderer, switchedSampleRate, postRemoveTopology),
           "failed_to_prepare_empty_topology_for_stale_add");

    OpenTune::OpenTunePlaybackRendererTestAccess::publishTopologyDuringRateChange(
        *firstRenderer, initialSampleRate, switchedSampleRate, postAddTopology);
    expect(OpenTune::OpenTunePlaybackRendererTestAccess::latestPlanMatches(
               *firstRenderer, switchedSampleRate, postAddTopology),
           "stale_rate_add_dropped_playback_region_topology");
    expectSilent(*firstRenderer, "stale_rate_add_bypassed_process_rate_gate");
    firstRenderer->collectPlaybackSnapshots();
    expect(OpenTune::OpenTunePlaybackRendererTestAccess::latestPlanMatches(
               *firstRenderer, initialSampleRate, postAddTopology),
           "collector_dropped_post_add_playback_region_topology");

    OpenTune::OpenTunePlaybackRendererTestAccess::publishTopologyDuringRateChange(
        *firstRenderer, switchedSampleRate, initialSampleRate, postRemoveTopology);
    expect(OpenTune::OpenTunePlaybackRendererTestAccess::latestPlanMatches(
               *firstRenderer, initialSampleRate, postRemoveTopology),
           "stale_rate_remove_dropped_playback_region_topology");
    firstRenderer->collectPlaybackSnapshots();
    expect(OpenTune::OpenTunePlaybackRendererTestAccess::latestPlanMatches(
               *firstRenderer, switchedSampleRate, postRemoveTopology),
           "collector_dropped_post_remove_playback_region_topology");

    if (expect(OpenTune::OpenTunePlaybackRendererTestAccess::fillRetiredPlans(*secondRenderer),
               "render_plan_retired_capacity_not_reached"))
    {
        secondRenderer->refreshRenderPlanFromDocument();
        expect(OpenTune::OpenTunePlaybackRendererTestAccess::publishedPlanIsLatest(*secondRenderer),
               "saturated_render_plan_not_published_before_refresh_return");
    }

    std::atomic<bool> inFlightReadStarted{false};
    std::atomic<bool> inFlightReadAdmitted{false};
    std::atomic<bool> borrowedPlanValidBeforeRelease{false};
    std::atomic<bool> detachFinished{false};
    std::thread inFlightRead([&]
    {
        OpenTune::OpenTunePlaybackRendererTestAccess::withLifecycleRead(
            *secondRenderer,
            [&](bool admitted, const OpenTune::OpenTunePlaybackRenderer::RenderPlan* plan)
            {
                inFlightReadAdmitted.store(admitted && plan != nullptr,
                                           std::memory_order_release);
                inFlightReadStarted.store(true, std::memory_order_release);
                if (!admitted || plan == nullptr)
                    return;

                while (!OpenTune::OpenTunePlaybackRendererTestAccess::lifecycleClosing(
                    *secondRenderer))
                    std::this_thread::yield();

                borrowedPlanValidBeforeRelease.store(
                    !detachFinished.load(std::memory_order_acquire)
                        && plan->targetSampleRate == switchedSampleRate
                        && !plan->items.empty(),
                    std::memory_order_release);
            });
    });
    while (!inFlightReadStarted.load(std::memory_order_acquire))
        std::this_thread::yield();

    secondRenderer->detachDocumentController(*controller);
    detachFinished.store(true, std::memory_order_release);
    inFlightRead.join();
    controller->unregisterPlaybackRenderer(*secondRenderer);
    expect(inFlightReadAdmitted.load(std::memory_order_acquire),
           "in_flight_renderer_read_was_not_admitted");
    expect(borrowedPlanValidBeforeRelease.load(std::memory_order_acquire),
           "renderer_borrow_changed_before_detach_completed");
    expect(detachFinished.load(std::memory_order_acquire)
               && OpenTune::OpenTunePlaybackRendererTestAccess::snapshotsReleased(*secondRenderer),
           "renderer_snapshots_not_reclaimed_after_detach");
    expectSilent(*secondRenderer, "detached_renderer_did_not_remain_silent");

    firstInstance->playbackRendererInterface->removePlaybackRegion(
        firstInstance->playbackRendererRef, playbackRegionRef);
    secondInstance->playbackRendererInterface->removePlaybackRegion(
        secondInstance->playbackRendererRef, playbackRegionRef);
    return passed;
}
}

int runChild(int argc, char** argv)
{
    juce::ignoreUnused(argc, argv);
    OpenTuneTest::trace("ARALifecycleContractTests", "process_begin", {
        {"watchdogMs", OpenTuneTest::jsonNumber(OpenTuneTest::lifecycleWatchdogApprovalTimeoutMs)}
    });
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    const auto* factory = createARAFactory();
    if (! check (factory != nullptr, "sdk_link_contract_unavailable"))
        return 1;

    ARA::ARAInterfaceConfiguration configuration {};
    configuration.structSize = sizeof (configuration);
    configuration.desiredApiGeneration = factory->highestSupportedApiGeneration;
    configuration.assertFunctionAddress = nullptr;
    factory->initializeARAWithConfiguration (&configuration);

    HostAudioAccess audioAccess;
    HostArchiving archiving;
    ARA::Host::DocumentControllerHostInstance host(&audioAccess, &archiving);
    ARA::ARADocumentProperties documentProperties {};
    documentProperties.structSize = sizeof (documentProperties);
    documentProperties.name = "ARALifecycleContractTests";

    const auto* instance = factory->createDocumentControllerWithDocument (&host, &documentProperties);
    if (! check (instance != nullptr, "document_controller_create_failed"))
    {
        factory->uninitializeARA();
        return 1;
    }

    ARA::Host::DocumentController documentController { instance };
    ARA::ARAMusicalContextProperties musicalContextProperties {};
    musicalContextProperties.structSize = sizeof (musicalContextProperties);
    musicalContextProperties.orderIndex = 0;
    const auto musicalContext = documentController.createMusicalContext (nullptr, &musicalContextProperties);
    if (! check (musicalContext != nullptr, "musical_context_create_failed"))
        return 1;

    ARA::ARARegionSequenceProperties regionSequenceProperties {};
    regionSequenceProperties.structSize = sizeof (regionSequenceProperties);
    regionSequenceProperties.orderIndex = 0;
    regionSequenceProperties.musicalContextRef = musicalContext;
    const auto regionSequence = documentController.createRegionSequence (nullptr, &regionSequenceProperties);
    if (! check (regionSequence != nullptr, "region_sequence_create_failed"))
        return 1;

    ARA::ARAAudioSourceProperties sourceProperties {};
    sourceProperties.structSize = sizeof (sourceProperties);
    sourceProperties.name = "source";
    sourceProperties.persistentID = "source-1";
    sourceProperties.sampleCount = 48000;
    sourceProperties.sampleRate = 48000.0;
    sourceProperties.channelCount = 1;

    const auto source = documentController.createAudioSource (nullptr, &sourceProperties);
    const auto sourceAgain = source;
    if (! check (source != nullptr && source == sourceAgain, "audio_source_wrapper_address_unstable"))
        return 1;

    ARA::ARAAudioModificationProperties modificationProperties {};
    modificationProperties.structSize = sizeof (modificationProperties);
    modificationProperties.persistentID = "modification-1";
    const auto modification = documentController.createAudioModification (source, nullptr, &modificationProperties);
    if (! check (modification != nullptr, "audio_modification_create_failed"))
        return 1;

    const auto stableModification = modification;
    std::vector<std::string> extraModificationIds;
    std::vector<std::remove_const_t<decltype(modification)>> extraModifications;
    extraModificationIds.reserve(32);
    extraModifications.reserve(32);
    for (int index = 0; index < 32; ++index)
    {
        extraModificationIds.push_back("modification-extra-" + std::to_string(index));
        auto extraProperties = modificationProperties;
        extraProperties.persistentID = extraModificationIds.back().c_str();
        const auto extra = documentController.createAudioModification(source, nullptr, &extraProperties);
        if (! check (extra != nullptr, "extra_audio_modification_create_failed"))
            return 1;
        extraModifications.push_back(extra);
    }
    if (! check (modification == stableModification, "audio_modification_address_changed_while_alive"))
        return 1;

    ARA::ARAPlaybackRegionProperties regionProperties {};
    regionProperties.structSize = sizeof (regionProperties);
    regionProperties.regionSequenceRef = regionSequence;
    regionProperties.startInModificationTime = 0.0;
    regionProperties.durationInModificationTime = 1.0;
    regionProperties.startInPlaybackTime = 0.0;
    regionProperties.durationInPlaybackTime = 1.0;
    const auto region = documentController.createPlaybackRegion (modification, nullptr, &regionProperties);
    if (! check (region != nullptr, "playback_region_create_failed"))
        return 1;

    const bool rendererLifecyclePassed = runPlaybackRendererLifecycleRegression(
        instance->documentControllerRef, documentController, source, region);
    documentController.enableAudioSourceSamplesAccess(source, false);

    documentController.destroyPlaybackRegion (region);
    documentController.destroyAudioModification (modification);
    for (auto* extra : extraModifications)
        documentController.destroyAudioModification (extra);

    const auto rebuiltStableModification = documentController.createAudioModification(
        source, nullptr, &modificationProperties);
    if (! check (rebuiltStableModification != nullptr,
                 "audio_modification_rebuild_failed"))
        return 1;
    documentController.destroyAudioModification (rebuiltStableModification);
    documentController.destroyAudioSource (source);
    documentController.destroyRegionSequence (regionSequence);
    documentController.destroyMusicalContext (musicalContext);

    const auto rebuiltSource = documentController.createAudioSource (nullptr, &sourceProperties);
    const auto rebuiltModification = documentController.createAudioModification (rebuiltSource, nullptr, &modificationProperties);
    if (! check (rebuiltSource != nullptr && rebuiltModification != nullptr,
                 "audio_model_rebuild_failed"))
        return 1;

    std::atomic<bool> dispatcherRan{false};
    if (! check (juce::MessageManager::callAsync ([&dispatcherRan]
        {
            dispatcherRan.store(true, std::memory_order_release);
            juce::MessageManager::getInstance()->stopDispatchLoop();
        }), "message_dispatch_post_failed"))
        return 1;
    juce::MessageManager::getInstance()->runDispatchLoop();
    if (! check (dispatcherRan.load(std::memory_order_acquire), "message_dispatch_not_run"))
        return 1;
    OpenTuneTest::trace("ARALifecycleContractTests", "message_dispatch_complete");

    documentController.destroyAudioModification (rebuiltModification);
    documentController.destroyAudioSource (rebuiltSource);
    documentController.destroyDocumentController();
    factory->uninitializeARA();

    juce::Timer::callPendingTimersSynchronously();
    if (rendererLifecyclePassed)
        std::fprintf (stdout, "{\"event\":\"ara_lifecycle_contract\",\"result\":\"success\",\"completionGate\":\"pumped\",\"wrapperGeneration\":\"recreated\"}\n");
    else
        std::fprintf (stderr, "{\"event\":\"ara_lifecycle_contract\",\"result\":\"failure\",\"reason\":\"renderer_lifecycle_regression_failed\"}\n");
    OpenTuneTest::trace("ARALifecycleContractTests", "process_end");
    return rendererLifecyclePassed ? 0 : 1;
}

int main(int argc, char** argv)
{
    return OpenTuneTest::run(argc, argv, "ARALifecycleContractTests", runChild);
}
