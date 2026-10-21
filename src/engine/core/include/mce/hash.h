#pragma once
// Non-cryptographic hashes used across the engine (core.md §1.2: core provides CRC32/xxhash64-class
// primitives; .mce containers use CRC32, StringId uses FNV-1a 64, state hashing uses Hasher64).

#include <cstdint>
#include <type_traits>

#include "mce/types.h"

namespace mce {

inline constexpr u64 kFnv64Offset = 0xcbf29ce484222325ull;
inline constexpr u64 kFnv64Prime = 0x100000001b3ull;

constexpr u64 fnv1a64(StringView s, u64 seed = kFnv64Offset) {
    u64 h = seed;
    for (char c : s) {
        h ^= static_cast<u8>(c);
        h *= kFnv64Prime;
    }
    return h;
}

u64 fnv1a64(const void* data, usize size, u64 seed = kFnv64Offset);

// CRC-32 (IEEE 802.3, reflected 0xEDB88320). crc32("123456789") == 0xCBF43926.
u32 crc32(const void* data, usize size, u32 crc = 0);

// 64-bit finalizer (splitmix64). Good avalanche for integer keys.
constexpr u64 mix64(u64 x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

constexpr u64 hash_combine(u64 seed, u64 v) { return mix64(seed ^ (v + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2))); }

// Streaming, order-sensitive, platform-independent hasher (little-endian byte feed).
// Used by World::state_hash and determinism harness hashers.
class Hasher64 {
public:
    explicit constexpr Hasher64(u64 seed = kFnv64Offset) : h_(seed) {}
    void bytes(const void* data, usize size) { h_ = fnv1a64(data, size, h_); }
    void u64v(u64 v) { h_ = hash_combine(h_, v); }
    void u32v(u32 v) { u64v(v); }
    void f32v(f32 v);  // hashes the bit pattern; -0.0f and +0.0f differ by design
    u64 value() const { return h_; }

private:
    u64 h_;
};

// Hash trait for HashMap keys.
template <class K, class Enable = void>
struct Hash;

template <class K>
struct Hash<K, std::enable_if_t<std::is_integral_v<K> || std::is_enum_v<K>>> {
    u64 operator()(K k) const { return mix64(static_cast<u64>(k)); }
};

template <class T>
struct Hash<T*> {
    u64 operator()(T* p) const { return mix64(reinterpret_cast<std::uintptr_t>(p)); }
};

template <>
struct Hash<StringView> {
    u64 operator()(StringView s) const { return fnv1a64(s); }
};

}  // namespace mce
