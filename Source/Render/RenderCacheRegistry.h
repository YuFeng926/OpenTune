#pragma once

#include "../Content/ContentKey.h"
#include "../Inference/RenderCache.h"
#include <juce_core/juce_core.h>
#include <map>
#include <memory>

namespace OpenTune {

/**
 * RenderCacheRegistry — RenderCache 生命周期管理。
 * 
 * 按 ContentKey 管理 RenderCache 实例的创建、查找、删除。
 * 
 * Phase 0: 提取自 MaterializationStore RenderCache 创建逻辑 (cpp:84-85)
 *          和 ContentRenderService renderCaches_ map (h:184)
 * 
 * 复用代码：
 * - MaterializationStore.cpp:84-85 (RenderCache 创建)
 * - MaterializationStore.cpp:363-378 (getRenderCache)
 * - ContentRenderService.cpp:65-87 (getOrCreateRenderCache)
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
    void put(ContentKey key, std::shared_ptr<RenderCache> cache);
    void remove(ContentKey key);
    void invalidate(ContentKey key);
    void clear();

private:
    mutable juce::ReadWriteLock lock_;
    std::map<ContentKey, std::shared_ptr<RenderCache>> caches_;
};

} // namespace OpenTune
