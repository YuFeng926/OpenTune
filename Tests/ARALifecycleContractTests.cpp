#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <ARA_Library/Dispatch/ARAHostDispatch.h>
#include "../Source/PluginProcessor.h"
#include "../Source/ARA/OpenTuneDocumentController.h"
#include "../Source/ARA/OpenTunePlaybackRenderer.h"
#include "../Source/Runtime/ProcessF0Runtime.h"
#include "../Source/Runtime/ProcessRenderRuntime.h"
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
struct ProcessRenderRuntimeTestAccess
{
    static bool isControlWorkerJoinable(ProcessRenderRuntime& runtime)
    {
        return runtime.isControlWorkerJoinable();
    }
};

struct F0ExtractionServiceTestAccess
{
    static std::unique_ptr<F0ExtractionService> createAraFace(size_t maxQueueSize)
    {
        return std::unique_ptr<F0ExtractionService>(new F0ExtractionService(maxQueueSize));
    }
};

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
    juce::MemoryBlock bytes;
    ARA::ARAPersistentID archiveId{};

    ARA::ARASize getArchiveSize(ARA::ARAArchiveReaderHostRef) noexcept override { return bytes.getSize(); }
    bool readBytesFromArchive(ARA::ARAArchiveReaderHostRef, ARA::ARASize offset,
                              ARA::ARASize count, ARA::ARAByte destination[]) noexcept override
    {
        bytes.copyTo(destination, static_cast<int>(offset), count);
        return true;
    }
    bool writeBytesToArchive(ARA::ARAArchiveWriterHostRef, ARA::ARASize offset,
                             ARA::ARASize count, const ARA::ARAByte source[]) noexcept override
    {
        bytes.ensureSize(offset + count);
        bytes.copyFrom(source, static_cast<int>(offset), count);
        return true;
    }
    void notifyDocumentArchivingProgress(float) noexcept override {}
    void notifyDocumentUnarchivingProgress(float) noexcept override {}
    ARA::ARAPersistentID getDocumentArchiveID(ARA::ARAArchiveReaderHostRef) noexcept override
    {
        return archiveId;
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

bool runUndispatchedCompletionRegression()
{
    int dispatched = 0;
    int discarded = 0;
    {
        juce::ScopedJuceInitialiser_GUI juceInitialiser;
        if (!check(OpenTune::ProcessRenderRuntime::postAsyncCallback([&](bool ran) {
                if (ran)
                    ++dispatched;
                else
                    ++discarded;
            }), "undispatched_completion_post_failed"))
            return false;
    }
    return check(dispatched == 0 && discarded == 1,
                 "undispatched_completion_not_released_once");
}

bool runNoteTopologyRegression(OpenTune::OpenTuneDocumentController& controller,
                               ARA::Host::DocumentController& host,
                               HostArchiving& archive,
                               const std::vector<juce::ARAPlaybackRegion*>& regions)
{
    using OpenTune::AudioEditingScheme::Scheme;
    const auto projections = controller.getPlaybackRegionProjectionsFor(regions);
    if (!check(projections.size() == 4, "note_topology_region_projections_missing"))
        return false;
    const auto restoredKey = projections[0].contentKey;
    const auto deletedKey = projections[1].contentKey;
    const auto unvoicedKey = projections[2].contentKey;
    const auto freshKey = projections[3].contentKey;
    const auto makeF0 = [](float hz) {
        auto curve = std::make_shared<OpenTune::PitchCurve>();
        curve->replaceOriginalAnalysis(std::vector<float>(64, hz), {}, 160, 16000.0);
        return curve;
    };

    auto& preferences = controller.getAppPreferences();
    preferences.setAudioEditingScheme(Scheme::CorrectedF0Primary);
    controller.applyOriginalF0ToModification(restoredKey, makeF0(220.0f));
    controller.applyOriginalF0ToModification(deletedKey, makeF0(220.0f));
    controller.applyNotesToModification(deletedKey, {});
    bool passed = check(!controller.readContentSnapshot(restoredKey)->noteTopologyInitialized,
                        "corrected_f0_mode_initialized_topology");
    if (!check(host.storeObjectsToArchive(reinterpret_cast<ARA::ARAArchiveWriterHostRef>(&archive), nullptr),
               "note_topology_archive_write_failed"))
        return false;

    preferences.setAudioEditingScheme(Scheme::NotesPrimary);
    const auto onSwitch = controller.readContentSnapshot(restoredKey);
    passed = check(onSwitch->noteTopologyInitialized && !onSwitch->notes.empty(),
                   "scheme_change_did_not_initialize_ready_topology") && passed;

    host.beginEditing();
    const bool restored = host.restoreObjectsFromArchive(
        reinterpret_cast<ARA::ARAArchiveReaderHostRef>(&archive), nullptr);
    const bool wasDeferred = !controller.readContentSnapshot(restoredKey)->noteTopologyInitialized;
    host.endEditing();
    if (!check(restored && wasDeferred, "note_topology_archive_batch_not_deferred"))
        return false;

    // The archive's Ready F0 carries no input stamp (the modifications were
    // filled through the API without a birth), so the restore clears the
    // stale analysis and arms the pending continuation instead of adopting.
    // User state survives: topology flags and archived notes stay untouched
    // and no PCM materializes without sample access.
    const auto onRestore = controller.readContentSnapshot(restoredKey);
    passed = check(onRestore->originalF0State == OpenTune::OriginalF0State::NotRequested
                       && onRestore->notes.empty()
                       && controller.readAudioBuffer(restoredKey) == nullptr,
                   "stampless_ready_archive_cleared_pending_reanalysis") && passed;
    const auto deleted = controller.readContentSnapshot(deletedKey);
    passed = check(deleted->noteTopologyInitialized && deleted->notes.empty(),
                   "restored_deleted_topology_regenerated") && passed;

    controller.applyOriginalF0ToModification(unvoicedKey, makeF0(0.0f));
    controller.applyOriginalF0ToModification(freshKey, makeF0(220.0f));
    const auto unvoiced = controller.readContentSnapshot(unvoicedKey);
    const auto fresh = controller.readContentSnapshot(freshKey);
    passed = check(unvoiced->noteTopologyInitialized && unvoiced->notes.empty()
                       && fresh->noteTopologyInitialized && !fresh->notes.empty(),
                   "ready_deliveries_did_not_initialize_each_modification") && passed;

    controller.applyOriginalF0ToModification(unvoicedKey, makeF0(220.0f));
    controller.applyOriginalF0ToModification(freshKey, makeF0(220.0f));
    preferences.setAudioEditingScheme(Scheme::CorrectedF0Primary);
    preferences.setAudioEditingScheme(Scheme::NotesPrimary);
    passed = check(controller.readContentSnapshot(unvoicedKey)->notesRevision == unvoiced->notesRevision
                       && controller.readContentSnapshot(freshKey)->notesRevision == fresh->notesRevision
                       && controller.readContentSnapshot(deletedKey)->notesRevision == deleted->notesRevision,
                   "initialized_topology_committed_twice") && passed;

    // The cleared analysis (pending continuation) has no original F0, so an
    // explicit note generation is rejected and the notes stay empty until the
    // re-analysis completes.
    const bool generated = controller.generateNotesOnly(deletedKey, {});
    passed = check(!generated
                       && controller.readContentSnapshot(deletedKey)->notes.empty(),
                   "explicit_generation_rejected_without_ready_f0") && passed;
    return passed;
}

bool runArchiveRestoreStampRegression(ARA::Host::DocumentController& documentController,
                                      HostArchiving& archive,
                                      OpenTune::OpenTuneDocumentController& controller,
                                      ARA::ARARegionSequenceRef regionSequence)
{
    using OpenTune::OriginalF0State;
    bool passed = true;
    const auto expect = [&passed](bool condition, const char* name)
    {
        passed = check(condition, name) && passed;
        return condition;
    };

    ARA::ARAAudioSourceProperties sourceProperties {};
    sourceProperties.structSize = sizeof (sourceProperties);
    sourceProperties.sampleCount = 9600;
    sourceProperties.sampleRate = 48000.0;
    sourceProperties.channelCount = 1;

    ARA::ARAAudioModificationProperties modificationProperties {};
    modificationProperties.structSize = sizeof (modificationProperties);

    ARA::ARAPlaybackRegionProperties regionProperties {};
    regionProperties.structSize = sizeof (regionProperties);
    regionProperties.regionSequenceRef = regionSequence;
    regionProperties.startInModificationTime = 0.0;
    regionProperties.durationInModificationTime = 0.2;
    regionProperties.startInPlaybackTime = 0.0;
    regionProperties.durationInPlaybackTime = 0.2;

    ARA::ARAPersistentID archiveSourceId = "restore-source-1";
    ARA::ARAPersistentID archiveModificationId = "restore-mod-1";
    ARA::ARAPersistentID pasteSourceId = "restore-source-2";
    ARA::ARAPersistentID pasteModificationId = "restore-mod-2";

    auto archiveSourceProps = sourceProperties;
    archiveSourceProps.persistentID = archiveSourceId;
    auto pasteSourceProps = sourceProperties;
    pasteSourceProps.persistentID = pasteSourceId;
    auto archiveModProps = modificationProperties;
    archiveModProps.persistentID = archiveModificationId;
    auto pasteModProps = modificationProperties;
    pasteModProps.persistentID = pasteModificationId;

    documentController.beginEditing();
    const auto archiveSource = documentController.createAudioSource(nullptr, &archiveSourceProps);
    const auto pasteSource = documentController.createAudioSource(nullptr, &pasteSourceProps);
    const auto archiveModification = documentController.createAudioModification(archiveSource, nullptr, &archiveModProps);
    const auto pasteModification = documentController.createAudioModification(pasteSource, nullptr, &pasteModProps);
    const auto archiveRegion = documentController.createPlaybackRegion(archiveModification, nullptr, &regionProperties);
    const auto pasteRegion = documentController.createPlaybackRegion(pasteModification, nullptr, &regionProperties);
    documentController.endEditing();
    if (!expect(archiveSource != nullptr && pasteSource != nullptr
                    && archiveModification != nullptr && pasteModification != nullptr
                    && archiveRegion != nullptr && pasteRegion != nullptr,
                "restore_stamp_graph_creation_failed"))
        return false;

    documentController.enableAudioSourceSamplesAccess(archiveSource, true);
    documentController.enableAudioSourceSamplesAccess(pasteSource, true);

    const std::vector<juce::ARAPlaybackRegion*> archiveRegionProjections{
        ARA::PlugIn::fromRef<juce::ARAPlaybackRegion>(archiveRegion)};
    const std::vector<juce::ARAPlaybackRegion*> pasteRegionProjections{
        ARA::PlugIn::fromRef<juce::ARAPlaybackRegion>(pasteRegion)};
    const auto archiveKey = controller.getPlaybackRegionProjectionsFor(archiveRegionProjections)[0].contentKey;
    const auto pasteKey = controller.getPlaybackRegionProjectionsFor(pasteRegionProjections)[0].contentKey;

    // User Read births the archive modification and records its input stamp.
    // The queued F0 completion stays undispatched and never reaches the state
    // under test; a restore-time revocation releases it without a commit.
    if (!expect(controller.requestReadAudioForPlaybackRegion(archiveRegionProjections[0]) == 1,
                "restore_stamp_read_birth_failed"))
        return false;

    // Ready F0 plus user edits: a manual correction segment and an erased range.
    auto curve = std::make_shared<OpenTune::PitchCurve>();
    curve->replaceOriginalAnalysis(std::vector<float>(64, 220.0f), {}, 160, 16000.0);
    OpenTune::PitchCorrectionSegment segment;
    segment.startFrame = 8;
    segment.endFrame = 16;
    segment.f0Data.assign(8, 440.0f);
    segment.baseF0Data.assign(8, 220.0f);
    segment.source = OpenTune::PitchCorrectionSegment::Source::HandDraw;
    curve->replaceCorrectionSegments({segment});
    curve->replaceErasedRangesInRange(0, 64, {{32, 40}});
    if (!expect(controller.applyOriginalF0ToModification(archiveKey, curve),
                "restore_stamp_f0_apply_failed"))
        return false;

    const auto setupCurve = controller.readPitchCurve(archiveKey);
    expect(setupCurve != nullptr
               && setupCurve->getErasedRanges().size() == 1
               && setupCurve->getErasedRanges().front().startFrame == 32
               && setupCurve->getErasedRanges().front().endFrameExclusive == 40,
           "restore_stamp_setup_edits_missing");

    if (!expect(documentController.storeObjectsToArchive(
                    reinterpret_cast<ARA::ARAArchiveWriterHostRef>(&archive), nullptr),
                "restore_stamp_archive_write_failed"))
        return false;

    // Same-source restore: the archived stamp matches the live parent, so the
    // Ready F0 is adopted without a new F0 run (the state never leaves Ready).
    ARA::ARARestoreObjectsFilter sameSourceFilter {};
    sameSourceFilter.structSize = sizeof (sameSourceFilter);
    sameSourceFilter.audioSourceIDsCount = 1;
    sameSourceFilter.audioSourceArchiveIDs = &archiveSourceId;
    sameSourceFilter.audioModificationIDsCount = 1;
    sameSourceFilter.audioModificationArchiveIDs = &archiveModificationId;

    documentController.beginEditing();
    const bool sameSourceRestored = documentController.restoreObjectsFromArchive(
        reinterpret_cast<ARA::ARAArchiveReaderHostRef>(&archive), &sameSourceFilter);
    documentController.endEditing();

    const auto sameSourceState = controller.readOriginalF0State(archiveKey);
    const auto sameSourceCurve = controller.readPitchCurve(archiveKey);
    expect(sameSourceRestored && sameSourceState == OriginalF0State::Ready,
           "same_source_restore_lost_ready_f0");
    expect(controller.readAudioBuffer(archiveKey) != nullptr,
           "same_source_restore_did_not_materialize_pcm");
    expect(sameSourceCurve != nullptr
               && sameSourceCurve->getOriginalF0().size() == 64
               && sameSourceCurve->getOriginalF0()[0] == 220.0f
               && sameSourceCurve->getCorrectionSegments().size() == 1
               && sameSourceCurve->getCorrectionSegments().front().f0Data.size() == 8
               && sameSourceCurve->getErasedRanges().size() == 1
               && sameSourceCurve->getErasedRanges().front().startFrame == 32,
           "same_source_restore_edits_damaged");

    // Cross-source paste restore: the archived stamp carries the archive source
    // id, so it mismatches the paste target. The stale analysis clears, the
    // user's edits survive and exactly one re-analysis is requested.
    ARA::ARARestoreObjectsFilter crossSourceFilter {};
    crossSourceFilter.structSize = sizeof (crossSourceFilter);
    crossSourceFilter.audioSourceIDsCount = 1;
    crossSourceFilter.audioSourceArchiveIDs = &archiveSourceId;
    crossSourceFilter.audioSourceCurrentIDs = &pasteSourceId;
    crossSourceFilter.audioModificationIDsCount = 1;
    crossSourceFilter.audioModificationArchiveIDs = &archiveModificationId;
    crossSourceFilter.audioModificationCurrentIDs = &pasteModificationId;

    documentController.beginEditing();
    const bool crossSourceRestored = documentController.restoreObjectsFromArchive(
        reinterpret_cast<ARA::ARAArchiveReaderHostRef>(&archive), &crossSourceFilter);
    documentController.endEditing();

    const auto pasteState = controller.readOriginalF0State(pasteKey);
    const auto pasteCurve = controller.readPitchCurve(pasteKey);
    expect(crossSourceRestored && pasteState == OriginalF0State::Extracting,
           "stamp_mismatch_restore_did_not_request_one_reanalysis");
    expect(pasteCurve != nullptr
               && pasteCurve->getOriginalF0().empty()
               && pasteCurve->getCorrectionSegments().size() == 1
               && pasteCurve->getCorrectionSegments().front().f0Data.size() == 8
               && pasteCurve->getErasedRanges().size() == 1
               && pasteCurve->getErasedRanges().front().startFrame == 32,
           "stamp_mismatch_restore_user_edits_damaged");

    // The one re-analysis commit re-derives the original F0 while erased frames
    // stay zero (mirror of commitF0Completion's replaceOriginalAnalysis).
    auto completionCurve = OpenTune::PitchCurve::fromSnapshot(pasteCurve);
    completionCurve->replaceOriginalAnalysis(std::vector<float>(64, 180.0f), {}, 160, 16000.0);
    const auto completed = completionCurve->getSnapshot();
    expect(completed->getOriginalF0()[34] == 0.0f && completed->getOriginalF0()[0] == 180.0f,
           "reanalysis_erase_frames_not_zeroed");

    // Clone of a non-Ready modification keeps the correction layer and the
    // erased ranges without resurrecting the cleared original analysis.
    ARA::ARAAudioModificationProperties cloneModProps {};
    cloneModProps.structSize = sizeof (cloneModProps);
    cloneModProps.persistentID = "restore-mod-clone";
    documentController.beginEditing();
    const auto cloneModification = documentController.cloneAudioModification(
        pasteModification, nullptr, &cloneModProps);
    const auto cloneRegion = cloneModification != nullptr
        ? documentController.createPlaybackRegion(cloneModification, nullptr, &regionProperties)
        : nullptr;
    documentController.endEditing();
    if (!expect(cloneModification != nullptr && cloneRegion != nullptr,
                "restore_stamp_clone_creation_failed"))
        return false;

    const std::vector<juce::ARAPlaybackRegion*> cloneRegionProjections{
        ARA::PlugIn::fromRef<juce::ARAPlaybackRegion>(cloneRegion)};
    const auto cloneKey = controller.getPlaybackRegionProjectionsFor(cloneRegionProjections)[0].contentKey;
    const auto cloneCurve = controller.readPitchCurve(cloneKey);
    expect(cloneCurve != nullptr
               && cloneCurve->getOriginalF0().empty()
               && cloneCurve->getCorrectionSegments().size() == 1
               && cloneCurve->getCorrectionSegments().front().f0Data.size() == 8
               && cloneCurve->getErasedRanges().size() == 1
               && cloneCurve->getErasedRanges().front().startFrame == 32,
           "clone_non_ready_lost_user_edits");

    documentController.enableAudioSourceSamplesAccess(archiveSource, false);
    documentController.enableAudioSourceSamplesAccess(pasteSource, false);
    documentController.beginEditing();
    documentController.destroyPlaybackRegion(archiveRegion);
    documentController.destroyPlaybackRegion(pasteRegion);
    documentController.destroyPlaybackRegion(cloneRegion);
    documentController.destroyAudioModification(archiveModification);
    documentController.destroyAudioModification(pasteModification);
    documentController.destroyAudioModification(cloneModification);
    documentController.destroyAudioSource(archiveSource);
    documentController.destroyAudioSource(pasteSource);
    documentController.endEditing();
    return passed;
}
}

