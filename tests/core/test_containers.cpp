#include <doctest/doctest.h>

#include "mce/containers/array.h"
#include "mce/containers/bitset.h"
#include "mce/containers/flat_map.h"
#include "mce/containers/hash_map.h"
#include "mce/containers/mpsc_queue.h"
#include "mce/containers/ring_buffer.h"
#include "mce/fixed_string.h"
#include "mce/memory/handle.h"
#include "mce/string_id.h"
#include "test_support.h"

#include <thread>
#include <new>
#include <type_traits>

using namespace mce;

namespace {
struct Tracked {
    static inline int live = 0;
    int v = 0;
    Tracked() { ++live; }
    explicit Tracked(int x) : v(x) { ++live; }
    Tracked(const Tracked& o) : v(o.v) { ++live; }
    Tracked(Tracked&& o) noexcept : v(o.v) { ++live; }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) = default;
    ~Tracked() { --live; }
    bool operator==(const Tracked& o) const { return v == o.v; }
};
}  // namespace

TEST_CASE("Array basic operations") {
    Array<int> a;
    for (int i = 0; i < 100; ++i) a.push_back(i);
    CHECK(a.size() == 100);
    CHECK(a[57] == 57);
    a.erase_swap(0);
    CHECK(a[0] == 99);
    CHECK(a.size() == 99);
    a.erase(0);
    CHECK(a[0] == 1);
    a.insert(0, -5);
    CHECK(a[0] == -5);
    CHECK(a[1] == 1);
    CHECK(a.contains(42));
    CHECK_FALSE(a.contains(1000));
    a.resize(10);
    CHECK(a.size() == 10);
    a.clear();
    CHECK(a.empty());
}

TEST_CASE("Array resize grows geometrically but stays exact from empty") {
    Array<int> a;
    a.resize(100);
    CHECK(a.capacity() == 100);  // a one-shot size is not padded
    a.resize(101);
    CHECK(a.capacity() >= 150);  // growing past it steps 1.5x, so a chunk-at-a-time loop is linear
    usize reallocs = 0, cap = a.capacity();
    for (usize n = 102; n < 5000; ++n) {
        a.resize(n);
        if (a.capacity() != cap) {
            ++reallocs;
            cap = a.capacity();
        }
    }
    CHECK(reallocs < 20);
    CHECK(a.size() == 4999);
    CHECK(a[0] == 0);
    CHECK(a[4998] == 0);

    Array<int> f;
    f.resize(10, 7);
    f.resize(50, 9);
    CHECK(f[9] == 7);
    CHECK(f[10] == 9);
    CHECK(f.capacity() >= 50);

    Array<byte> u;
    u.resize_uninitialized(64);
    CHECK(u.capacity() == 64);
    u.resize_uninitialized(65);
    CHECK(u.capacity() >= 96);
}

TEST_CASE("Array push_back of an aliased element survives growth") {
    Array<Tracked> a;
    a.emplace_back(7);
    for (int i = 0; i < 20; ++i) a.push_back(a[0]);
    for (const Tracked& t : a) CHECK(t.v == 7);
}

TEST_CASE("Array destroys elements and frees memory") {
    const usize live_before = mem::heap_live_allocations();
    {
        Array<Tracked> a;
        for (int i = 0; i < 33; ++i) a.emplace_back(i);
        Array<Tracked> b = a;
        Array<Tracked> c = std::move(a);
        CHECK(Tracked::live == 66);
        CHECK(c.size() == 33);
        CHECK(a.empty());  // NOLINT(bugprone-use-after-move): moved-from state is specified
    }
    CHECK(Tracked::live == 0);
    CHECK(mem::heap_live_allocations() == live_before);
}

