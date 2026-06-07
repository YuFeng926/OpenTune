#pragma once
#include "DomainContentOwner.h"
#include "EditableContentState.h"
#include "RetiredContentRecord.h"
#include <memory>
#include <vector>

namespace OpenTune {

/// Regular VST3 Capture 段的内容所有者。轻量级实现，不涉及全套编辑功能。
class CaptureSegmentContent : public DomainContentOwner
{
public:
    explicit CaptureSegmentContent(uint64_t id);
    ~CaptureSegmentContent() override = default;

    ContentKey contentKey() const override;
    std::shared_ptr<const EditableContentSnapshot> snapshotContent() const override;
    void applyContentCommand(class ContentCommand& cmd) override;

    // 生命周期管理
    void retireContent(ContentKey key) override;
    void reviveContent(ContentKey key) override;
    void releaseRetiredContent(ContentKey key) override;

    EditableContentState& editable() { return editable_; }
    const EditableContentState& editable() const { return editable_; }

private:
    uint64_t id_;
    EditableContentState editable_;
    std::vector<RetiredContentRecord> retired_;
};

} // namespace OpenTune
