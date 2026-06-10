#include "StandaloneClipContent.h"
#include "EditableContentSnapshot.h"
#include <algorithm>
#include <utility>

namespace OpenTune {

StandaloneClipContent::StandaloneClipContent(StandaloneClipId clipId)
    : clipId_(clipId)
{
    content_.lifecycle = ContentLifecycle::Ready;
}

ContentKey StandaloneClipContent::contentKey() const
{
    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = clipId_;
    return key;
}

std::shared_ptr<const EditableContentSnapshot> StandaloneClipContent::snapshotContent() const
{
    auto snap = std::make_shared<EditableContentSnapshot>();
    snap->sourceWindow = content_.sourceWindow;
    snap->notes = content_.notes;
    snap->pitchCurve = content_.pitchCurve;
    snap->timeGrid = content_.timeGrid;
    snap->pitchShiftSettings = content_.pitchShiftSettings;
    snap->originalF0State = content_.originalF0State;
    snap->detectedKey = content_.detectedKey;
    snap->notesRevision = content_.notesRevision;
    snap->pitchRevision = content_.pitchRevision;
    snap->timeGridRevision = content_.timeGridRevision;
    snap->pitchShiftRevision = content_.pitchShiftRevision;
    snap->contentRevision = content_.contentRevision;
    snap->audioBuffer = content_.audioBuffer;
    snap->audioRevision = content_.audioRevision;
    return snap;
}

void StandaloneClipContent::applyContentCommand(ContentCommand& cmd)
{
    (void)cmd;
    // TODO: 由 coordinator 负责 dispatch content commands
}

// ── Lifecycle ───────────────────────────────────────────────

void StandaloneClipContent::retireContent(ContentKey key)
{
    if (key != contentKey())
        return;

    StandaloneRetiredContentRecord record;
    record.key = key;
    record.content = std::move(content_);
    record.content.lifecycle = ContentLifecycle::Retired;

    // 重置当前 content 为空状态
    content_ = ContentPayloadState{};
    content_.lifecycle = ContentLifecycle::Retired;

    retired_.push_back(std::move(record));
}

void StandaloneClipContent::reviveContent(ContentKey key)
{
    if (key != contentKey())
        return;

    for (auto it = retired_.begin(); it != retired_.end(); ++it) {
        if (it->key == key) {
            content_ = std::move(it->content);
            content_.lifecycle = ContentLifecycle::Ready;
            bumpContentRevision();
            retired_.erase(it);
            return;
        }
    }
}

void StandaloneClipContent::releaseRetiredContent(ContentKey key)
{
    if (key != contentKey())
        return;

    retired_.erase(
        std::remove_if(retired_.begin(), retired_.end(),
            [&key](const StandaloneRetiredContentRecord& r) { return r.key == key; }),
        retired_.end());
}

bool StandaloneClipContent::isRetired() const
{
    return content_.lifecycle == ContentLifecycle::Retired;
}

bool StandaloneClipContent::hasActiveContent() const
{
    return content_.lifecycle != ContentLifecycle::Retired
        && content_.lifecycle != ContentLifecycle::Empty;
}

// ── Apply commands ──────────────────────────────────────────

void StandaloneClipContent::applyNotes(std::vector<Note> notes)
{
    content_.notes = std::move(notes);
    ++content_.notesRevision;
    bumpContentRevision();
}

void StandaloneClipContent::applyPitchCurve(std::shared_ptr<PitchCurve> curve)
{
    content_.pitchCurve = std::move(curve);
    ++content_.pitchRevision;
    bumpContentRevision();
}

void StandaloneClipContent::applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> snapshot)
{
    content_.timeGrid = std::move(snapshot);
    ++content_.timeGridRevision;
    bumpContentRevision();
}

void StandaloneClipContent::applyPitchShiftSettings(const PitchShiftSettings& settings)
{
    content_.pitchShiftSettings = settings;
    ++content_.pitchShiftRevision;
    bumpContentRevision();
}

void StandaloneClipContent::applyDetectedKey(const DetectedKey& key)
{
    content_.detectedKey = key;
    bumpContentRevision();
}

void StandaloneClipContent::applyReferenceFeatures(const ReferenceFeatureSet& features)
{
    content_.referenceFeatures = features;
    bumpContentRevision();
}

void StandaloneClipContent::applyOriginalF0State(OriginalF0State state)
{
    content_.originalF0State = state;
    bumpContentRevision();
}

void StandaloneClipContent::applyAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer, double sampleRate)
{
    content_.audioBuffer = std::move(buffer);
    content_.sampleRate = sampleRate;
    ++content_.audioRevision;
    bumpContentRevision();
}

// ── Private helpers ─────────────────────────────────────────

void StandaloneClipContent::bumpContentRevision()
{
    ++content_.contentRevision;
}

} // namespace OpenTune
