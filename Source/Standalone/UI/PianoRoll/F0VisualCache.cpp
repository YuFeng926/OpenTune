#include "F0VisualCache.h"
#include "PianoRollTimeMap.h"
#include "../ViewMapper.h"

namespace OpenTune {

namespace {

bool sameProjection(const ContentTimelineProjection& a, const ContentTimelineProjection& b) noexcept
{
    return a.timelineStartSeconds == b.timelineStartSeconds
        && a.timelineDurationSeconds == b.timelineDurationSeconds
        && a.contentStartSeconds == b.contentStartSeconds
        && a.contentDurationSeconds == b.contentDurationSeconds;
}

// 缓存键：帧轴取值 + committed 网格身份 + 投影逐位。时间投影只由这三者决定。
bool sameKey(const F0VisualCacheEntry& a, const F0VisualCacheEntry& b) noexcept
{
    return a.timeGrid == b.timeGrid
        && a.frameCount == b.frameCount
        && a.hopSize == b.hopSize
        && a.sampleRate == b.sampleRate
        && sameProjection(a.projection, b.projection);
}

} // namespace

const F0VisualCacheEntry* F0VisualCacheStore::resolve(
    std::shared_ptr<const EditableContentSnapshot> ownerSnapshot,
    std::shared_ptr<const EditableContentSnapshot> committedSnapshot,
    const ContentTimelineProjection& projection)
{
    if (!ownerSnapshot || !committedSnapshot || !committedSnapshot->timeGrid)
        return nullptr;
    const auto& pitchCurve = ownerSnapshot->pitchCurve;
    if (!pitchCurve || pitchCurve->size() == 0)
        return nullptr;

    F0VisualCacheEntry key;
    key.timeGrid = committedSnapshot->timeGrid;
    key.projection = projection;
    key.frameCount = static_cast<int>(pitchCurve->size());
    key.hopSize = pitchCurve->getHopSize();
    key.sampleRate = pitchCurve->getSampleRate();

    for (auto& stored : entries_)
    {
        if (sameKey(stored.entry, key))
        {
            stored.used = true;
            return &stored.entry;
        }
    }

    // 未命中：按同一帧轴/网格/投影构建时间数组。owner 快照（含拖拽预览的
    // 临时烘焙快照）与 committed 快照共用——预览保留帧轴，时间数组不变。
    const F0Timeline f0Timeline(key.hopSize, key.sampleRate, key.frameCount);
    const PianoRollTimeMap map(key.projection, *key.timeGrid, ViewMapper{});
    key.timelineSeconds.resize(static_cast<std::size_t>(key.frameCount));
    for (int frame = 0; frame < key.frameCount; ++frame)
        key.timelineSeconds[static_cast<std::size_t>(frame)] =
            map.sourceToTimeline(f0Timeline.timeAtFrame(frame));

    entries_.push_back(Stored{ std::move(key), true });
    return &entries_.back().entry;
}

void F0VisualCacheStore::prune()
{
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [](const Stored& s) { return !s.used; }),
                   entries_.end());
    for (auto& stored : entries_)
        stored.used = false;
}

} // namespace OpenTune
