#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace OpenTune {

// Readers borrow the published raw pointer. publish(), collect(), and clear()
// require external writer serialization; the snapshot must outlive every Read.
template<class T, std::size_t RetiredCapacity = 4>
class RealtimeSnapshot
{
    static_assert(RetiredCapacity > 0, "RealtimeSnapshot requires retirement capacity");
    static_assert(std::atomic<const T*>::is_always_lock_free,
                  "RealtimeSnapshot pointer atomic must be lock-free");
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "RealtimeSnapshot reader counter must be lock-free");

public:
    using Owner = std::shared_ptr<const T>;

    class Read
    {
    public:
        Read() noexcept = default;
        ~Read() { release(); }

        Read(const Read&) = delete;
        Read& operator=(const Read&) = delete;

        Read(Read&& other) noexcept
            : value_(std::exchange(other.value_, nullptr)),
              readers_(std::exchange(other.readers_, nullptr))
        {
        }

        Read& operator=(Read&& other) noexcept
        {
            if (this == &other)
                return *this;
            release();
            value_ = std::exchange(other.value_, nullptr);
            readers_ = std::exchange(other.readers_, nullptr);
            return *this;
        }

        const T* get() const noexcept { return value_; }
        const T* operator->() const noexcept { return value_; }
        const T& operator*() const noexcept { return *value_; }
        explicit operator bool() const noexcept { return value_ != nullptr; }

    private:
        friend class RealtimeSnapshot;

        Read(const T* value, std::atomic<std::size_t>* readers) noexcept
            : value_(value), readers_(readers)
        {
        }

        void release() noexcept
        {
            if (readers_ != nullptr)
                readers_->fetch_sub(1, std::memory_order_seq_cst);
            value_ = nullptr;
            readers_ = nullptr;
        }

        const T* value_ = nullptr;
        std::atomic<std::size_t>* readers_ = nullptr;
    };

    RealtimeSnapshot() = default;
    RealtimeSnapshot(const RealtimeSnapshot&) = delete;
    RealtimeSnapshot& operator=(const RealtimeSnapshot&) = delete;

    Read read() const noexcept
    {
        readers_.fetch_add(1, std::memory_order_seq_cst);
        return Read(published_.load(std::memory_order_seq_cst), &readers_);
    }

    void publish(Owner owner)
    {
        if (retiredCount_ == RetiredCapacity) {
            pending_ = std::move(owner);
            hasPending_ = true;
            return;
        }
        publishCurrent(std::move(owner));
    }

    // True marks a reader-free reclamation point after promoting pending state.
    // A reader arriving later can only borrow the current published version.
    bool collect()
    {
        if (readers_.load(std::memory_order_seq_cst) != 0)
            return false;

        for (std::size_t i = 0; i < retiredCount_; ++i)
            retired_[i].reset();
        retiredCount_ = 0;

        if (!hasPending_)
            return true;

        Owner next = std::move(pending_);
        hasPending_ = false;
        return !publishCurrent(std::move(next));
    }

    void clear() { publish(Owner{}); }

    // Writer-side query; serialize access with publish(), collect(), and clear().
    const Owner& latestOwner() const noexcept
    {
        return hasPending_ ? pending_ : current_;
    }

    // State queries are writer-side except hasReaders().
    std::size_t retiredCount() const noexcept { return retiredCount_; }
    bool hasReaders() const noexcept { return readers_.load(std::memory_order_seq_cst) != 0; }
    bool hasPending() const noexcept { return hasPending_; }

private:
    // Returns whether a reader was active after the pointer exchange.
    bool publishCurrent(Owner owner)
    {
        published_.exchange(owner.get(), std::memory_order_seq_cst);
        const bool readersActive = readers_.load(std::memory_order_seq_cst) != 0;

        Owner previous = std::move(current_);
        current_ = std::move(owner);
        if (readersActive && previous != nullptr)
            retired_[retiredCount_++] = std::move(previous);
        return readersActive;
    }

    mutable std::atomic<std::size_t> readers_ { 0 };
    std::atomic<const T*> published_ { nullptr };
    Owner current_;
    Owner pending_;
    std::array<Owner, RetiredCapacity> retired_ {};
    std::size_t retiredCount_ = 0;
    bool hasPending_ = false;
};

} // namespace OpenTune
