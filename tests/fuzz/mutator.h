#pragma once
// M-FUZZ: a deterministic byte mutator for the fuzz tests. Everything is a pure function of the seed, so a
// failure is replayed by its (seed, iteration) alone. The output lives in 8-byte aligned storage because the
// readers cast payload bytes to structs, as they do for a real 16-byte aligned chunk.

#include <cstdint>
#include <cstring>
#include <vector>

#include "mce/containers/array.h"
#include "mce/types.h"

namespace mce::fuzz {

class Mutator {
public:
    explicit Mutator(u64 seed) : state_(seed) {}

    u64 next() {
        state_ += 0x9E3779B97F4A7C15ull;
        u64 z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    u64 below(u64 n) { return n ? next() % n : 0; }

    // `out` becomes a mutated copy of `in`: 1-4 damages from flips, field values at the edges of their range
    // (the length and count fields a reader trusts), zeroed or duplicated ranges, swapped words, and a
    // truncation about one time in six. Returns the mutant's size in bytes.
    usize mutate(Span<const byte> in, std::vector<std::uint64_t>& out) {
        std::vector<byte> b(in.begin(), in.end());
        const u32 damages = 1 + static_cast<u32>(below(4));
        for (u32 d = 0; d < damages && !b.empty(); ++d) {
            switch (below(6)) {
                case 0: b[below(b.size())] = static_cast<byte>(next()); break;
                case 1: b[below(b.size())] ^= static_cast<byte>(1u << below(8)); break;
                case 2: put_u32(b, below(b.size()), edge32()); break;
                case 3: {
                    const usize at = below(b.size()), n = 1 + below(16);
                    for (usize i = at; i < b.size() && i < at + n; ++i) b[i] = byte{0};
                    break;
                }
                case 4: {
                    const usize at = below(b.size()), n = 1 + below(32);
                    const usize to = below(b.size());
                    for (usize i = 0; i < n && at + i < b.size() && to + i < b.size(); ++i) b[to + i] = b[at + i];
                    break;
                }
                default: {
                    const usize x = below(b.size()), y = below(b.size());
                    std::swap(b[x], b[y]);
                    break;
                }
            }
        }
        if (!b.empty() && below(6) == 0) b.resize(below(b.size()));
        out.assign((b.size() + 7) / 8 + 1, 0);
        if (!b.empty()) std::memcpy(out.data(), b.data(), b.size());
        return b.size();
    }

private:
    u32 edge32() {
        static constexpr u32 kEdges[] = {0u, 1u, 2u, 0x7Fu, 0x80u, 0xFFu, 0x100u, 0x7FFFu, 0x8000u, 0xFFFFu,
                                         0x10000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu};
        return kEdges[below(sizeof(kEdges) / sizeof(kEdges[0]))];
    }
    static void put_u32(std::vector<byte>& b, usize at, u32 v) {
        for (usize i = 0; i < 4 && at + i < b.size(); ++i) b[at + i] = static_cast<byte>(v >> (8 * i));
    }

    u64 state_;
};

}  // namespace mce::fuzz
