#include "mce/render/sort_key.h"

#include <cstring>

#include "mce/assert.h"

namespace mce::render {

void radix_sort(Span<u64> keys, Span<u32> values, mem::IAllocator& scratch) {
    MCE_ASSERT_MSG(keys.size() == values.size(), "radix_sort: keys/values size mismatch");
    const usize n = keys.size();
    if (n < 2) return;

    // Which bytes vary at all? A byte equal in every key leaves the order untouched.
    u64 all_and = ~u64(0), all_or = 0;
    for (u64 k : keys) {
        all_and &= k;
        all_or |= k;
    }
    const u64 varying = all_and ^ all_or;

    mem::Allocation ka = scratch.allocate(n * sizeof(u64), alignof(u64));
    mem::Allocation va = scratch.allocate(n * sizeof(u32), alignof(u32));
    MCE_ASSERT_MSG(ka.ptr && va.ptr, "radix_sort: scratch exhausted");
    if (!ka.ptr || !va.ptr) return;
    u64* src_k = keys.data();
    u32* src_v = values.data();
    u64* dst_k = static_cast<u64*>(ka.ptr);
    u32* dst_v = static_cast<u32*>(va.ptr);

    for (u32 pass = 0; pass < 8; ++pass) {
        const u32 shift = pass * 8;
        if (((varying >> shift) & 0xFF) == 0) continue;
        usize count[256] = {};
        for (usize i = 0; i < n; ++i) ++count[(src_k[i] >> shift) & 0xFF];
        usize sum = 0;
        for (usize& c : count) {
            const usize t = c;
            c = sum;
            sum += t;
        }
        for (usize i = 0; i < n; ++i) {
            const usize at = count[(src_k[i] >> shift) & 0xFF]++;
            dst_k[at] = src_k[i];
            dst_v[at] = src_v[i];
        }
        u64* tk = src_k;
        src_k = dst_k;
        dst_k = tk;
        u32* tv = src_v;
        src_v = dst_v;
        dst_v = tv;
    }
    if (src_k != keys.data()) {
        std::memcpy(keys.data(), src_k, n * sizeof(u64));
        std::memcpy(values.data(), src_v, n * sizeof(u32));
    }
    scratch.free(va.ptr);
    scratch.free(ka.ptr);
}

}  // namespace mce::render
