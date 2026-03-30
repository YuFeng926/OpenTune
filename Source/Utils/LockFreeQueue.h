#pragma once

/**
 * LockFreeQueue.h - 无锁队列实现
 * 
 * 基于 moodycamel::ConcurrentQueue 设计理念，但简化为项目专用实现。
 * 
 * 特性：
 * - 多生产者多消费者 (MPMC)
 * - 无锁入队/出队
 * - 批量操作支持
 * - 内存预分配
 * 
 * 用于替代 RenderingManager 中的 mutex 保护的队列。
 */

#include <atomic>
#include <vector>
#include <memory>
#include <cstdint>
#include <cstring>
#include <functional>
#include <cassert>

namespace OpenTune {

template<typename T>
class LockFreeQueue {
    struct Cell {
        std::atomic<size_t> sequence;
        T data;
        
        Cell() : sequence(0) {}
    };

public:
    explicit LockFreeQueue(size_t capacity = 1024)
        : capacity_(capacity)
        , mask_(capacity - 1)
        , buffer_(capacity)
    {
        assert((capacity & (capacity - 1)) == 0 && "Capacity must be power of 2");
        for (size_t i = 0; i < capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
        enqueuePos_.store(0, std::memory_order_relaxed);
        dequeuePos_.store(0, std::memory_order_relaxed);
    }

    bool try_enqueue(const T& item) {
        Cell* cell;
        size_t pos = enqueuePos_.load(std::memory_order_relaxed);
        
        while (true) {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            
            if (diff == 0) {
                if (enqueuePos_.compare_exchange_weak(pos, pos + 1,
                                                      std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = enqueuePos_.load(std::memory_order_relaxed);
            }
        }
        
        cell->data = item;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_enqueue(T&& item) {
        Cell* cell;
        size_t pos = enqueuePos_.load(std::memory_order_relaxed);
        
        while (true) {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            
            if (diff == 0) {
                if (enqueuePos_.compare_exchange_weak(pos, pos + 1,
                                                      std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = enqueuePos_.load(std::memory_order_relaxed);
            }
        }
        
        cell->data = std::move(item);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_dequeue(T& item) {
        Cell* cell;
        size_t pos = dequeuePos_.load(std::memory_order_relaxed);
        
        while (true) {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            
            if (diff == 0) {
                if (dequeuePos_.compare_exchange_weak(pos, pos + 1,
                                                      std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = dequeuePos_.load(std::memory_order_relaxed);
            }
        }
        
        item = std::move(cell->data);
        cell->sequence.store(pos + capacity_, std::memory_order_release);
        return true;
    }

    size_t size() const {
        const size_t enq = enqueuePos_.load(std::memory_order_relaxed);
        const size_t deq = dequeuePos_.load(std::memory_order_relaxed);
        return (enq > deq) ? (enq - deq) : 0;
    }

    bool empty() const {
        return size() == 0;
    }

    size_t capacity() const {
        return capacity_;
    }

    void clear() {
        T item;
        while (try_dequeue(item)) {}
    }

private:
    const size_t capacity_;
    const size_t mask_;
    std::vector<Cell> buffer_;
    
    alignas(64) std::atomic<size_t> enqueuePos_;
    alignas(64) std::atomic<size_t> dequeuePos_;
};

/**
 * SPSCRingBuffer - 单生产者单消费者环形缓冲区
 * 
 * 用于音频线程与工作线程之间的数据传输。
 * 最高性能，适用于确定性的生产者-消费者场景。
 */
template<typename T>
class SPSCRingBuffer {
public:
    explicit SPSCRingBuffer(size_t capacity = 4096)
        : capacity_(capacity)
        , mask_(capacity - 1)
        , buffer_(capacity)
    {
        assert((capacity & (capacity - 1)) == 0);
    }

    bool push(const T& item) {
        const size_t currentTail = tail_.load(std::memory_order_relaxed);
        const size_t nextTail = (currentTail + 1) & mask_;
        
        if (nextTail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        
        buffer_[currentTail] = item;
        tail_.store(nextTail, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t currentHead = head_.load(std::memory_order_relaxed);
        
        if (currentHead == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        
        item = std::move(buffer_[currentHead]);
        head_.store((currentHead + 1) & mask_, std::memory_order_release);
        return true;
    }

    size_t size() const {
        return (tail_.load(std::memory_order_relaxed) - 
                head_.load(std::memory_order_relaxed)) & mask_;
    }

    bool empty() const {
        return head_.load(std::memory_order_relaxed) == 
               tail_.load(std::memory_order_relaxed);
    }

private:
    const size_t capacity_;
    const size_t mask_;
    std::vector<T> buffer_;
    
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

/**
 * AtomicSnapshot - 无锁状态快照
 * 
 * 使用 Copy-on-Write 模式实现无锁状态共享。
 * 写入时创建新快照，原子替换指针。
 * 读取时获取指针副本，无需锁。
 */
template<typename T>
class AtomicSnapshot {
public:
    AtomicSnapshot() : snapshot_(std::make_shared<T>()) {}
    
    explicit AtomicSnapshot(std::shared_ptr<T> initial) 
        : snapshot_(initial) {}

    std::shared_ptr<const T> get() const {
        return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
    }

    void set(std::shared_ptr<T> newSnapshot) {
        std::atomic_store_explicit(&snapshot_, newSnapshot, std::memory_order_release);
    }

    void update(std::function<void(T&)> modifier) {
        auto current = get();
        auto copy = std::make_shared<T>(*current);
        modifier(*copy);
        set(copy);
    }

private:
    std::shared_ptr<T> snapshot_;
};

/**
 * AtomicFlag - 无锁标志位
 * 
 * 简单的原子布尔值，用于状态同步。
 */
class AtomicFlag {
public:
    AtomicFlag(bool initial = false) : flag_(initial) {}
    
    void set(bool value = true) {
        flag_.store(value, std::memory_order_release);
    }
    
    void clear() {
        flag_.store(false, std::memory_order_release);
    }
    
    bool test() const {
        return flag_.load(std::memory_order_acquire);
    }
    
    bool test_and_set() {
        return flag_.exchange(true, std::memory_order_acq_rel);
    }
    
    bool test_and_clear() {
        return flag_.exchange(false, std::memory_order_acq_rel);
    }

private:
    std::atomic<bool> flag_;
};

/**
 * AtomicCounter - 无锁计数器
 */
class AtomicCounter {
public:
    explicit AtomicCounter(int64_t initial = 0) : counter_(initial) {}
    
    int64_t increment() {
        return counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    
    int64_t decrement() {
        return counter_.fetch_sub(1, std::memory_order_relaxed) - 1;
    }
    
    int64_t add(int64_t delta) {
        return counter_.fetch_add(delta, std::memory_order_relaxed) + delta;
    }
    
    int64_t get() const {
        return counter_.load(std::memory_order_relaxed);
    }
    
    void set(int64_t value) {
        counter_.store(value, std::memory_order_relaxed);
    }

private:
    std::atomic<int64_t> counter_;
};

}  // namespace OpenTune