// 06_showcase — the M-VISUAL milestone demo (Linear SBA-15): one scene, a pavilion on a tiled courtyard,
// drawn with today's M5a frame ("current") and with the M-VISUAL features on each quality tier, at the
// same cameras and times of day.
//
//   * the room is lit by the light it bounces (an irradiance volume: the red back wall bleeds onto the
//     polished floor and the ceiling), its floor and the chrome sphere reflect a box-projected probe
//     and the physically based sky (IBL)
//   * lanterns: spot and point lights casting shadows (the local-light atlas)
//   * dusk: the physically based sky colors the sun, volumetric fog takes the sun shafts through the
//     door and the lanterns' glow
//   * SSAO, TAA, auto exposure; a wind-swept grass batch culled on the GPU, a puddle and a sigil decal,
//     brazier embers as GPU particles
//   * M-VISUAL2: screen-space reflections in the puddle and the polished floor, contact shadows,
//     sharpening, a glass block refracting the path, a lacquered crate and a brushed-metal statue
//     ("highbase" is High without them, for the on/off sheet)
//
//   06_showcase                                   run until the window closes (the High profile)
//   06_showcase -profile current                  the same scene with the M5a frame
//   06_showcase -profile medium|low               a quality tier (r.quality's knobs)
//   06_showcase -profile highbase                 High without the M-VISUAL2 effects
//   06_showcase -shot 1 -hour 12                  another camera / time of day (S cycles the shots)
//   06_showcase -sheet sheet.ppm                  side by side: every shot under every profile in
//                                                 -sheet.profiles, each settled for -sheet.frames
//                                                 frames, written as one image; prints the per-stage
//                                                 GPU budget table of each profile
//   06_showcase -headless 1 -ticks 300            determinism run: no GL, CPU mesh store, prints the hash
//
// Keys: S next shot, H +1 hour, arrows orbit, Space auto orbit, Esc quits.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "hud_font.h"
#include "mce/asset/asset_system.h"
#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/asset_upload.h"
#include "mce/render/atmosphere.h"
#include "mce/render/quality.h"
#include "mce/render/renderer.h"
#include "mce/rhi/default_device.h"
#include "ship_paths.h"

