#pragma once
#include "ContentKey.h"
#include <memory>

namespace OpenTune {

struct EditableContentSnapshot;

class DomainContentOwner
{
public:
    virtual ~DomainContentOwner() = default;

    virtual ContentKey contentKey() const = 0;
    virtual std::shared_ptr<const EditableContentSnapshot> snapshotContent() const = 0;

    // 应用内容命令并推高 revision
    virtual void applyContentCommand(class ContentCommand& cmd) = 0;

    // 生命周期管理
    virtual void retireContent(ContentKey key) = 0;
    virtual void reviveContent(ContentKey key) = 0;
    virtual void releaseRetiredContent(ContentKey key) = 0;
};

} // namespace OpenTune
