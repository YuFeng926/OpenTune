#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <ARA_Library/Dispatch/ARAHostDispatch.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>
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
}

int runChild(int argc, char** argv)
{
    juce::ignoreUnused(argc, argv);
    OpenTuneTest::trace("ARALifecycleContractTests", "process_begin", {
        {"watchdogMs", OpenTuneTest::jsonNumber(OpenTuneTest::lifecycleWatchdogApprovalTimeoutMs)},
        {"queueLimit", OpenTuneTest::jsonNumber(OpenTuneTest::lifecycleQueueApprovalLimit)}
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
