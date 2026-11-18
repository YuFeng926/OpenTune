#include "../Source/PluginProcessor.h"
#include "../Source/Content/StandaloneContentRepository.h"
#include "../Source/StandaloneArrangement.h"
#include "../Source/Utils/AppPreferences.h"
#include "../Source/Utils/ProjectSession.h"
#include "../Source/Utils/PlacementActions.h"
#include "../Source/SourceStore.h"

#include <cstdio>
#include <set>
#include <utility>

namespace {

using namespace OpenTune;

bool testActiveRetiredAndSharedSourceReferences()
{
    StandaloneContentRepository repository;
    const ContentKey firstKey = repository.createClip(1);
    const ContentKey secondKey = repository.createClip(2);
    auto* first = repository.findClip(firstKey);
    auto* second = repository.findClip(secondKey);
    if (first == nullptr || second == nullptr)
        return false;

    constexpr uint64_t sharedSourceId = 71;
    first->content().sourceWindow.sourceId = sharedSourceId;
    if (!repository.isSourceReferenced(sharedSourceId))
        return false;

    if (!repository.retireClip(firstKey)
        || first->content().sourceWindow.sourceId != 0
        || !repository.isSourceReferenced(sharedSourceId))
        return false;

    const auto* retired = repository.getRetiredContent(firstKey);
    if (retired == nullptr || retired->content.sourceWindow.sourceId != sharedSourceId)
        return false;

    if (!repository.retireClip(firstKey) || !repository.reviveClip(firstKey)
        || first->content().sourceWindow.sourceId != sharedSourceId
        || !repository.isSourceReferenced(sharedSourceId)
        || !repository.retireClip(firstKey))
        return false;

    second->content().sourceWindow.sourceId = sharedSourceId;
    if (!repository.releaseClip(firstKey) || !repository.isSourceReferenced(sharedSourceId))
        return false;

    if (!repository.retireClip(secondKey) || !repository.isSourceReferenced(sharedSourceId))
        return false;
    if (!repository.releaseClip(secondKey))
        return false;

    return !repository.isSourceReferenced(sharedSourceId)
        && repository.getRetiredContent(secondKey) == nullptr;
}

uint64_t addSource(SourceStore& store, uint64_t sourceId)
{
    SourceStore::CreateSourceRequest request;
    request.displayName = "source-" + juce::String(sourceId);
    request.sourceFilePath = "source-" + juce::String(sourceId) + ".wav";
    request.sampleRate = 48000.0;
    request.numChannels = 1;
    request.numSamples = 48000;
    return store.createSource(std::move(request), sourceId);
}

ContentKey addClip(StandaloneContentRepository& repository,
                   StandaloneArrangement& arrangement,
                   uint64_t clipId,
                   uint64_t sourceId,
                   bool place)
{
    const auto key = repository.createClip(clipId);
    auto* clip = repository.findClip(key);
    if (clip == nullptr)
        return {};

    clip->content().sourceWindow = SourceWindow{sourceId, {}, 0.0, 1.0};
    if (place) {
        StandaloneArrangement::Placement placement;
        placement.contentKey = key;
        placement.timelineStartSeconds = static_cast<double>(clipId);
        placement.durationSeconds = 1.0;
        if (!arrangement.insertPlacement(0, placement))
            return {};
    }
    return key;
}

bool testSweepUsesRemainingHistoryAndSharedReferences()
{
    OpenTuneAudioProcessor processor;
    processor.prepareToPlay(48000.0, 512);
    auto& store = *processor.getSourceStore();
    auto& repository = *processor.getStandaloneContentRepository();
    auto& arrangement = *processor.getStandaloneArrangement();
    auto& history = processor.getUndoManager();
    const auto exclusiveSource = addSource(store, 91);
    const auto sharedSource = addSource(store, 92);
    const auto exclusive = addClip(repository, arrangement, 1, exclusiveSource, true);
    const auto shared = addClip(repository, arrangement, 2, sharedSource, true);
    StandaloneArrangement::Placement survivor;
    survivor.contentKey = shared;
    survivor.durationSeconds = 1.0;
    if (!arrangement.insertPlacement(1, survivor) || !processor.deleteTrack(0, 2))
        return false;
    processor.runReclaimSweepOnMessageThread();
    if (repository.findClip(exclusive) != nullptr || store.containsSource(exclusiveSource)
        || !repository.findClip(shared)->hasActiveContent() || !store.containsSource(sharedSource))
        return false;

    auto deletion = commitPlacementDelete(processor, {{0, survivor.placementId}});
    if (deletion.undoAction == nullptr)
        return false;
    history.addAction(std::move(deletion.undoAction));
    processor.runReclaimSweepOnMessageThread();
    if (repository.findClip(shared) == nullptr || !store.containsSource(sharedSource)
        || !arrangement.hasPlacement(arrangement.getTrackIdentity(0), survivor.placementId))
        return false;
    history.undo();
    history.clearRedo();
    processor.runReclaimSweepOnMessageThread();
    if (!repository.findClip(shared)->hasActiveContent()
        || !arrangement.getPlacementById(0, survivor.placementId, survivor))
        return false;

    deletion = commitPlacementDelete(processor, {{0, survivor.placementId}});
    history.addAction(std::move(deletion.undoAction));
    history.clear();
    processor.runReclaimSweepOnMessageThread();
    return repository.findClip(shared) == nullptr && store.getTotalCount() == 0
        && arrangement.getRetiredPlacements().empty();
}

bool testSnapshotSourcesFollowSavedClips()
{
    OpenTuneAudioProcessor processor;
    processor.prepareToPlay(48000.0, 512);
    AppPreferences::StorageOptions storageOptions;
    storageOptions.applicationName = "OpenTuneStandaloneSourceLifetimeTests";
    storageOptions.settingsDirectory = juce::File::getSpecialLocation(
        juce::File::tempDirectory).getChildFile(storageOptions.applicationName);
    AppPreferences preferences(storageOptions);
    ProjectSession session(processor, preferences);

    auto* sourceStore = processor.getSourceStore();
    auto* repository = processor.getStandaloneContentRepository();
    auto* arrangement = processor.getStandaloneArrangement();
    if (sourceStore == nullptr || repository == nullptr || arrangement == nullptr) {
        std::fputs("FAIL: processor runtime stores unavailable\n", stderr);
        return false;
    }

    constexpr uint64_t sharedSourceId = 81;
    constexpr uint64_t secondSourceId = 82;
    constexpr uint64_t unusedSourceId = 83;
    constexpr uint64_t unplacedSourceId = 84;
    if (addSource(*sourceStore, sharedSourceId) != sharedSourceId
        || addSource(*sourceStore, secondSourceId) != secondSourceId
        || addSource(*sourceStore, unusedSourceId) != unusedSourceId
        || addSource(*sourceStore, unplacedSourceId) != unplacedSourceId)
        return false;

    if (!sourceStore->retireSource(sharedSourceId) || !sourceStore->isRetired(sharedSourceId)) {
        std::fputs("FAIL: shared source was not retired\n", stderr);
        return false;
    }
    if (!addClip(*repository, *arrangement, 1, sharedSourceId, true).isValid()
        || !addClip(*repository, *arrangement, 2, sharedSourceId, true).isValid()
        || !addClip(*repository, *arrangement, 3, secondSourceId, true).isValid()
        || !addClip(*repository, *arrangement, 4, unplacedSourceId, false).isValid())
        return false;

    const auto snapshot = session.captureSnapshot();
    std::set<uint64_t> sourceIds;
    for (const auto& source : snapshot.sources) {
        if (!sourceIds.insert(source.sourceId).second) {
            std::fputs("FAIL: snapshot contains duplicate source IDs\n", stderr);
            return false;
        }
    }

    if (snapshot.contents.size() != 3) {
        std::fputs("FAIL: snapshot does not contain exactly the placed clips\n", stderr);
        return false;
    }
    if (sourceIds != std::set<uint64_t>{sharedSourceId, secondSourceId}) {
        std::fputs("FAIL: snapshot source IDs do not follow the saved clips\n", stderr);
        return false;
    }
    return true;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    if (!testActiveRetiredAndSharedSourceReferences()) {
        std::fputs("FAIL: standalone source reference lifetime\n", stderr);
        return 1;
    }
    if (!testSnapshotSourcesFollowSavedClips()) {
        std::fputs("FAIL: snapshot sources follow saved clips\n", stderr);
        return 1;
    }
    if (!testSweepUsesRemainingHistoryAndSharedReferences()) {
        std::fputs("FAIL: sweep must retain shared content and release discarded history resources\n", stderr);
        return 1;
    }

    std::puts("Standalone source lifetime tests passed.");
    return 0;
}