namespace {

using namespace mce;
using namespace mce::math;
using namespace mce::render;
using namespace mce::samples;

MCE_CVAR(cv_frames, "frames", 0, "Quit after N frames (0 = run until the window closes)", CVarFlags::None);
MCE_CVAR(cv_capture, "capture", "", "Write the last frame (with -frames) to this .ppm path", CVarFlags::None);
MCE_CVAR(cv_ticks, "ticks", 300, "Headless: ticks to simulate", CVarFlags::None);
MCE_CVAR(cv_pack, "pack", "", "Cooked pack to mount (default: assets.mce next to the executable)", CVarFlags::None);
MCE_CVAR(cv_gl_debug, "r.gl_debug", true, "Enable the GL_KHR_debug validation callback", CVarFlags::RequiresRestart);
MCE_CVAR(cv_profile, "profile", "high", "current (the M5a frame), low, medium or high (M-VISUAL on that tier)",
         CVarFlags::RequiresRestart);
MCE_CVAR(cv_shot, "shot", 0, "Camera and time of day: 0 dusk at the door, 1 the interior, 2 the courtyard at noon",
         CVarFlags::None);
MCE_CVAR(cv_hour, "hour", -1.0f, "Overrides the shot's hour of the day (0..24)", CVarFlags::None);
MCE_CVAR(cv_grass, "grass", 6000, "Grass tufts scattered over the courtyard", CVarFlags::RequiresRestart);
MCE_CVAR(cv_sheet, "sheet", "", "Write the side-by-side sheet (.ppm) of every shot under every profile, then quit",
         CVarFlags::None);
MCE_CVAR(cv_sheet_profiles, "sheet.profiles", "current,highbase,high,medium,low", "Columns of the sheet", CVarFlags::None);
MCE_CVAR(cv_sheet_frames, "sheet.frames", 48, "Frames each shot settles (TAA, GI, auto exposure) before its capture",
         CVarFlags::None);
MCE_CVAR(cv_sheet_w, "sheet.w", 480, "Width of one sheet cell", CVarFlags::None);
MCE_CVAR(cv_sheet_h, "sheet.h", 270, "Height of one sheet cell", CVarFlags::None);

// ---- profiles and shots -------------------------------------------------------------------------

enum class Profile : u8 { Current, Low, Medium, High, HighBase };

bool parse_profile(StringView s, Profile& out) {
    if (s == StringView("current")) out = Profile::Current;
    else if (s == StringView("low")) out = Profile::Low;
    else if (s == StringView("medium")) out = Profile::Medium;
    else if (s == StringView("high")) out = Profile::High;
    else if (s == StringView("highbase")) out = Profile::HighBase;
    else return false;
    return true;
}

const char* profile_name(Profile p) {
    switch (p) {
        case Profile::Current: return "current";
        case Profile::Low: return "low";
        case Profile::Medium: return "medium";
        case Profile::High: return "high";
        case Profile::HighBase: return "highbase";
    }
    return "?";
}

QualitySettings profile_quality(Profile p) {
    switch (p) {
        case Profile::Low: return quality_settings(QualityTier::Low);
        case Profile::Medium: return quality_settings(QualityTier::Medium);
        default: return quality_settings(QualityTier::High);
    }
}

struct Shot {
    const char* name;
    vec3 target;
    f32 yaw, pitch, distance, fov_deg, hour;
};
constexpr Shot kShots[] = {
    {"dusk", {0.0f, 1.3f, 1.0f}, 0.28f, 0.16f, 10.5f, 55.0f, 18.35f},      // outside, the low sun behind the camera's left
    {"interior", {0.6f, 1.2f, -2.2f}, 0.42f, 0.10f, 4.6f, 70.0f, 15.5f},  // inside: back wall, statue, window light
    {"noon", {0.0f, 0.6f, 0.0f}, -0.75f, 0.42f, 17.0f, 50.0f, 12.0f},     // the courtyard from above
};
constexpr u32 kShotCount = sizeof(kShots) / sizeof(kShots[0]);

// ---- content -------------------------------------------------------------------------------------

enum Model : u32 { Courtyard, Pavilion, Statue, Lantern, Brazier, Crate, Grass, kModelCount };
constexpr const char* kModelPaths[kModelCount] = {"models/courtyard.glb", "models/pavilion.glb", "models/statue.glb",
                                                  "models/lantern.glb",   "models/brazier.glb",  "models/crate.glb",
                                                  "models/grass.glb"};

// The pavilion's room (pavilion.glb): x -3.5..3.5, z -3..3, 3.2 m tall, the door on +Z, a window on +X.
constexpr aabb3 kRoom{{-3.5f, 0.0f, -3.0f}, {3.5f, 3.2f, 3.0f}};
constexpr vec3 kBrazier{3.4f, 0.0f, 6.2f};
struct LanternSpot {
    vec3 position;
    vec3 color;
    f32 intensity, radius;
    bool spot;  // hangs from the ceiling and points down
};
constexpr LanternSpot kLanterns[] = {
    {{-2.2f, 2.95f, -1.6f}, {1.0f, 0.78f, 0.5f}, 9.0f, 7.0f, true},   // over the back wall's west half
    {{-2.2f, 1.75f, 4.4f}, {1.0f, 0.62f, 0.32f}, 5.0f, 6.0f, false},  // posts along the path
    {{2.2f, 1.75f, 4.4f}, {1.0f, 0.62f, 0.32f}, 5.0f, 6.0f, false},
    {{-2.2f, 1.75f, 8.4f}, {1.0f, 0.62f, 0.32f}, 4.0f, 6.0f, false},
    {{2.2f, 1.75f, 8.4f}, {1.0f, 0.62f, 0.32f}, 4.0f, 6.0f, false},
};


// One renderer (or the CPU mesh store) with the pack loaded. A sheet builds one per profile, since
// the tier's MSAA, shadow size and render scale are Forward3D creation knobs.
struct Content {
    Renderer renderer;
    asset::AssetSystem assets;
    AssetUploadSink sink{renderer, assets};
    MeshStore cpu_meshes;
    const MeshStore* meshes = nullptr;
    u32 keys[kModelCount] = {};
    Array<asset::AnyHandle> handles;
    bool gpu = false;

    ResultV load(rhi::RenderDevice* device, Profile profile) {
        if (device) {
            MCE_TRY(renderer.init(*device));
            Forward3DDesc fd;
            if (profile != Profile::Current) apply_quality(profile_quality(profile), fd);
            MCE_TRY(renderer.init_3d(fd));
            assets.set_upload_sink(&sink);
            meshes = &renderer.meshes();
            gpu = true;
        } else {
            cpu_meshes.init(nullptr);
            meshes = &cpu_meshes;
        }
        FixedString<1024> path;
        resolve_shipped_path(cv_pack.get_string(), "assets.mce", MCE_SAMPLE_DEV_PACK, path);
        const StringView roots[1] = {path.view()};
        asset::MountOptions mo;
        mo.verify_payload_crc = false;
        MCE_TRY(assets.mount(Span<const StringView>(roots, 1), mo));
        for (asset::AssetKey k = 0; k < assets.asset_count(); ++k) {  // textures first: meshes resolve their maps
            if (assets.info(k).type != asset::ChunkType::Texture) continue;
            MCE_TRY_ASSIGN(asset::AssetHandle<asset::Texture> h, assets.load_sync<asset::Texture>(k));
            handles.push_back(asset::AnyHandle(h));
        }
        for (u32 m = 0; m < kModelCount; ++m) {
            asset::AssetKey key = ~0u;
            for (asset::AssetKey k = 0; k < assets.asset_count(); ++k)
                if (assets.info(k).path == StringView(kModelPaths[m])) key = k;
            if (key == ~0u) return Error(Errc::NotFound, "06_showcase: model missing from the pack", kModelPaths[m]);
            MCE_TRY_ASSIGN(asset::AssetHandle<asset::Mesh> h, assets.load_sync<asset::Mesh>(key));
            handles.push_back(asset::AnyHandle(h));
            if (!device) MCE_TRY(cpu_meshes.upload(key, assets.generation(key), assets.resolve(h)));
            keys[m] = key;
        }
        return ok();
    }

