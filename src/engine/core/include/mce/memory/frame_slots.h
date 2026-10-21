#pragma once
// Frame-slot ring (core.md §3.1, §4.3; ADR-0005). MCE_FRAMES_IN_FLIGHT arenas; slot N mod 3 is
// reused only after the fence registered for frame N-3 signals. Core owns the CPU half; the
// renderer registers a FrameSlotFence per frame (headless: none = always signaled).

#include "mce/memory/allocator.h"

namespace mce::mem {

class FrameSlotFence {
public:
    virtual ~FrameSlotFence() = default;
    virtual bool is_signaled() = 0;
    virtual void wait_blocking() = 0;
};

class FrameSlots {
public:
    static constexpr usize kDefaultArenaSize = 16 * MiB;

    explicit FrameSlots(IAllocator& backing, usize arena_size = kDefaultArenaSize);
    ~FrameSlots();
    FrameSlots(const FrameSlots&) = delete;
    FrameSlots& operator=(const FrameSlots&) = delete;

    // Starts frame `frame_index`: waits for the slot's previous fence, resets its arena and makes
    // it the calling thread's frame() allocator. Returns the arena.
    LinearArena& begin_frame(u64 frame_index);
    // Ends the current frame. `fence` (may be null) guards the slot's memory until signaled.
    void end_frame(FrameSlotFence* fence);

    u64 frame_index() const { return frame_index_; }
    u32 slot() const { return static_cast<u32>(frame_index_ % MCE_FRAMES_IN_FLIGHT); }
    LinearArena& arena(u32 slot) { return arenas_[slot]; }
    u64 fence_waits() const { return fence_waits_; }

private:
    IAllocator& backing_;
    LinearArena arenas_[MCE_FRAMES_IN_FLIGHT];
    FrameSlotFence* fences_[MCE_FRAMES_IN_FLIGHT] = {};
    u64 frame_index_ = 0;
    u64 fence_waits_ = 0;
    bool in_frame_ = false;
};

// The calling thread's current frame-slot arena. Asserts if no frame has begun on this thread.
LinearArena& frame_arena();
IAllocator& frame();
bool has_frame_arena();
// Overrides the thread's frame arena (tests, worker threads). Returns the previous one.
LinearArena* set_frame_arena(LinearArena* arena);

}  // namespace mce::mem
