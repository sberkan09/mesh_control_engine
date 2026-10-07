#pragma once
// Fundamental types and compiler helpers for every MCE module (L0).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace mce {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;
using isize = std::ptrdiff_t;
using byte = std::byte;

template <class T>
using Span = std::span<T>;
using StringView = std::string_view;

inline constexpr usize KiB = 1024;
inline constexpr usize MiB = 1024 * KiB;

// One engine-wide frame-slot ring depth (ARCHITECTURE fix F1 / ADR-0005).
inline constexpr u32 MCE_FRAMES_IN_FLIGHT = 3;

constexpr bool is_pow2(usize v) { return v != 0 && (v & (v - 1)) == 0; }

constexpr usize align_up(usize v, usize align) { return (v + (align - 1)) & ~(align - 1); }

constexpr usize next_pow2(usize v) {
    if (v <= 1) return 1;
    --v;
    for (usize shift = 1; shift < sizeof(usize) * 8; shift <<= 1) v |= v >> shift;
    return v + 1;
}

}  // namespace mce

#if defined(_MSC_VER)
#define MCE_FORCEINLINE __forceinline
#define MCE_NOINLINE __declspec(noinline)
#else
#define MCE_FORCEINLINE inline __attribute__((always_inline))
#define MCE_NOINLINE __attribute__((noinline))
#endif

#define MCE_CONCAT_IMPL(a, b) a##b
#define MCE_CONCAT(a, b) MCE_CONCAT_IMPL(a, b)
