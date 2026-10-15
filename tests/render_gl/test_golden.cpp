// Golden images of the 3D frame (FR-DBG-020; golden.h has the threshold and the rebaseline rules).
// Dev-machine-only: not registered with ctest; scripts/golden.sh runs it three times per backend.
// One case per render feature, added with the feature's M-VISUAL step.

#include <doctest/doctest.h>

#include "fixture3d.h"
#include "golden.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

void check_golden(const char* name, const Fixture3D& f) {
    CHECK(f.device->validation_error_count() == 0);
    golden::check(name, kW, kH, f.pixels.span());
}

Sky gradient_sky() {
    Sky sky;
    sky.zenith = {0.15f, 0.3f, 0.9f};
    sky.horizon = {0.7f, 0.6f, 0.5f};
    sky.ground = {0.35f, 0.2f, 0.1f};
    sky.ibl = true;
    return sky;
}

}  // namespace

// ---- M-VISUAL 2: image-based lighting + reflection probes --------------------------------------

TEST_CASE("golden: the frame without image-based lighting") {
    Fixture3D f(4, true, false);
    f.render();
    check_golden("ibl_off", f);
}

TEST_CASE("golden: a gradient sky alone lights the box and the ground through IBL") {
    Fixture3D f;
    f.render(0.0f, false, [](Scene3D& s) {
        no_sun(s);
        s.set_sky(gradient_sky());
    });
    check_golden("ibl_sky", f);
}

TEST_CASE("golden: metal boxes from smooth to rough reflect the pre-filtered sky") {
    Fixture3D f;
    f.pitch_deg = 15.0f;
    constexpr u32 kBoxes = 4;
    for (u32 i = 0; i < kBoxes; ++i) {
        mesh_fixture::Builder b;
        b.add_node(-1);
        mesh_fixture::Box box;
        box.center = {-2.4f + 1.6f * f32(i), 0.5f, 1.0f};
        box.half = {0.5f, 0.5f, 0.5f};
        box.color[0] = box.color[1] = box.color[2] = 0.95f;
        b.add_box(box);
        b.submeshes.back().material.metallic = 1.0f;
        b.submeshes.back().material.roughness = 0.05f + 0.3f * f32(i);
        REQUIRE(f.renderer.meshes().upload(20 + i, 1, b.build().span()).has_value());
    }
    f.render(0.0f, true, [&](Scene3D& s) {
        s.set_sky(gradient_sky());
        for (u32 i = 0; i < kBoxes; ++i) {
            MeshInstance m;
            m.mesh = 20 + i;
            s.add(m);
        }
    });
    check_golden("ibl_roughness", f);
}

TEST_CASE("golden: an IBL texture lights the scene while the dome keeps the visible sky") {
    Fixture3D f;
    f.pitch_deg = 20.0f;
    // A 4x2 equirect: warm above the horizon, dark below.
    Array<u8> px;
    for (u32 i = 0; i < 8; ++i) {
        const bool up = i < 4;
        px.push_back(up ? 255 : 40);
        px.push_back(up ? 140 : 30);
        px.push_back(up ? 60 : 30);
        px.push_back(255);
    }
    const TextureMipUpload mip{4, 2, std::as_bytes(px.span())};
    TextureUpload up;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    const u32 tex = f.renderer.textures().allocate_key();
    REQUIRE(f.renderer.textures().upload(tex, 1, up).has_value());
    f.render(0.0f, false, [&](Scene3D& s) {
        no_sun(s);
        Sky sky = gradient_sky();
        sky.ibl_texture = tex;
        s.set_sky(sky);
    });
    check_golden("ibl_texture", f);
}

TEST_CASE("golden: a reflection probe mirrors the room in the metal floor") {
    Fixture3D f;
    upload_probe_room(f);
    f.render(0.0f, false, [](Scene3D& s) {
        no_sun(s);
        s.set_sky(constant_sky(0.05f, true));
        MeshInstance m;
        m.mesh = 7;
        s.add(m);
        m.mesh = 8;
        s.add(m);
        s.add_reflection_probe(room_probe());
    });
    CHECK(f.renderer.forward_3d().last_stats().probes_captured == 1);
    check_golden("probe_room", f);
}

// ---- M-VISUAL 3: screen-space ambient occlusion ----------------------------------------------