    void unload() {
        for (asset::AnyHandle h : handles) assets.release(h);
        handles.clear();
        if (gpu) renderer.shutdown();
        gpu = false;
    }
};

constexpr usize kCurrentGrass = 2500;

// M-VISUAL2's materials (step 4): a glass block on the path, a lacquered crate and the statue in
// brushed metal. Overrides replace the whole submesh material, so each spells out its base.
MaterialOverride glass_material() {
    MaterialOverride m;
    m.params.base_color[0] = 0.55f;
    m.params.base_color[1] = 0.9f;
    m.params.base_color[2] = 0.75f;
    m.params.base_color[3] = 1.0f;
    m.params.roughness = 0.05f;
    m.params.metallic = 0.0f;
    m.ext.transmission = 1.0f;
    m.ext.ior = 1.5f;
    return m;
}
MaterialOverride lacquer_material() {
    MaterialOverride m;
    m.params.base_color[0] = 0.45f;
    m.params.base_color[1] = 0.05f;
    m.params.base_color[2] = 0.04f;
    m.params.base_color[3] = 1.0f;
    m.params.roughness = 0.6f;
    m.params.metallic = 0.0f;
    m.ext.clearcoat = 1.0f;
    m.ext.clearcoat_roughness = 0.04f;
    return m;
}
MaterialOverride brushed_material() {
    MaterialOverride m;
    m.params.base_color[0] = 0.9f;
    m.params.base_color[1] = 0.72f;
    m.params.base_color[2] = 0.48f;
    m.params.base_color[3] = 1.0f;
    m.params.roughness = 0.3f;
    m.params.metallic = 1.0f;
    m.ext.anisotropy = 0.7f;
    return m;
}

// The static layout and the grass, shared by every profile.
struct Layout {
    Array<MeshInstance> statics;
    Array<MeshInstance> visual2;  // drawn by the M-VISUAL2 profiles only
    Array<mat4> grass;
    MaterialOverride glass = glass_material(), lacquer = lacquer_material(), brushed = brushed_material();

