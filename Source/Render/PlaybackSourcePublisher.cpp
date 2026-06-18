#include "PlaybackSourcePublisher.h"

namespace OpenTune {

void PlaybackSourcePublisher::publish(ContentKey key, PlaybackReadSource source)
{
    source.contentKey = key;
    std::lock_guard<std::mutex> lg(writerMutex_);
    auto current = std::atomic_load(&data_);
    auto next = current
        ? std::make_shared<std::map<ContentKey, PlaybackReadSource>>(*current)
        : std::make_shared<std::map<ContentKey, PlaybackReadSource>>();
    (*next)[key] = std::move(source);
    std::atomic_store(&data_, std::shared_ptr<const std::map<ContentKey, PlaybackReadSource>>(std::move(next)));
}

bool PlaybackSourcePublisher::get(ContentKey key, PlaybackReadSource& out) const noexcept
{
    auto snap = std::atomic_load(&data_);
    if (!snap) return false;
    auto it = snap->find(key);
    if (it == snap->end()) return false;
    out = it->second;
    return true;
}

void PlaybackSourcePublisher::remove(ContentKey key)
{
    std::lock_guard<std::mutex> lg(writerMutex_);
    auto current = std::atomic_load(&data_);
    if (!current || current->find(key) == current->end()) return;
    auto next = std::make_shared<std::map<ContentKey, PlaybackReadSource>>(*current);
    next->erase(key);
    std::atomic_store(&data_, std::shared_ptr<const std::map<ContentKey, PlaybackReadSource>>(std::move(next)));
}

void PlaybackSourcePublisher::clear()
{
    std::lock_guard<std::mutex> lg(writerMutex_);
    std::atomic_store(&data_, std::shared_ptr<const std::map<ContentKey, PlaybackReadSource>>());
}

} // namespace OpenTune
