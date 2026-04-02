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
 * 用于 VocoderRenderScheduler 内部队列（串行任务调度）。
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

}  // namespace OpenTune