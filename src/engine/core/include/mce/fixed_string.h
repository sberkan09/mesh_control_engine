#pragma once
// FixedString<N>: inline, length-prefixed, always NUL-terminated. No heap (core.md §3.2).

#include <cstring>

#include "mce/types.h"

namespace mce {

template <usize N>
class FixedString {
    static_assert(N >= 2 && N <= 65536, "FixedString capacity out of range");

public:
    constexpr FixedString() = default;
    FixedString(StringView s) { assign(s); }  // NOLINT(google-explicit-constructor)
    FixedString(const char* s) { assign(s ? StringView(s) : StringView()); }  // NOLINT

    // Truncates silently to capacity(); truncated() reports it.
    void assign(StringView s) {
        const usize n = s.size() < capacity() ? s.size() : capacity();
        if (n) std::memcpy(buf_, s.data(), n);
        len_ = static_cast<u32>(n);
        buf_[n] = '\0';
        truncated_ = n < s.size() ? 1u : 0u;
    }

    void append(StringView s) {
        const usize room = capacity() - len_;
        const usize n = s.size() < room ? s.size() : room;
        if (n) std::memcpy(buf_ + len_, s.data(), n);
        len_ += static_cast<u32>(n);
        buf_[len_] = '\0';
        truncated_ = truncated_ != 0 || n < s.size() ? 1u : 0u;
    }

    void clear() {
        len_ = 0;
        buf_[0] = '\0';
        truncated_ = 0;
    }

    static constexpr usize capacity() { return N - 1; }
    usize size() const { return len_; }
    bool empty() const { return len_ == 0; }
    bool truncated() const { return truncated_ != 0; }
    const char* c_str() const { return buf_; }
    StringView view() const { return StringView(buf_, len_); }
    operator StringView() const { return view(); }  // NOLINT(google-explicit-constructor)

    friend bool operator==(const FixedString& a, StringView b) { return a.view() == b; }

private:
    // No padding bytes (the buffer rounds up to 4, the flag is a u32): components holding a FixedString
    // are hashed and saved as raw bytes, so every byte must be written. The size is what N, u32 and bool
    // took before.
    char buf_[(N + 3) / 4 * 4] = {};
    u32 len_ = 0;
    u32 truncated_ = 0;
};

}  // namespace mce
