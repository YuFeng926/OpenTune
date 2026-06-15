#pragma once

#include "PlaybackReadSource.h"
#include "../Content/ContentKey.h"
#include <juce_core/juce_core.h>
#include <map>
#include <memory>

namespace OpenTune {

/**
 * Publishes ContentKey-indexed PlaybackReadSource snapshots.
 *
 * Writers rebuild an immutable map on the message/background side; audio readers
 * take the latest atomic shared_ptr snapshot and do not lock the model graph.
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
    void rebuildSnapshotLocked();

    juce::ReadWriteLock lock_;
    std::map<ContentKey, PlaybackReadSource> sources_;
    mutable std::shared_ptr<const std::map<ContentKey, PlaybackReadSource>> snapshot_;
};

} // namespace OpenTune
