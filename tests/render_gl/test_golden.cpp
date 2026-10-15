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

namespace {
void taa_only(Scene3D& s) {
    PostFx post;
    post.fxaa = false;
    post.taa = true;
    s.set_post(post);
}
}  // namespace

TEST_CASE("golden: TAA after 16 still frames, no MSAA") {
    Fixture3D f(1);
    for (int i = 0; i < 16; ++i) f.render(0.0f, true, taa_only);
    CHECK(f.renderer.forward_3d().last_stats().taa_history);
    check_golden("taa_still", f);
}

TEST_CASE("golden: temporal upscaling of a half-scale frame after 24 frames") {
    Fixture3D f(1, true, true, 0.5f);
    for (int i = 0; i < 24; ++i) f.render(0.0f, true, taa_only);
    check_golden("taa_upscale", f);
}

// ---- M-VISUAL 6: global illumination (the irradiance volume) -----------------------------------

TEST_CASE("golden: a red emissive wall bleeds onto the ground through the irradiance volume") {
    Fixture3D f;
    upload_probe_room(f);
    for (int i = 0; i < 2; ++i) f.render(0.0f, true, [](Scene3D& s) { gi_wall_scene(s, true); });  // the fill
    CHECK(f.renderer.forward_3d().last_stats().gi);
    check_golden("gi_bleed", f);
}

TEST_CASE("golden: sunlight bounced off the ground lights the box's unlit face") {
    Fixture3D f;
    const auto scene = [](Scene3D& s) {
        DirectionalLight sun;
        sun.direction = {0.3f, -1.0f, 0.9f};
        sun.intensity = 3.0f;
        sun.ambient_sky = sun.ambient_ground = {0, 0, 0};
        s.set_light(sun);
        Sky sky;
        sky.enabled = false;
        sky.specular_ambient = 0.0f;
        s.set_sky(sky);
        Fog fog;
        fog.color = {0, 0, 0};
        s.set_fog(fog);
        s.set_gi_volume(room_gi());
    };
    for (int i = 0; i < 8; ++i) f.render(0.0f, true, scene);
    check_golden("gi_bounce", f);
}

// ---- M-VISUAL 7: atmosphere and volumetric fog -------------------------------------------------

TEST_CASE("golden: the physically based sky at sunset lights the scene through its IBL") {
    Fixture3D f;
    f.pitch_deg = 8.0f;  // the upper half is sky
    f.render(0.0f, true, [](Scene3D& s) {
        DirectionalLight sun;
        sun.direction = sun_direction_at(17.4f, 60.0f, 20.0f);
        sun.color = {1, 1, 1};
        sun.intensity = 3.0f;
        s.set_light(sun);
        Atmosphere a;
        a.enabled = true;
        s.set_atmosphere(a);
        Sky sky;
        sky.ibl = true;
        s.set_sky(sky);
    });
    CHECK(f.renderer.forward_3d().last_stats().atmosphere);
    check_golden("atmosphere_sunset", f);
}

TEST_CASE("golden: volumetric fog: sun shafts past a box and a point light's glow") {
    Fixture3D f;
    f.pitch_deg = 15.0f;
    f.render(0.0f, true, [](Scene3D& s) {
        DirectionalLight sun;
        sun.direction = math::normalize(vec3{0.6f, -0.6f, 1.0f});  // low, in front of the camera
        sun.intensity = 1.0f;
        sun.ambient_sky = sun.ambient_ground = {0.03f, 0.03f, 0.04f};
        s.set_light(sun);
        Sky sky;
        sky.zenith = sky.horizon = sky.ground = {0.02f, 0.02f, 0.03f};
        sky.sun_intensity = 0.0f;
        s.set_sky(sky);
        PointLight l;
        l.position = {-2.0f, 1.2f, 1.5f};
        l.radius = 2.5f;
        l.color = {1.0f, 0.5f, 0.2f};
        l.intensity = 1.5f;
        s.add_point_light(l);
        Volumetrics v;
        v.enabled = true;
        v.density = 0.05f;
        v.height_falloff = 0.3f;
        v.anisotropy = 0.3f;
        v.max_distance = 24.0f;
        s.set_volumetrics(v);
    });
    CHECK(f.renderer.forward_3d().last_stats().volumetrics);
    check_golden("volumetric_shafts", f);
}

// ---- M-VISUAL 8: post effects ---------------------------------------------------------------------

TEST_CASE("golden: depth of field focused on the box, the far ground and sky blurred") {
    Fixture3D f;
    f.pitch_deg = 12.0f;
    f.render(0.0f, true, [](Scene3D& s) {
        PostFx p;
        p.dof.enabled = true;
        p.dof.focus_distance = 7.0f;
        p.dof.focus_range = 0.8f;
        p.dof.blur_range = 4.0f;
        p.dof.max_radius = 0.03f;
        s.set_post(p);
    });
    CHECK(f.renderer.forward_3d().last_stats().dof);
    check_golden("dof_focus", f);
}

TEST_CASE("golden: motion blur of a box sliding sideways, auto exposure and a warm grade") {
    Fixture3D f;
    Array<u8> lut;
    make_identity_lut(lut);
    for (usize i = 0; i < lut.size(); i += 4) {  // warm: red up, blue down
        lut[i] = static_cast<u8>(math::min(255, lut[i] + 24));
        lut[i + 2] = static_cast<u8>(math::max(0, lut[i + 2] - 24));
    }
    const u32 key = f.renderer.textures().allocate_key();
    const TextureMipUpload mip{kLutSize * kLutSize, kLutSize, std::as_bytes(lut.span())};
    TextureUpload up;
    up.format = rhi::TextureFormat::RGBA8_UNorm;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    REQUIRE(f.renderer.textures().upload(key, 1, up).has_value());
    const math::mat4 prev = math::translation({1.4f, 0.0f, 1.2f});  // a second box, 0.8 m to the left last frame
    for (int i = 0; i < 2; ++i)
        f.render(0.0f, true, [&](Scene3D& s) {
            PostFx p;
            p.auto_exposure.enabled = true;
            p.auto_exposure.compensation = 1.5f;  // the bright ground pulls the average up
            p.lut_key = key;
            p.motion_blur.enabled = true;
            p.motion_blur.intensity = 1.0f;
            s.set_post(p);
            MeshInstance m;
            m.mesh = 1;
            m.world = math::translation({2.2f, 0.0f, 1.2f});
            m.prev_world = &prev;
            s.add(m);
        });
    CHECK(f.renderer.forward_3d().last_stats().motion_blur);
    check_golden("motion_blur_grade", f);
}
