#pragma once
// Compile-time type names and hashes without RTTI.
//
// Identity is a hash of the compiler's pretty function name, so it is stable across DLL
// boundaries for one compiler (finding F11: no function-local statics in engine templates;
// type identity is a value, and registries map it to dense ids through non-template functions).

#include "mce/hash.h"

namespace mce {

template <class T>
constexpr StringView type_name() {
#if defined(_MSC_VER)
    constexpr StringView sig = __FUNCSIG__;
    constexpr StringView open = "type_name<";
    constexpr StringView close = ">(void)";
    const usize start = sig.find(open) + open.size();
    const usize end = sig.rfind(close);
    return sig.substr(start, end - start);
#else
    constexpr StringView sig = __PRETTY_FUNCTION__;
    constexpr StringView key = "T = ";
    const usize start = sig.find(key) + key.size();
    usize end = sig.find(';', start);
    if (end == StringView::npos) end = sig.rfind(']');
    return sig.substr(start, end - start);
#endif
}

template <class T>
inline constexpr u64 kTypeHash = fnv1a64(type_name<T>());

}  // namespace mce