    void build(const Content& c) {
        auto place = [&](Model m, vec3 p, f32 yaw = 0.0f, f32 s = 1.0f) {
            MeshInstance i;
            i.mesh = c.keys[m];
            i.world = trs(p, quat_from_axis_angle({0, 1, 0}, yaw), {s, s, s});
            statics.push_back(i);
        };
        place(Courtyard, {0.0f, 0.0f, 0.0f});
        place(Pavilion, {0.0f, 0.0f, 0.0f});
        place(Statue, {1.4f, 0.0f, -1.3f});
        place(Crate, {-2.8f, 0.0f, -2.3f}, 0.2f);
        place(Crate, {-2.7f, 0.8f, -2.25f}, -0.15f, 0.8f);
        place(Crate, {-2.0f, 0.0f, -2.4f}, 0.5f, 0.9f);
        place(Crate, {4.4f, 0.0f, 1.8f}, 0.3f);
        place(Brazier, kBrazier);
        for (const LanternSpot& l : kLanterns) place(Lantern, l.position);
        auto place2 = [&](Model m, vec3 p, f32 yaw, vec3 s, const MaterialOverride* mat) {
            MeshInstance i;
            i.mesh = c.keys[m];
            i.world = trs(p, quat_from_axis_angle({0, 1, 0}, yaw), s);
            i.material = mat;
            visual2.push_back(i);
        };
        place2(Crate, {-1.0f, 0.0f, 3.6f}, 0.25f, {0.9f, 1.2f, 0.35f}, &glass);  // a glass slab beside the path
        place2(Crate, {1.25f, 0.0f, 5.2f}, -0.4f, {0.8f, 0.8f, 0.8f}, &lacquer);  // on the path's edge
        place2(Statue, {1.4f, 0.0f, -1.3f}, 0.0f, {1.0f, 1.0f, 1.0f}, &brushed);  // in place of the plain one
        // Grass: a seeded scatter over the courtyard, kept off the pavilion, the path and the brazier.
        FoliageScatter sc;
        sc.half_extents = {19.0f, 19.0f};
        sc.count = static_cast<u32>(max<i64>(cv_grass.get_int(), 0));
        sc.seed = 6;
        sc.scale_min = 0.7f;
        sc.scale_max = 1.5f;
        Array<mat4> all;
        scatter_foliage(sc, all);
        for (const mat4& m : all) {
            const vec3 p{m.c[3][0], 0.0f, m.c[3][2]};
            const bool room = p.x > kRoom.min.x - 0.6f && p.x < kRoom.max.x + 0.6f && p.z > kRoom.min.z - 0.6f &&
                              p.z < kRoom.max.z + 0.6f;
            const bool path = std::fabs(p.x) < 1.6f && p.z > 0.0f;
            const bool fire = length(p - kBrazier) < 1.2f;
            if (!room && !path && !fire) grass.push_back(m);
        }
    }
};

// The frame of `profile` for `shot` at `hour`: today's M5a features for Current, everything of
// M-VISUAL on the tier's knobs otherwise. `t` / `dt` drive the wind, the embers and auto exposure.
void fill_scene(Scene3D& scene, const Content& c, const Layout& layout, Profile profile, const Shot& shot, f32 hour, f32 yaw_offset,
                f32 t, f32 dt, rhi::Extent2D viewport) {
    const bool visual = profile != Profile::Current;
    const bool visual2 = visual && profile != Profile::HighBase;  // M-VISUAL2's effects and materials
    const QualitySettings q = profile_quality(profile);
    Camera3D cam = orbit_camera(shot.target, shot.yaw + yaw_offset, shot.pitch, shot.distance, radians(shot.fov_deg));
    cam.near_z = 0.05f;
    cam.far_z = 150.0f;
    scene.begin(cam, viewport);
    scene.set_time(t, dt);

    const vec3 sun_dir = sun_direction_at(hour, 58.0f, 35.0f);
    const f32 day = clamp(-sun_dir.y * 4.0f, 0.0f, 1.0f);  // the sun's height: 0 at the horizon
    DirectionalLight sun;
    sun.direction = sun_dir;
    sun.intensity = 3.0f;
    if (visual) {
        sun.color = {1.0f, 1.0f, 1.0f};  // above the air: the atmosphere tints it and sets the ambient
    } else {
        // The M5a frame: a hand-picked warm sun at dusk and a hemisphere ambient.
        sun.color = lerp(vec3{1.0f, 0.55f, 0.3f}, vec3{1.0f, 0.95f, 0.88f}, day);
        sun.intensity = 0.6f + 2.4f * day;
        sun.ambient_sky = lerp(vec3{0.18f, 0.16f, 0.22f}, vec3{0.32f, 0.38f, 0.5f}, day);
        sun.ambient_ground = lerp(vec3{0.08f, 0.06f, 0.05f}, vec3{0.18f, 0.16f, 0.14f}, day);
    }
    scene.set_light(sun);
    Fog fog;
    fog.color = visual ? vec3{0.5f, 0.55f, 0.62f} : lerp(vec3{0.45f, 0.35f, 0.38f}, vec3{0.62f, 0.72f, 0.85f}, day);
    fog.density = 0.006f;
    scene.set_fog(fog);
    Sky sky;
    if (!visual) {
        sky.zenith = lerp(vec3{0.12f, 0.15f, 0.32f}, vec3{0.2f, 0.4f, 0.85f}, day);
        sky.horizon = lerp(vec3{0.85f, 0.45f, 0.3f}, vec3{0.7f, 0.78f, 0.9f}, day);
    }
    sky.ibl = visual;
    scene.set_sky(sky);
    ShadowSettings shadows;
    shadows.distance = 45.0f;
    shadows.cascades = 3;
    PostFx post;
    post.fxaa = true;
    if (visual) {
        Atmosphere atmosphere;
        atmosphere.enabled = true;
        scene.set_atmosphere(atmosphere);
        Volumetrics vol;
        vol.enabled = hour >= 17.0f || hour <= 7.0f;  // the dusk haze
        vol.density = 0.035f;
        vol.height_falloff = 0.25f;
        vol.anisotropy = 0.6f;
        vol.max_distance = 40.0f;
        apply_quality(q, vol);
        scene.set_volumetrics(vol);
        AmbientOcclusion ao;
        ao.enabled = true;
        scene.set_ambient_occlusion(ao);
        GiVolume gi;
        gi.enabled = true;
        gi.bounds = {{-3.2f, 0.3f, -2.7f}, {3.2f, 2.9f, 2.7f}};
        gi.counts[0] = 6;
        gi.counts[1] = 3;
        gi.counts[2] = 5;
        gi.probes_per_frame = 15;
        apply_quality(q, gi);
        scene.set_gi_volume(gi);
        ReflectionProbe probe;
        probe.position = {0.0f, 1.4f, 0.0f};
        probe.bounds = kRoom;
        probe.blend_distance = 0.3f;
        scene.add_reflection_probe(probe);
        post.auto_exposure.enabled = true;
        post.auto_exposure.dt = max(dt, 1.0f / 240.0f);
        post.taa = true;
        apply_quality(q, shadows, post);
        LocalShadowSettings local;
        local.max_lights = 8;
        scene.set_local_shadows(local);
    }
    if (visual2) {
        ScreenSpaceReflections ssr;
        ssr.enabled = true;
        ssr.max_distance = 12.0f;
        apply_quality(q, ssr);
        scene.set_reflections(ssr);
        ContactShadows contact;
        contact.enabled = true;
        apply_quality(q, contact);
        scene.set_contact_shadows(contact);
        post.sharpen = 0.3f;
    }
    shadows.resolution = visual ? q.shadow_size : 2048;  // = Forward3DDesc::shadow_size
    scene.set_shadows(shadows);
    scene.set_post(post);

    for (const MeshInstance& i : layout.statics)
        if (!visual2 || i.mesh != c.keys[Statue]) scene.add(i);
    if (visual2)
        for (const MeshInstance& i : layout.visual2) scene.add(i);
    for (const LanternSpot& l : kLanterns) {
        PointLight pl;
        pl.position = l.position + vec3{0.0f, l.spot ? -0.25f : 0.0f, 0.0f};
        pl.color = l.color;
        pl.intensity = l.intensity;
        pl.radius = l.radius;
        if (l.spot) {
            pl.direction = normalize(vec3{0.0f, -1.0f, -0.55f});
            pl.spot_outer = radians(42.0f);
            pl.spot_inner = radians(26.0f);
        }
        pl.cast_shadows = visual;
        scene.add_point_light(pl);
    }
    if (visual) {
        InstanceBatch grass;
        grass.id = 1;
        grass.version = 1;
        grass.mesh = c.keys[Grass];
        grass.transforms = layout.grass.span();
        grass.cull_distance = 60.0f;
        grass.wind_amplitude = 0.35f;
        grass.wind_frequency = 0.6f;
        grass.wind_direction = {0.8f, 0.3f};
        scene.add_batch(grass);
        Decal puddle;  // a dark, glossy puddle on the path
        puddle.world = translation({0.6f, 0.0f, 6.0f}) * scaling({2.2f, 0.4f, 1.4f});
        puddle.color = {0.12f, 0.12f, 0.13f, 0.75f};
        puddle.roughness = 0.04f;
        puddle.edge_fade = 0.5f;
        scene.add_decal(puddle);
        Decal sigil;  // a faintly glowing sigil inside the door
        sigil.world = translation({0.0f, 0.0f, 1.2f}) * scaling({1.3f, 0.4f, 1.3f});
        sigil.color = {0.25f, 0.55f, 1.0f, 0.6f};
        sigil.emissive = 0.6f;
        scene.add_decal(sigil);
        ParticleEmitter embers;
        embers.id = 1;
        embers.max_particles = 2048;
        embers.position = kBrazier + vec3{0.0f, 0.9f, 0.0f};
        embers.box = {0.25f, 0.02f, 0.25f};
        embers.direction = {0.1f, 1.0f, 0.0f};
        embers.spread = 0.3f;
        embers.rate = 160.0f;
        embers.speed_min = 0.6f;
        embers.speed_max = 1.6f;
        embers.lifetime_min = 0.8f;
        embers.lifetime_max = 2.2f;
        embers.gravity = {0.15f, 0.35f, 0.0f};  // the hot air carries them up and downwind
        embers.drag = 0.6f;
        embers.size_start = 0.05f;
        embers.size_end = 0.015f;
        embers.color_start = {6.0f, 2.2f, 0.5f, 1.0f};
        embers.color_end = {1.5f, 0.2f, 0.02f, 0.0f};
        scene.add_particle_emitter(embers);
    } else {
        // The M5a frame draws the tufts one instance at a time. Past ~3000 draws the per-frame uniform
        // ring overflows (GL drops the commands), so it keeps an even subset of at most kCurrentGrass.
        const usize stride = (layout.grass.size() + kCurrentGrass - 1) / kCurrentGrass;
        for (usize g = 0; g < layout.grass.size(); g += max<usize>(stride, 1)) {
            MeshInstance i;
            i.mesh = c.keys[Grass];
            i.world = layout.grass[g];
            i.cast_shadows = false;
            scene.add(i);
        }
    }
    scene.build();
}

// ---- the interactive app ---------------------------------------------------------------------------

class ShowcaseApp final : public app::Application, public app::IPlatformHooks {
public:
    ShowcaseApp(platform::Window* window, rhi::RenderDevice* device, Profile profile)
        : window_(window), device_(device), profile_(profile) {}

