// 04_cubes — the M5a demo (ROADMAP M5a): the 3D renderer made real.
//
//   * a PBR material sphere row (dielectric and metal, roughness 0 -> 1, Cook-Torrance GGX)
//   * a tiled floor and panel cubes with base color + normal + ORM + emissive maps
//   * 256 moving point lights through Forward+ CPU-clustered lighting
//   * a low sun with 3 texel-snapped shadow cascades, the sky dome, bloom, ACES, FXAA
//   * an orrery animated by multi-node glTF tracks, cross-fading between two clips
//   * three reeds skinned to 4-joint chains (FR-R3D-060, optional skinning): G turns their
//     skeletons off, and they stand rigid in their bind pose
//   * the per-stage frame-budget table (CPU from the clock, GPU from timestamp queries)
//   * M5b (-walk 1, or C): a capsule character walks the floor through World3D::move_character
//     (mce_physics3d over Jolt), followed by a third-person boom that World3D::camera_sweep pulls in
//     front of any panel cube it would pass through. Without input the character patrols on its own.
//
//   04_cubes                              run until the window closes
//   04_cubes -frames 300                  run 300 frames, then print the budget table
//   04_cubes -frames 60 -capture a.ppm    also write the last frame as a PPM image
//   04_cubes -headless 1 -ticks 300       determinism run: no GL, CPU mesh store, prints the hash
//   04_cubes -headless 1 -walk 1          the same with the capsule walk (physics + camera in the hash)
//   04_cubes -skin 0                      the reeds without their skeletons (rigid bind pose)
//   04_cubes -r.sky_ibl 1                 M-VISUAL 2: image-based lighting from the sky
//   04_cubes -gi 1                        M-VISUAL 6: an irradiance-probe volume (global illumination)
//   04_cubes -atmos 1 -hour 17.5          M-VISUAL 7: the physically based sky, the sun at 17:30
//   04_cubes -volfog 1                    M-VISUAL 7: froxel volumetric fog (sun shafts, the lights' glow)
//
// Keys: drag (left mouse) or arrows orbit, wheel or Z/X zoom, Space auto orbit, L lights,
// B bloom, A FXAA, 1/2/3 cascades, K sky texture, I image-based lighting, N night, P cross-fade the orrery clip, G reed
// skeletons on/off, Esc quits.
// Walk mode (C toggles): arrows move relative to the camera, Space jumps, drag orbits the boom.

#include <cmath>
#include <cstdio>

#include "hud_font.h"
#include "mce/asset/asset_system.h"
#include "mce/core.h"
#include "mce/physics3d/world3d.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/asset_upload.h"
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
MCE_CVAR(cv_msaa, "r.msaa", 4, "3D MSAA samples (1 = off)", CVarFlags::RequiresRestart);
MCE_CVAR(cv_lights, "lights", 256, "Point lights (Forward+)", CVarFlags::None);
MCE_CVAR(cv_cascades, "r.cascades", 3, "Sun shadow cascades (1-4)", CVarFlags::None);
MCE_CVAR(cv_bloom, "r.bloom", true, "Bloom", CVarFlags::None);
MCE_CVAR(cv_fxaa, "r.fxaa", true, "FXAA", CVarFlags::None);
MCE_CVAR(cv_sky_texture, "r.sky_texture", false, "Equirect sky texture instead of the gradient dome", CVarFlags::None);
MCE_CVAR(cv_sky_ibl, "r.sky_ibl", false, "M-VISUAL 2: the sky lights the scene (SH9 irradiance + pre-filtered reflections)",
         CVarFlags::None);
MCE_CVAR(cv_shadow_lights, "shadow_lights", 0, "M-VISUAL 4: the first N point lights ask for cube shadows", CVarFlags::None);
MCE_CVAR(cv_taa, "taa", false, "M-VISUAL 5: temporal anti-aliasing (replaces FXAA)", CVarFlags::None);
MCE_CVAR(cv_ao, "ao", false, "M-VISUAL 3: screen-space ambient occlusion", CVarFlags::None);
MCE_CVAR(cv_gi, "gi", false, "M-VISUAL 6: an irradiance-probe volume over the cubes", CVarFlags::None);
MCE_CVAR(cv_atmosphere, "atmos", false, "M-VISUAL 7: physically based sky (the sun's color, the ambient and the dome)",
         CVarFlags::None);
MCE_CVAR(cv_hour, "hour", -1.0f, "M-VISUAL 7: the sun at this hour of a simple day (-1 = the fixed evening sun)", CVarFlags::None);
MCE_CVAR(cv_volfog, "volfog", false, "M-VISUAL 7: froxel volumetric fog lit by the sun and the point lights", CVarFlags::None);
MCE_CVAR(cv_night, "night", false, "Dim the sun so the point lights carry the scene", CVarFlags::None);
MCE_CVAR(cv_yaw, "cam.yaw", 0.35f, "Initial orbit yaw (radians)", CVarFlags::None);
MCE_CVAR(cv_pitch, "cam.pitch", 0.42f, "Initial orbit pitch (radians, down from the horizon)", CVarFlags::None);
MCE_CVAR(cv_dist, "cam.dist", 12.0f, "Initial orbit distance (meters)", CVarFlags::None);
MCE_CVAR(cv_orbit, "cam.auto", true, "Slow automatic orbit", CVarFlags::None);
MCE_CVAR(cv_walk, "walk", false, "M5b: start in walk mode (capsule character + swept third-person camera)", CVarFlags::None);
MCE_CVAR(cv_skin, "skin", true, "FR-R3D-060: the reeds use their skeletons (G toggles; off = rigid bind pose)", CVarFlags::None);

enum Model : u32 { SphereRow, Floor, PanelCube, Orrery, Bulb, Walker, Reed, kModelCount };
constexpr const char* kModelPaths[kModelCount] = {"models/sphere_row.glb", "models/floor.glb", "models/panel_cube.glb",
                                                  "models/orrery.glb",     "models/bulb.glb",  "models/walker.glb",
                                                  "models/reed.glb"};