TEST_CASE("SmallVector stays inline then spills") {
    const usize live_before = mem::heap_live_allocations();
    SmallVector<int, 4> v;
    for (int i = 0; i < 4; ++i) v.push_back(i);
    CHECK(mem::heap_live_allocations() == live_before);
    v.push_back(4);
    CHECK(mem::heap_live_allocations() == live_before + 1);
    SmallVector<int, 4> moved = std::move(v);
    CHECK(moved.size() == 5);
    CHECK(moved[4] == 4);

    SmallVector<Tracked, 2> inl;
    inl.emplace_back(1);
    SmallVector<Tracked, 2> inl2 = std::move(inl);
    CHECK(inl2[0].v == 1);
    CHECK(inl.empty());  // NOLINT(bugprone-use-after-move): moved-from state is specified
}

#if MCE_ASSERTS_ENABLED
TEST_CASE("Array index out of range asserts") {
    Array<int> a;
    a.push_back(1);
    test::ExpectAssert expect;
    (void)a[3];
}
#endif

TEST_CASE("HashMap insert/find/erase") {
    HashMap<u32, int> m;
    for (u32 i = 0; i < 1000; ++i) m.insert_or_assign(i * 7, static_cast<int>(i));
    CHECK(m.size() == 1000);
    for (u32 i = 0; i < 1000; ++i) {
        const int* v = m.find(i * 7);
        REQUIRE(v);
        CHECK(*v == static_cast<int>(i));
    }
    CHECK(m.find(3) == nullptr);
    for (u32 i = 0; i < 1000; i += 2) CHECK(m.erase(i * 7));
    CHECK(m.size() == 500);
    for (u32 i = 0; i < 1000; ++i) CHECK(m.contains(i * 7) == (i % 2 == 1));
    usize visited = 0;
    for (auto& e : m) {
        CHECK(e.key % 7 == 0);
        ++visited;
    }
    CHECK(visited == 500);
    auto [ptr, inserted] = m.try_emplace(7, 99);
    CHECK_FALSE(inserted);
    CHECK(*ptr == 1);
    m[123456] = 5;
    CHECK(*m.find(123456) == 5);
}

TEST_CASE("HashMap with StringId keys and non-trivial values") {
    HashMap<StringId, Array<int>> m;
    m["alpha"].push_back(1);
    m["beta"].push_back(2);
    m["alpha"].push_back(3);
    CHECK(m.find("alpha")->size() == 2);
    CHECK(m.erase("alpha"));
    CHECK(m.find("alpha") == nullptr);
    CHECK(m.find("beta")->front() == 2);
}

TEST_CASE("HashMap iteration order is deterministic for the same operations") {
    auto build = [] {
        HashMap<u64, int> m;
        for (u64 i = 0; i < 200; ++i) m.insert_or_assign(i * 2654435761ull, static_cast<int>(i));
        for (u64 i = 0; i < 200; i += 3) m.erase(i * 2654435761ull);
        Array<int> order;
        for (auto& e : m) order.push_back(e.value);
        return order;
    };
    Array<int> a = build();
    Array<int> b = build();
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}

TEST_CASE("FlatMap keeps keys sorted") {
    FlatMap<int, char> m;
    m.insert_or_assign(5, 'e');
    m.insert_or_assign(1, 'a');
    m.insert_or_assign(3, 'c');
    m[2] = 'b';
    int prev = -1;
    for (const auto& e : m) {
        CHECK(e.key > prev);
        prev = e.key;
    }
    CHECK(*m.find(3) == 'c');
    CHECK(m.erase(3));
    CHECK_FALSE(m.contains(3));
    CHECK(m.size() == 3);
}

TEST_CASE("BitSet") {
    BitSet<130> b;
    b.set(0);
    b.set(64);
    b.set(129);
    CHECK(b.count() == 3);
    CHECK(b.test(64));
    b.reset(64);
    CHECK_FALSE(b.test(64));
    BitSet<130> sub;
    sub.set(129);
    CHECK(b.contains_all(sub));
    CHECK(b.intersects(sub));
    sub.set(5);
    CHECK_FALSE(b.contains_all(sub));
}

