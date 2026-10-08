#pragma once
// XXH64 (Yann Collet's xxHash, 64-bit variant), one-shot. The asset pipeline's content hash:
// source hashes, settings hashes, cooked-container names (assets.md §3.1, §4.3).
// Reference vectors: xxh64("", 0) == 0xEF46DB3751D8E999, xxh64("abc", 0) == 0x44BC2CF5AD770999.

#include "mce/types.h"

namespace mce::asset {

u64 xxh64(const void* data, usize size, u64 seed = 0);

inline u64 xxh64(StringView s, u64 seed = 0) { return xxh64(s.data(), s.size(), seed); }

}  // namespace mce::asset
