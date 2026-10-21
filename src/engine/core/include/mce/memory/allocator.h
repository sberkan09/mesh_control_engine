#pragma once
// Allocator interface and the built-in allocators (core.md §2.3, §3.1).
//
// Rules: engine APIs never allocate behind your back; anything that needs memory takes an
// IAllocator&. heap() is the only path to malloc in engine code. Per-frame allocations come from
// frame() (the current frame-slot arena), which is reset when its slot is reused.

#include <new>
#include <utility>

#include "mce/assert.h"
#include "mce/types.h"

namespace mce::mem {

struct Allocation {
    void* ptr = nullptr;
    usize size = 0;
};

inline constexpr usize kDefaultAlign = 16;

class IAllocator {
public:
    virtual Allocation allocate(usize size, usize align = kDefaultAlign) = 0;
    virtual void free(void* ptr) = 0;
    virtual void reset() {}
    virtual usize used() const = 0;
    virtual const char* name() const = 0;

protected:
    ~IAllocator() = default;
};

// Process-wide CRT heap wrapper. Thread-safe. Tracks live bytes/allocations.
IAllocator& heap();
usize heap_live_allocations();

// Real-time guard (FR-AUD-010): while a NoHeapScope is live on a thread, any heap() allocate/free
// on that thread is a programmer error — it asserts and bumps heap_rt_violations() (the counter
// runs in every build so release tests can check it too). The audio callback runs inside one.
class NoHeapScope {
public:
    NoHeapScope();
    ~NoHeapScope();
    NoHeapScope(const NoHeapScope&) = delete;
    NoHeapScope& operator=(const NoHeapScope&) = delete;
};
bool heap_forbidden_on_this_thread();
usize heap_rt_violations();

// Typed helpers over any allocator.
template <class T, class... Args>
T* make(IAllocator& a, Args&&... args) {
    Allocation m = a.allocate(sizeof(T), alignof(T) > kDefaultAlign ? alignof(T) : kDefaultAlign);
    if (!m.ptr) return nullptr;
    return new (m.ptr) T(std::forward<Args>(args)...);
}

template <class T>
void destroy(IAllocator& a, T* p) {
    if (!p) return;
    p->~T();
    a.free(p);
}

// Owning pointer that remembers its allocator (UniquePtr in the core.md sketches).
template <class T>
class UniquePtr {
public:
    UniquePtr() = default;
    UniquePtr(IAllocator& a, T* p) : alloc_(&a), ptr_(p) {}
    UniquePtr(const UniquePtr&) = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;
    UniquePtr(UniquePtr&& o) noexcept : alloc_(o.alloc_), ptr_(std::exchange(o.ptr_, nullptr)) {}
    template <class U>
    UniquePtr(UniquePtr<U>&& o) noexcept : alloc_(o.allocator()), ptr_(o.release()) {}  // NOLINT
    UniquePtr& operator=(UniquePtr&& o) noexcept {
        if (this != &o) {
            reset();
            alloc_ = o.alloc_;
            ptr_ = std::exchange(o.ptr_, nullptr);
        }
        return *this;
    }
    ~UniquePtr() { reset(); }

    void reset() {
        if (ptr_) destroy(*alloc_, ptr_);
        ptr_ = nullptr;
    }
    T* release() { return std::exchange(ptr_, nullptr); }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    IAllocator* allocator() const { return alloc_; }

private:
    IAllocator* alloc_ = nullptr;
    T* ptr_ = nullptr;
};

template <class T, class... Args>
UniquePtr<T> make_unique(IAllocator& a, Args&&... args) {
    return UniquePtr<T>(a, make<T>(a, std::forward<Args>(args)...));
}

// Bump allocator over a fixed buffer. free() is a no-op; reset() rewinds everything.
// Used for frame-slot arenas (16 MiB each), scratch, and cook LIFO scopes (via markers).
class LinearArena final : public IAllocator {
public:
    LinearArena() = default;
    // Takes `capacity` bytes from `backing` for its lifetime.
    LinearArena(IAllocator& backing, usize capacity, const char* name = "linear");
    // Uses caller-owned memory.
    LinearArena(void* buffer, usize capacity, const char* name = "linear");
    LinearArena(const LinearArena&) = delete;
    LinearArena& operator=(const LinearArena&) = delete;
    ~LinearArena();

    Allocation allocate(usize size, usize align = kDefaultAlign) override;
    void free(void*) override {}
    void reset() override;
    usize used() const override { return offset_; }
    const char* name() const override { return name_; }

    usize capacity() const { return capacity_; }
    usize high_water() const { return high_water_; }
    bool owns(const void* p) const;

    using Marker = usize;
    Marker mark() const { return offset_; }
    void rewind(Marker m);

private:
    IAllocator* backing_ = nullptr;
    u8* base_ = nullptr;
    usize capacity_ = 0;
    usize offset_ = 0;
    usize high_water_ = 0;
    const char* name_ = "linear";
};

// RAII scratch scope: rewinds the arena to where it was on construction.
class ArenaScope {
public:
    explicit ArenaScope(LinearArena& arena) : arena_(arena), mark_(arena.mark()) {}
    ~ArenaScope() { arena_.rewind(mark_); }
    ArenaScope(const ArenaScope&) = delete;
    ArenaScope& operator=(const ArenaScope&) = delete;

private:
    LinearArena& arena_;
    LinearArena::Marker mark_;
};

}  // namespace mce::mem
