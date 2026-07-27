#pragma once

#include "../Content/ContentKey.h"
#include "../Inference/RenderCache.h"
#include "../Utils/TimeCoordinate.h"
#include <juce_core/juce_core.h>
#include <map>
#include <memory>

namespace OpenTune {

/**
 * RenderCacheRegistry — RenderCache 生命周期管理。
 *
 * 按 ContentKey 管理 RenderCache 实例的创建、查找、删除。
 * 保存当前 target rate；getOrCreate 的 cache 立即 prepare；切率时 write lock 内更新 rate + 复制列表后锁外重建。
 */
class RenderCacheRegistry
{
public:
    RenderCacheRegistry() = default;
    ~RenderCacheRegistry() = default;

    RenderCacheRegistry(const RenderCacheRegistry&) = delete;
    RenderCacheRegistry& operator=(const RenderCacheRegistry&) = delete;

    std::shared_ptr<RenderCache> getOrCreate(ContentKey key);
    std::shared_ptr<RenderCache> get(ContentKey key) const;
    void remove(ContentKey key);
    void invalidate(ContentKey key);
    void clear();

    /** 遍历所有 cache 调用 prepareForPlaybackSampleRate（write lock 内更新 rate + 复制列表，锁外重建）。 */
    void preparePlaybackSampleRate(double targetSr);

private:
    mutable juce::ReadWriteLock lock_;
    std::map<ContentKey, std::shared_ptr<RenderCache>> caches_;
    double currentTargetRate_{TimeCoordinate::kRenderSampleRate};
};

} // namespace OpenTune
