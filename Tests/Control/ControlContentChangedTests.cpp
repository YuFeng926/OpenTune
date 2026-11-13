// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Content-change notifications: committed snapshots, message-thread delivery, and editor-style async coalescing.

#include "ControlContentFixtures.h"
#include "ControlProcessorSupport.h"

#include "Content/EditableContentSnapshot.h"
#include "PluginProcessor.h"
#include "Utils/PitchShiftEditAction.h"

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace OpenTune {
namespace ControlTest {
namespace {

struct ContentChange
{
    ContentKey key;
    bool messageThread = false;
};

class ContentChangedTestSink final : public juce::AsyncUpdater
{
public:
    explicit ContentChangedTestSink(OpenTuneAudioProcessor& processor) : processor_(processor) {}
    ~ContentChangedTestSink() override { cancelPendingUpdate(); }

    void observe(ContentKey key)
    {
        auto* messages = juce::MessageManager::getInstanceWithoutCreating();
        changes.push_back({key, messages != nullptr && messages->isThisTheMessageThread()});
        pendingKeys.push_back(key);
        triggerAsyncUpdate();
    }

    void beginMutation() noexcept { mutationActive = true; }
    void endMutation() noexcept { mutationActive = false; }
    std::shared_ptr<const EditableContentSnapshot> readSnapshot(ContentKey key) const
    {
        return processor_.getContentSnapshot(key);
    }

    std::vector<ContentChange> changes;
    std::vector<ContentKey> pendingKeys;
    std::vector<std::vector<ContentKey>> deliveredBatches;
    std::shared_ptr<const EditableContentSnapshot> lastConsumedSnapshot;
    bool consumedDuringMutation = false;

private:
    void handleAsyncUpdate() override
    {
        consumedDuringMutation = consumedDuringMutation || mutationActive;
        lastConsumedSnapshot = readSnapshot(pendingKeys.back());
        deliveredBatches.push_back(std::move(pendingKeys));
        pendingKeys.clear();
    }

    OpenTuneAudioProcessor& processor_;
    bool mutationActive = false;
};

class PendingUpdateSink final : public juce::AsyncUpdater
{
public:
    explicit PendingUpdateSink(int& deliveries) : deliveries_(deliveries) {}
    ~PendingUpdateSink() override { cancelPendingUpdate(); }

    void schedule() { triggerAsyncUpdate(); }

private:
    void handleAsyncUpdate() override { ++deliveries_; }

    int& deliveries_;
};

template <typename Mutation, typename SnapshotCheck>
bool expectContentChange(ContentChangedTestSink& sink,
                         const ContentKey& key,
                         Mutation&& mutation,
                         SnapshotCheck&& snapshotCheck)
{
    const auto before = sink.changes.size();
    sink.beginMutation();
    const bool applied = mutation();
    sink.endMutation();

    if (!CONTROL_CHECK(applied))
        return false;
    if (!CONTROL_CHECK(sink.changes.size() == before + 1))
        return false;

    const auto& change = sink.changes.back();
    CONTROL_CHECK(change.key == key);
    CONTROL_CHECK(change.messageThread);
    const auto snapshot = sink.readSnapshot(key);
    CONTROL_CHECK(snapshot != nullptr && snapshot->audioBuffer != nullptr
                  && snapshot->pitchCurve != nullptr);
    if (snapshot != nullptr)
        CONTROL_CHECK(snapshotCheck(*snapshot));
    return true;
}

} // namespace

void scenarioContentChanged()
{
    auto processor = startedProcessor();
    if (!processor->getControlService().isRunning())
        return;

    ClipSpec spec;
    spec.name = "content-changed";
    spec.durationSec = 6.0;
    spec.notes = {{0.5, 1.5, 69.0}, {2.0, 3.0, 71.0}};
    DetectedKey initialKey;
    initialKey.root = Key::A;
    initialKey.scale = Scale::Minor;
    initialKey.origin = Origin::Manual;
    initialKey.confidence = 1.0f;
    spec.key = initialKey;
    const auto clip = fixtureAddClip(*processor, spec);
    if (!CONTROL_CHECK(clip.key.isValid()))
        return;
    if (!CONTROL_CHECK(settleBackgroundWork(*processor)))
        return;

    ContentChangedTestSink sink(*processor);
    processor->setContentChangedCallback([&sink](ContentKey key) { sink.observe(key); });

    DetectedKey updatedKey;
    updatedKey.root = Key::Fs;
    updatedKey.scale = Scale::Major;
    updatedKey.origin = Origin::Manual;
    updatedKey.confidence = 1.0f;
    expectContentChange(sink, clip.key,
                        [&] { return processor->setContentDetectedKey(clip.key, updatedKey); },
                        [&](const EditableContentSnapshot& snapshot) {
                            return snapshot.detectedKey.root == updatedKey.root
                                   && snapshot.detectedKey.scale == updatedKey.scale;
                        });

    std::vector<TimeHandle> handles(3);
    handles[0] = {1, 0.0, 0.0, HandleKind::ClipStart};
    handles[1] = {2, 2.0, 2.5, HandleKind::UserAdded};
    handles[2] = {3, spec.durationSec, spec.durationSec, HandleKind::ClipEnd};
    const auto grid = TimeGridSnapshot::makeFromHandles(std::move(handles));
    expectContentChange(sink, clip.key,
                        [&] { return grid != nullptr && processor->setContentTimeGrid(clip.key, grid); },
                        [&](const EditableContentSnapshot& snapshot) {
                            return snapshot.timeGrid != nullptr && snapshot.timeGrid->handles().size() == 3
                                   && snapshot.timeGrid->handles()[1].output_seconds == 2.5;
                        });

    const auto envelope = AutomationLane::fromSnapshot({{0.0, -6.0f}, {2.0, -2.0f}});
    expectContentChange(sink, clip.key,
                        [&] { return processor->commitVolumeEnvelope(clip.key, envelope) != nullptr; },
                        [](const EditableContentSnapshot& snapshot) {
                            return std::abs(snapshot.volumeEnvelope.evalAt(1.0) - (-4.0f)) < 1.0e-5f;
                        });

    Note fullNote;
    fullNote.startTime = 0.25;
    fullNote.endTime = 1.25;
    fullNote.pitch = 233.0f;
    fullNote.originalPitch = 220.0f;
    const std::vector<Note> fullNotes{fullNote};
    expectContentChange(sink, clip.key,
                        [&] { return processor->replaceContentNotesForFullMutation(clip.key, fullNotes); },
                        [](const EditableContentSnapshot& snapshot) {
                            return snapshot.notes.size() == 1 && snapshot.notes[0].pitch == 233.0f;
                        });

    Note topologyNote = fullNote;
    topologyNote.pitch = 240.0f;
    ContentNoteRangePatch topology;
    topology.affectedRange = {0.0, 2.0};
    topology.afterNotesInRange = {topologyNote};
    expectContentChange(sink, clip.key,
                        [&] { return processor->commitContentNoteTopologyPatch(clip.key, topology) != nullptr; },
                        [](const EditableContentSnapshot& snapshot) {
                            return snapshot.notes.size() == 1 && snapshot.notes[0].pitch == 240.0f;
                        });

    auto beforeCombinedEdit = processor->getContentSnapshot(clip.key);
    if (!CONTROL_CHECK(beforeCombinedEdit != nullptr && beforeCombinedEdit->pitchCurve != nullptr
                       && !beforeCombinedEdit->notes.empty()
                       && !beforeCombinedEdit->pitchCurve->getOriginalF0().empty()))
        return;
    auto editedNotes = beforeCombinedEdit->notes;
    editedNotes[0].pitch = 250.0f;
    auto originalF0 = beforeCombinedEdit->pitchCurve->getOriginalF0();
    originalF0[0] += 1.0f;
    auto correctedF0 = originalF0;
    for (auto& value : correctedF0)
        if (value > 0.0f)
            value *= 1.02f;
    const int frameCount = static_cast<int>(correctedF0.size());
    std::vector<PitchCorrectionSegment> segments;
    segments.emplace_back(0, frameCount, correctedF0, PitchCorrectionSegment::Source::HandDraw);
    const ContentEditRangeFrames affectedRange{0, frameCount};
    expectContentChange(sink, clip.key,
                        [&] {
                            return processor->commitContentNotesAndSegmentsWithOriginalF0(
                                       clip.key, editedNotes, segments, affectedRange, originalF0)
                                   != nullptr;
                        },
                        [&](const EditableContentSnapshot& snapshot) {
                            return snapshot.pitchCurve != nullptr && !snapshot.notes.empty()
                                   && snapshot.notes[0].pitch == 250.0f
                                   && !snapshot.pitchCurve->getOriginalF0().empty()
                                   && snapshot.pitchCurve->getOriginalF0()[0] == originalF0[0]
                                   && snapshot.pitchCurve->getCorrectionSegments().size() == 1;
                        });

    expectContentChange(sink, clip.key,
                        [&] { return processor->setContentOriginalF0State(clip.key, OriginalF0State::Extracting); },
                        [](const EditableContentSnapshot& snapshot) {
                            return snapshot.originalF0State == OriginalF0State::Extracting;
                        });

    auto shiftState = PitchShiftEditState{};
    const auto beforeShift = processor->getContentSnapshot(clip.key);
    if (!CONTROL_CHECK(beforeShift != nullptr && beforeShift->pitchCurve != nullptr))
        return;
    shiftState.settings.semitone = 1;
    shiftState.notes = beforeShift->notes;
    shiftState.segments = beforeShift->pitchCurve->getCorrectionSegments();
    expectContentChange(sink, clip.key,
                        [&] { return processor->applyContentPitchShiftState(clip.key, shiftState); },
                        [](const EditableContentSnapshot& snapshot) {
                            return snapshot.pitchShiftSettings.semitone == 1;
                        });

    PitchShiftSettings committedShift;
    committedShift.semitone = 2;
    std::unique_ptr<PitchShiftEditAction> pitchShiftAction;
    expectContentChange(sink, clip.key,
                        [&] {
                            pitchShiftAction = processor->commitPitchShiftEdit(clip.key, committedShift);
                            return pitchShiftAction != nullptr;
                        },
                        [](const EditableContentSnapshot& snapshot) {
                            return snapshot.pitchShiftSettings.semitone == 2;
                        });

    if (pitchShiftAction != nullptr) {
        auto& undo = processor->getUndoManager();
        undo.addAction(std::move(pitchShiftAction));
        expectContentChange(sink, clip.key,
                            [&] { return undo.undo() != nullptr; },
                            [](const EditableContentSnapshot& snapshot) {
                                return snapshot.pitchShiftSettings.semitone == 1;
                            });
        expectContentChange(sink, clip.key,
                            [&] { return undo.redo() != nullptr; },
                            [](const EditableContentSnapshot& snapshot) {
                                return snapshot.pitchShiftSettings.semitone == 2;
                            });
    }

    expectContentChange(sink, clip.key,
                        [&] { return processor->setContentOriginalF0State(clip.key, OriginalF0State::Failed); },
                        [](const EditableContentSnapshot& snapshot) {
                            return snapshot.originalF0State == OriginalF0State::Failed;
                        });
    expectContentChange(sink, clip.key,
                        [&] { return processor->setContentOriginalF0FailureReason(clip.key, "notification contract"); },
                        [](const EditableContentSnapshot& snapshot) {
                            return snapshot.originalF0FailureReason == "notification contract";
                        });

    const auto notificationCount = sink.changes.size();
    const ContentKey invalidKey{DomainKind::StandaloneClip, 0, 0};
    CONTROL_CHECK(!processor->setContentDetectedKey(invalidKey, updatedKey));
    CONTROL_CHECK(sink.changes.size() == notificationCount);

    CONTROL_CHECK(sink.deliveredBatches.empty());
    CONTROL_CHECK(sink.pendingKeys.size() == sink.changes.size());
    CONTROL_CHECK(!sink.consumedDuringMutation);
    CONTROL_CHECK(pumpMessagesUntil([&sink] { return !sink.deliveredBatches.empty(); }, 1000));
    CONTROL_CHECK(sink.deliveredBatches.size() == 1);
    if (!sink.deliveredBatches.empty()) {
        CONTROL_CHECK(sink.deliveredBatches[0].size() == notificationCount);
        for (const auto& key : sink.deliveredBatches[0])
            CONTROL_CHECK(key == clip.key);
    }
    CONTROL_CHECK(!sink.consumedDuringMutation);

    // The deferred consumer reads the final state after all synchronous commits have returned.
    CONTROL_CHECK(sink.lastConsumedSnapshot != nullptr);
    if (sink.lastConsumedSnapshot != nullptr) {
        CONTROL_CHECK(sink.lastConsumedSnapshot->detectedKey.root == updatedKey.root);
        CONTROL_CHECK(sink.lastConsumedSnapshot->pitchShiftSettings.semitone == 2);
        CONTROL_CHECK(sink.lastConsumedSnapshot->originalF0State == OriginalF0State::Failed);
        CONTROL_CHECK(sink.lastConsumedSnapshot->originalF0FailureReason == "notification contract");
    }

    int detachedCallbacks = 0;
    processor->setContentChangedCallback([&detachedCallbacks](ContentKey) { ++detachedCallbacks; });
    CONTROL_CHECK(processor->setContentDetectedKey(clip.key, initialKey));
    CONTROL_CHECK(detachedCallbacks == 1);
    processor->setContentChangedCallback({});
    CONTROL_CHECK(processor->setContentDetectedKey(clip.key, updatedKey));
    CONTROL_CHECK(detachedCallbacks == 1);

    int canceledDeliveries = 0;
    {
        auto pending = std::make_unique<PendingUpdateSink>(canceledDeliveries);
        processor->setContentChangedCallback([&pending](ContentKey) { pending->schedule(); });
        CONTROL_CHECK(processor->setContentDetectedKey(clip.key, initialKey));
        processor->setContentChangedCallback({});
        pending.reset();
    }
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(canceledDeliveries == 0);
}

} // namespace ControlTest
} // namespace OpenTune
