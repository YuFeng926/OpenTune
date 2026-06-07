#include "StandaloneClipContent.h"
#include "EditableContentSnapshot.h"
#include <algorithm>

namespace OpenTune {

StandaloneClipContent::StandaloneClipContent(uint64_t id)
    : id_(id)
{
}

ContentKey StandaloneClipContent::contentKey() const
{
    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = id_;
    return key;
}

std::shared_ptr<const EditableContentSnapshot> StandaloneClipContent::snapshotContent() const
{
    auto snap = std::make_shared<EditableContentSnapshot>();
    snap->notes = editable_.notes;
    snap->pitchCurve = pitchCurve_;
    snap->timeGrid = editable_.timeGrid;
    snap->pitchShiftSettings = editable_.pitchShiftSettings;
    snap->originalF0State = originalF0State_;
    snap->detectedKey = detectedKey_;
    snap->notesRevision = editable_.notesRevision;
    snap->pitchRevision = editable_.pitchRevision;
    snap->timeGridRevision = editable_.timeGridRevision;
    snap->pitchShiftRevision = editable_.pitchShiftRevision;
    snap->contentRevision = editable_.contentRevision;
    return snap;
}

void StandaloneClipContent::applyContentCommand(ContentCommand& cmd)
{
    (void)cmd;
    // TODO: Implement content command dispatch
    // Command types: SetNotes, CommitNotesAndSegments, SetCorrectedSegments,
    // SetPitchCurve, SetTimeGrid, SetDetectedKey, SetPitchShiftSettings,
    // EnqueuePartialRender, CommitAutoTuneGeneratedNotes
}

void StandaloneClipContent::retireContent(ContentKey key)
{
    if (key != contentKey()) return;
    RetiredContentRecord record;
    record.key = key;
    // Move current editable state into retired record
    record.content.editable = std::move(editable_);
    record.content.contentRevision = editable_.contentRevision;
    retired_.push_back(std::move(record));
}

void StandaloneClipContent::reviveContent(ContentKey key)
{
    if (key != contentKey()) return;
    for (auto it = retired_.begin(); it != retired_.end(); ++it) {
        if (it->key == key) {
            editable_ = std::move(it->content.editable);
            retired_.erase(it);
            return;
        }
    }
}

void StandaloneClipContent::releaseRetiredContent(ContentKey key)
{
    if (key != contentKey()) return;
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
        [&key](const RetiredContentRecord& r) { return r.key == key; }),
        retired_.end());
}

} // namespace OpenTune