    ResultV on_init(app::Context&) override {
        MCE_TRY(content_.load(device_, profile_));
        layout_.build(content_);
        shot_ = static_cast<u32>(cv_shot.get_int()) % kShotCount;
        hour_ = cv_hour.get_float() >= 0.0f ? cv_hour.get_float() : kShots[shot_].hour;
        if (device_) {
            HudFont hud;
            MCE_TRY_ASSIGN(font_, content_.renderer.create_font(hud.metrics, hud.glyphs.span(), {}, std::as_bytes(hud.atlas.span()),
                                                                "06_showcase.hud_font"));
            window_->set_event_handler(&ShowcaseApp::on_event, this);
        }
        MCE_LOG_INFO("06_showcase: profile {}, {} grass tufts", profile_name(profile_), layout_.grass.size());
        return ok();
    }

    void on_tick(const app::TickContext& t) override {
        const f32 dt = static_cast<f32>(t.dt);
        prev_time_ = time_;
        time_ += dt;
        prev_yaw_ = yaw_;
        if (auto_orbit_) yaw_ = 0.12f * std::sin(time_ * 0.2f);
        yaw_ += static_cast<f32>(key_yaw_) * 0.8f * dt;
        if (!device_) {  // headless: the CPU frame (culling, cascades, clusters, decals) feeds the hash
            Scene3D scene(*content_.meshes, mem::heap());
            fill_scene(scene, content_, layout_, profile_, kShots[shot_], hour_, yaw_, time_, dt, {1280, 720});
            fold(scene);
            if (ticks_ % 100 == 99) shot_ = (shot_ + 1) % kShotCount;  // every shot takes part
        }
        ++ticks_;
    }