int runChild(int argc, char** argv)
{
    juce::ignoreUnused(argc, argv);
    OpenTuneTest::trace("ARALifecycleContractTests", "process_begin", {
        {"watchdogMs", OpenTuneTest::jsonNumber(OpenTuneTest::lifecycleWatchdogApprovalTimeoutMs)}
    });
    if (!runUndispatchedCompletionRegression())
        return 1;

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
    archiving.archiveId = factory->documentArchiveID;
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

    auto* controller = juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<
        OpenTune::OpenTuneDocumentController>(
            ARA::PlugIn::fromRef<ARA::PlugIn::DocumentController>(instance->documentControllerRef));
    std::vector<ARA::ARAPlaybackRegionRef> topologyRegions;
    std::vector<juce::ARAPlaybackRegion*> topologyProjections{
        ARA::PlugIn::fromRef<juce::ARAPlaybackRegion>(region)};
    documentController.beginEditing();
    for (std::size_t i = 0; i < 3; ++i)
    {
        const auto extraRegion = documentController.createPlaybackRegion(extraModifications[i], nullptr, &regionProperties);
        topologyRegions.push_back(extraRegion);
        topologyProjections.push_back(ARA::PlugIn::fromRef<juce::ARAPlaybackRegion>(extraRegion));
    }
    documentController.endEditing();
    const bool noteTopologyPassed = runNoteTopologyRegression(*controller, documentController, archiving, topologyProjections);
    const bool restoreStampPassed = runArchiveRestoreStampRegression(
        documentController, archiving, *controller, regionSequence);

    documentController.beginEditing();
    for (auto extraRegion : topologyRegions)
        documentController.destroyPlaybackRegion(extraRegion);
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

    documentController.endEditing();

    auto& f0Runtime = OpenTune::ProcessF0Runtime::getInstance();
    auto& renderRuntime = OpenTune::ProcessRenderRuntime::getInstance();
    f0Runtime.shutdownAraAnalysisAndJoin();

    // Pump-shutdown convergence: a delivery queued without a running dispatch
    // loop is released on the value side exactly once by the non-message-thread
    // drain branch instead of waiting for a dispatcher forever. This phase runs
    // before the dispatch session below because stopDispatchLoop makes every
    // later post fail.
    int pendingReleased = 0;
    const bool pendingPosted = OpenTune::ProcessRenderRuntime::postAsyncCallback(
        [&pendingReleased](bool dispatched) {
            if (!dispatched)
                ++pendingReleased;
        });
    if (!check(pendingPosted, "pump_shutdown_pending_post_failed"))
        return 1;
    std::thread shutdownJoin([&renderRuntime] { renderRuntime.shutdownAndJoin(); });
    shutdownJoin.join();
    if (!check(pendingReleased == 1, "pump_shutdown_pending_delivery_not_released_once"))
        return 1;

    int rejected = 0;
    const bool postedAfterShutdown = OpenTune::ProcessRenderRuntime::postAsyncCallback([&](bool dispatched) {
        if (!dispatched)
            ++rejected;
    });
    const bool shutdownPassed = check(
        !OpenTune::ProcessRenderRuntimeTestAccess::isControlWorkerJoinable(renderRuntime)
            && !postedAfterShutdown && rejected == 1,
        "final_delivery_fence_not_closed");

    // ARA face: a dispatcher rejection releases the batch as a value without
    // ever invoking the document sink.
    bool araFacePassed = true;
    {
        OpenTune::AraF0DocumentFlights flights;
        std::atomic<int> sinkCalls{0};
        flights.sink = std::make_shared<OpenTune::AraF0DocumentFlights::CompletionSink>(
            [&sinkCalls](const std::vector<OpenTune::F0TaskId>&, const OpenTune::AraF0Result&) {
                ++sinkCalls;
            });

        auto araFace = OpenTune::F0ExtractionServiceTestAccess::createAraFace(4);
        OpenTune::OriginalF0InputStamp stamp;
        stamp.sourcePersistentId = "ara-rejection-source";
        stamp.sourceStartSeconds = 0.0;
        stamp.sourceEndSeconds = 0.2;
        stamp.sourceSampleRate = 48000.0;
        stamp.numSamples = 9600;
        stamp.numChannels = 1;
        const OpenTune::AraF0LocalKey local{stamp,
            OpenTune::F0ExtractionService::snapshotAraConfig(48000.0)};
        const OpenTune::AraF0Receiver receiver{
            OpenTune::ContentKey{OpenTune::DomainKind::ARAAudioModification, 7002, 0},
            1, 1, "ara-rejection-modification", stamp};
        const auto submission = araFace->submit(flights, local,
            OpenTune::AraF0Pcm{std::vector<float>(9600, 0.25f), 48000.0, 1}, receiver);
        if (check(submission.admission == OpenTune::AraF0Admission::Accepted,
                  "ara_face_regression_submit_rejected"))
        {
            while (araFace->isActive(submission.taskId))
                std::this_thread::yield();
            araFacePassed = check(sinkCalls == 0, "ara_face_rejection_invoked_document_sink");
        }
        else
            araFacePassed = false;
    }

    // Processor face: a dispatcher rejection re-delivers the failed result
    // through the processor-private mailbox, settling waiting content as
    // Failed with its reason. The mailbox commit posts the loop-stop message,
    // so the message-dispatch assertion below rides the same dispatch session
    // and every queued message runs before the loop exits.
    bool mailboxPassed = true;
    std::atomic<bool> dispatcherRan{false};
    {
        OpenTune::F0ExtractionService processorFace(1, 4, {});
        std::atomic<int> committed{0};
        std::atomic<int> mailboxDelivered{0};
        std::string mailboxReason;
        const auto submitResult = processorFace.submit(
            OpenTune::F0RequestKey{OpenTune::ContentKey{OpenTune::DomainKind::StandaloneClip, 7001, 0}},
            [](const std::shared_ptr<OpenTune::F0RunOwnerState>&) {
                OpenTune::F0ExtractionService::Result result;
                result.errorMessage = "dispatcher rejection regression";
                return result;
            },
            [&committed](OpenTune::F0ExtractionService::Result&&) { ++committed; },
            [&](OpenTune::F0ExtractionService::Result&& result) {
                mailboxReason = result.errorMessage;
                ++mailboxDelivered;
                juce::MessageManager::callAsync([] {
                    juce::MessageManager::getInstance()->stopDispatchLoop();
                });
            });
        if (check(submitResult == OpenTune::F0ExtractionService::SubmitResult::Accepted,
                  "processor_mailbox_regression_submit_rejected")
            && check(juce::MessageManager::callAsync([&dispatcherRan] {
                   dispatcherRan.store(true, std::memory_order_release);
               }), "message_dispatch_post_failed"))
        {
            juce::MessageManager::getInstance()->runDispatchLoop();
            mailboxPassed = check(mailboxDelivered == 1 && committed == 0
                                      && mailboxReason == "dispatcher rejection regression",
                                  "processor_mailbox_did_not_deliver_failed_result");
            mailboxPassed = check(dispatcherRan.load(std::memory_order_acquire),
                                  "message_dispatch_not_run") && mailboxPassed;
        }
        else
            mailboxPassed = false;
    }
    OpenTuneTest::trace("ARALifecycleContractTests", "message_dispatch_complete");

    documentController.beginEditing();
    documentController.destroyAudioModification (rebuiltModification);
    documentController.destroyAudioSource (rebuiltSource);
    documentController.endEditing();
    documentController.destroyDocumentController();
    factory->uninitializeARA();

    juce::Timer::callPendingTimersSynchronously();
    const bool passed = rendererLifecyclePassed && noteTopologyPassed && restoreStampPassed
        && shutdownPassed && mailboxPassed && araFacePassed;
    if (passed)
        std::fprintf (stdout, "{\"event\":\"ara_lifecycle_contract\",\"result\":\"success\",\"completionGate\":\"pumped\",\"wrapperGeneration\":\"recreated\",\"noteTopology\":\"initialized_once\",\"restoreStamp\":\"adopted_or_reanalyzed\"}\n");
    OpenTuneTest::trace("ARALifecycleContractTests", "process_end");
    return passed ? 0 : 1;
}

int main(int argc, char** argv)
{
    return OpenTuneTest::run(argc, argv, "ARALifecycleContractTests", runChild);
}
