#include "CaptureSegmentContent.h"
#include "EditableContentSnapshot.h"
#include <algorithm>

namespace OpenTune {

CaptureSegmentContent::CaptureSegmentContent(uint64_t id)
    : id_(id)
{
}

ContentKey CaptureSegmentContent::contentKey() const
{
    ContentKey key;
    key.domainKind = DomainKind::RegularVST3Capture;
    key.objectId = id_;
    return key;
}

std::shared_ptr<const EditableContentSnapshot> CaptureSegmentContent::snapshotContent() const
{
    auto snap = std::make_shared<EditableContentSnapshot>();
    snap->notes = editable_.notes;
    snap->pitchCurve = nullptr;  // Capture segments don't have pitch curves
    snap->timeGrid = editable_.timeGrid;
    snap->pitchShiftSettings = editable_.pitchShiftSettings;
    snap->originalF0State = OriginalF0State::NotRequested;
    snap->notesRevision = editable_.notesRevision;
    snap->pitchRevision = editable_.pitchRevision;
    snap->timeGridRevision = editable_.timeGridRevision;
    snap->pitchShiftRevision = editable_.pitchShiftRevision;
    snap->contentRevision = editable_.contentRevision;
    return snap;
}

void CaptureSegmentContent::applyContentCommand(ContentCommand& cmd)
{
    (void)cmd;
    // Capture 段不支持编辑命令
}

void CaptureSegmentContent::retireContent(ContentKey key)
{
    if (key != contentKey()) return;
    RetiredContentRecord record;
    record.key = key;
    record.content.editable = std::move(editable_);
    record.content.contentRevision = editable_.contentRevision;
    retired_.push_back(std::move(record));
}

void CaptureSegmentContent::reviveContent(ContentKey key)
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

void CaptureSegmentContent::releaseRetiredContent(ContentKey key)
{
    if (key != contentKey()) return;
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
        [&key](const RetiredContentRecord& r) { return r.key == key; }),
        retired_.end());
}

} // namespace OpenTune
