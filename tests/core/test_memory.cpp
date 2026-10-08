#include <doctest/doctest.h>

#include "mce/memory/allocator.h"
#include "mce/memory/frame_slots.h"
#include "mce/memory/pool.h"
#include "test_support.h"

using namespace mce;
using namespace mce::mem;

TEST_CASE("heap allocations are aligned and tracked") {
    const usize live = heap_live_allocations();
    const usize used = heap().used();
    Allocation a = heap().allocate(100, 64);
    REQUIRE(a.ptr);
    CHECK(reinterpret_cast<usize>(a.ptr) % 64 == 0);
    CHECK(heap_live_allocations() == live + 1);
    CHECK(heap().used() == used + 100);
    heap().free(a.ptr);
    CHECK(heap_live_allocations() == live);
    CHECK(heap().used() == used);
}

TEST_CASE("make_unique destroys through its allocator") {
    struct S {
        int* flag;
        ~S() { *flag = 1; }
    };
    int flag = 0;
    {
        UniquePtr<S> p = make_unique<S>(heap(), S{&flag});
        flag = 0;  // the temporary's destructor ran; reset
        CHECK(p);
    }
    CHECK(flag == 1);
}

TEST_CASE("LinearArena bump, align, rewind, overflow") {
    LinearArena arena(heap(), 1024, "test");
    Allocation a = arena.allocate(10, 1);
    Allocation b = arena.allocate(16, 64);
    REQUIRE(a.ptr);
    REQUIRE(b.ptr);
    CHECK(reinterpret_cast<usize>(b.ptr) % 64 == 0);
    CHECK(arena.owns(b.ptr));
    const auto mark = arena.mark();
    {
        ArenaScope scope(arena);
        (void)arena.allocate(200);
        CHECK(arena.used() > mark);
    }
    CHECK(arena.used() == mark);
    arena.reset();
    CHECK(arena.used() == 0);
    CHECK(arena.high_water() >= 200);
    {
        test::ExpectAssert expect;  // overflow asserts in debug; always returns null
        Allocation big = arena.allocate(4096);
        CHECK(big.ptr == nullptr);
    }
}

namespace {
struct FakeFence final : FrameSlotFence {
    bool signaled = false;
    int waits = 0;
    bool is_signaled() override { return signaled; }
    void wait_blocking() override {
        ++waits;
        signaled = true;
    }
};
}  // namespace

TEST_CASE("FrameSlots reuse a slot only after its fence signals") {
    FrameSlots slots(heap(), 4096);
    FakeFence fences[4];
    for (u64 f = 0; f < 4; ++f) {
        LinearArena& arena = slots.begin_frame(f);
        CHECK(&frame_arena() == &arena);
        CHECK(arena.used() == 0);
        (void)frame().allocate(128);
        slots.end_frame(&fences[f]);
    }
    // Frame 3 reused slot 0 and had to wait on frame 0's fence.
    CHECK(fences[0].waits == 1);
    CHECK(slots.fence_waits() == 1);
    // A signaled fence does not block.
    fences[1].signaled = true;
    slots.begin_frame(4);
    CHECK(fences[1].waits == 0);
    slots.end_frame(nullptr);
    CHECK(slots.slot() == 4 % MCE_FRAMES_IN_FLIGHT);
    set_frame_arena(nullptr);
}

TEST_CASE("Pool acquire/release") {
    Pool<u64> pool(heap(), 3);
    u64* a = pool.acquire(1u);
    u64* b = pool.acquire(2u);
    u64* c = pool.acquire(3u);
    CHECK(pool.acquire(4u) == nullptr);
    CHECK(pool.live() == 3);
    pool.release(b);
    u64* d = pool.acquire(5u);
    CHECK(d == b);
    CHECK(*d == 5);
    pool.release(a);
    pool.release(c);
    pool.release(d);
    CHECK(pool.live() == 0);
}

TEST_CASE("NoHeapScope flags heap traffic on the guarded thread only") {
    using namespace mce;
    const usize before = mem::heap_rt_violations();
    CHECK_FALSE(mem::heap_forbidden_on_this_thread());
    {
        mem::NoHeapScope guard;
        CHECK(mem::heap_forbidden_on_this_thread());
        test::ExpectAssert expect;
        mem::Allocation a = mem::heap().allocate(16);
        mem::heap().free(a.ptr);
    }
    CHECK_FALSE(mem::heap_forbidden_on_this_thread());
    CHECK(mem::heap_rt_violations() == before + 2);
    mem::Allocation b = mem::heap().allocate(16);  // fine again outside the scope
    mem::heap().free(b.ptr);
    CHECK(mem::heap_rt_violations() == before + 2);
}
