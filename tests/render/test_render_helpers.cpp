// Pure render helpers no GPU is needed for: the 3D sort key's field layout and ordering, the sRGB colour
// packing, and the flipbook clip's step and frame arithmetic.

#include <doctest/doctest.h>

#include "mce/math/math.h"
#include "mce/render/sort_key.h"
#include "mce/render/sprite_animation.h"
#include "mce/render/types.h"

using namespace mce;
using namespace mce::render;

TEST_CASE("sort key: sort_key_3d packs pass, material and 24-bit depth, and orders by them in that order") {
    const u64 k = sort_key_3d(PassId::Transparent, 0xABCD, 0x123456);
    CHECK((k >> 56) == u64(PassId::Transparent));
    CHECK(((k >> 24) & 0xFFFFu) == 0xABCDu);
    CHECK((k & 0xFFFFFFu) == 0x123456u);
    CHECK(sort_key_3d(PassId::Opaque, 0, 0) == (u64(1) << 56));
    CHECK(sort_key_3d(PassId::Opaque, 0, 0x1FFFFFFu) ==
          sort_key_3d(PassId::Opaque, 0, 0xFFFFFFu));  // depth is masked to 24 bits

    // The pass sorts before the material, the material before the depth.
    CHECK(sort_key_3d(PassId::Opaque, 0xFFFF, 0xFFFFFF) < sort_key_3d(PassId::Sky, 0, 0));
    CHECK(sort_key_3d(PassId::Opaque, 1, 0xFFFFFF) < sort_key_3d(PassId::Opaque, 2, 0));
    CHECK(sort_key_3d(PassId::Opaque, 2, 5) < sort_key_3d(PassId::Opaque, 2, 6));
}

TEST_CASE("types: pack_color_srgb keeps white, encodes rgb as sRGB, keeps alpha linear and clamps") {
    CHECK(pack_color_srgb(math::Color::white()) == 0xFFFFFFFFu);
    CHECK(pack_color_srgb(math::Color::black()) == 0xFF000000u);
    CHECK(pack_color_srgb({0, 0, 0, 0}) == 0u);
    CHECK(pack_color_srgb({1, 0, 0, 1}) == 0xFF0000FFu);  // r is the low byte
    CHECK(pack_color_srgb({0, 1, 0, 1}) == 0xFF00FF00u);
    CHECK(pack_color_srgb({0, 0, 1, 1}) == 0xFFFF0000u);

    // Linear 0.5 is about 188 in sRGB; alpha 0.5 stays 128 (rounded), not encoded.
    const u32 mid = pack_color_srgb({0.5f, 0.5f, 0.5f, 0.5f});
    const u32 r = mid & 0xFFu;
    CHECK(r >= 186u);
    CHECK(r <= 190u);
    CHECK(((mid >> 8) & 0xFFu) == r);
    CHECK(((mid >> 16) & 0xFFu) == r);
    CHECK((mid >> 24) == 128u);

    // Out-of-range channels clamp instead of wrapping.
    CHECK(pack_color_srgb({2.0f, -1.0f, 0.0f, 3.0f}) == 0xFF0000FFu);
}

TEST_CASE("sprite animation: step and frame arithmetic for loop, ping-pong and once") {
    const u32 regions[] = {10, 11, 12, 13};
    SpriteClip clip;
    clip.frames = Span<const u32>(regions, 4);
    clip.fps = 10.0f;

    // Steps: whole frames elapsed, with a small epsilon; none for no time or an empty / zero-rate clip.
    CHECK(render::detail::clip_steps_at(clip, 0.0) == 0);
    CHECK(render::detail::clip_steps_at(clip, -1.0) == 0);
    CHECK(render::detail::clip_steps_at(clip, 0.25) == 2);
    CHECK(render::detail::clip_steps_at(clip, 0.3) ==
          3);  // 0.3 * 10 is 2.9999999.. in floating point: the epsilon rounds it up
    SpriteClip empty;
    CHECK(render::detail::clip_steps_at(empty, 5.0) == 0);
    SpriteClip frozen = clip;
    frozen.fps = 0.0f;
    CHECK(render::detail::clip_steps_at(frozen, 5.0) == 0);

    // Loop wraps; ping-pong goes 0 1 2 3 2 1 0 1 ...
    clip.mode = PlayMode::Loop;
    CHECK(render::detail::clip_frame_for_step(clip, 3) == 3);
    CHECK(render::detail::clip_frame_for_step(clip, 4) == 0);
    CHECK(render::detail::clip_frame_for_step(clip, 9) == 1);
    clip.mode = PlayMode::PingPong;
    const u32 want[] = {0, 1, 2, 3, 2, 1, 0, 1, 2, 3};
    for (u64 s = 0; s < 10; ++s) CHECK(render::detail::clip_frame_for_step(clip, s) == want[s]);

    // Once stops on the last frame, both in steps and in frames.
    clip.mode = PlayMode::Once;
    CHECK(render::detail::clip_steps_at(clip, 100.0) == 3);
    CHECK(render::detail::clip_frame_for_step(clip, 2) == 2);
    CHECK(render::detail::clip_frame_for_step(clip, 50) == 3);

    // One frame: always frame 0.
    const u32 one[] = {7};
    SpriteClip single;
    single.frames = Span<const u32>(one, 1);
    single.mode = PlayMode::PingPong;
    CHECK(render::detail::clip_frame_for_step(single, 5) == 0);
}
