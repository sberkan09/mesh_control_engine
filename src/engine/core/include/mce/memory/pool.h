#pragma once
// Fixed-capacity object pool with an intrusive free list (voices, packets, chunks).

#include "mce/memory/allocator.h"

namespace mce::mem {

template <class T>
class Pool {
    union Slot {
        Slot() {}
        ~Slot() {}
        Slot* next;
        alignas(T) unsigned char storage[sizeof(T)];
    };

public:
    Pool(IAllocator& backing, usize capacity) : backing_(backing), capacity_(capacity) {
        Allocation m = backing_.allocate(sizeof(Slot) * capacity, alignof(Slot) > kDefaultAlign ? alignof(Slot) : kDefaultAlign);
        MCE_ASSERT_MSG(m.ptr != nullptr, "Pool backing allocation failed");
        slots_ = static_cast<Slot*>(m.ptr);
        for (usize i = 0; i < capacity; ++i) slots_[i].next = (i + 1 < capacity) ? &slots_[i + 1] : nullptr;
        free_ = capacity ? &slots_[0] : nullptr;
    }
    ~Pool() {
        MCE_ASSERT_MSG(live_ == 0, "Pool destroyed with live objects");
        backing_.free(slots_);
    }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // Returns nullptr when exhausted.
    template <class... Args>
    T* acquire(Args&&... args) {
        if (!free_) return nullptr;
        Slot* s = free_;
        free_ = s->next;
        ++live_;
        return new (s->storage) T(std::forward<Args>(args)...);
    }

    void release(T* p) {
        if (!p) return;
        Slot* s = reinterpret_cast<Slot*>(p);
        MCE_ASSERT_MSG(s >= slots_ && s < slots_ + capacity_, "Pool::release foreign pointer");
        p->~T();
        s->next = free_;
        free_ = s;
        --live_;
    }

    usize live() const { return live_; }
    usize capacity() const { return capacity_; }

private:
    IAllocator& backing_;
    Slot* slots_ = nullptr;
    Slot* free_ = nullptr;
    usize capacity_ = 0;
    usize live_ = 0;
};

}  // namespace mce::mem
