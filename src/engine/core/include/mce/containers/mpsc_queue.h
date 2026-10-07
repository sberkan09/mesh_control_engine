#pragma once
// MpScQueue<T>: bounded multi-producer / single-consumer queue (fix F10). Vyukov's bounded MPMC
// ring (per-cell sequence numbers, CAS on enqueue) used with one consumer — the main thread.
// Consumers: editor event pump, log fan-out, asset commit queue.

#include <atomic>
#include <new>
#include <utility>

#include "mce/memory/allocator.h"

namespace mce {

template <class T>
class MpScQueue {
    struct Cell {
        std::atomic<usize> seq;
        T data;
    };

public:
    MpScQueue(mem::IAllocator& a, usize capacity_pow2) : alloc_(a), mask_(capacity_pow2 - 1) {
        MCE_ASSERT_MSG(is_pow2(capacity_pow2), "MpScQueue capacity must be a power of two");
        mem::Allocation m = alloc_.allocate(sizeof(Cell) * capacity_pow2, alignof(Cell) > mem::kDefaultAlign ? alignof(Cell) : mem::kDefaultAlign);
        MCE_ASSERT_MSG(m.ptr != nullptr, "MpScQueue out of memory");
        cells_ = static_cast<Cell*>(m.ptr);
        for (usize i = 0; i < capacity_pow2; ++i) {
            new (&cells_[i].seq) std::atomic<usize>(i);
        }
    }
    ~MpScQueue() {
        T tmp;
        while (try_pop(tmp)) {
        }
        alloc_.free(cells_);
    }
    MpScQueue(const MpScQueue&) = delete;
    MpScQueue& operator=(const MpScQueue&) = delete;

    // Any thread. Returns false when full.
    bool try_push(T v) {
        usize pos = enqueue_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells_[pos & mask_];
            const usize seq = c.seq.load(std::memory_order_acquire);
            const isize diff = static_cast<isize>(seq) - static_cast<isize>(pos);
            if (diff == 0) {
                if (enqueue_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    new (&c.data) T(std::move(v));
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;  // full
            } else {
                pos = enqueue_.load(std::memory_order_relaxed);
            }
        }
    }

    // Consumer thread only. Returns false when empty.
    bool try_pop(T& out) {
        const usize pos = dequeue_;
        Cell& c = cells_[pos & mask_];
        const usize seq = c.seq.load(std::memory_order_acquire);
        if (static_cast<isize>(seq) - static_cast<isize>(pos + 1) < 0) return false;  // empty
        out = std::move(c.data);
        c.data.~T();
        c.seq.store(pos + mask_ + 1, std::memory_order_release);
        dequeue_ = pos + 1;
        return true;
    }

    usize capacity() const { return mask_ + 1; }

private:
    mem::IAllocator& alloc_;
    Cell* cells_ = nullptr;
    usize mask_;
    alignas(64) std::atomic<usize> enqueue_{0};
    alignas(64) usize dequeue_ = 0;
};

}  // namespace mce
