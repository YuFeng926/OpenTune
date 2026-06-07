#pragma once
#include <cstdint>
#include <string>

namespace OpenTune {

enum class DomainKind : uint8_t
{
    ARAAudioModification,
    StandaloneClip,
    RegularVST3Capture
};

// 标识一个域内容根，不包含 materializationId、store 指针、render-cache 指针。
struct ContentKey
{
    DomainKind domainKind{DomainKind::ARAAudioModification};
    uint64_t objectId{0};                    // 域持有的持久对象 ID
    uint64_t sourceWindowDiscriminator{0};   // 可选：source-window 区分符

    bool isValid() const noexcept { return objectId != 0; }
    bool operator==(const ContentKey& rhs) const noexcept {
        return domainKind == rhs.domainKind
            && objectId == rhs.objectId
            && sourceWindowDiscriminator == rhs.sourceWindowDiscriminator;
    }
    bool operator!=(const ContentKey& rhs) const noexcept { return !(*this == rhs); }
    bool operator<(const ContentKey& rhs) const noexcept {
        if (domainKind != rhs.domainKind) return domainKind < rhs.domainKind;
        if (objectId != rhs.objectId) return objectId < rhs.objectId;
        return sourceWindowDiscriminator < rhs.sourceWindowDiscriminator;
    }
};

} // namespace OpenTune
