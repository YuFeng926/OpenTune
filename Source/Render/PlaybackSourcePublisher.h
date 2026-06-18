#pragma once

#include "PlaybackReadSource.h"
#include "../Content/ContentKey.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace OpenTune {

/**
 * Publishes ContentKey-indexed PlaybackReadSource snapshots.
 *
 * Single atomic shared_ptr + copy-on-write pattern:
 * - Writers (ARA host thread / message thread) clone current map, modify, atomic_store
 * - Audio readers atomic_load and do not lock.
 * - std::mutex protects concurrent writers (lost-update prevention).
 */
class PlaybackSourcePublisher
{
public:
    PlaybackSourcePublisher() = default;
    ~PlaybackSourcePublisher() = default;

    PlaybackSourcePublisher(const PlaybackSourcePublisher&) = delete;
    PlaybackSourcePublisher& operator=(const PlaybackSourcePublisher&) = delete;

    void publish(ContentKey key, PlaybackReadSource source);
    bool get(ContentKey key, PlaybackReadSource& out) const noexcept;
    void remove(ContentKey key);
    void clear();

private:
    mutable std::mutex writerMutex_;  // Protects concurrent writers only
    mutable std::shared_ptr<const std::map<ContentKey, PlaybackReadSource>> data_;
};

} // namespace OpenTune
