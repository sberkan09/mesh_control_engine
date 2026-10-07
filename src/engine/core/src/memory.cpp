#include <atomic>
#include <cstdlib>
#include <cstring>

#include "mce/memory/allocator.h"
#include "mce/memory/frame_slots.h"

#if defined(_WIN32)
#include <malloc.h>
#endif

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>
#define MCE_TRACY_ALLOC(p, s, n) TracyAllocN(p, s, n)
#define MCE_TRACY_FREE(p, n) TracyFreeN(p, n)
#else
#define MCE_TRACY_ALLOC(p, s, n) ((void)0)
#define MCE_TRACY_FREE(p, n) ((void)0)
#endif

namespace mce::mem {
namespace {

// Header stored in front of every heap block so free() knows the size and alignment offset.
struct HeapHeader {
    usize size;
    usize offset;  // bytes from the raw malloc pointer to the user pointer
};

class HeapAllocator final : public IAllocator {
public:
    Allocation allocate(usize size, usize align) override {
        if (align < alignof(HeapHeader)) align = alignof(HeapHeader);
        MCE_ASSERT_MSG(is_pow2(align), "alignment must be a power of two");
        const usize total = size + align + sizeof(HeapHeader);
        u8* raw = static_cast<u8*>(std::malloc(total));
        if (!raw) return {};
        const usize user_addr = align_up(reinterpret_cast<usize>(raw) + sizeof(HeapHeader), align);
        u8* user = reinterpret_cast<u8*>(user_addr);
        HeapHeader* h = reinterpret_cast<HeapHeader*>(user) - 1;
        h->size = size;
        h->offset = static_cast<usize>(user - raw);
        used_.fetch_add(size, std::memory_order_relaxed);
        live_.fetch_add(1, std::memory_order_relaxed);
        MCE_TRACY_ALLOC(user, size, "heap");
        return {user, size};
    }

    void free(void* ptr) override {
        if (!ptr) return;
        MCE_TRACY_FREE(ptr, "heap");
        HeapHeader* h = static_cast<HeapHeader*>(ptr) - 1;
        used_.fetch_sub(h->size, std::memory_order_relaxed);
        live_.fetch_sub(1, std::memory_order_relaxed);
        std::free(static_cast<u8*>(ptr) - h->offset);
    }

    usize used() const override { return used_.load(std::memory_order_relaxed); }
    const char* name() const override { return "heap"; }
    usize live() const { return live_.load(std::memory_order_relaxed); }

private:
    std::atomic<usize> used_{0};
    std::atomic<usize> live_{0};
};

HeapAllocator& heap_instance() {
    static HeapAllocator h;
    return h;
}

thread_local LinearArena* t_frame_arena = nullptr;

#if MCE_ASSERTS_ENABLED
constexpr u8 kResetFill = 0xCD;
#endif

}  // namespace

IAllocator& heap() { return heap_instance(); }
usize heap_live_allocations() { return heap_instance().live(); }

// ------------------------------------------------------------------ LinearArena

LinearArena::LinearArena(IAllocator& backing, usize capacity, const char* name)
    : backing_(&backing), capacity_(capacity), name_(name) {
    Allocation m = backing.allocate(capacity, 64);
    MCE_ASSERT_MSG(m.ptr != nullptr, "LinearArena backing allocation failed");
    base_ = static_cast<u8*>(m.ptr);
}

LinearArena::LinearArena(void* buffer, usize capacity, const char* name)
    : base_(static_cast<u8*>(buffer)), capacity_(capacity), name_(name) {}

LinearArena::~LinearArena() {
    if (backing_ && base_) backing_->free(base_);
}

Allocation LinearArena::allocate(usize size, usize align) {
    MCE_ASSERT_MSG(is_pow2(align), "alignment must be a power of two");
    const usize start = align_up(reinterpret_cast<usize>(base_) + offset_, align) - reinterpret_cast<usize>(base_);
    if (start + size > capacity_) {
        MCE_ASSERT_MSG(false, "LinearArena overflow");
        return {};
    }
    offset_ = start + size;
    if (offset_ > high_water_) high_water_ = offset_;
    return {base_ + start, size};
}

void LinearArena::reset() { rewind(0); }

void LinearArena::rewind(Marker m) {
    MCE_ASSERT_MSG(m <= offset_, "LinearArena::rewind past the current offset");
#if MCE_ASSERTS_ENABLED
    // Surface use-after-reset in debug builds.
    if (base_ && offset_ > m) std::memset(base_ + m, kResetFill, offset_ - m);
#endif
    offset_ = m;
}

bool LinearArena::owns(const void* p) const {
    const u8* b = static_cast<const u8*>(p);
    return b >= base_ && b < base_ + capacity_;
}

// ------------------------------------------------------------------ FrameSlots

FrameSlots::FrameSlots(IAllocator& backing, usize arena_size) : backing_(backing) {
    // LinearArena is non-movable; rebuild the default-constructed members in place.
    for (u32 i = 0; i < MCE_FRAMES_IN_FLIGHT; ++i) {
        arenas_[i].~LinearArena();
        new (&arenas_[i]) LinearArena(backing_, arena_size, "frame");
    }
}

FrameSlots::~FrameSlots() {
    for (u32 i = 0; i < MCE_FRAMES_IN_FLIGHT; ++i) {
        if (t_frame_arena == &arenas_[i]) t_frame_arena = nullptr;
    }
}

LinearArena& FrameSlots::begin_frame(u64 frame_index) {
    MCE_ASSERT_MSG(!in_frame_, "begin_frame without end_frame");
    frame_index_ = frame_index;
    const u32 s = slot();
    if (FrameSlotFence* f = fences_[s]) {
        if (!f->is_signaled()) {
            ++fence_waits_;
            f->wait_blocking();
        }
        fences_[s] = nullptr;
    }
    arenas_[s].reset();
    t_frame_arena = &arenas_[s];
    in_frame_ = true;
    return arenas_[s];
}

void FrameSlots::end_frame(FrameSlotFence* fence) {
    MCE_ASSERT_MSG(in_frame_, "end_frame without begin_frame");
    fences_[slot()] = fence;
    in_frame_ = false;
}

LinearArena& frame_arena() {
    MCE_ASSERT_MSG(t_frame_arena != nullptr, "no frame arena on this thread (begin_frame not called)");
    return *t_frame_arena;
}

IAllocator& frame() { return frame_arena(); }

bool has_frame_arena() { return t_frame_arena != nullptr; }

LinearArena* set_frame_arena(LinearArena* arena) {
    LinearArena* prev = t_frame_arena;
    t_frame_arena = arena;
    return prev;
}

}  // namespace mce::mem
