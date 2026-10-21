#include "mce/render/types.h"

#include <cmath>

namespace mce::render {

namespace {

// Linear [0, 1] quantized to 4096 steps -> sRGB u8. Exact to the u8 except in the darkest few
// steps, where 1/4096 linear is finer than the sRGB curve's resolution anyway.
struct SrgbLut {
    static constexpr u32 kSize = 4096;
    u8 table[kSize + 1];
    SrgbLut() {
        for (u32 i = 0; i <= kSize; ++i) {
            const f32 c = static_cast<f32>(i) / static_cast<f32>(kSize);
            const f32 s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
            table[i] = static_cast<u8>(std::lround(s * 255.0f));
        }
    }
};
const SrgbLut kLut;

u32 encode_channel(f32 linear) {
    const f32 c = math::clamp(linear, 0.0f, 1.0f);
    return kLut.table[static_cast<u32>(std::lround(c * static_cast<f32>(SrgbLut::kSize)))];
}

}  // namespace

u32 pack_orientation(f32 rotation, bool flip_x, bool flip_y) {
    constexpr f32 kTurn = 6.28318530718f;
    f32 turns = rotation / kTurn;
    turns -= std::floor(turns);  // [0, 1)
    const u32 q = static_cast<u32>(std::lround(turns * 16384.0f)) & 0x3FFFu;
    return q | (flip_x ? 1u << 14 : 0u) | (flip_y ? 1u << 15 : 0u);
}

u32 pack_color_srgb(math::Color c) {
    if (c == math::Color::white()) return 0xFFFFFFFFu;  // the common case: skip three pow()s
    const u32 a = static_cast<u32>(std::lround(math::clamp(c.a, 0.0f, 1.0f) * 255.0f));
    return encode_channel(c.r) | (encode_channel(c.g) << 8) | (encode_channel(c.b) << 16) | (a << 24);
}

}  // namespace mce::render