// The reeds: behind the orrery, each a third of the sway clip out of phase.
constexpr vec3 kReeds[] = {{-1.1f, 0.0f, 4.4f}, {0.0f, 0.0f, 4.7f}, {1.1f, 0.0f, 4.4f}};
constexpr u32 kReedCount = sizeof(kReeds) / sizeof(kReeds[0]);

// M5b walk: World3D's default controller capsule, a 4 m/s walk, the boom 5 m behind the head.
constexpr f32 kWalkSpeed = 4.0f;
constexpr f32 kJumpSpeed = 5.0f;
constexpr f32 kGravity = 9.81f;
constexpr f32 kBoomLength = 5.0f;
constexpr f32 kBoomPitch = 0.22f;
constexpr f32 kBoomRadius = 0.2f;
constexpr vec3 kHeadOffset{0.0f, 0.6f, 0.0f};  // boom pivot above the capsule centre
// The autopilot's patrol: the lanes between the panel cubes, then across the middle past the orrery.
constexpr vec3 kPatrol[] = {{-6.4f, 0, 6.4f}, {6.4f, 0, 6.4f}, {6.4f, 0, 0.0f},   {-6.4f, 0, 0.0f},
                            {-6.4f, 0, -6.4f}, {6.4f, 0, -6.4f}, {6.4f, 0, 0.0f}, {-6.4f, 0, 0.0f}};

Entity body_id(u32 i) { return Entity{i, 1}; }
constexpr u32 kCharacterId = 1;
constexpr const char* kSkyPath = "textures/sky_equirect.png";

f64 now_ms() { return platform::now_seconds() * 1000.0; }

u32 hash_u32(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    return x ^ (x >> 16);
}
f32 hash01(u32 x) { return f32(hash_u32(x) & 0xFFFFu) / 65535.0f; }

vec3 hue(f32 h) {
    const f32 r = std::fabs(h * 6.0f - 3.0f) - 1.0f, g = 2.0f - std::fabs(h * 6.0f - 2.0f), b = 2.0f - std::fabs(h * 6.0f - 4.0f);
    return {clamp(r, 0.0f, 1.0f), clamp(g, 0.0f, 1.0f), clamp(b, 0.0f, 1.0f)};
}

// The point lights: a 16 × 16 grid of anchors over the floor, each light circling its anchor.
struct LightRig {
    vec3 anchor;
    vec3 color;
    f32 orbit, speed, phase;
};

// Means of the per-stage times over the measured frames (NFR-F-01 table shape).
struct Budget {
    f64 scene = 0, clusters = 0, record = 0, submit = 0;
    f64 gpu_clusters = 0, gpu_ssao = 0, gpu_gi = 0, gpu_vol = 0, gpu_taa = 0, gpu_shadow = 0, gpu_main = 0, gpu_resolve = 0, gpu_bloom = 0, gpu_tonemap = 0, gpu_fxaa = 0, gpu_total = 0;
    u64 frames = 0, gpu_frames = 0;
};

class CubesApp final : public app::Application, public app::IPlatformHooks {
public:
    CubesApp(platform::Window* window, rhi::RenderDevice* device) : window_(window), device_(device), sink_(renderer_, assets_) {}

    ResultV on_init(app::Context&) override {
        if (device_) {
            MCE_TRY(renderer_.init(*device_));
            Forward3DDesc fd;
            fd.msaa = static_cast<u32>(math::clamp<i64>(cv_msaa.get_int(), 1, 8));
            MCE_TRY(renderer_.init_3d(fd));
            assets_.set_upload_sink(&sink_);
            meshes_ = &renderer_.meshes();
        } else {
            cpu_meshes_.init(nullptr);
            meshes_ = &cpu_meshes_;
        }
        FixedString<1024> path;
        resolve_shipped_path(cv_pack.get_string(), "assets.mce", MCE_SAMPLE_DEV_PACK, path);
        const StringView roots[1] = {path.view()};
        asset::MountOptions mo;
        mo.verify_payload_crc = false;
        MCE_TRY(assets_.mount(Span<const StringView>(roots, 1), mo));

        // Textures first: a mesh upload resolves its materials' image GUIDs to resident textures.
        for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k) {
            if (assets_.info(k).type != asset::ChunkType::Texture) continue;
            MCE_TRY_ASSIGN(asset::AssetHandle<asset::Texture> h, assets_.load_sync<asset::Texture>(k));
            handles_.push_back(asset::AnyHandle(h));
            if (assets_.info(k).path == StringView(kSkyPath)) sky_key_ = k;
        }
        for (u32 m = 0; m < kModelCount; ++m) {
            const asset::AssetKey k = find_path(kModelPaths[m]);
            if (k == ~0u) return Error(Errc::NotFound, "04_cubes: model missing from the pack", kModelPaths[m]);
            MCE_TRY_ASSIGN(asset::AssetHandle<asset::Mesh> h, assets_.load_sync<asset::Mesh>(k));
            handles_.push_back(asset::AnyHandle(h));
            if (!device_) MCE_TRY(cpu_meshes_.upload(k, assets_.generation(k), assets_.resolve(h)));
            keys_[m] = k;
        }
        const GpuMesh* orrery = meshes_->get(keys_[Orrery]);
        if (!orrery) return Error(Errc::NotFound, "04_cubes: orrery mesh not resident");
        spin_clip_ = orrery->find_clip("spin");
        pulse_clip_ = orrery->find_clip("pulse");
        orrery_pose_.resize(orrery->nodes.size());
        player_.play(spin_clip_);
        const GpuMesh* reed = meshes_->get(keys_[Reed]);
        if (!reed || !reed->skinned()) return Error(Errc::NotFound, "04_cubes: skinned reed mesh not resident");
        sway_clip_ = reed->find_clip("sway");
        reed_pose_.resize(reed->nodes.size() * kReedCount);
        reed_skinning_ = cv_skin.get_bool();

