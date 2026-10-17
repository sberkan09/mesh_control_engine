// Golden images of the 3D frame (FR-DBG-020; golden.h has the threshold and the rebaseline rules).
// Dev-machine-only: not registered with ctest; scripts/golden.sh runs it three times per backend.
// One case per render feature, added with the feature's M-VISUAL step.

#include <doctest/doctest.h>

#include "../asset/hdr_fixtures.h"
#include "fixture3d.h"
#include "golden.h"
#include "mce/asset/container.h"
#include "mce/cook/importer.h"
#include "mce/render/asset_upload.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

void check_golden(const char* name, const Fixture3D& f) {
    CHECK(f.device->validation_error_count() == 0);
    golden::check(name, kW, kH, f.pixels.span(), f.device->caps().device_name.c_str());
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

// ---- M-VISUAL2 1: screen-space reflections -------------------------------------------------------

TEST_CASE("golden: SSR mirrors the wall and the box in the metal floor") {
    Fixture3D f;
    upload_probe_room(f);
    const auto scene = [](Scene3D& s) {
        no_sun(s);
        s.set_sky(constant_sky(0.05f, true));
        MeshInstance m;
        m.mesh = 7;
        s.add(m);
        m.mesh = 8;
        s.add(m);
        ScreenSpaceReflections ssr;
        ssr.enabled = true;
        s.set_reflections(ssr);
    };
    for (int i = 0; i < 2; ++i) f.render(0.0f, false, scene);  // the second frame reflects the first
    CHECK(f.renderer.forward_3d().last_stats().ssr_history);
    check_golden("ssr_room", f);
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

// ---- M-VISUAL2 2: spatial upsample and sharpening ------------------------------------------------

TEST_CASE("golden: a half-scale frame through the spatial upsample and the sharpen") {
    Fixture3D f(1, true, true, 0.5f);
    f.render(0.0f, true, [](Scene3D& s) {
        PostFx post;
        post.bloom = false;
        post.fxaa = false;
        post.upscaler = Upscaler::Spatial;
        post.sharpen = 0.5f;
        s.set_post(post);
    });
    CHECK(f.renderer.forward_3d().last_stats().upscaled);
    CHECK(f.renderer.forward_3d().last_stats().sharpened);
    check_golden("spatial_sharpen", f);
}

// ---- M-VISUAL2 3: contact shadows ----------------------------------------------------------------

TEST_CASE("golden: contact shadows alone at the box's foot under a low sun") {
    Fixture3D f;
    f.render(0.0f, false, [](Scene3D& s) {  // no cascades: the contact shadow is the only one
        DirectionalLight sun = s.light();
        sun.direction = {0.0f, -1.0f, 1.2f};
        sun.ambient_sky = sun.ambient_ground = {0.0f, 0.0f, 0.0f};
        s.set_light(sun);
        Sky sky;
        sky.specular_ambient = 0.0f;
        s.set_sky(sky);
        ContactShadows cs;
        cs.enabled = true;
        cs.length = 0.6f;
        s.set_contact_shadows(cs);
    });
    CHECK(f.renderer.forward_3d().last_stats().contact_shadows);
    check_golden("contact_foot", f);
}

// ---- M-VISUAL2 4: material extensions -----------------------------------------------------------

TEST_CASE("golden: clear coat, sheen and anisotropy on three boxes under the sun and the sky") {
    Fixture3D f;
    const auto lobed = [&](u32 key, f32 x, const f32 (&color)[3], f32 rough, f32 metal, const asset::MaterialExt& ext) {
        mesh_fixture::Builder box;
        box.add_node(-1);
        mesh_fixture::Box b;
        b.center = {x, 0.4f, 1.6f};  // in front of the fixture's own box
        b.half = {0.4f, 0.4f, 0.4f};
        for (int i = 0; i < 3; ++i) b.color[i] = color[i];
        b.uvs = true;
        box.add_box(b);
        box.submeshes.back().material.roughness = rough;
        box.submeshes.back().material.metallic = metal;
        box.materials.push_back(ext);
        REQUIRE(f.renderer.meshes().upload(key, 1, box.build().span()).has_value());
    };
    asset::MaterialExt coat{}, sheen{}, aniso{};
    coat.clearcoat = 1.0f;
    coat.clearcoat_roughness = 0.05f;
    sheen.sheen_color[0] = 1.0f;
    sheen.sheen_color[1] = 0.9f;
    sheen.sheen_color[2] = 0.8f;
    sheen.sheen_roughness = 0.4f;
    aniso.anisotropy = 0.8f;
    lobed(30, -1.4f, {0.05f, 0.1f, 0.4f}, 0.7f, 0.0f, coat);
    lobed(31, 0.0f, {0.3f, 0.02f, 0.05f}, 0.9f, 0.0f, sheen);
    lobed(32, 1.4f, {0.9f, 0.8f, 0.6f}, 0.3f, 1.0f, aniso);
    f.render(0.0f, true, [](Scene3D& s) {
        Sky sky;
        sky.ibl = true;
        sky.specular_ambient = 1.0f;
        s.set_sky(sky);
        for (u32 key : {30u, 31u, 32u}) {
            MeshInstance m;
            m.mesh = key;
            s.add(m);
        }
    });
    check_golden("material_lobes", f);
}

TEST_CASE("golden: a green glass box in front of the fixture's box refracts it") {
    Fixture3D f;
    mesh_fixture::Builder box;
    box.add_node(-1);
    mesh_fixture::Box b;
    b.center = {0.3f, 0.45f, 1.3f};
    b.half = {0.45f, 0.45f, 0.45f};
    b.color[0] = 0.6f;
    b.color[1] = 0.95f;
    b.color[2] = 0.7f;
    b.uvs = true;
    box.add_box(b);
    box.submeshes.back().material.roughness = 0.1f;
    asset::MaterialExt glass{};
    glass.transmission = 1.0f;
    glass.ior = 1.5f;
    box.materials.push_back(glass);
    REQUIRE(f.renderer.meshes().upload(30, 1, box.build().span()).has_value());
    f.render(0.0f, true, [](Scene3D& s) {
        Sky sky;
        sky.ibl = true;
        s.set_sky(sky);
        MeshInstance m;
        m.mesh = 30;
        s.add(m);
    });
    CHECK(f.renderer.forward_3d().last_stats().transmissive == 1);
    check_golden("material_glass", f);
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

// ---- M-VISUAL 9: scale -----------------------------------------------------------------------------

TEST_CASE("golden: a scattered, wind-swept batch with its shadows and a projected decal") {
    Fixture3D f;
    FoliageScatter sc;
    sc.half_extents = {3.0f, 3.0f};
    sc.count = 60;
    sc.seed = 9;
    sc.scale_min = 0.15f;
    sc.scale_max = 0.35f;
    Array<math::mat4> grass;
    scatter_foliage(sc, grass);
    f.render(0.0f, true, [&](Scene3D& s) {
        s.set_time(0.6f, 0.016f);
        InstanceBatch b;
        b.id = 1;
        b.version = 1;
        b.mesh = 1;
        b.transforms = grass.span();
        b.tint = {0.25f, 0.8f, 0.3f, 1.0f};
        b.wind_amplitude = 0.6f;
        b.wind_frequency = 0.7f;
        s.add_batch(b);
        Decal d;
        d.world = math::translation({1.6f, 0.0f, 1.8f}) * math::scaling({1.4f, 0.6f, 1.0f});
        d.color = {0.1f, 0.15f, 0.9f, 1.0f};
        d.roughness = 0.2f;
        s.add_decal(d);
    });
    CHECK(f.renderer.forward_3d().last_stats().batches == 1);
    CHECK(f.renderer.forward_3d().last_stats().decals == 1);
    check_golden("scale_foliage_decal", f);
}

// ---- M-VISUAL3 6: foliage variation and wind ------------------------------------------------------------------------

TEST_CASE("golden: a windy meadow whose copies vary in colour, with a wind height, gusts and flutter") {
    Fixture3D f;
    FoliageScatter sc;
    sc.half_extents = {3.0f, 3.0f};
    sc.count = 90;
    sc.seed = 4;
    sc.scale_min = 0.2f;
    sc.scale_max = 0.5f;
    Array<math::mat4> grass;
    scatter_foliage(sc, grass);
    f.render(0.0f, true, [&](Scene3D& s) {
        s.set_time(0.8f, 0.016f);
        InstanceBatch b;
        b.id = 1;
        b.version = 1;
        b.mesh = 1;
        b.transforms = grass.span();
        b.tint = {0.55f, 0.9f, 0.4f, 1.0f};
        b.tint_variation = {0.1f, 2.5f, 3.0f, 1.0f};
        b.wind_amplitude = 0.25f;
        b.wind_frequency = 0.7f;
        b.wind_ext = {0.5f, 0.7f, 0.03f, 0.0f};
        s.add_batch(b);
    });
    CHECK(f.renderer.forward_3d().last_stats().batches == 1);
    check_golden("foliage_variation", f);
}

TEST_CASE("golden: a GPU particle burst glowing above the ground") {
    Fixture3D f;
    if (!f.device->caps().compute) return;
    for (int i = 0; i < 10; ++i)
        f.render(0.0f, true, [&](Scene3D& s) {
            s.set_time(0.05f * f32(i), 0.05f);
            ParticleEmitter e;
            e.id = 1;
            e.max_particles = 512;
            e.position = {1.6f, 0.4f, 1.4f};
            e.box = {0.1f, 0.1f, 0.1f};
            e.rate = 0.0f;
            e.burst = i == 0 ? 300u : 0u;
            e.collide = true;
            s.add_particle_emitter(e);
        });
    CHECK(f.renderer.forward_3d().last_stats().particle_emitters == 1);
    check_golden("particles_burst", f);
}

// ---- M-VISUAL3 2: parallax occlusion mapping ----------------------------------------------------

TEST_CASE("golden: parallax_cobble: a grazing view of a brick floor whose joints are carved by its height map") {
    Fixture3D f;
    ParallaxFloor p = make_parallax_floor(f);
    f.pitch_deg = 12.0f;
    f.render(0.0f, true, [&](Scene3D& s) {
        MeshInstance fl;
        fl.mesh = 20;
        fl.material = &p.on;
        s.add(fl);
    });
    check_golden("parallax_cobble", f);
}

// ---- M-VISUAL3 4: HDR sky -----------------------------------------------------------------------

TEST_CASE("golden: a cooked .hdr sky with a 200x hot spot lights a white sphere, a box and the ground") {
    Fixture3D f;
    f.pitch_deg = 12.0f;
    // A 64x32 Radiance file written here, cooked by the texture importer (RGBA16F, mips), uploaded as the asset
    // system would: base 0.25, a 50 patch on the horizon toward +Z and +X (equirect u = 0.5625): the light comes from the camera's right and lights the
    // faces it sees.
    std::vector<float> grey(64 * 32, 0.25f);
    for (unsigned y = 14; y < 18; ++y)
        for (unsigned x = 32; x < 40; ++x) grey[y * 64 + x] = 50.0f;
    const std::string hdr = hdr_fixtures::make_rgbe_hdr(64, 32, grey);
    const auto importer = cook::make_texture_importer();
    cook::ImportInput in;
    in.guid = asset::Guid{1, 2};
    in.display_path = "hdr_sky.hdr";
    in.source_path = "hdr_sky.hdr";
    in.source = Span<const byte>(reinterpret_cast<const byte*>(hdr.data()), hdr.size());
    cook::DiagList diags;
    asset::ContainerWriter writer;
    REQUIRE(importer->cook(in, writer, diags));
    Array<byte> packed;
    writer.finish(packed);
    auto view = asset::ContainerView::open(packed.span());
    REQUIRE(view.has_value());
    const u32 chunk = view.value().find(asset::ChunkType::Texture);
    REQUIRE(chunk < view.value().chunk_count());
    const u32 tex = f.renderer.textures().allocate_key();
    REQUIRE(upload_cooked_texture(f.renderer.textures(), tex, 1, view.value().payload(chunk)).has_value());
    upload_sphere(f, 40, {-1.8f, 1.0f, 0.6f}, 0.8f);
    f.render(0.0f, false, [&](Scene3D& s) {
        no_sun(s);
        Sky sky = constant_sky(0.0f, true);
        sky.texture = tex;
        sky.texture_intensity = 0.1f;
        s.set_sky(sky);
        MeshInstance m;
        m.mesh = 40;
        s.add(m);
    });
    check_golden("hdr_sky", f);
}

// ---- M-VISUAL3 3: the camera's look ---------------------------------------------------------------

TEST_CASE("golden: filmic_lens: vignette, chromatic aberration and grain on the ground, the box and the sky at a fixed time") {
    Fixture3D f;
    f.pitch_deg = 12.0f;
    f.render(0.0f, true, [&](Scene3D& s) {
        PostFx p;
        p.fxaa = false;
        p.lens.vignette = 0.7f;
        p.lens.chromatic_aberration = 0.8f;
        p.lens.grain = 0.25f;
        s.set_post(p);
        s.set_time(2.0f, 1.0f / 60.0f);
    });
    check_golden("filmic_lens", f);
}

TEST_CASE("golden: tonemap_agx: the same scene through the AgX curve") {
    Fixture3D f;
    f.pitch_deg = 12.0f;
    f.render(0.0f, true, [&](Scene3D& s) {
        PostFx p;
        p.fxaa = false;
        p.tonemapper = Tonemapper::AgX;
        s.set_post(p);
    });
    check_golden("tonemap_agx", f);
}

TEST_CASE("golden: tonemap_neutral: the same scene through Khronos PBR Neutral") {
    Fixture3D f;
    f.pitch_deg = 12.0f;
    f.render(0.0f, true, [&](Scene3D& s) {
        PostFx p;
        p.fxaa = false;
        p.tonemapper = Tonemapper::PbrNeutral;
        s.set_post(p);
    });
    check_golden("tonemap_neutral", f);
}

// ---- M-VISUAL3 5a: automatic instancing ----------------------------------------------------------

TEST_CASE("golden: instanced_props: a grid of identical crates under three cascades, drawn as instanced runs") {
    Fixture3D f;
    f.pitch_deg = 30.0f;
    upload_crate(f, 60);
    f.render(0.0f, true, [](Scene3D& s) {
        s.set_sky(gradient_sky());
        add_crate_grid(s, 60);
    });
    const Forward3DStats& st = f.renderer.forward_3d().last_stats();
    CHECK(st.cascades == 3);
    CHECK(st.runs_instanced >= 2);
    CHECK(st.shadow_runs_instanced >= 2);
    check_golden("instanced_props", f);
}

// ---- M-VISUAL3 5b: static merge ------------------------------------------------------------------------------------

TEST_CASE("golden: static_merge_kit: a modular kit of 32 pieces merged into two cells, one draw per material and cell") {
    Fixture3D f;
    f.pitch_deg = 30.0f;
    MergeKit kit;
    upload_merge_kit(f, kit);
    f.render(0.0f, true, [&](Scene3D& s) {
        s.set_sky(gradient_sky());
        add_merge_kit(s, kit, true);
    });
    const Forward3DStats& st = f.renderer.forward_3d().last_stats();
    CHECK(st.cascades == 3);
    CHECK(st.main_draws == 2 + kit.materials * kit.cells);  // the fixture's ground and box, then the kit
    check_golden("static_merge_kit", f);
}
