#include "mce/hash.h"

#include <cstring>

namespace mce {

u64 fnv1a64(const void* data, usize size, u64 seed) {
    const u8* p = static_cast<const u8*>(data);
    u64 h = seed;
    for (usize i = 0; i < size; ++i) {
        h ^= p[i];
        h *= kFnv64Prime;
    }
    return h;
}

namespace {
struct Crc32Table {
    u32 v[256];
    constexpr Crc32Table() : v() {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            v[i] = c;
        }
    }
};
constexpr Crc32Table kCrcTable;
}  // namespace

u32 crc32(const void* data, usize size, u32 crc) {
    const u8* p = static_cast<const u8*>(data);
    crc = ~crc;
    for (usize i = 0; i < size; ++i) crc = kCrcTable.v[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

void Hasher64::f32v(f32 v) {
    u32 bits;
    std::memcpy(&bits, &v, sizeof(bits));
    u32v(bits);
}

}  // namespace mce
