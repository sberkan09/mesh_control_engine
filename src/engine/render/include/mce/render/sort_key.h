#pragma once
// 64-bit draw sort keys and the LSD radix sort over them (rhi-renderer.md §3.4 step 3).
//
//   [63:56] pass id        [55:40] sortLayer (2D)   [39:24] material / pipeline
//   [23:00] depth: 2D = sortOrder (authored), opaque 3D = view z ASC, transparent = z DESC
//
// Ties keep submission order (the sort is stable), so draw order is a function of scene state and
// camera only (FR-RUN-040) as long as callers submit in ECS iteration order.

#include "mce/memory/allocator.h"
#include "mce/types.h"

namespace mce::render {

enum class PassId : u8 { Shadow = 0, Opaque = 1, Sky = 2, Transparent = 3, World2D = 4, Screen2D = 5, Debug = 6 };

// Signed values are biased so that ascending unsigned order matches ascending signed order.
constexpr u64 sort_key_2d(PassId pass, i16 layer, u16 material, i32 order) {
    constexpr i32 kMaxOrder = (1 << 23) - 1;
    const i32 clamped = order < -kMaxOrder - 1 ? -kMaxOrder - 1 : (order > kMaxOrder ? kMaxOrder : order);
    const u64 layer_bits = static_cast<u16>(static_cast<i32>(layer) + 0x8000);
    const u64 order_bits = static_cast<u32>(clamped + (1 << 23)) & 0xFFFFFFu;
    return (u64(static_cast<u8>(pass)) << 56) | (layer_bits << 40) | (u64(material) << 24) | order_bits;
}

// 3D: [39:24] material (texture key / state bits), [23:00] quantized view depth. Opaque passes
// pass the depth as is (front-to-back); transparent passes invert it (back-to-front).
constexpr u64 sort_key_3d(PassId pass, u16 material, u32 depth24) {
    return (u64(static_cast<u8>(pass)) << 56) | (u64(material) << 24) | (u64(depth24) & 0xFFFFFFu);
}

constexpr PassId sort_key_pass(u64 key) { return static_cast<PassId>(key >> 56); }
constexpr i16 sort_key_layer(u64 key) { return static_cast<i16>(static_cast<i32>((key >> 40) & 0xFFFF) - 0x8000); }
constexpr u16 sort_key_material(u64 key) { return static_cast<u16>((key >> 24) & 0xFFFF); }
constexpr i32 sort_key_order(u64 key) { return static_cast<i32>(key & 0xFFFFFF) - (1 << 23); }

// Stable LSD radix sort of `keys` (8-bit digits). `values` is permuted alongside (typically item
// indices). Digit passes whose byte is constant across all keys are skipped, so 2D frames usually
// pay 3-4 passes of the 8. Scratch comes from `scratch` (the frame arena); no heap traffic.
void radix_sort(Span<u64> keys, Span<u32> values, mem::IAllocator& scratch);

}  // namespace mce::render
