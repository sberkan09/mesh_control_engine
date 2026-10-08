#pragma once
// Guid: 128-bit asset identity (assets.md §4.1). UUIDv4, rendered as canonical hyphenated hex
// ("0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0"). Stable across moves/renames; created once by the
// sidecar (cook/editor side), never at runtime.

#include <compare>

#include "mce/fixed_string.h"
#include "mce/hash.h"
#include "mce/result.h"

namespace mce::asset {

struct Guid {
    u64 hi = 0;  // first 8 bytes of the canonical text form, big-endian
    u64 lo = 0;

    constexpr bool is_null() const { return hi == 0 && lo == 0; }
    friend constexpr bool operator==(Guid a, Guid b) { return a.hi == b.hi && a.lo == b.lo; }
    friend constexpr std::strong_ordering operator<=>(Guid a, Guid b) {
        if (auto c = a.hi <=> b.hi; c != 0) return c;
        return a.lo <=> b.lo;
    }
};

using GuidString = FixedString<40>;  // 36 chars + NUL, rounded

// Canonical lower-case hyphenated form.
GuidString to_string(Guid g);
// 32 lower-case hex digits without hyphens (cooked/ directory names).
FixedString<40> to_hex(Guid g);

// Accepts the canonical form (either case). Anything else is Errc::ParseError.
Result<Guid> parse_guid(StringView text);

}  // namespace mce::asset

template <>
struct mce::Hash<mce::asset::Guid> {
    u64 operator()(mce::asset::Guid g) const { return mce::hash_combine(mce::mix64(g.hi), g.lo); }
};