        build_layout();
        build_physics();
        walking_ = cv_walk.get_bool();
        yaw_ = prev_yaw_ = cv_yaw.get_float();
        pitch_ = cv_pitch.get_float();
        distance_ = cv_dist.get_float();
        auto_orbit_ = cv_orbit.get_bool();
        if (device_) {
            HudFont hud;
            MCE_TRY_ASSIGN(font_, renderer_.create_font(hud.metrics, hud.glyphs.span(), {}, std::as_bytes(hud.atlas.span()),
                                                        "04_cubes.hud_font"));
            window_->set_event_handler(&CubesApp::on_event, this);
        }
        return ok();
    }

    void on_tick(const app::TickContext& t) override {
        const f32 dt = static_cast<f32>(t.dt);
        prev_time_ = time_;
        time_ += dt;
        prev_yaw_ = yaw_;
        if (walking_) {
            walk(dt);
        } else {
            if (auto_orbit_) yaw_ += 0.1f * dt;
            yaw_ += static_cast<f32>(key_yaw_) * 1.2f * dt;
            pitch_ = clamp(pitch_ + static_cast<f32>(key_pitch_) * 0.9f * dt, 0.08f, 1.45f);
        }
        // The orrery alternates its clips every 6 s with a 0.8 s cross-fade (or on P).
        if (ticks_ % 360 == 359 || toggle_clip_) {
            player_.play(player_.clip() == spin_clip_ ? pulse_clip_ : spin_clip_, 0.8f);
            toggle_clip_ = false;
        }
        player_.advance(dt);
        if (!device_) {
            Scene3D scene(*meshes_, mem::heap());
            fill_scene(scene, 1.0f, {1280, 720});
            fold(scene);
            budget_.clusters += scene.stats().cluster_ms;
            budget_.scene += scene.stats().build_ms;
            ++budget_.frames;
        }
        ++ticks_;
    }

    void on_render(const app::RenderContext& rc) override {
        if (!device_) return;
        device_->begin_frame(rc.frame_index);
        const rhi::Extent2D bb = device_->backbuffer_size();
        const f64 t0 = now_ms();
        Scene3D scene(*meshes_, mem::frame());
        fill_scene(scene, static_cast<f32>(rc.alpha), bb);
        const f64 t1 = now_ms();
        rhi::CommandBuffer cmd(mem::frame());
        renderer_.record_3d(cmd, scene);
        record_hud(cmd, scene, bb);
        const f64 t2 = now_ms();
        device_->submit(cmd);
        const f64 t3 = now_ms();
        last_ = {t1 - t0, f64(scene.stats().cluster_ms), t2 - t1, t3 - t2};
        gpu_ = renderer_.forward_3d().gpu_times(device_->last_gpu_timestamps());
        if (rc.frame_index >= 10) {  // skip warm-up (pipeline compiles, first uploads)
            budget_.scene += last_.scene;
            budget_.clusters += last_.clusters;
            budget_.record += last_.record;
            budget_.submit += last_.submit;
            ++budget_.frames;
            if (gpu_.total >= 0.0) {
                budget_.gpu_clusters += gpu_.clusters;
                budget_.gpu_ssao += gpu_.ssao;
                budget_.gpu_taa += gpu_.taa;
                budget_.gpu_gi += gpu_.gi;
                budget_.gpu_vol += gpu_.volumetrics;
                budget_.gpu_shadow += gpu_.shadow;
                budget_.gpu_main += gpu_.main;
                budget_.gpu_resolve += gpu_.resolve;
                budget_.gpu_bloom += gpu_.bloom;
                budget_.gpu_tonemap += gpu_.tonemap;
                budget_.gpu_fxaa += gpu_.fxaa;
                budget_.gpu_total += gpu_.total;
                ++budget_.gpu_frames;
            }
        }
        const bool last = cv_frames.get_int() > 0 && rc.frame_index + 1 >= static_cast<u64>(cv_frames.get_int());
        if (last && !cv_capture.get_string().empty()) capture(cv_capture.get_string().data(), bb);
        device_->end_frame();
        assets_.end_frame();
        stats_ = scene.stats();
        if (last) quit(0);
    }

    void on_shutdown() override {
        for (asset::AnyHandle h : handles_) assets_.release(h);
        handles_.clear();
        if (device_) renderer_.shutdown();
    }

    bool pump_events() override { return platform::pump_events(); }
    void present(bool vsync) override { device_->present(vsync); }

    // The NFR-F-01 per-stage table (milliseconds, means over the measured frames).
    void report_budget() const {
        if (budget_.frames == 0) return;
        const f64 n = f64(budget_.frames);
        std::printf("04_cubes: frame budget over %llu frames (%u point lights, %u cascades)\n",
                    static_cast<unsigned long long>(budget_.frames), stats_.point_lights, cascades_);
        std::printf("  | stage | CPU ms | GPU ms |\n  |---|---|---|\n");
        std::printf("  | scene build (anim + cull + sort + cascades + clusters) | %.3f | - |\n", budget_.scene / n);
        std::printf("  | of which Forward+ cluster build (CPU path / setup) | %.3f | - |\n", budget_.clusters / n);
        std::printf("  | command recording (3D + HUD) | %.3f | - |\n", budget_.record / n);
        std::printf("  | GL translate + submit | %.3f | - |\n", budget_.submit / n);
        if (budget_.gpu_frames > 0) {
            const f64 g = f64(budget_.gpu_frames);
            std::printf("  | Forward+ light assignment (compute) | - | %.3f |\n", budget_.gpu_clusters / g);
            std::printf("  | SSAO (prepass + occlusion + blur) | - | %.3f |\n", budget_.gpu_ssao / g);
            std::printf("  | shadows (cascades + %u local lights, %u tiles) | - | %.3f |\n", stats_.local_shadow_lights,
                        stats_.local_shadow_tiles, budget_.gpu_shadow / g);
            std::printf("  | GI volume update (sun fit + captures + projection) | - | %.3f |\n", budget_.gpu_gi / g);
            std::printf("  | volumetric fog (froxel injection + integration) | - | %.3f |\n", budget_.gpu_vol / g);
            std::printf("  | main pass (Forward+ PBR + sky) | - | %.3f |\n", budget_.gpu_main / g);
            std::printf("  | MSAA resolve | - | %.3f |\n", budget_.gpu_resolve / g);
            std::printf("  | TAA (velocity prepass + resolve) | - | %.3f |\n", budget_.gpu_taa / g);
            std::printf("  | post: bloom | - | %.3f |\n", budget_.gpu_bloom / g);
            std::printf("  | post: tonemap | - | %.3f |\n", budget_.gpu_tonemap / g);
            std::printf("  | post: FXAA | - | %.3f |\n", budget_.gpu_fxaa / g);
            std::printf("  | 3D frame total | - | %.3f |\n", budget_.gpu_total / g);
        }
    }
    void report_headless() const {
        const f64 n = f64(max<u64>(budget_.frames, 1));
        if (walking_) {
            std::printf("04_cubes: walk | %.1f m walked, %u jumps, %u grounded ticks, %u wall ticks, %u waypoints | "
                        "camera hits %u, closest boom %.2f m | physics %.3f ms (mean)\n",
                        f64(walked_), jumps_, grounded_ticks_, wall_ticks_, waypoints_reached_, camera_hits_,
                        f64(closest_boom_), physics_ms_ / n);
        }
        std::printf("04_cubes: %llu ticks | %u instances, %u visible submeshes, %u tris, %u point lights, %u light "
                    "indices | cluster build %.3f ms, scene build %.3f ms (mean) | hash %016llx\n",
                    static_cast<unsigned long long>(ticks_), stats_.instances, stats_.visible, stats_.triangles,
                    stats_.point_lights, light_indices_, budget_.clusters / n, budget_.scene / n,
                    static_cast<unsigned long long>(hash_));
    }

