#include "AudioModification.h"
#include <algorithm>

namespace OpenTune {

void AudioModification::updateIdentity(juce::ARAAudioModification* modification)
{
    audioModification = modification;
    persistentId = modification != nullptr
        ? juce::String(modification->getPersistentID())
        : juce::String();
}

void AudioModification::attachSource(const AudioSource& source)
{
    sourcePersistentId = source.getIdentity().persistentId;
    const auto& shape = source.getShape();
    contentWindow = SourceWindow{sourceId, 0.0, shape.durationSeconds()};
    content.sourceWindow = contentWindow;
}

void AudioModification::resetContent() noexcept
{
    materializationDurationSeconds = 0.0;
    contentWindow = {};
    content = AudioModificationContentState{};
    birthState = AudioModificationBirthState::Empty;
}

bool AudioModification::isRenderable() const noexcept
{
    return content.lifecycle == ContentLifecycle::Ready
        && materializationDurationSeconds > 0.0;
}

ContentKey AudioModification::contentKey() const noexcept
{
    ContentKey key;
    key.domainKind = DomainKind::ARAAudioModification;
    key.objectId = persistentId.isEmpty() ? 0 : static_cast<uint64_t>(persistentId.hashCode64());
    return key;
}

std::shared_ptr<const EditableContentSnapshot> AudioModification::snapshotContent() const
{
    auto snap = std::make_shared<EditableContentSnapshot>();
    snap->sourceWindow = content.sourceWindow;
    snap->notes = content.editable.notes;
    snap->pitchCurve = content.analysis.pitchCurve;
    snap->timeGrid = content.editable.timeGrid;
    snap->pitchShiftSettings = content.editable.pitchShiftSettings;
    snap->originalF0State = content.analysis.originalF0State;
    snap->detectedKey = content.analysis.detectedKey;
    snap->notesRevision = content.editable.notesRevision;
    snap->pitchRevision = content.editable.pitchRevision;
    snap->timeGridRevision = content.editable.timeGridRevision;
    snap->pitchShiftRevision = content.editable.pitchShiftRevision;
    snap->contentRevision = content.contentRevision;
    return snap;
}

void AudioModification::retireContent()
{
    RetiredContentRecord record;
    record.key = contentKey();
    record.content = content;
    retiredContentRecords.push_back(std::move(record));

    content = AudioModificationContentState{};
    content.lifecycle = ContentLifecycle::Retired;
    ++content.contentRevision;
}

bool AudioModification::reviveContent(ContentKey key)
{
    auto it = std::find_if(retiredContentRecords.begin(), retiredContentRecords.end(),
        [&](const auto& r) { return r.key == key; });
    if (it == retiredContentRecords.end())
        return false;

    content = it->content;
    content.lifecycle = ContentLifecycle::Ready;
    ++content.contentRevision;
    retiredContentRecords.erase(it);
    birthState = AudioModificationBirthState::Ready;
    return true;
}

void AudioModification::releaseRetiredContent(ContentKey key)
{
    auto it = std::find_if(retiredContentRecords.begin(), retiredContentRecords.end(),
        [&](const auto& r) { return r.key == key; });
    if (it != retiredContentRecords.end())
        retiredContentRecords.erase(it);
}

void AudioModification::applyNotes(const std::vector<Note>& notes)
{
    content.editable.notes = notes;
    ++content.editable.notesRevision;
    ++content.editable.contentRevision;
    ++content.contentRevision;
}

void AudioModification::applyPitchCurve(std::shared_ptr<PitchCurve> curve)
{
    content.analysis.pitchCurve = std::move(curve);
    ++content.editable.pitchRevision;
    ++content.editable.contentRevision;
    ++content.contentRevision;
}

void AudioModification::applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> grid)
{
    content.editable.timeGrid = std::move(grid);
    ++content.editable.timeGridRevision;
    ++content.editable.contentRevision;
    ++content.contentRevision;
}

void AudioModification::applyPitchShift(const PitchShiftSettings& settings)
{
    content.editable.pitchShiftSettings = settings;
    ++content.editable.pitchShiftRevision;
    ++content.editable.contentRevision;
    ++content.contentRevision;
}

void AudioModification::applyDetectedKey(const DetectedKey& key)
{
    content.analysis.detectedKey = key;
    ++content.contentRevision;
}

void AudioModification::applyF0Analysis(std::shared_ptr<PitchCurve> curve)
{
    content.analysis.pitchCurve = std::move(curve);
    content.analysis.originalF0State = OriginalF0State::Ready;
    content.analysis.f0Lifecycle = AnalysisLifecycle::Ready;
    ++content.analysis.analysisRevision;
    ++content.contentRevision;
}

} // namespace OpenTune