TEST_CASE("RingBuffer SPSC") {
    RingBuffer<int> rb(mem::heap(), 4);
    CHECK(rb.push(1));
    CHECK(rb.push(2));
    CHECK(rb.push(3));
    CHECK(rb.push(4));
    CHECK_FALSE(rb.push(5));
    int v = 0;
    CHECK(rb.pop(v));
    CHECK(v == 1);

    // Cross-thread transfer preserves order.
    RingBuffer<u32> q(mem::heap(), 64);
    constexpr u32 kCount = 100000;
    std::thread producer([&] {
        for (u32 i = 0; i < kCount;) {
            if (q.push(i)) ++i;
        }
    });
    u32 expected = 0;
    while (expected < kCount) {
        u32 got;
        if (q.pop(got)) {
            REQUIRE(got == expected);
            ++expected;
        }
    }
    producer.join();
}

TEST_CASE("MpScQueue with several producers") {
    MpScQueue<u64> q(mem::heap(), 1024);
    constexpr u64 kPerThread = 20000;
    constexpr int kThreads = 4;
    std::thread producers[kThreads];
    for (int t = 0; t < kThreads; ++t) {
        producers[t] = std::thread([&q, t] {
            for (u64 i = 0; i < kPerThread;) {
                if (q.try_push((static_cast<u64>(t) << 32) | i)) ++i;
            }
        });
    }
    u64 next[kThreads] = {};
    u64 received = 0;
    while (received < kPerThread * kThreads) {
        u64 v;
        if (q.try_pop(v)) {
            const int t = static_cast<int>(v >> 32);
            REQUIRE((v & 0xFFFFFFFFu) == next[t]);  // per-producer FIFO
            ++next[t];
            ++received;
        }
    }
    for (auto& p : producers) p.join();
}

TEST_CASE("HandleTable: stale handles fail, never alias") {
    struct Tag {};
    HandleTable<int, Tag> t;
    auto a = t.insert(10);
    auto b = t.insert(20);
    CHECK(*t.try_get(a) == 10);
    CHECK(t.erase(a));
    CHECK(t.try_get(a) == nullptr);
    auto c = t.insert(30);  // reuses a's index with a new generation
    CHECK(c.index == a.index);
    CHECK(c.generation != a.generation);
    CHECK(t.try_get(a) == nullptr);
    CHECK(*t.try_get(c) == 30);
    CHECK(*t.try_get(b) == 20);
    CHECK(t.size() == 2);
    CHECK_FALSE(t.erase(a));
    Handle<Tag> null_handle;
    CHECK(t.try_get(null_handle) == nullptr);
}

// Components hold FixedStrings and World::state_hash hashes their bytes: no padding, sizes kept.
static_assert(std::has_unique_object_representations_v<FixedString<24>>);
static_assert(std::has_unique_object_representations_v<FixedString<41>>);
static_assert(std::has_unique_object_representations_v<FixedString<66>>);
static_assert(sizeof(FixedString<24>) == 32 && sizeof(FixedString<41>) == 52 && sizeof(FixedString<66>) == 76);

TEST_CASE("FixedString: a copy carries no stray bytes") {
    alignas(FixedString<24>) unsigned char dirty[sizeof(FixedString<24>)];
    std::memset(dirty, 0x30, sizeof dirty);
    auto* tmp = new (dirty) FixedString<24>("step");
    FixedString<24> a;
    a = *tmp;
    const FixedString<24> clean("step");
    CHECK(std::memcmp(&a, &clean, sizeof a) == 0);
}

TEST_CASE("FixedString truncates safely") {
    FixedString<8> s("hello world");
    CHECK(s.size() == 7);
    CHECK(s.truncated());
    CHECK(s == "hello w");
    s.assign("hi");
    s.append(" there");
    CHECK(s == "hi ther");
}