    void on_render(const app::RenderContext& rc) override {
        if (!device_) return;
        device_->begin_frame(rc.frame_index);
        const rhi::Extent2D bb = device_->backbuffer_size();
        const f32 t = lerp(prev_time_, time_, static_cast<f32>(rc.alpha));
        Scene3D scene(*content_.meshes, mem::frame());
        fill_scene(scene, content_, layout_, profile_, kShots[shot_], hour_, lerp(prev_yaw_, yaw_, static_cast<f32>(rc.alpha)), t,
                   max(t - last_render_t_, 0.0f), bb);
        last_render_t_ = t;
        rhi::CommandBuffer cmd(mem::frame());
        content_.renderer.record_3d(cmd, scene);
        record_hud(cmd, bb);
        device_->submit(cmd);
        const bool last = cv_frames.get_int() > 0 && rc.frame_index + 1 >= static_cast<u64>(cv_frames.get_int());
        if (last && !cv_capture.get_string().empty()) capture(cv_capture.get_string().data(), bb);
        device_->end_frame();
        content_.assets.end_frame();
        if (last) quit(0);
    }

    void on_shutdown() override { content_.unload(); }
    bool pump_events() override { return platform::pump_events(); }
    void present(bool vsync) override { device_->present(vsync); }

    void report_headless() const {
        std::printf("06_showcase: %llu ticks | profile %s, %u grass tufts | hash %016llx\n", static_cast<unsigned long long>(ticks_),
                    profile_name(profile_), static_cast<u32>(layout_.grass.size()), static_cast<unsigned long long>(hash_));
    }

private:
    void fold(const Scene3D& scene) {
        u64 h = hash_;
        for (const Scene3D::Draw& d : scene.draws()) {
            h = hash_combine(h, (static_cast<u64>(d.instance) << 32) | d.submesh);
            h = fnv1a64(&d.model, sizeof(d.model), h);
        }
        for (const Cascade& cs : scene.cascades()) h = fnv1a64(&cs.view_proj, sizeof(cs.view_proj), h);
        h = fnv1a64(scene.local_shadow_tiles().data(), scene.local_shadow_tiles().size_bytes(), h);
        h = fnv1a64(scene.decals().data(), scene.decals().size_bytes(), h);
        const Scene3D::Stats& st = scene.stats();
        h = hash_combine(h, (static_cast<u64>(st.batch_instances) << 32) | st.decals);
        hash_ = h;
    }

    void record_hud(rhi::CommandBuffer& cmd, rhi::Extent2D bb) {
        const Forward3DStats& fs = content_.renderer.forward_3d().last_stats();
        const Forward3DGpuTimes gpu = content_.renderer.forward_3d().gpu_times(device_->last_gpu_timestamps());
        char line[400];
        std::snprintf(line, sizeof(line),
                      "MCE M-VISUAL SHOWCASE  PROFILE %s  SHOT %s  HOUR %.1f\nDRAWS %u  SHADOW TILES %u  BATCH DRAWS %u  DECALS %u  "
                      "PARTICLES %u\nGPU MS %.2f\nS NEXT SHOT  H +1 HOUR  ARROWS ORBIT  SPACE AUTO",
                      profile_name(profile_), kShots[shot_].name, static_cast<f64>(hour_), fs.main_draws, fs.local_shadow_tiles,
                      fs.batch_draws, fs.decals, fs.particle_emitters, gpu.total);
        for (char* p = line; *p; ++p)
            if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 'a' + 'A');  // the HUD font has capitals only
        TextLayout text(mem::frame());
        layout_text(font_.face, line, {}, text);
        SpriteBatch hud(content_.renderer.sprite_sets(), mem::frame());
        hud.begin(compute_screen_view(bb), PassId::Screen2D);
        TextPlacement p;
        p.origin = vec2(14, 14);
        p.tint = Color{0.02f, 0.03f, 0.06f, 0.85f};
        hud.add_text(font_, text, p);
        p.origin = vec2(12, 12);
        p.tint = Color{1.0f, 0.97f, 0.88f, 1};
        hud.add_text(font_, text, p);
        hud.build();
        rhi::PassAttachment color;
        color.load = rhi::LoadOp::Load;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "hud";
        cmd.begin_pass(pass);
        content_.renderer.record_2d(cmd, hud);
        cmd.end_pass();
    }

    void capture(const char* path, rhi::Extent2D size) {
        Array<u8> px;
        px.resize(usize(size.width) * size.height * 4);
        if (!device_->read_pixels({}, rhi::Rect2D{0, 0, size.width, size.height}, px.span())) return;
        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            MCE_LOG_ERROR("06_showcase: cannot write '{}'", path);
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", size.width, size.height);
        for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
        MCE_LOG_INFO("06_showcase: wrote {}", path);
    }

    static void on_event(const platform::Event& e, void* user) {
        auto* self = static_cast<ShowcaseApp*>(user);
        using platform::EventType;
        using platform::Key;
        if (e.type != EventType::KeyDown && e.type != EventType::KeyUp) return;
        const bool down = e.type == EventType::KeyDown;
        switch (static_cast<Key>(e.code)) {
            case Key::Escape: if (down) self->quit(0); break;
            case Key::Space: if (down) self->auto_orbit_ = !self->auto_orbit_; break;
            case Key::S:
                if (down) {
                    self->shot_ = (self->shot_ + 1) % kShotCount;
                    self->hour_ = kShots[self->shot_].hour;
                }
                break;
            case Key::H: if (down) self->hour_ = std::fmod(self->hour_ + 1.0f, 24.0f); break;
            case Key::Left: self->key_yaw_ = down ? -1 : 0; break;
            case Key::Right: self->key_yaw_ = down ? 1 : 0; break;
            default: break;
        }
    }

