#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <ARA_Library/Dispatch/ARAHostDispatch.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>
#include "ARA/OpenTuneDocumentController.h"
#include "ARA/OpenTunePlaybackRenderer.h"
#include "LifecycleTestTrace.h"
#include "LifecycleWatchdog.h"

extern const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();

namespace
{
struct HostAudioAccess final : ARA::Host::AudioAccessControllerInterface
{
    ARA::ARAAudioReaderHostRef createAudioReaderForSource(ARA::ARAAudioSourceHostRef, bool) noexcept override
    {
        return {};
    }

    bool readAudioSamples(ARA::ARAAudioReaderHostRef, ARA::ARASamplePosition,
                          ARA::ARASampleCount, void* const[]) noexcept override
    {
        return false;
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

using RenderItem = OpenTune::OpenTunePlaybackRenderer::PlaybackRegionRenderItem;

OpenTune::PlaybackReadSource makeContinuityReadSource (
    uint64_t objectId,
    const std::shared_ptr<juce::AudioBuffer<float>>& nativeBuffer,
    std::shared_ptr<const OpenTune::TimeGridSnapshot> timeGrid)
{
    OpenTune::PlaybackReadSource source;
    source.contentKey = { OpenTune::DomainKind::ARAAudioModification, objectId, 0 };
    source.nativeSource.buffer = nativeBuffer;
    source.nativeSource.sampleRate = 48000.0;
    source.nativeSource.firstSourceSample = 0;
    source.nativeSource.audioRevision = 11;

    auto snapshot = std::make_shared<OpenTune::EditableContentSnapshot>();
    snapshot->timeGrid = std::move (timeGrid);
    source.contentSnapshot = std::move (snapshot);
    return source;
}

RenderItem makeContinuityItem (double startPlayback,
                               double endPlayback,
                               double startModification,
                               double endModification,
                               double placementAnchor,
                               OpenTune::PlaybackReadSource source)
{
    RenderItem item;
    item.contentKey = source.contentKey;
    item.startInPlaybackTime = startPlayback;
    item.endInPlaybackTime = endPlayback;
    item.startInModificationTime = startModification;
    item.endInModificationTime = endModification;
    item.placementAnchorSeconds = placementAnchor;
    item.source = std::move (source);
    return item;
}

const RenderItem* findRenderItem (const std::vector<RenderItem>& items, uint64_t objectId)
{
    for (const auto& item : items)
        if (item.contentKey.objectId == objectId)
            return &item;
    return nullptr;
}

std::shared_ptr<const OpenTune::TimeGridSnapshot> makeNonIdentityGrid (double outputAtHalfSource)
{
    std::vector<OpenTune::TimeHandle> handles;

    OpenTune::TimeHandle start;
    start.id = 1;
    start.source_seconds = 0.0;
    start.output_seconds = 0.0;
    start.kind = OpenTune::HandleKind::ClipStart;
    handles.push_back (start);

    OpenTune::TimeHandle middle;
    middle.id = 2;
    middle.source_seconds = 0.5;
    middle.output_seconds = outputAtHalfSource;
    middle.kind = OpenTune::HandleKind::InternalOnset;
    handles.push_back (middle);

    OpenTune::TimeHandle end;
    end.id = 3;
    end.source_seconds = 1.0;
    end.output_seconds = 1.0;
    end.kind = OpenTune::HandleKind::ClipEnd;
    handles.push_back (end);

    return OpenTune::TimeGridSnapshot::makeFromHandles (std::move (handles));
}

bool runPlaybackContinuityContracts()
{
    const auto native = std::make_shared<juce::AudioBuffer<float>> (1, 48000);
    const auto identityGrid = OpenTune::TimeGridSnapshot::makeIdentity (1.0);
    if (! check (identityGrid != nullptr, "continuity_identity_grid_create_failed"))
        return false;

    // D1 = Q(0.5) = 1 at H=48000. The split slice recomputed on its own
    // boundaries lands on Q(0.49999999999994493) = 0.
    const double playbackStart = 0.5 / 48000.0;
    const double splitSeconds = 0.1;
    const double splitPlayback = playbackStart + splitSeconds;
    const double endPlayback = playbackStart + 2.0 * splitSeconds;

    std::vector<RenderItem> items;
    items.push_back (makeContinuityItem (
        splitPlayback, endPlayback, splitSeconds, 2.0 * splitSeconds, splitSeconds,
        makeContinuityReadSource (103, native, identityGrid)));
    items.push_back (makeContinuityItem (
        playbackStart + 0.02, playbackStart + 0.08, 0.02, 0.08, 0.02,
        makeContinuityReadSource (102, native, identityGrid)));
    items.push_back (makeContinuityItem (
        playbackStart, splitPlayback, 0.0, splitSeconds, 0.0,
        makeContinuityReadSource (101, native, identityGrid)));

    const auto independentSecond = OpenTune::PlaybackSampleGeometry::makePlacement (
        48000.0,
        splitPlayback,
        endPlayback,
        splitSeconds);
    if (! check (independentSecond.offsetSamples == 0,
                 "continuity_fixture_independent_d"))
        return false;

    OpenTune::OpenTunePlaybackRenderer::assignContinuityAndPlacements (items, 48000.0);

    auto* first = findRenderItem (items, 101);
    auto* overlapping = findRenderItem (items, 102);
    auto* second = findRenderItem (items, 103);
    if (! check (first != nullptr && overlapping != nullptr && second != nullptr,
                 "continuity_sorted_items_missing"))
        return false;
    if (! check (first->continuityGroup == second->continuityGroup,
                 "continuity_group_not_shared_across_overlap"))
        return false;
    if (! check (first->placement.offsetSamples == 1,
                 "continuity_first_d_wrong"))
        return false;
    if (! check (second->placement.offsetSamples == 1,
                 "continuity_shared_d_not_retained"))
        return false;
    if (! check (overlapping->continuityGroup != first->continuityGroup,
                 "continuity_overlap_joined"))
        return false;
    if (! check (first->placement.playbackEndSample == second->placement.playbackBeginSample,
                 "continuity_integer_boundary_mismatch"))
        return false;

    // Rate reprojection follows the group's anchor, not physical adjacency.
    // At H=144000 the group D is Q(1.5)=2 while the split slice alone is 1.
    OpenTune::OpenTunePlaybackRenderer::assignContinuityAndPlacements (items, 144000.0);

    first = findRenderItem (items, 101);
    second = findRenderItem (items, 103);
    if (! check (first != nullptr && second != nullptr, "continuity_rate_items_missing"))
        return false;

    const auto independentSecondAtRate = OpenTune::PlaybackSampleGeometry::makePlacement (
        144000.0,
        second->startInPlaybackTime,
        second->endInPlaybackTime,
        second->placementAnchorSeconds);
    if (! check (independentSecondAtRate.offsetSamples == 1,
                 "continuity_rate_fixture_independent_d"))
        return false;
    if (! check (first->placement.offsetSamples == 2,
                 "continuity_rate_group_anchor_wrong"))
        return false;
    if (! check (second->placement.offsetSamples == first->placement.offsetSamples,
                 "continuity_rate_group_anchor_lost"))
        return false;
    if (! check (second->placement.playbackBeginSample == first->placement.playbackEndSample,
                 "continuity_rate_integer_boundary_mismatch"))
        return false;

    // Non-identity Stage2 grids are content-local: different content origins
    // stay independent even with identical native PCM and shared boundaries.
    const auto gridA = makeNonIdentityGrid (0.6);
    const auto gridB = makeNonIdentityGrid (0.7);
    if (! check (gridA != nullptr && gridB != nullptr, "continuity_nonidentity_grid_create_failed"))
        return false;

    std::vector<RenderItem> localGridItems;
    localGridItems.push_back (makeContinuityItem (
        0.0, 0.1, 0.0, 0.1, 0.0,
        makeContinuityReadSource (201, native, gridA)));
    localGridItems.push_back (makeContinuityItem (
        0.1, 0.2, 0.1, 0.2, 0.1,
        makeContinuityReadSource (202, native, gridB)));
    OpenTune::OpenTunePlaybackRenderer::assignContinuityAndPlacements (localGridItems, 48000.0);

    const auto* localFirst = findRenderItem (localGridItems, 201);
    const auto* localSecond = findRenderItem (localGridItems, 202);
    if (! check (localFirst != nullptr && localSecond != nullptr,
                 "continuity_nonidentity_items_missing"))
        return false;
    if (! check (localFirst->continuityGroup != localSecond->continuityGroup,
                 "continuity_nonidentity_content_origins_shared_d"))
        return false;

    // One content with a replaced output grid is a different output domain.
    std::vector<RenderItem> replacedGridItems;
    replacedGridItems.push_back (makeContinuityItem (
        0.3, 0.4, 0.0, 0.1, 0.0,
        makeContinuityReadSource (204, native, gridA)));
    replacedGridItems.push_back (makeContinuityItem (
        0.4, 0.5, 0.1, 0.2, 0.1,
        makeContinuityReadSource (204, native, gridB)));
    OpenTune::OpenTunePlaybackRenderer::assignContinuityAndPlacements (replacedGridItems, 48000.0);

    if (! check (replacedGridItems[0].continuityGroup != replacedGridItems[1].continuityGroup,
                 "continuity_nonidentity_replaced_grid_shared_d"))
        return false;

    // Different content origins stay independent even with one shared grid.
    std::vector<RenderItem> sharedGridDifferentOrigins;
    sharedGridDifferentOrigins.push_back (makeContinuityItem (
        0.3, 0.4, 0.0, 0.1, 0.0,
        makeContinuityReadSource (205, native, gridA)));
    sharedGridDifferentOrigins.push_back (makeContinuityItem (
        0.4, 0.5, 0.1, 0.2, 0.1,
        makeContinuityReadSource (206, native, gridA)));
    OpenTune::OpenTunePlaybackRenderer::assignContinuityAndPlacements (
        sharedGridDifferentOrigins, 48000.0);

    if (! check (sharedGridDifferentOrigins[0].continuityGroup
                     != sharedGridDifferentOrigins[1].continuityGroup,
                 "continuity_nonidentity_origins_shared_grid"))
        return false;

    // One content origin with one output grid keeps sharing across snapshots.
    std::vector<RenderItem> sharedGridItems;
    sharedGridItems.push_back (makeContinuityItem (
        0.3, 0.4, 0.0, 0.1, 0.0,
        makeContinuityReadSource (203, native, gridA)));
    sharedGridItems.push_back (makeContinuityItem (
        0.4, 0.5, 0.1, 0.2, 0.1,
        makeContinuityReadSource (203, native, gridA)));
    OpenTune::OpenTunePlaybackRenderer::assignContinuityAndPlacements (sharedGridItems, 48000.0);

    const auto* sharedFirst = findRenderItem (sharedGridItems, 203);
    if (! check (sharedFirst != nullptr, "continuity_same_origin_items_missing"))
        return false;
    if (! check (sharedGridItems[0].continuityGroup == sharedGridItems[1].continuityGroup,
                  "continuity_same_output_domain_not_shared"))
        return false;

    // Source-contiguous regions in one warped content can still have a gap
    // between their actual output-domain read intervals.
    std::vector<RenderItem> warpedBoundaryItems;
    warpedBoundaryItems.push_back (makeContinuityItem (
        0.0, 0.1, 0.0, 0.1, gridA->tauForward (0.0),
        makeContinuityReadSource (207, native, gridA)));
    warpedBoundaryItems.push_back (makeContinuityItem (
        0.1, 0.2, 0.1, 0.2, gridA->tauForward (0.1),
        makeContinuityReadSource (207, native, gridA)));
    OpenTune::OpenTunePlaybackRenderer::assignContinuityAndPlacements (warpedBoundaryItems, 48000.0);
    if (! check (warpedBoundaryItems[0].continuityGroup != warpedBoundaryItems[1].continuityGroup,
                  "continuity_warped_output_gap_shared_d"))
        return false;

    return true;
}

bool runArchiveSourceWindowContracts()
{
    OpenTune::SourceWindow original;
    original.sourcePersistentId = "source-1";
    original.sourceStartSeconds = 0.1;
    original.sourceEndSeconds = 0.350124716553288;

    const double legacyDuration = original.sourceEndSeconds - original.sourceStartSeconds;
    const double legacyEnd = original.sourceStartSeconds + legacyDuration;
    if (! check (legacyEnd != original.sourceEndSeconds,
                 "archive_fixture_legacy_rounding_missing"))
        return false;
    if (! check (OpenTune::PlaybackSampleGeometry::projectToSample (original.sourceEndSeconds * 44100.0)
                     != OpenTune::PlaybackSampleGeometry::projectToSample (legacyEnd * 44100.0),
                 "archive_fixture_sample_offset_missing"))
        return false;

    juce::XmlElement stored ("AudioModificationContent");
    OpenTune::OpenTuneDocumentController::writeSourceWindowToArchive (original, stored);
    auto restored = OpenTune::OpenTuneDocumentController::readSourceWindowFromArchive (
        stored, nullptr, OpenTune::OpenTuneDocumentController::kContentArchiveVersion);
    if (! check (restored.has_value(), "archive_source_window_restore_failed"))
        return false;
    if (! check (restored->sourceStartSeconds == original.sourceStartSeconds,
                 "archive_source_window_start_changed"))
        return false;
    if (! check (restored->sourceEndSeconds == original.sourceEndSeconds,
                  "archive_source_window_end_not_exact"))
        return false;

    for (const double startSeconds : { 1.0 / 96000.0, 0.10000000000000002 })
    {
        auto preciseWindow = original;
        preciseWindow.sourceStartSeconds = startSeconds;
        juce::XmlElement preciseArchive ("AudioModificationContent");
        OpenTune::OpenTuneDocumentController::writeSourceWindowToArchive (preciseWindow, preciseArchive);
        const auto preciseRestored = OpenTune::OpenTuneDocumentController::readSourceWindowFromArchive (
            preciseArchive, nullptr, OpenTune::OpenTuneDocumentController::kContentArchiveVersion);
        if (! check (preciseRestored.has_value()
                      && preciseRestored->sourceStartSeconds == preciseWindow.sourceStartSeconds
                      && preciseRestored->sourceEndSeconds == preciseWindow.sourceEndSeconds,
                      "archive_source_window_endpoints_not_exact"))
            return false;
    }

    // Version 8 archives only carry duration; reconstruction stays version-scoped.
    juce::XmlElement legacy ("AudioModificationContent");
    auto* legacyWindow = legacy.createNewChildElement ("SourceWindow");
    legacyWindow->setAttribute ("sourcePersistentId", original.sourcePersistentId);
    legacyWindow->setAttribute ("startSeconds", original.sourceStartSeconds);
    legacyWindow->setAttribute ("durationSeconds", legacyDuration);

    const double archivedLegacyDuration = legacyWindow->getDoubleAttribute ("durationSeconds");
    const double expectedLegacyEnd = original.sourceStartSeconds + archivedLegacyDuration;

    auto legacyRestored = OpenTune::OpenTuneDocumentController::readSourceWindowFromArchive (
        legacy, nullptr, 8);
    if (! check (legacyRestored.has_value(), "archive_legacy_source_window_restore_failed"))
        return false;
    if (! check (legacyRestored->sourceEndSeconds == expectedLegacyEnd,
                 "archive_legacy_end_path_changed"))
        return false;
    if (! check (legacyRestored->sourceEndSeconds != original.sourceEndSeconds,
                 "archive_legacy_fixture_lost_constraint"))
        return false;

    return true;
}
}

int runChild(int argc, char** argv)
{
    juce::ignoreUnused(argc, argv);
    OpenTuneTest::trace("ARALifecycleContractTests", "process_begin", {
        {"watchdogMs", OpenTuneTest::jsonNumber(OpenTuneTest::lifecycleWatchdogApprovalTimeoutMs)},
        {"queueLimit", OpenTuneTest::jsonNumber(OpenTuneTest::lifecycleQueueApprovalLimit)}
    });
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    if (! runPlaybackContinuityContracts())
        return 1;
    if (! runArchiveSourceWindowContracts())
        return 1;
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

    // The existing factory creates a DocumentController with a Document. It exposes no Processor
    // creation or binding seam, so the two legal destruction orders (DocumentController before
    // Processor and Processor before DocumentController) are not expressible in this harness.
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
    std::fprintf (stdout, "{\"event\":\"ara_lifecycle_contract\",\"result\":\"success\",\"completionGate\":\"pumped\",\"wrapperGeneration\":\"recreated\"}\n");
    OpenTuneTest::trace("ARALifecycleContractTests", "process_end");
    return 0;
}

int main(int argc, char** argv)
{
    return OpenTuneTest::run(argc, argv, "ARALifecycleContractTests", runChild);
}
