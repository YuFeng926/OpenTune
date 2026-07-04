#pragma once
#include <cstdint>
#include <string>
#include <functional>

namespace OpenTune {

enum class DomainKind : uint8_t
{
    ARAAudioModification,
    StandaloneClip,
    RegularVST3Capture,
    StandaloneArrangement
};

// Identifies one domain content root. It does not carry store pointers,
// render-cache pointers, or cross-domain materialization identity.
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

// std::hash specialization for ContentKey
namespace std {
    template<>
    struct hash<OpenTune::ContentKey>
    {
        size_t operator()(const OpenTune::ContentKey& key) const noexcept
        {
            // Combine domain, objectId, and discriminator
            size_t h1 = std::hash<uint8_t>{}(static_cast<uint8_t>(key.domainKind));
            size_t h2 = std::hash<uint64_t>{}(key.objectId);
            size_t h3 = std::hash<uint64_t>{}(key.sourceWindowDiscriminator);
            
            // Simple hash combine (boost-style)
            h1 ^= h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
            h1 ^= h3 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
            return h1;
        }
    };
}