    platform::Window* window_;
    rhi::RenderDevice* device_;
    Profile profile_;
    Content content_;
    Layout layout_;
    Font font_;
    u32 shot_ = 0;
    f32 hour_ = 18.0f;
    f32 time_ = 0.0f, prev_time_ = 0.0f, last_render_t_ = 0.0f;
    f32 yaw_ = 0.0f, prev_yaw_ = 0.0f;
    bool auto_orbit_ = false;
    i32 key_yaw_ = 0;
    u64 hash_ = kFnv64Offset;
    u64 ticks_ = 0;
};

// ---- the side-by-side sheet ------------------------------------------------------------------------

// Every shot (rows) under every profile (columns), each settled for -sheet.frames frames at 60 Hz into
// an offscreen target, then the per-stage GPU means of each profile over the settled frames' second half.
int run_sheet(rhi::RenderDevice& device) {
    // No App loop here: the sheet drives its own frame slots for the per-frame arena.
    mem::FrameSlots slots(mem::heap());
    Array<Profile> profiles;
    {
        const StringView list = cv_sheet_profiles.get_string();
        usize start = 0;
        for (usize i = 0; i <= list.size(); ++i) {
            if (i < list.size() && list[i] != ',') continue;
            Profile p;
            if (parse_profile(list.substr(start, i - start), p)) profiles.push_back(p);
            start = i + 1;
        }
    }
    if (profiles.empty()) {
        MCE_LOG_ERROR("06_showcase: -sheet.profiles names no profile");
        return 1;
    }
    const u32 w = static_cast<u32>(clamp<i64>(cv_sheet_w.get_int(), 64, 1920));
    const u32 h = static_cast<u32>(clamp<i64>(cv_sheet_h.get_int(), 64, 1080));
    const u32 frames = static_cast<u32>(clamp<i64>(cv_sheet_frames.get_int(), 2, 600));
    const u32 cols = static_cast<u32>(profiles.size());
    Array<u8> sheet;
    sheet.resize(usize(w) * cols * h * kShotCount * 3, 0);
    Array<u8> px;
    px.resize(usize(w) * h * 4);
    struct Times {
        f64 clusters = 0, scale = 0, ssao = 0, ssr = 0, contact = 0, shadow = 0, gi = 0, vol = 0, main = 0, resolve = 0, taa = 0, post_fx = 0, bloom = 0,
            tonemap = 0, fxaa = 0, total = 0;
        u32 n = 0;
    };
    Array<Times> times;
    times.resize(cols);
    u64 frame = 0;
    for (u32 col = 0; col < cols; ++col) {
        const Profile profile = profiles[col];
        Content content;
        slots.begin_frame(frame++);
        if (ResultV r = content.load(&device, profile); !r) {
            FixedString<512> msg;
            format_error(r.error(), msg);
            MCE_LOG_ERROR("06_showcase: {}", msg.view());
            return 1;
        }
        Layout layout;
        layout.build(content);
        slots.end_frame(nullptr);
        rhi::TextureDesc td;
        td.size = {w, h};
        td.usage = rhi::TextureUsage::SampledColorTarget;
        td.debug_name = "06_showcase.sheet_cell";
        const rhi::TextureHandle target = device.create_texture(td);
        for (u32 s = 0; s < kShotCount; ++s) {
            const Shot& shot = kShots[s];
            for (u32 f = 0; f < frames; ++f) {
                const f32 t = f32(f) / 60.0f;
                slots.begin_frame(frame);
                device.begin_frame(frame++);
                Scene3D scene(*content.meshes, mem::frame());
                fill_scene(scene, content, layout, profile, shot, shot.hour, 0.0f, t, 1.0f / 60.0f, {w, h});
                rhi::CommandBuffer cmd(mem::frame());
                content.renderer.record_3d(cmd, scene, target);
                device.submit(cmd);
                if (f + 1 == frames) {
                    if (!device.read_pixels(target, rhi::Rect2D{0, 0, w, h}, px.span())) return 1;
                    for (u32 y = 0; y < h; ++y)
                        for (u32 x = 0; x < w; ++x) {
                            const usize src = (usize(y) * w + x) * 4;
                            const usize dst = ((usize(s) * h + y) * (usize(w) * cols) + usize(col) * w + x) * 3;
                            std::memcpy(&sheet[dst], &px[src], 3);
                        }
                }
                device.end_frame();
                content.assets.end_frame();
                slots.end_frame(nullptr);
                const Forward3DGpuTimes g = content.renderer.forward_3d().gpu_times(device.last_gpu_timestamps());
                if (f >= frames / 2 && g.total >= 0.0) {
                    Times& tm = times[col];
                    tm.clusters += g.clusters;
                    tm.scale += max(g.scale, 0.0);
                    tm.ssao += max(g.ssao, 0.0);
                    tm.ssr += max(g.ssr, 0.0);
                    tm.contact += max(g.contact, 0.0);
                    tm.shadow += g.shadow;
                    tm.gi += max(g.gi, 0.0);
                    tm.vol += max(g.volumetrics, 0.0);
                    tm.main += g.main;
                    tm.resolve += max(g.resolve, 0.0);
                    tm.taa += max(g.taa, 0.0);
                    tm.post_fx += max(g.post_fx, 0.0);
                    tm.bloom += max(g.bloom, 0.0);
                    tm.tonemap += g.tonemap;
                    tm.fxaa += max(g.fxaa, 0.0);
                    tm.total += g.total;
                    ++tm.n;
                }
            }
            MCE_LOG_INFO("06_showcase: {} / {} done", profile_name(profile), shot.name);
        }
        device.destroy_texture(target);
        content.unload();
    }
    const char* path = cv_sheet.get_string().data();
    std::FILE* fp = std::fopen(path, "wb");
    if (!fp) {
        MCE_LOG_ERROR("06_showcase: cannot write '{}'", path);
        return 1;
    }
    std::fprintf(fp, "P6\n%u %u\n255\n", w * cols, h * kShotCount);
    std::fwrite(sheet.data(), 1, sheet.size(), fp);
    std::fclose(fp);
    std::printf("06_showcase: sheet %s, %u x %u cells (rows:", path, w, h);
    for (const Shot& s : kShots) std::printf(" %s", s.name);
    std::printf("; columns:");
    for (Profile p : profiles) std::printf(" %s", profile_name(p));
    std::printf(")\n  GPU ms per stage, %u x %u, means over the settled frames of every shot\n  | stage |", w, h);
    for (Profile p : profiles) std::printf(" %s |", profile_name(p));
    std::printf("\n  |---|");
    for (u32 i = 0; i < cols; ++i) std::printf("---|");
    std::printf("\n");
    auto row = [&](const char* name, f64 Times::*field) {
        std::printf("  | %s |", name);
        for (const Times& tm : times) std::printf(" %.2f |", tm.n ? tm.*field / f64(tm.n) : 0.0);
        std::printf("\n");
    };
    row("Forward+ light assignment", &Times::clusters);
    row("scale: occluders, culling, particles", &Times::scale);
    row("SSAO", &Times::ssao);
    row("SSR", &Times::ssr);
    row("contact shadows", &Times::contact);
    row("shadows: cascades + local lights", &Times::shadow);
    row("GI volume update", &Times::gi);
    row("volumetric fog", &Times::vol);
    row("main pass", &Times::main);
    row("MSAA resolve", &Times::resolve);
    row("velocity + TAA", &Times::taa);
    row("post effects (exposure histogram)", &Times::post_fx);
    row("bloom", &Times::bloom);
    row("tonemap", &Times::tonemap);
    row("FXAA", &Times::fxaa);
    row("3D frame total", &Times::total);
    return 0;
}

