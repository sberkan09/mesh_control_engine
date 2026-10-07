#include "mce/render/types.h"

#include <cmath>

namespace mce::render {

namespace {

u32 encode_channel(f32 linear) {
    const f32 c = math::clamp(linear, 0.0f, 1.0f);
    const f32 s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<u32>(s * 255.0f + 0.5f);
}

}  // namespace

u32 pack_color_srgb(math::Color c) {
    const u32 a = static_cast<u32>(math::clamp(c.a, 0.0f, 1.0f) * 255.0f + 0.5f);
    return encode_channel(c.r) | (encode_channel(c.g) << 8) | (encode_channel(c.b) << 16) | (a << 24);
}

}  // namespace mce::render
