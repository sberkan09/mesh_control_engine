// EdgeOutline3D (M-OUTLINE 39d): the component defaults match the render setting and the pad is zero.

#include <doctest/doctest.h>

#include <bit>

#include "mce/kit/edge_outline.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

}  // namespace

TEST_CASE("edge outline: component defaults match the render setting") {
    const EdgeOutline3D c;
    const render::EdgeOutline d;
    CHECK(c.enabled == 1u);
    CHECK(same_bits(c.color_r, d.color.x));
    CHECK(same_bits(c.color_g, d.color.y));
    CHECK(same_bits(c.color_b, d.color.z));
    CHECK(same_bits(c.opacity, d.opacity));
    CHECK(same_bits(c.thickness, d.thickness));
    CHECK(same_bits(c.depth_threshold, d.depth_threshold));
    CHECK(same_bits(c.normal_threshold, d.normal_threshold));
    CHECK(same_bits(c.depth_weight, d.depth_weight));
    CHECK(same_bits(c.normal_weight, d.normal_weight));
    CHECK(same_bits(c.fade_start, d.fade_start));
    CHECK(same_bits(c.fade_end, d.fade_end));
    CHECK((c.sky_silhouette != 0u) == d.sky_silhouette);
    CHECK((c.fog_follow != 0u) == d.fog_follow);
    CHECK(c.sky_silhouette == 1u);
    CHECK(c.fog_follow == 1u);
}

TEST_CASE("edge outline: the explicit pad is zero and the layout has no hidden bytes") {
    const EdgeOutline3D c;
    CHECK(c.pad_[0] == 0u);
    CHECK(sizeof(EdgeOutline3D) == 11 * sizeof(f32) + 4);
}
