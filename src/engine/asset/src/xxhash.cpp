#include "mce/asset/xxhash.h"

#include <cstring>

namespace mce::asset {

namespace {
constexpr u64 P1 = 0x9E3779B185EBCA87ull;
constexpr u64 P2 = 0xC2B2AE3D27D4EB4Full;
constexpr u64 P3 = 0x165667B19E3779F9ull;
constexpr u64 P4 = 0x85EBCA77C2B2AE63ull;
constexpr u64 P5 = 0x27D4EB2F165667C5ull;

constexpr u64 rotl(u64 x, int r) { return (x << r) | (x >> (64 - r)); }

// Little-endian loads regardless of host order (every supported target is LE; memcpy keeps
// the loads alignment-safe).
u64 read64(const u8* p) {
    u64 v;
    std::memcpy(&v, p, 8);
    return v;
}
u32 read32(const u8* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}

u64 xxround(u64 acc, u64 input) {
    acc += input * P2;
    acc = rotl(acc, 31);
    return acc * P1;
}
u64 merge_round(u64 acc, u64 val) {
    acc ^= xxround(0, val);
    return acc * P1 + P4;
}
}  // namespace

u64 xxh64(const void* data, usize size, u64 seed) {
    const u8* p = static_cast<const u8*>(data);
    const u8* const end = p + size;
    u64 h;
    if (size >= 32) {
        const u8* const limit = end - 32;
        u64 v1 = seed + P1 + P2;
        u64 v2 = seed + P2;
        u64 v3 = seed;
        u64 v4 = seed - P1;
        do {
            v1 = xxround(v1, read64(p));
            v2 = xxround(v2, read64(p + 8));
            v3 = xxround(v3, read64(p + 16));
            v4 = xxround(v4, read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = merge_round(h, v1);
        h = merge_round(h, v2);
        h = merge_round(h, v3);
        h = merge_round(h, v4);
    } else {
        h = seed + P5;
    }
    h += static_cast<u64>(size);
    while (p + 8 <= end) {
        h ^= xxround(0, read64(p));
        h = rotl(h, 27) * P1 + P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<u64>(read32(p)) * P1;
        h = rotl(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<u64>(*p) * P5;
        h = rotl(h, 11) * P1;
        ++p;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

}  // namespace mce::asset
