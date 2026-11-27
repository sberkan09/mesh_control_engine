// M-VISUAL4 5 against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: window interior mapping in
// the lit mesh shader (MCE_GLSL_INTERIOR, ObjectU::o_retro.y from the material's flags bits 6..7). A glossy (glass) wall
// with the flag shows a room behind it, so the same window pixel changes as the camera swings round; without the flag
// it barely does (only the glass's Fresnel moves). The flag on a material with no glossy texel, and the flag set then
// cleared, draw the plain frame byte for byte.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>

#include "../render/mesh_fixture.h"
#include "fixture3d.h"
#include "mce/asset/material_interior.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

bool same(const Array<u8>& a, const Array<u8>& b) { return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin()); }

int diff(Rgb a, Rgb b) { return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b); }

// Mesh 1 again (generation `gen`): a 3 m x 2 m wall facing +z (its front face spans the texture once), dark glass with a
// warm emission (a lit window), `cells` window cells per repeat (0 = off), roughness `rough`.
void upload_window_wall(Fixture3D& f, u32 gen, u32 cells, f32 rough) {
    mesh_fixture::Builder wall;
    wall.add_node(-1);
    mesh_fixture::Box b;
    b.center = {0, 1.0f, 0};
    b.half = {1.5f, 1.0f, 0.1f};
    b.color[0] = 0.06f;
    b.color[1] = 0.08f;
    b.color[2] = 0.10f;
    b.uvs = true;
    asset::MaterialChunk m{};
    asset::set_material_interior_cells(m, cells);
    b.flags = m.flags;
    wall.add_box(b);
    wall.submeshes.back().material.roughness = rough;
    wall.submeshes.back().material.metallic = 0.0f;
    wall.submeshes.back().material.emissive[0] = 1.0f;
    wall.submeshes.back().material.emissive[1] = 0.7f;
    wall.submeshes.back().material.emissive[2] = 0.4f;
    REQUIRE(f.renderer.meshes().upload(1, gen, wall.build().span()).has_value());
}

void dusk(Scene3D& s) {  // no sun, a dim even sky: the window's own light dominates
    no_sun(s);
    s.set_sky(constant_sky(0.08f, false));
}

}  // namespace

TEST_CASE("render3d_gl: window interiors (M-VISUAL4 5): a flagged glass wall shows a room that moves with the view; off draws the plain frame") {
    CHECK(asset::valid_interior_cells(4));
    CHECK_FALSE(asset::valid_interior_cells(3));
    {
        asset::MaterialChunk m{};
        m.flags = 0xFFFFFF3Fu;  // every other bit set: the setter keeps them
        asset::set_material_interior_cells(m, 4);
        CHECK(asset::material_interior_cells(m) == 4);
        CHECK((m.flags | asset::MaterialInterior::Mask) == 0xFFFFFFFFu);
        asset::set_material_interior_cells(m, 3);  // invalid: off
        CHECK(asset::material_interior_cells(m) == 0);
        CHECK(m.flags == 0xFFFFFF3Fu);
    }

    Fixture3D f;
    const vec3 probe = {0.0f, 1.0f, 0.1f};  // the wall's front face, the middle of its one window cell
    u32 gen = 2;
    const auto shot = [&](u32 cells, f32 rough, f32 yaw) {
        upload_window_wall(f, gen++, cells, rough);
        f.yaw_deg = yaw;
        f.render(0.0f, false, dusk);
        return f.at_world(probe);
    };

    const Rgb off_a = shot(0, 0.05f, 0.0f);
    const Rgb off_b = shot(0, 0.05f, 40.0f);
    const Rgb on_a = shot(1, 0.05f, 0.0f);
    const Rgb on_b = shot(1, 0.05f, 40.0f);
    MESSAGE("off: " << off_a.r << "," << off_a.g << "," << off_a.b << " -> " << off_b.r << "," << off_b.g << "," << off_b.b);
    MESSAGE("on:  " << on_a.r << "," << on_a.g << "," << on_a.b << " -> " << on_b.r << "," << on_b.g << "," << on_b.b);
    CHECK(diff(off_a, off_b) <= 6);    // the flat window: the same from both sides
    CHECK(diff(on_a, on_b) >= 20);     // the room: the floor and back wall head-on, a side wall from the side
    CHECK(diff(on_a, off_a) >= 10);    // the room's shading replaces the flat emission

    // Byte for byte: the flag on a wall with no glossy texel (no glass), and the flag cleared again.
    f.yaw_deg = 0.0f;
    upload_window_wall(f, gen++, 0, 0.8f);
    f.render(0.0f, false, dusk);
    const Array<u8> plain = f.pixels;
    upload_window_wall(f, gen++, 4, 0.8f);
    f.render(0.0f, false, dusk);
    CHECK(same(plain, f.pixels));
    upload_window_wall(f, gen++, 0, 0.05f);
    f.render(0.0f, false, dusk);
    const Array<u8> glass_plain = f.pixels;
    upload_window_wall(f, gen++, 1, 0.05f);
    f.render(0.0f, false, dusk);
    CHECK_FALSE(same(glass_plain, f.pixels));
    upload_window_wall(f, gen++, 0, 0.05f);
    f.render(0.0f, false, dusk);
    CHECK(same(glass_plain, f.pixels));
    CHECK(f.device->validation_error_count() == 0);
}
