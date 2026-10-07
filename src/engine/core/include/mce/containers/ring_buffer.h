#pragma once
// RingBuffer<T>: bounded single-producer / single-consumer FIFO, power-of-two capacity,
// acquire/release atomics, no locks, no allocation after construction (audio command FIFO,
// log fan-out — core.md §3.2). T must be trivially copyable (POD commands).

#include <atomic>
#include <type_traits>

#include "mce/memory/allocator.h"

namespace mce {

template <class T>
class RingBuffer {
    static_assert(std::is_trivially_copyable_v<T>, "RingBuffer carries POD payloads only");

public:
    RingBuffer(mem::IAllocator& a, usize capacity_pow2) : alloc_(a), capacity_(capacity_pow2) {
        MCE_ASSERT_MSG(is_pow2(capacity_pow2), "RingBuffer capacity must be a power of two");
        mem::Allocation m = alloc_.allocate(sizeof(T) * capacity_, alignof(T) > mem::kDefaultAlign ? alignof(T) : mem::kDefaultAlign);
        MCE_ASSERT_MSG(m.ptr != nullptr, "RingBuffer out of memory");
        data_ = static_cast<T*>(m.ptr);
    }
    ~RingBuffer() { alloc_.free(data_); }
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // Producer side. Returns false when full.
    bool push(const T& v) {
        const usize head = head_.load(std::memory_order_relaxed);
        const usize tail = tail_.load(std::memory_order_acquire);
        if (head - tail == capacity_) return false;
        data_[head & (capacity_ - 1)] = v;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false when empty.
    bool pop(T& out) {
        const usize tail = tail_.load(std::memory_order_relaxed);
        const usize head = head_.load(std::memory_order_acquire);
        if (head == tail) return false;
        out = data_[tail & (capacity_ - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    usize size_approx() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }
    usize capacity() const { return capacity_; }

private:
    mem::IAllocator& alloc_;
    T* data_ = nullptr;
    usize capacity_;
    alignas(64) std::atomic<usize> head_{0};
    alignas(64) std::atomic<usize> tail_{0};
};

}  // namespace mce
