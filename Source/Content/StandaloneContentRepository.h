#pragma once
#include "ContentKey.h"
#include "StandaloneClipContent.h"
#include <juce_core/juce_core.h>
#include <map>
#include <memory>
#include <vector>
#include <cstdint>

namespace OpenTune {

using StandaloneClipId = uint64_t;

/// Standalone 域内容仓库。管理 StandaloneClipContent lifecycle。
class StandaloneContentRepository
{
public:
    StandaloneContentRepository();
    ~StandaloneContentRepository();

    /// 创建新 clip content，返回 ContentKey。
    ContentKey createClip(StandaloneClipId forcedId = 0);

    /// 查找 content owner（不区分 active/retired）。
    StandaloneClipContent* findClip(ContentKey key) const;

    // ── Lifecycle ───────────────────────────────────────────

    /// 标记 lifecycle = Retired
    bool retireClip(ContentKey key);

    /// 恢复 lifecycle = Ready，bump contentRevision
    bool reviveClip(ContentKey key);

    /// 物理删除（需确认无引用）
    bool releaseClip(ContentKey key);

    /// 引用检查（用于 reclaim）
    bool isRetired(ContentKey key) const;

    /// 获取所有已退休 clip 的 key
    std::vector<ContentKey> getRetiredClips() const;

    /// 获取所有 clip 的 key（包括 active 和 retired）
    std::vector<ContentKey> getAllClips() const;

    void clear();

private:
    StandaloneClipContent* findClipImpl(StandaloneClipId clipId) const;

    juce::ReadWriteLock lock_;
    std::map<StandaloneClipId, std::unique_ptr<StandaloneClipContent>> clips_;
    StandaloneClipId nextClipId_{1};
};

} // namespace OpenTune
