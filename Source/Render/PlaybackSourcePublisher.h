#pragma once

#include "PlaybackReadSource.h"
#include "../Content/ContentKey.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/TimeCoordinate.h"
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace OpenTune {

// Control/worker-side source registry. Audio callbacks borrow the immutable
// views already held by their Capture, Standalone, or ARA playback snapshot.
class PlaybackSourcePublisher
{
public:
    PlaybackSourcePublisher();
    ~PlaybackSourcePublisher();

    PlaybackSourcePublisher(const PlaybackSourcePublisher&) = delete;
    PlaybackSourcePublisher& operator=(const PlaybackSourcePublisher&) = delete;

    void setPlaybackSampleRate(double sr);

    void publish(ContentKey key, PlaybackReadSource source);

    bool get(ContentKey key, PlaybackReadSource& out) const noexcept;
    void remove(ContentKey key);
    void clear();
    void collectPlaybackSnapshots();

    // Preparation thread / offline export. Native identity and global origin
    // travel with the immutable result.
    static PlaybackPreparedDry prepareDry(const NativePcmSource& native, double targetSr);

private:
    void prepareSourceDry(PlaybackReadSource& src, double targetSr);
    void recomputePreparedBytes();
    void collectRetiredMapsLocked();

    mutable std::mutex writerMutex_;
    mutable std::shared_ptr<const std::map<ContentKey, PlaybackReadSource>> data_;
    double playbackSampleRate_{TimeCoordinate::kRenderSampleRate};

    // Writer-owned retirement pool: prevents audio thread from being final destructor of map/PCM.
    mutable std::vector<std::shared_ptr<const std::map<ContentKey, PlaybackReadSource>>> retiredMaps_;

    // Active and retired prepared dry byte tracking in global cache.
    // Updated atomically against RenderCache::globalCacheCurrentBytes/globalCachePeakBytes.
    size_t totalPreparedBytes_{0};
};

} // namespace OpenTune
