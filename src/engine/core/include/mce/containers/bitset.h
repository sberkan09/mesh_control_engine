#pragma once
// BitSet<N>: fixed-size bit array over 64-bit words.

#include <bit>

#include "mce/assert.h"
#include "mce/types.h"

namespace mce {

template <usize N>
class BitSet {
public:
    static constexpr usize kWords = (N + 63) / 64;

    constexpr void set(usize i) { word(i) |= bit(i); }
    constexpr void reset(usize i) { word(i) &= ~bit(i); }
    constexpr void assign(usize i, bool v) { v ? set(i) : reset(i); }
    constexpr bool test(usize i) const {
        MCE_ASSERT(i < N);
        return (words_[i / 64] & bit(i)) != 0;
    }
    constexpr void clear() {
        for (u64& w : words_) w = 0;
    }
    constexpr usize count() const {
        usize c = 0;
        for (u64 w : words_) c += static_cast<usize>(std::popcount(w));
        return c;
    }
    constexpr bool any() const {
        for (u64 w : words_)
            if (w) return true;
        return false;
    }
    constexpr bool none() const { return !any(); }
    // True when every bit set in `o` is set here.
    constexpr bool contains_all(const BitSet& o) const {
        for (usize i = 0; i < kWords; ++i)
            if ((words_[i] & o.words_[i]) != o.words_[i]) return false;
        return true;
    }
    constexpr bool intersects(const BitSet& o) const {
        for (usize i = 0; i < kWords; ++i)
            if (words_[i] & o.words_[i]) return true;
        return false;
    }
    friend constexpr bool operator==(const BitSet& a, const BitSet& b) {
        for (usize i = 0; i < kWords; ++i)
            if (a.words_[i] != b.words_[i]) return false;
        return true;
    }
    static constexpr usize size() { return N; }
    constexpr u64 word_at(usize w) const { return words_[w]; }

private:
    static constexpr u64 bit(usize i) { return u64(1) << (i % 64); }
    constexpr u64& word(usize i) {
        MCE_ASSERT(i < N);
        return words_[i / 64];
    }
    u64 words_[kWords] = {};
};

}  // namespace mce
