#pragma once

#include "../Content/ContentKey.h"
#include "../Inference/RenderCache.h"
#include "../Utils/TimeCoordinate.h"
#include <juce_core/juce_core.h>
#include <map>
#include <memory>
#include <mutex>

namespace OpenTune {

/**
 * RenderCacheRegistry — RenderCache 生命周期管理。
 *
 * 按 ContentKey 管理 RenderCache 实例的创建、查找、删除。
 * 保存当前 target rate；创建 cache 与全局切率重建串行化，避免旧 rate 的
 * 锁外重建覆盖更新后的 prepared snapshot。
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

    /** 更新目标率并串行重建所有 cache 的 prepared snapshot。 */
    void preparePlaybackSampleRate(double targetSr);

private:
    mutable juce::ReadWriteLock lock_;
    std::mutex prepareMutex_;
    std::map<ContentKey, std::shared_ptr<RenderCache>> caches_;
    double currentTargetRate_{TimeCoordinate::kRenderSampleRate};
};

} // namespace OpenTune