private:
    asset::AssetKey find_path(StringView path) const {
        for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k)
            if (assets_.info(k).path == path) return k;
        return ~0u;
    }

    void build_layout() {
        MeshInstance floor;
        floor.mesh = keys_[Floor];
        statics_.push_back(floor);
        MeshInstance row;
        row.mesh = keys_[SphereRow];
        row.world = translation({0.0f, 0.0f, -2.2f});
        statics_.push_back(row);
        // Panel cubes on a 6 × 6 grid, the middle left open for the spheres and the orrery.
        for (i32 j = 0; j < 6; ++j)
            for (i32 i = 0; i < 6; ++i) {
                if ((i == 2 || i == 3) && (j >= 1 && j <= 4)) continue;
                const u32 h = hash_u32(u32(i * 7 + j * 131));
                const f32 height = 0.6f + 1.8f * hash01(h);
                MeshInstance c;
                c.mesh = keys_[PanelCube];
                c.world = trs({(f32(i) - 2.5f) * 3.2f, 0.0f, (f32(j) - 2.5f) * 3.2f}, quat_from_axis_angle({0, 1, 0}, 0.4f * hash01(h + 1)),
                              {1.0f, height, 1.0f});
                statics_.push_back(c);
            }
        // Light anchors.
        const u32 n = static_cast<u32>(clamp<i64>(cv_lights.get_int(), 0, 4096));
        const u32 side = max(1u, static_cast<u32>(std::ceil(std::sqrt(f32(n)))));
        for (u32 i = 0; i < n; ++i) {
            LightRig r;
            const u32 gx = i % side, gz = i / side;
            const f32 u = (f32(gx) + 0.5f) / f32(side), v = (f32(gz) + 0.5f) / f32(side);
            r.anchor = {(u - 0.5f) * 24.0f, 0.3f + 0.5f * hash01(i * 3 + 1), (v - 0.5f) * 24.0f};
            r.color = hue(hash01(i * 3 + 2));
            r.orbit = 0.4f + 0.8f * hash01(i * 5 + 3);
            r.speed = (hash01(i * 5 + 4) - 0.5f) * 2.4f;
            r.phase = hash01(i * 7 + 5) * kTau;
            rigs_.push_back(r);
        }
    }

    // The physics mirror of build_layout(): floor, panel cubes, the sphere row and the orrery's
    // stand. Ids are creation order, so the World3D hash is stable.
    void build_physics() {
        u32 id = 100;
        auto add_box = [&](vec3 pos, vec3 half, quat rot) {
            physics::BodyDesc3D d;
            d.shape = physics::Shape3D::box(half);
            d.position = pos;
            d.rotation = rot;
            MCE_VERIFY(world_.create_body(body_id(id++), d));
        };
        add_box({0.0f, -0.5f, 0.0f}, {15.0f, 0.5f, 15.0f}, {});  // floor.glb: 30 m, top at y = 0
        for (i32 j = 0; j < 6; ++j)
            for (i32 i = 0; i < 6; ++i) {
                if ((i == 2 || i == 3) && (j >= 1 && j <= 4)) continue;
                const u32 h = hash_u32(u32(i * 7 + j * 131));
                const f32 height = 0.6f + 1.8f * hash01(h);
                add_box({(f32(i) - 2.5f) * 3.2f, 0.5f * height, (f32(j) - 2.5f) * 3.2f}, {0.5f, 0.5f * height, 0.5f},
                        quat_from_axis_angle({0, 1, 0}, 0.4f * hash01(h + 1)));
            }
        for (i32 row = 0; row < 2; ++row)  // sphere_row.glb at z = -2.2: radius 0.42, resting on the floor
            for (i32 i = 0; i < 7; ++i) {
                physics::BodyDesc3D d;
                d.shape = physics::Shape3D::sphere(0.42f);
                d.position = {f32(i - 3), 0.42f, -2.2f + 1.1f * f32(row)};
                MCE_VERIFY(world_.create_body(body_id(id++), d));
            }
        add_box({0.0f, 0.15f, 1.6f}, {0.6f, 0.15f, 0.6f}, {});  // orrery base + pillar (the arm swings free)
        add_box({0.0f, 0.9f, 1.6f}, {0.08f, 0.7f, 0.08f}, {});
        const physics::ControllerParams3D cp;
        char_pos_ = prev_char_pos_ = kPatrol[0] + vec3{0.0f, cp.half_height + cp.radius + cp.padding, 0.0f};
        MCE_VERIFY(world_.create_character(body_id(kCharacterId), char_pos_, cp));
        heading_ = 0.5f * kPi;  // facing +x, the first patrol leg
        pivot_ = prev_pivot_ = char_pos_ + kHeadOffset;
    }

    // One fixed tick of the M5b walk: input (or the autopilot) -> move_character -> step -> the boom.
    void walk(f32 dt) {
        const f64 t0 = now_ms();
        prev_char_pos_ = char_pos_;
        prev_pivot_ = pivot_;
        prev_boom_ = boom_;
        prev_heading_ = heading_;
        // Camera-relative input; with none held the autopilot walks the patrol.
        const vec3 cam_fwd{-std::sin(yaw_), 0.0f, -std::cos(yaw_)}, cam_right{std::cos(yaw_), 0.0f, -std::sin(yaw_)};
        vec3 dir = cam_fwd * f32(key_pitch_) + cam_right * f32(key_yaw_);
        const bool manual = key_pitch_ != 0 || key_yaw_ != 0;
        if (!manual) {
            vec3 to = kPatrol[waypoint_] - vec3{char_pos_.x, 0.0f, char_pos_.z};
            if (length(to) < 0.5f || ticks_ - waypoint_tick_ > 600) {  // reached, or stuck for 10 s
                waypoint_ = (waypoint_ + 1) % u32(sizeof(kPatrol) / sizeof(kPatrol[0]));
                waypoint_tick_ = ticks_;
                ++waypoints_reached_;
                to = kPatrol[waypoint_] - vec3{char_pos_.x, 0.0f, char_pos_.z};
            }
            dir = to;
            if (ticks_ % 150 == 75) jump_ = true;  // a hop every 2.5 s
        }
        const bool grounded = (last_move_.flags & physics::ControllerFlags3D::Ground) != 0;
        const f32 len = length(dir);
        vec3 v{};
        if (len > 1e-4f) {
            v = dir * (kWalkSpeed / len);
            // Turn toward the walk direction at 10 rad/s (the shortest way round).
            f32 d = std::atan2(v.x, v.z) - heading_;
            while (d > kPi) d -= kTau;
            while (d < -kPi) d += kTau;
            heading_ += clamp(d, -10.0f * dt, 10.0f * dt);
        }
        if (grounded) {
            vy_ = 0.0f;
            if (jump_) {
                vy_ = kJumpSpeed;
                ++jumps_;
            }
        }
        jump_ = false;
        vy_ -= kGravity * dt;
        v = v + last_move_.ground_velocity;
        last_move_ = world_.move_character(body_id(kCharacterId), {v.x * dt, vy_ * dt, v.z * dt});
        if (last_move_.hit_ceiling && vy_ > 0.0f) vy_ = 0.0f;
        world_.step(dt);
        const vec3 moved = last_move_.position - char_pos_;
        walked_ += length(vec3{moved.x, 0.0f, moved.z});
        char_pos_ = last_move_.position;
        if (last_move_.flags & physics::ControllerFlags3D::Ground) ++grounded_ticks_;
        if (last_move_.flags & physics::ControllerFlags3D::Wall) ++wall_ticks_;

        // The autopilot's boom trails the heading (it swings across the cubes on every turn).
        if (!manual && !dragging_) {
            f32 d = (heading_ + kPi) - yaw_;
            while (d > kPi) d -= kTau;
            while (d < -kPi) d += kTau;
            yaw_ += d * min(1.0f, 2.0f * dt);
            pitch_ = kBoomPitch;
        }
        // UC-B camera gate: sweep the boom sphere from the head to the wanted eye; on a hit the eye
        // sits at the hit centre, still on the boom, so the orientation is unchanged.
        pivot_ = char_pos_ + kHeadOffset;
        const vec3 want = orbit_camera(pivot_, yaw_, pitch_, kBoomLength).position;
        boom_ = kBoomLength;
        if (std::optional<physics::ShapeHit3D> hit = world_.camera_sweep(pivot_, want, kBoomRadius)) {
            boom_ = length(hit->position - pivot_);
            if (boom_ < kBoomLength - 0.05f) ++camera_hits_;  // pulled in (not just grazing at full length)
            closest_boom_ = min(closest_boom_, boom_);
        }
        physics_ms_ += now_ms() - t0;
    }

    Camera3D camera(f32 alpha) const {
        const f32 yaw = math::lerp(prev_yaw_, yaw_, alpha);
        if (walking_) {
            // Interpolate the pivot and boom length, then rebuild: the eye stays on the swept boom.
            const vec3 pivot = math::lerp(prev_pivot_, pivot_, alpha);
            Camera3D c = orbit_camera(pivot, yaw, pitch_, math::lerp(prev_boom_, boom_, alpha), radians(60.0f));
            c.near_z = 0.05f;
            c.far_z = 120.0f;
            return c;
        }
        Camera3D c = orbit_camera({0.0f, 0.6f, 0.0f}, yaw, pitch_, distance_, radians(50.0f));
        c.far_z = 120.0f;
        return c;
    }

    void fill_scene(Scene3D& scene, f32 alpha, rhi::Extent2D viewport) {
        const f32 t = math::lerp(prev_time_, time_, alpha);
        scene.begin(camera(alpha), viewport);
        const bool night = cv_night.get_bool();
        DirectionalLight sun;
        sun.direction = normalize(vec3{-0.55f, -0.42f, -0.72f});  // low evening sun: long cascaded shadows
        sun.color = {1.0f, 0.86f, 0.68f};
        if (cv_hour.get_float() >= 0.0f) sun.direction = sun_direction_at(cv_hour.get_float(), 60.0f, 35.0f);
        if (cv_atmosphere.get_bool()) sun.color = {1.0f, 1.0f, 1.0f};  // above the air: the atmosphere tints it
        sun.intensity = night ? 0.05f : 2.2f;
        sun.ambient_sky = night ? vec3{0.02f, 0.025f, 0.04f} : vec3{0.22f, 0.27f, 0.36f};
        sun.ambient_ground = night ? vec3{0.01f, 0.01f, 0.01f} : vec3{0.12f, 0.10f, 0.09f};
        scene.set_light(sun);
        Atmosphere atmosphere;
        atmosphere.enabled = cv_atmosphere.get_bool();
        scene.set_atmosphere(atmosphere);
        Volumetrics volumetrics;
        volumetrics.enabled = cv_volfog.get_bool();
        scene.set_volumetrics(volumetrics);
        Sky sky;
        if (night) {
            sky.zenith = {0.01f, 0.012f, 0.03f};
            sky.horizon = {0.04f, 0.04f, 0.06f};
            sky.ground = {0.01f, 0.01f, 0.01f};
            sky.sun_intensity = 0.0f;
        }
        if (cv_sky_texture.get_bool() && sky_key_ != ~0u) sky.texture = sky_key_;
        sky.ibl = cv_sky_ibl.get_bool();
        scene.set_sky(sky);
        AmbientOcclusion ao;
        ao.enabled = cv_ao.get_bool();
        scene.set_ambient_occlusion(ao);
        if (cv_gi.get_bool()) {
            // 7 × 3 × 7 probes between the panel cubes (their centers sit on a 3.2 m grid offset by 1.6).
            GiVolume gi;
            gi.enabled = true;
            gi.bounds = {{-9.6f, 0.3f, -9.6f}, {9.6f, 3.3f, 9.6f}};
            gi.counts[0] = 7;
            gi.counts[1] = 3;
            gi.counts[2] = 7;
            gi.probes_per_frame = 7;  // a sweep of the 147 probes every 21 frames
            scene.set_gi_volume(gi);
        }
        PostFx post;
        post.bloom = cv_bloom.get_bool();
        post.fxaa = cv_fxaa.get_bool();
        post.taa = cv_taa.get_bool();
        scene.set_post(post);
        ShadowSettings shadows;
        cascades_ = static_cast<u32>(clamp<i64>(cv_cascades.get_int(), 1, 4));
        shadows.cascades = cascades_;
        shadows.distance = 40.0f;
        shadows.splits[0] = 0.12f;
        shadows.splits[1] = 0.35f;
        scene.set_shadows(shadows);

        for (const MeshInstance& i : statics_) scene.add(i);

        const GpuMesh& orrery = *meshes_->get(keys_[Orrery]);
        player_.pose(orrery, orrery_pose_.span());
        MeshInstance o;
        o.mesh = keys_[Orrery];
        o.world = translation({0.0f, 0.0f, 1.6f});
        o.pose = orrery_pose_.span();
        scene.add(o);

        // The reeds: one pose per instance, the joints driven by the "sway" clip like any nodes.
        const GpuMesh& reed = *meshes_->get(keys_[Reed]);
        const usize nodes = reed.nodes.size();
        for (u32 r = 0; r < kReedCount; ++r) {
            const Span<mat4> pose = reed_pose_.span().subspan(r * nodes, nodes);
            const f32 duration = sway_clip_ < reed.clips.size() ? reed.clips[sway_clip_].duration : 1.0f;
            pose_nodes(reed, sway_clip_, t + duration * f32(r) / f32(kReedCount), pose);
            MeshInstance m;
            m.mesh = keys_[Reed];
            m.world = translation(kReeds[r]);
            m.pose = pose;
            m.skinning = reed_skinning_;
            scene.add(m);
        }

        if (walking_) {
            MeshInstance w;
            w.mesh = keys_[Walker];
            w.world = trs(math::lerp(prev_char_pos_, char_pos_, alpha),
                          quat_from_axis_angle({0, 1, 0}, math::lerp(prev_heading_, heading_, alpha)), {1.0f, 1.0f, 1.0f});
            scene.add(w);
        }

        if (lights_on_) {
            for (u32 i = 0; i < rigs_.size(); ++i) {
                const LightRig& r = rigs_[i];
                const f32 a = r.phase + t * r.speed;
                PointLight l;
                l.position = r.anchor + vec3{std::cos(a) * r.orbit, 0.12f * std::sin(a * 1.7f), std::sin(a) * r.orbit};
                l.color = r.color;
                l.intensity = night ? 1.6f : 0.5f;
                l.radius = 2.6f;
                l.cast_shadows = i < static_cast<u32>(max(cv_shadow_lights.get_int(), 0));
                scene.add_point_light(l);
                MeshInstance b;
                b.mesh = keys_[Bulb];
                b.world = translation(l.position);
                b.tint = vec4(r.color * 6.0f, 1.0f);  // unlit and over 1: the bulbs bloom
                b.cast_shadows = false;
                scene.add(b);
            }
        }
        scene.build();
    }

    void fold(const Scene3D& scene) {
        u64 h = hash_;
        for (const Scene3D::Draw& d : scene.draws()) {
            h = hash_combine(h, (static_cast<u64>(d.instance) << 32) | d.submesh);
            h = fnv1a64(&d.model, sizeof(d.model), h);
        }
        for (const Cascade& c : scene.cascades()) h = fnv1a64(&c.view_proj, sizeof(c.view_proj), h);
        h = fnv1a64(scene.cascade_casters().data(), scene.cascade_casters().size_bytes(), h);
        const ClusterLights& cl = scene.clusters();
        h = fnv1a64(cl.ranges().data(), cl.ranges().size_bytes(), h);
        h = fnv1a64(cl.indices().data(), cl.indices().size_bytes(), h);
        h = fnv1a64(orrery_pose_.data(), orrery_pose_.size() * sizeof(mat4), h);
        h = fnv1a64(scene.joint_palette().data(), scene.joint_palette().size_bytes(), h);
        if (walking_) {  // M5b: World3D state + the swept boom (the camera itself is in the cascades)
            h = world_.hash_state(h);
            h = fnv1a64(&boom_, sizeof(boom_), h);
        }
        hash_ = h;
        stats_ = scene.stats();
        light_indices_ = static_cast<u32>(cl.indices().size());
    }

    void record_hud(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::Extent2D bb) {
        const Scene3D::Stats& st = scene.stats();
        const Forward3DStats& fs = renderer_.forward_3d().last_stats();
        char walk_line[160];
        if (walking_)
            std::snprintf(walk_line, sizeof(walk_line), "\nWALK: ARROWS MOVE  SPACE JUMP  C ORBIT  |  %s  BOOM %.1f M  CAMERA HITS %u",
                          (last_move_.flags & physics::ControllerFlags3D::Ground) ? "GROUND" : "AIR", f64(boom_), camera_hits_);
        else
            std::snprintf(walk_line, sizeof(walk_line), "\nC WALK (M5B CAPSULE + SWEPT CAMERA)");
        char line[800];
        std::snprintf(line, sizeof(line),
                      "MCE 04 CUBES - M5A RENDERER\n"
                      "TRIS %u  DRAWS %u  SHADOW DRAWS %u  CASCADES %u  LIGHTS %u  LIGHT INDICES %u\n"
                      "CPU MS: SCENE %.2f  CLUSTERS %.3f  RECORD %.2f  SUBMIT %.2f\n"
                      "GPU MS: SHADOW %.2f  MAIN %.2f  POST %.2f  TOTAL %.2f\n"
                      "L LIGHTS  B BLOOM %s  A FXAA %s  1-3 CASCADES  K SKY TEX  N NIGHT  P CLIP  G SKELETONS %s (%u JOINTS)%s",
                      st.triangles, fs.main_draws, fs.shadow_draws, fs.cascades, fs.point_lights, fs.light_indices,
                      last_.scene, last_.clusters, last_.record, last_.submit, gpu_.shadow, gpu_.main, gpu_.post(), gpu_.total,
                      cv_bloom.get_bool() ? "ON" : "OFF", cv_fxaa.get_bool() ? "ON" : "OFF", reed_skinning_ ? "ON" : "OFF",
                      st.joints, walk_line);
        TextLayout layout(mem::frame());
        layout_text(font_.face, line, {}, layout);
        SpriteBatch hud(renderer_.sprite_sets(), mem::frame());
        hud.begin(compute_screen_view(bb), PassId::Screen2D);
        TextPlacement p;
        p.origin = vec2(14, 14);
        p.tint = Color{0.02f, 0.03f, 0.06f, 0.85f};
        hud.add_text(font_, layout, p);
        p.origin = vec2(12, 12);
        p.tint = Color{1.0f, 0.97f, 0.88f, 1};
        hud.add_text(font_, layout, p);
        hud.build();
        rhi::PassAttachment color;
        color.load = rhi::LoadOp::Load;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "hud";
        cmd.begin_pass(pass);
        renderer_.record_2d(cmd, hud);
        cmd.end_pass();
    }

    void capture(const char* path, rhi::Extent2D size) {
        Array<u8> px;
        px.resize(usize(size.width) * size.height * 4);
        if (!device_->read_pixels({}, rhi::Rect2D{0, 0, size.width, size.height}, px.span())) return;
        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            MCE_LOG_ERROR("04_cubes: cannot write '{}'", path);
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", size.width, size.height);
        for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
        MCE_LOG_INFO("04_cubes: wrote {}", path);
    }

    static void toggle(ConVar& cv) { cv.set_bool(!cv.get_bool()); }

    static void on_event(const platform::Event& e, void* user) {
        auto* self = static_cast<CubesApp*>(user);
        using platform::EventType;
        using platform::Key;
        switch (e.type) {
            case EventType::KeyDown:
            case EventType::KeyUp: {
                const bool down = e.type == EventType::KeyDown;
                const i32 v = down ? 1 : 0;
                switch (static_cast<Key>(e.code)) {
                    case Key::Escape: if (down) self->quit(0); break;
                    case Key::Space:
                        if (down && self->walking_) self->jump_ = true;
                        else if (down) self->auto_orbit_ = !self->auto_orbit_;
                        break;
                    case Key::C: if (down) self->walking_ = !self->walking_; break;
                    case Key::L: if (down) self->lights_on_ = !self->lights_on_; break;
                    case Key::B: if (down) toggle(cv_bloom); break;
                    case Key::A: if (down) toggle(cv_fxaa); break;
                    case Key::K: if (down) toggle(cv_sky_texture); break;
                    case Key::I: if (down) toggle(cv_sky_ibl); break;
                    case Key::N: if (down) toggle(cv_night); break;
                    case Key::P: if (down) self->toggle_clip_ = true; break;
                    case Key::G: if (down) self->reed_skinning_ = !self->reed_skinning_; break;
                    case Key::Num1: if (down) cv_cascades.set_int(1); break;
                    case Key::Num2: if (down) cv_cascades.set_int(2); break;
                    case Key::Num3: if (down) cv_cascades.set_int(3); break;
                    case Key::Left: self->key_yaw_ = -v; break;
                    case Key::Right: self->key_yaw_ = v; break;
                    case Key::Up: self->key_pitch_ = v; break;
                    case Key::Down: self->key_pitch_ = -v; break;
                    case Key::Z: if (down) self->zoom(1.0f); break;
                    case Key::X: if (down) self->zoom(-1.0f); break;
                    default: break;
                }
                break;
            }
            case EventType::MouseButtonDown:
                if (e.code == static_cast<u32>(platform::MouseButton::Left)) {
                    self->dragging_ = true;
                    self->mouse_ = vec2(e.x, e.y);
                }
                break;
            case EventType::MouseButtonUp:
                if (e.code == static_cast<u32>(platform::MouseButton::Left)) self->dragging_ = false;
                break;
            case EventType::MouseMove:
                if (self->dragging_) {
                    const vec2 m(e.x, e.y);
                    self->yaw_ -= (m.x - self->mouse_.x) * 0.006f;
                    self->prev_yaw_ = self->yaw_;
                    self->pitch_ = clamp(self->pitch_ + (m.y - self->mouse_.y) * 0.004f, 0.08f, 1.45f);
                    self->mouse_ = m;
                }
                break;
            case EventType::MouseWheel: self->zoom(e.y); break;
            default: break;
        }
    }

    void zoom(f32 steps) { distance_ = clamp(distance_ * std::pow(0.9f, steps), 3.0f, 40.0f); }

    struct CpuTimes {
        f64 scene = 0, clusters = 0, record = 0, submit = 0;
    };

    platform::Window* window_;
    rhi::RenderDevice* device_;
    Renderer renderer_;
    asset::AssetSystem assets_;
    AssetUploadSink sink_;
    MeshStore cpu_meshes_;
    MeshStore* meshes_ = nullptr;
    Array<asset::AnyHandle> handles_;
    u32 keys_[kModelCount] = {};
    u32 sky_key_ = ~0u;
    u32 spin_clip_ = 0, pulse_clip_ = 0;
    AnimationPlayer player_;
    bool toggle_clip_ = false;
    Array<mat4> orrery_pose_;
    Array<mat4> reed_pose_;
    u32 sway_clip_ = ~0u;
    bool reed_skinning_ = true;
    Array<MeshInstance> statics_;
    Array<LightRig> rigs_;
    Font font_;

    // M5b walk.
    physics::World3D world_;
    bool walking_ = false, jump_ = false;
    physics::MoveResult3D last_move_;
    vec3 char_pos_, prev_char_pos_, pivot_, prev_pivot_;
    f32 heading_ = 0.0f, prev_heading_ = 0.0f, vy_ = 0.0f;
    f32 boom_ = kBoomLength, prev_boom_ = kBoomLength, closest_boom_ = kBoomLength;
    u32 waypoint_ = 1;
    u64 waypoint_tick_ = 0;
    f32 walked_ = 0.0f;
    u32 jumps_ = 0, grounded_ticks_ = 0, wall_ticks_ = 0, waypoints_reached_ = 0, camera_hits_ = 0;
    f64 physics_ms_ = 0.0;

    f32 time_ = 0.0f, prev_time_ = 0.0f;
    f32 yaw_ = 0.0f, prev_yaw_ = 0.0f, pitch_ = 0.4f, distance_ = 12.0f;
    bool auto_orbit_ = true, lights_on_ = true, dragging_ = false;
    i32 key_yaw_ = 0, key_pitch_ = 0;
    vec2 mouse_;
    u32 cascades_ = 3;
    CpuTimes last_;
    Forward3DGpuTimes gpu_;
    Budget budget_;
    Scene3D::Stats stats_;
    u32 light_indices_ = 0;
    u64 hash_ = kFnv64Offset;
    u64 ticks_ = 0;
};