// ---- entry ---------------------------------------------------------------------------------------

int fail(const char* what, const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    FixedString<640> text;
    text.append(what);
    text.append(": ");
    text.append(msg.view());
    platform::show_error_box("06_showcase", text.c_str());
    return 1;
}

int run_headless(Profile profile) {
    ShowcaseApp app(nullptr, nullptr, profile);
    app::AppDesc desc;
    desc.name = "06_showcase";
    desc.headless = true;
    desc.max_ticks = static_cast<u64>(cv_ticks.get_int() > 0 ? cv_ticks.get_int() : 300);
    const int rc = app::run(app, desc);
    app.report_headless();
    return rc;
}

int run_windowed(Profile profile) {
    platform::WindowDesc wd;
    wd.title = "MCE - 06_showcase";
    wd.width = 1280;
    wd.height = 720;
    auto window = platform::Window::create(wd);
    if (!window) return fail("window creation failed", window.error());
    rhi::DeviceDesc dd;
    dd.debug_layer = cv_gl_debug.get_bool();
    auto device = rhi::create_default_device(*window.value(), dd);
    if (!device) return fail("render device creation failed", device.error());
    int rc = 0;
    if (!cv_sheet.get_string().empty()) {
        rc = run_sheet(*device.value());
    } else {
        ShowcaseApp app(window.value().get(), device.value().get(), profile);
        app::AppDesc desc;
        desc.name = "06_showcase";
        desc.platform = &app;
        desc.fences = device.value().get();
        app::RunStats stats;
        rc = app::run(app, desc, &stats);
        std::printf("06_showcase: %llu frames\n", static_cast<unsigned long long>(stats.frames));
    }
    const u32 gl_errors = device.value()->validation_error_count();
    std::printf("06_showcase: %u GL errors\n", gl_errors);
    if (rc == 0 && gl_errors != 0) rc = 2;
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    log::LogDesc log_desc;
    log_desc.app_name = "06_showcase";
    log::init(log_desc);
    console::parse_command_line(argc, argv);
    Profile profile = Profile::High;
    if (!parse_profile(cv_profile.get_string(), profile)) MCE_LOG_WARN("06_showcase: unknown -profile, using high");

    const ConVar* headless = ConVar::find(StringId("headless"));
    const bool is_headless = headless && headless->get_bool();
    int rc = 1;
    if (is_headless) {
        rc = run_headless(profile);
    } else if (ResultV r = platform::init(); !r) {
        rc = fail("platform init failed", r.error());
    } else {
        rc = run_windowed(profile);
        platform::shutdown();
    }
    log::shutdown();
    return rc;
}