TEST_CASE("golden: SSAO alone: the box's crease under a flat grey ambient") {
    Fixture3D f;
    f.render(0.0f, false, [](Scene3D& s) {
        no_sun(s);
        DirectionalLight sun = s.light();
        sun.ambient_sky = sun.ambient_ground = {0.6f, 0.6f, 0.6f};
        s.set_light(sun);
        Sky sky;
        sky.specular_ambient = 0.0f;
        s.set_sky(sky);
        AmbientOcclusion ao;
        ao.enabled = true;
        s.set_ambient_occlusion(ao);
    });
    CHECK(f.renderer.forward_3d().last_stats().ssao);
    check_golden("ssao_crease", f);
}

TEST_CASE("golden: SSAO over the sun, IBL and a row of boxes") {
    Fixture3D f;
    f.pitch_deg = 25.0f;
    for (u32 i = 0; i < 4; ++i) {
        mesh_fixture::Builder b;
        b.add_node(-1);
        mesh_fixture::Box box;
        box.center = {-2.4f + 1.6f * f32(i), 0.4f + 0.2f * f32(i), 1.2f};
        box.half = {0.4f, 0.4f + 0.2f * f32(i), 0.4f};
        box.color[0] = 0.8f;
        box.color[1] = 0.75f;
        box.color[2] = 0.7f;
        b.add_box(box);
        REQUIRE(f.renderer.meshes().upload(30 + i, 1, b.build().span()).has_value());
    }
    f.render(0.0f, true, [](Scene3D& s) {
        s.set_sky(gradient_sky());
        for (u32 i = 0; i < 4; ++i) {
            MeshInstance m;
            m.mesh = 30 + i;
            s.add(m);
        }
        AmbientOcclusion ao;
        ao.enabled = true;
        s.set_ambient_occlusion(ao);
    });
    check_golden("ssao_scene", f);
}

TEST_CASE("golden: a spot light and a point light shadow a row of boxes at night") {
    Fixture3D f;
    f.pitch_deg = 30.0f;
    for (u32 i = 0; i < 3; ++i) {
        mesh_fixture::Builder b;
        b.add_node(-1);
        mesh_fixture::Box box;
        box.center = {-2.2f + 2.2f * f32(i), 0.35f + 0.15f * f32(i), 1.4f};
        box.half = {0.35f, 0.35f + 0.15f * f32(i), 0.35f};
        box.color[0] = 0.8f;
        box.color[1] = 0.8f;
        box.color[2] = 0.75f;
        b.add_box(box);
        REQUIRE(f.renderer.meshes().upload(40 + i, 1, b.build().span()).has_value());
    }
    f.render(0.0f, false, [](Scene3D& s) {
        no_sun(s);
        DirectionalLight sun = s.light();
        sun.ambient_sky = sun.ambient_ground = {0.03f, 0.03f, 0.05f};
        s.set_light(sun);
        Sky sky;
        sky.enabled = false;
        sky.specular_ambient = 0.0f;
        s.set_sky(sky);
        Fog fog;
        fog.color = {0.01f, 0.01f, 0.02f};
        s.set_fog(fog);
        LocalShadowSettings ls;
        ls.tile = 256;
        s.set_local_shadows(ls);
        for (u32 i = 0; i < 3; ++i) {
            MeshInstance m;
            m.mesh = 40 + i;
            s.add(m);
        }
        PointLight spot;  // warm, from the back left
        spot.position = {-3.5f, 3.5f, -1.5f};
        spot.direction = {1.0f, -1.1f, 0.6f};
        spot.radius = 14.0f;
        spot.intensity = 40.0f;
        spot.color = {1.0f, 0.75f, 0.45f};
        spot.spot_inner = math::radians(25.0f);
        spot.spot_outer = math::radians(38.0f);
        spot.cast_shadows = true;
        s.add_point_light(spot);
        PointLight lamp;  // cool, low between the boxes
        lamp.position = {1.1f, 0.9f, 2.4f};
        lamp.radius = 7.0f;
        lamp.intensity = 6.0f;
        lamp.color = {0.5f, 0.7f, 1.0f};
        lamp.cast_shadows = true;
        s.add_point_light(lamp);
    });
    CHECK(f.renderer.forward_3d().last_stats().local_shadow_tiles == 7);
    check_golden("local_shadows", f);
}
