#pragma once
// Generated images and the pinned encoder hash shared by test_cook_bc.cpp and test_cook_bc_arch.cpp (M-CONTENT 1b-ii).
//
// kPinnedBcHash is FNV-1a 64 over the BC7 (sRGB), BC7 (linear weights), BC5, BC4 and BC1 chains of a generated 64x64 image, every level down
// to 1x1 (the small ones are padded blocks). It was recorded on x86_64 (gcc, -ffp-contract=off). Changing an encoder
// setting in texture_bc.cpp, or the pinned bc7enc_rdo commit, changes it: re-record it together with a bump of the texture
// importer's version(). An arm64 host must produce the same value (test_cook_bc_arch, a slow test); if it does not, cook
// goldens stay x86_64-only and the cache must not be shared across architectures. Note the hash covers the encoder only:
// the cook's sRGB mip LUT uses std::pow, so equal encoder hashes across hosts do not prove whole packs are equal.

#include <algorithm>
#include <vector>

#include "texture_bc.h"

namespace bc_pinned {

using namespace mce;

// A w x h gradient with a little deterministic noise (an LCG, no <random>): every channel varies, alpha is 255.
inline std::vector<u8> gradient_noise(u32 w, u32 h) {
    std::vector<u8> px(usize(w) * h * 4);
    u32 s = 12345u;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            s = s * 1664525u + 1013904223u;
            const int n = int((s >> 24) & 15u) - 8;
            auto cl = [](int v) { return u8(v < 0 ? 0 : v > 255 ? 255 : v); };
            u8* p = &px[(usize(y) * w + x) * 4];
            p[0] = cl(int(x * 255 / (w - 1)) + n);
            p[1] = cl(int(y * 255 / (h - 1)) + n);
            p[2] = cl(int((x + y) * 255 / (w + h - 2)) + n);
            p[3] = 255;
        }
    return px;
}

// Box-halves an RGBA8 level (plain average; the test needs a chain, not the cook's exact filter).
inline std::vector<u8> halve(const std::vector<u8>& src, u32 w, u32 h) {
    const u32 nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    std::vector<u8> out(usize(nw) * nh * 4);
    for (u32 y = 0; y < nh; ++y)
        for (u32 x = 0; x < nw; ++x)
            for (u32 c = 0; c < 4; ++c) {
                u32 sum = 0;
                for (u32 dy = 0; dy < 2; ++dy)
                    for (u32 dx = 0; dx < 2; ++dx) {
                        const u32 sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, h - 1);
                        sum += src[(usize(sy) * w + sx) * 4 + c];
                    }
                out[(usize(y) * nw + x) * 4 + c] = u8((sum + 2) / 4);
            }
    return out;
}

inline u64 fnv1a(u64 h, const u8* d, usize n) {
    for (usize i = 0; i < n; ++i) h = (h ^ d[i]) * 1099511628211ull;
    return h;
}

inline u64 chain_hash() {
    u64 h = 1469598103934665603ull;
    struct Chain {
        cook::BcFormat f;
        bool srgb;
    };
    const Chain chains[] = {{cook::BcFormat::BC7, true}, {cook::BcFormat::BC7, false}, {cook::BcFormat::BC5, false},
                            {cook::BcFormat::BC4, false}, {cook::BcFormat::BC1, false}};
    for (const Chain& c : chains) {
        const cook::BcFormat f = c.f;
        u32 w = 64, ht = 64;
        std::vector<u8> level = gradient_noise(w, ht);
        for (;;) {
            std::vector<u8> out(usize(cook::bc_level_bytes(f, w, ht)));
            cook::encode_bc_level(f, c.srgb, level.data(), w, ht, out.data());
            h = fnv1a(h, out.data(), out.size());
            if (w == 1 && ht == 1) break;
            level = halve(level, w, ht);
            w = w > 1 ? w / 2 : 1;
            ht = ht > 1 ? ht / 2 : 1;
        }
    }
    return h;
}

constexpr u64 kPinnedBcHash = 11015943935995630614ull;

}  // namespace bc_pinned
