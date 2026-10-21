#pragma once
// StringId: 64-bit FNV-1a name hash for all named lookups (convars, events, asset aliases).
// Constructible at compile time; `MCE_SID("name")` forces it.

#include "mce/hash.h"

namespace mce {

struct StringId {
    u64 value = 0;

    constexpr StringId() = default;
    constexpr explicit StringId(u64 v) : value(v) {}
    constexpr StringId(StringView s) : value(fnv1a64(s)) {}   // NOLINT(google-explicit-constructor)
    constexpr StringId(const char* s) : value(fnv1a64(StringView(s))) {}  // NOLINT

    constexpr bool valid() const { return value != 0; }
    friend constexpr bool operator==(StringId a, StringId b) { return a.value == b.value; }
    friend constexpr auto operator<=>(StringId a, StringId b) { return a.value <=> b.value; }
};

template <>
struct Hash<StringId> {
    u64 operator()(StringId id) const { return mix64(id.value); }
};

namespace detail {
consteval StringId make_sid(const char* s) { return StringId(StringView(s)); }
}  // namespace detail

}  // namespace mce

#define MCE_SID(str) (::mce::detail::make_sid(str))