int fail(const char* what, const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    FixedString<640> text;
    text.append(what);
    text.append(": ");
    text.append(msg.view());
    platform::show_error_box("04_cubes", text.c_str());
    return 1;
}

int run_headless() {
    CubesApp app(nullptr, nullptr);
    app::AppDesc desc;
    desc.name = "04_cubes";
    desc.headless = true;
    desc.max_ticks = static_cast<u64>(cv_ticks.get_int() > 0 ? cv_ticks.get_int() : 300);
    const int rc = app::run(app, desc);
    app.report_headless();
    return rc;
}

int run_windowed() {
    platform::WindowDesc wd;
    wd.title = "MCE - 04_cubes";
    wd.width = 1280;
    wd.height = 720;
    auto window = platform::Window::create(wd);
    if (!window) return fail("window creation failed", window.error());
    rhi::DeviceDesc dd;
    dd.debug_layer = cv_gl_debug.get_bool();
    auto device = rhi::create_default_device(*window.value(), dd);
    if (!device) return fail("render device creation failed", device.error());

    CubesApp app(window.value().get(), device.value().get());
    app::AppDesc desc;
    desc.name = "04_cubes";
    desc.platform = &app;
    desc.fences = device.value().get();
    app::RunStats stats;
    int rc = app::run(app, desc, &stats);
    app.report_budget();
    const u32 gl_errors = device.value()->validation_error_count();
    std::printf("04_cubes: %llu frames, %u GL errors\n", static_cast<unsigned long long>(stats.frames), gl_errors);
    if (rc == 0 && gl_errors != 0) rc = 2;
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    log::LogDesc log_desc;
    log_desc.app_name = "04_cubes";
    log::init(log_desc);
    console::parse_command_line(argc, argv);

    const ConVar* headless = ConVar::find(StringId("headless"));
    const bool is_headless = headless && headless->get_bool();
    int rc = 1;
    if (is_headless) {
        rc = run_headless();
    } else if (ResultV r = platform::init(); !r) {
        rc = fail("platform init failed", r.error());
    } else {
        rc = run_windowed();
        platform::shutdown();
    }
    log::shutdown();
    return rc;
}
