// 05_ashen_court — the M7 slice (ROADMAP M7): a third-person melee game in a ruined court.
//
// The game itself lives in game/court.cpp (ECS + World3D + fsm.h + input actions + audio events,
// no host knowledge); this file is the host: window, the M5 3D renderer, the input map, the audio
// device and listener, the HUD and the frame-budget table.
//
//   * a hero with a sword against husks (melee), wisps (ranged orbs) and brutes (charges), each an
//     fsm.h table; ember hearts heal
//   * a third-person boom swept against the court (World3D::camera_sweep): it never clips a wall
//   * brazier point lights that flicker, a low dusk sun with cascaded shadows, bloom on the embers
//   * 3D one-shots at the actors (PlaySfx positions) heard from the camera
//   * the level's triangle total checked against its stated budget at load
//
//   05_ashen_court                              play (title -> fight -> victory or death -> again)
//   05_ashen_court -autoplay 1                  the bot plays
//   05_ashen_court -frames 600 -autoplay 1      run 600 frames, then print the budget table
//   05_ashen_court -frames 300 -capture a.ppm   also write the last frame as a PPM image
//   05_ashen_court -headless 1 -ticks N         determinism run: bot input, no GL, prints the hash
//
// Controls (keyboard / gamepad): WASD or the left stick move (camera-relative); the mouse, Q/E or
// the right stick turn the camera; Space or A jump; J, left mouse or X / RB swing; Esc or Start
// pause; Enter or A confirm.

#include <cmath>
#include <cstdio>

#include "game/court.h"
#include "hud_font.h"
#include "mce/asset/asset_system.h"
#include "mce/audio/audio.h"
#include "mce/audio/components.h"
#include "mce/core.h"
#include "mce/input/ecs.h"
#include "mce/input/input_map.h"
#include "mce/input/input_system.h"
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
using namespace court;

MCE_CVAR(cv_frames, "frames", 0, "Quit after N frames (0 = run until the window closes)", CVarFlags::None);
MCE_CVAR(cv_capture, "capture", "", "Write the last frame (with -frames) to this .ppm path", CVarFlags::None);
MCE_CVAR(cv_ticks, "ticks", 1800, "Headless: ticks to simulate", CVarFlags::None);
MCE_CVAR(cv_autoplay, "autoplay", false, "The bot plays (attract mode)", CVarFlags::None);
MCE_CVAR(cv_pack, "pack", "", "Cooked pack to mount (default: assets.mce next to the executable)", CVarFlags::None);
MCE_CVAR(cv_gl_debug, "r.gl_debug", true, "Enable the GL_KHR_debug validation callback", CVarFlags::RequiresRestart);
MCE_CVAR(cv_msaa, "r.msaa", 4, "3D MSAA samples (1 = off)", CVarFlags::RequiresRestart);

f64 now_ms() { return platform::now_seconds() * 1000.0; }

u32 hash_u32(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    return x ^ (x >> 16);
}
f32 hash01(u32 x) { return f32(hash_u32(x) & 0xFFFFu) / 65535.0f; }
f32 smooth_noise(f32 t, u32 seed) {  // value noise, 1D: flicker and shake
    const f32 i = std::floor(t);
    const f32 u = t - i;
    const u32 k = static_cast<u32>(static_cast<i32>(i)) * 7919u + seed * 104729u;
    const f32 s = u * u * (3.0f - 2.0f * u);
    return math::lerp(hash01(k), hash01(k + 7919u), s) * 2.0f - 1.0f;
}

// ---- synthesized SFX (mono, 48 kHz) -------------------------------------------------------------
constexpr f32 kRate = static_cast<f32>(audio::kSampleRate);
usize frames(f32 seconds) { return static_cast<usize>(seconds * kRate); }

// A pitch sweep with optional noise, an exponential decay and a short attack.
void sweep(Array<f32>& out, f32 seconds, f32 hz0, f32 hz1, f32 gain, f32 noise, f32 decay) {
    out.resize(frames(seconds));
    f32 phase = 0.0f;
    u32 rng = 0x1234567u;
    for (usize i = 0; i < out.size(); ++i) {
        const f32 t = f32(i) / f32(out.size());
        phase += (hz0 + (hz1 - hz0) * t) / kRate;
        rng = rng * 1664525u + 1013904223u;
        const f32 n = f32(rng >> 9) / f32(1u << 23) * 2.0f - 1.0f;
        const f32 tone = std::sin(phase * kTau);
        const f32 env = std::exp(-decay * t) * min(1.0f, f32(i) / 96.0f);
        out[i] = gain * env * ((1.0f - noise) * tone + noise * n);
    }
}

struct Clips {
    Array<f32> swing, hit, hurt, death, cast, heal, roar;
};
void make_clips(Clips& c) {
    sweep(c.swing, 0.18f, 900.0f, 300.0f, 0.35f, 0.85f, 6.0f);   // a whoosh: mostly noise, falling
    sweep(c.hit, 0.14f, 160.0f, 60.0f, 0.6f, 0.35f, 12.0f);      // a thud
    sweep(c.hurt, 0.30f, 380.0f, 120.0f, 0.4f, 0.3f, 6.0f);
    sweep(c.death, 0.7f, 220.0f, 40.0f, 0.5f, 0.5f, 4.0f);
    sweep(c.cast, 0.35f, 300.0f, 1100.0f, 0.3f, 0.15f, 5.0f);    // a rising crackle
    sweep(c.heal, 0.4f, 660.0f, 1320.0f, 0.3f, 0.0f, 5.0f);
    sweep(c.roar, 0.6f, 90.0f, 70.0f, 0.6f, 0.55f, 3.0f);
}

struct Budget {
    f64 sim = 0, scene = 0, record = 0, submit = 0;
    f64 gpu_shadow = 0, gpu_main = 0, gpu_post = 0, gpu_total = 0;
    u64 frames = 0, gpu_frames = 0;
};

class CourtApp final : public app::Application, public app::IPlatformHooks {
public:
    CourtApp(platform::Window* window, rhi::RenderDevice* device, audio::AudioDevice& audio, bool autoplay)
        : window_(window), device_(device), audio_(audio), autoplay_(autoplay), sink_(renderer_, assets_) {}

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
        samples::resolve_shipped_path(cv_pack.get_string(), "assets.mce", MCE_SAMPLE_DEV_PACK, path);
        const StringView roots[1] = {path.view()};
        asset::MountOptions mo;
        mo.verify_payload_crc = false;
        MCE_TRY(assets_.mount(Span<const StringView>(roots, 1), mo));
        for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k) {
            if (assets_.info(k).type != asset::ChunkType::Texture) continue;
            MCE_TRY_ASSIGN(asset::AssetHandle<asset::Texture> h, assets_.load_sync<asset::Texture>(k));
            handles_.push_back(asset::AnyHandle(h));
        }
        for (u32 m = 0; m < static_cast<u32>(Model::Count); ++m) {
            const asset::AssetKey k = find_path(kModelPaths[m]);
            if (k == ~0u) return Error(Errc::NotFound, "05_ashen_court: model missing from the pack", kModelPaths[m]);
            MCE_TRY_ASSIGN(asset::AssetHandle<asset::Mesh> h, assets_.load_sync<asset::Mesh>(k));
            handles_.push_back(asset::AnyHandle(h));
            if (!device_) MCE_TRY(cpu_meshes_.upload(k, assets_.generation(k), assets_.resolve(h)));
            keys_[m] = k;
        }
        build_layout(layout_);
        MCE_TRY(check_level_budget());

        // Sound: synthesized clips, one distance curve for combat, the listener at the camera.
        make_clips(clips_);
        Sounds sounds;
        sounds.swing = register_clip(clips_.swing);
        sounds.hit = register_clip(clips_.hit);
        sounds.hurt = register_clip(clips_.hurt);
        sounds.death = register_clip(clips_.death);
        sounds.cast = register_clip(clips_.cast);
        sounds.heal = register_clip(clips_.heal);
        sounds.roar = register_clip(clips_.roar);
        audio::AttenuationCurve curve;
        curve.model = audio::AttenuationCurve::Model::Inverse;
        curve.min_distance = 2.0f;
        curve.max_distance = 40.0f;
        MCE_TRY(audio_.set_attenuation(kAttenuationCombat, curve));

        input::install(world_);
        audio::install(world_, audio_);
        world_.set_resource(sounds);
        MCE_TRY(court::install(world_));
        interactive_ = window_ && !autoplay_;
        if (interactive_) world_.resource<CourtState>().mode = Mode::Title;

        if (window_) {
            Array<byte> map;
            build_input_map(map);
            MCE_TRY(input_.load_map(map.span()));
            window_->set_event_handler(&CourtApp::on_event, this);
        }
        if (device_) {
            samples::HudFont hud;
            MCE_TRY_ASSIGN(font_, renderer_.create_font(hud.metrics, hud.glyphs.span(), {}, std::as_bytes(hud.atlas.span()),
                                                        "05_ashen_court.hud_font"));
        }
        return ok();
    }

    void on_tick(const app::TickContext& t) override {
        const f64 t0 = now_ms();
        const input::InputState in = (autoplay_ || !t.actions) ? court::autopilot(world_, t.tick_index) : *t.actions;
        input_hash_ = hash_combine(input_hash_, input::hash_state(in));
        input::publish(world_, in);
        world_.tick(t.dt, t.tick_index);
        prev_time_ = time_;
        time_ += static_cast<f32>(t.dt);
        ++ticks_;
        sim_ms_ += now_ms() - t0;
        if (!device_) audio_.update();  // headless: the Silent backend mixes one block per call
    }

    void on_render(const app::RenderContext& rc) override {
        audio_.update();
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
        last_scene_ = t1 - t0;
        gpu_ = renderer_.forward_3d().gpu_times(device_->last_gpu_timestamps());
        if (rc.frame_index >= 10) {  // skip warm-up (pipeline compiles, first uploads)
            budget_.scene += t1 - t0;
            budget_.record += t2 - t1;
            budget_.submit += t3 - t2;
            ++budget_.frames;
            if (gpu_.total >= 0.0) {
                budget_.gpu_shadow += gpu_.shadow;
                budget_.gpu_main += gpu_.main;
                budget_.gpu_post += gpu_.post();
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

    u64 state_hash() { return hash_combine(hash_game(world_, kFnv64Offset), hash_combine(input_hash_, audio_.command_hash())); }

    void report() {
        const CourtState& gs = world_.resource<CourtState>();
        const Health* h = world_.try_get<Health>(gs.player);
        const PlayerCtl* pc = world_.try_get<PlayerCtl>(gs.player);
        static constexpr const char* kModes[] = {"title", "playing", "paused", "dead", "victory"};
        std::printf("05_ashen_court: level %u tris / %u budget, %u statics | round %u %s, kills %u/%u, deaths %u, hp %d, "
                    "damage taken %u, swings %u (%u hit) | camera hits %u, closest boom %.2f m | sim %.3f ms/tick\n",
                    level_tris_, kLevelTriangleBudget, static_cast<u32>(layout_.statics.size()), gs.serial,
                    kModes[static_cast<u32>(gs.mode)], gs.kills, gs.enemies_total, gs.deaths, h ? h->hp : 0,
                    gs.damage_taken, pc ? pc->swings : 0, pc ? pc->hits : 0, gs.camera_hits, f64(gs.closest_boom),
                    sim_ms_ / f64(max<u64>(ticks_, 1)));
        std::printf("05_ashen_court: %llu ticks | hash %016llx\n", static_cast<unsigned long long>(ticks_),
                    static_cast<unsigned long long>(state_hash()));
    }

    // The NFR-F-01 per-stage table (milliseconds, means over the measured frames).
    void report_budget() const {
        if (budget_.frames == 0) return;
        const f64 n = f64(budget_.frames);
        std::printf("05_ashen_court: frame budget over %llu frames (%u tris drawn, %u point lights)\n",
                    static_cast<unsigned long long>(budget_.frames), stats_.triangles, stats_.point_lights);
        std::printf("  | stage | CPU ms | GPU ms |\n  |---|---|---|\n");
        std::printf("  | gameplay tick (ECS + World3D + FSMs) | %.3f | - |\n", sim_ms_ / f64(max<u64>(ticks_, 1)));
        std::printf("  | scene build (cull + sort + cascades + clusters) | %.3f | - |\n", budget_.scene / n);
        std::printf("  | command recording (3D + HUD) | %.3f | - |\n", budget_.record / n);
        std::printf("  | GL translate + submit | %.3f | - |\n", budget_.submit / n);
        if (budget_.gpu_frames > 0) {
            const f64 g = f64(budget_.gpu_frames);
            std::printf("  | shadow cascades | - | %.3f |\n", budget_.gpu_shadow / g);
            std::printf("  | main pass (Forward+ PBR + sky) | - | %.3f |\n", budget_.gpu_main / g);
            std::printf("  | post (resolve + bloom + tonemap + FXAA) | - | %.3f |\n", budget_.gpu_post / g);
            std::printf("  | 3D frame total | - | %.3f |\n", budget_.gpu_total / g);
        }
    }

public:
    input::InputSystem input_;

private:
    asset::AssetKey find_path(StringView path) const {
        for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k)
            if (assets_.info(k).path == path) return k;
        return ~0u;
    }

    u32 key(Model m) const { return keys_[static_cast<u32>(m)]; }

    // The level's stated budget: every placed static plus the cast at full strength.
    ResultV check_level_budget() {
        u32 tris = 0;
        for (const Placement& p : layout_.statics) tris += meshes_->get(key(p.model))->triangle_count();
        const Model cast[] = {Model::Hero, Model::Sword, Model::Husk, Model::Husk, Model::Husk, Model::Husk, Model::Wisp,
                              Model::Wisp, Model::Wisp, Model::Brute, Model::Brute, Model::Heart, Model::Heart,
                              Model::Heart, Model::Heart};
        for (Model m : cast) tris += meshes_->get(key(m))->triangle_count();
        level_tris_ = tris;
        if (tris > kLevelTriangleBudget) {
            MCE_LOG_ERROR("05_ashen_court: the level is {} triangles, over its budget of {}", tris, kLevelTriangleBudget);
            return Error(Errc::InvalidArgument, "05_ashen_court: level over its triangle budget");
        }
        return ok();
    }

    audio::ClipHandle register_clip(const Array<f32>& pcm) {
        audio::ClipPayload p;
        p.frames = pcm.data();
        p.frame_count = static_cast<u32>(pcm.size());
        auto h = audio_.register_clip(p);
        return h ? h.value() : audio::ClipHandle{};
    }

    // One map for play and menus, keyboard + mouse and gamepad. Look is camera turn in mouse pixels
    // per tick; the stick and Q/E are scaled to a comfortable ~2.5 rad/s.
    static void build_input_map(Array<byte>& out) {
        using platform::GamepadAxis;
        using platform::GamepadButton;
        using platform::Key;
        input::InputMapBuilder b;
        b.map(MCE_SID("AshenCourt"));
        b.action(MCE_SID("Move"), input::ActionType::Axis2D)
            .key(Key::D, 0, +1.f).key(Key::A, 0, -1.f).key(Key::W, 1, +1.f).key(Key::S, 1, -1.f)
            .key(Key::Right, 0, +1.f).key(Key::Left, 0, -1.f).key(Key::Up, 1, +1.f).key(Key::Down, 1, -1.f)
            .gamepad_axis(GamepadAxis::LeftX, 0, +1.f).gamepad_axis(GamepadAxis::LeftY, 1, -1.f);
        b.action(MCE_SID("Look"), input::ActionType::Axis2D)
            .mouse_delta(0).mouse_delta(1)
            .key(Key::Q, 0, -9.f).key(Key::E, 0, +9.f)
            .gamepad_axis(GamepadAxis::RightX, 0, +9.f).gamepad_axis(GamepadAxis::RightY, 1, +14.f);
        b.action(MCE_SID("Jump")).key(Key::Space).gamepad_button(GamepadButton::South);
        b.action(MCE_SID("Fire")).key(Key::J).mouse_button(platform::MouseButton::Left)
            .gamepad_button(GamepadButton::West).gamepad_button(GamepadButton::RightShoulder);
        b.action(MCE_SID("Pause")).key(Key::Escape).key(Key::P).gamepad_button(GamepadButton::Start);
        b.action(MCE_SID("Confirm")).key(Key::Return).key(Key::Space).gamepad_button(GamepadButton::South);
        b.encode(out);
    }

    // ---- rendering ------------------------------------------------------------------------------------

    Camera3D camera(f32 alpha, vec3* pivot_out) {
        const CourtState& gs = world_.resource<CourtState>();
        vec3 pivot{0.0f, 1.5f, 17.0f};
        if (const Actor* p = world_.try_get<Actor>(gs.player)) pivot = math::lerp(p->prev_pos, p->pos, alpha) + kHeadOffset;
        // Shake: noise on the pivot, scaled by the sim's decaying amount (the boom stays swept).
        const f32 t = math::lerp(prev_time_, time_, alpha);
        const f32 s = gs.shake * gs.shake * 0.25f;
        pivot = pivot + vec3{smooth_noise(t * 30.0f, 1) * s, smooth_noise(t * 30.0f, 2) * s, smooth_noise(t * 30.0f, 3) * s};
        f32 dyaw = gs.cam_yaw - gs.prev_cam_yaw;
        while (dyaw > kPi) dyaw -= kTau;
        while (dyaw < -kPi) dyaw += kTau;
        Camera3D c = orbit_camera(pivot, gs.prev_cam_yaw + dyaw * alpha, gs.cam_pitch, math::lerp(gs.prev_boom, gs.boom, alpha),
                                  radians(60.0f));
        c.near_z = 0.05f;
        c.far_z = 140.0f;
        if (pivot_out) *pivot_out = pivot;
        return c;
    }

    void add_model(Scene3D& scene, Model m, const mat4& world, vec4 tint = {1, 1, 1, 1}, bool shadows = true) {
        MeshInstance i;
        i.mesh = key(m);
        i.world = world;
        i.tint = tint;
        i.cast_shadows = shadows;
        scene.add(i);
    }

    void fill_scene(Scene3D& scene, f32 alpha, rhi::Extent2D viewport) {
        const CourtState& gs = world_.resource<CourtState>();
        const f32 t = math::lerp(prev_time_, time_, alpha);
        vec3 pivot;
        const Camera3D cam = camera(alpha, &pivot);
        scene.begin(cam, viewport);

        // The listener rides the camera, facing the pivot.
        audio::Listener ear;
        ear.position = cam.position;
        ear.forward = normalize(pivot - cam.position);
        audio_.set_listener(ear);

        DirectionalLight sun;  // a low ember-red dusk: braziers carry the court
        sun.direction = normalize(vec3{0.62f, -0.38f, 0.55f});
        sun.color = {1.0f, 0.62f, 0.38f};
        sun.intensity = 1.1f;
        sun.ambient_sky = {0.10f, 0.11f, 0.16f};
        sun.ambient_ground = {0.07f, 0.05f, 0.04f};
        scene.set_light(sun);
        Sky sky;
        sky.zenith = {0.05f, 0.06f, 0.12f};
        sky.horizon = {0.55f, 0.28f, 0.16f};
        sky.ground = {0.05f, 0.04f, 0.035f};
        sky.sun_intensity = 6.0f;
        scene.set_sky(sky);
        Fog fog;
        fog.color = {0.22f, 0.13f, 0.10f};
        fog.density = 0.012f;
        scene.set_fog(fog);
        PostFx post;
        post.bloom_intensity = 0.07f;
        scene.set_post(post);
        ShadowSettings shadows;
        shadows.cascades = 3;
        shadows.distance = 45.0f;
        shadows.splits[0] = 0.12f;
        shadows.splits[1] = 0.35f;
        scene.set_shadows(shadows);

        for (const Placement& p : layout_.statics)
            add_model(scene, p.model, trs(p.position, quat_from_axis_angle({0, 1, 0}, p.yaw), p.scale));
        for (u32 i = 0; i < layout_.braziers.size(); ++i) {
            const court::Light& b = layout_.braziers[i];
            const f32 flicker = 0.8f + 0.2f * smooth_noise(t * 9.0f, 10 + i) + 0.08f * smooth_noise(t * 23.0f, 40 + i);
            PointLight l;
            l.position = b.position + vec3{0.0f, 0.06f * smooth_noise(t * 7.0f, 70 + i), 0.0f};
            l.color = b.color;
            l.intensity = b.intensity * flicker;
            l.radius = b.radius;
            scene.add_point_light(l);
        }

        world_.query<const Actor&, const Health&>().each([&](Entity e, const Actor& a, const Health& h) {
            const vec3 pos = math::lerp(a.prev_pos, a.pos, alpha);
            f32 dyaw = a.yaw - a.prev_yaw;
            while (dyaw > kPi) dyaw -= kTau;
            while (dyaw < -kPi) dyaw += kTau;
            const quat rot = quat_from_axis_angle({0, 1, 0}, a.prev_yaw + dyaw * alpha);
            vec4 tint{1, 1, 1, 1};
            if (h.flash > 0) tint = {3.0f, 2.2f, 1.6f, 1.0f};  // hit flash (bloomed)
            switch (a.kind) {
                case Kind::Player: {
                    if (h.invuln > 0 && (h.invuln / 4) % 2 == 1) tint = {1.6f, 0.6f, 0.5f, 1.0f};
                    if (gs.mode == Mode::Dead) tint = {0.3f, 0.3f, 0.3f, 1.0f};
                    const mat4 body = trs(pos, rot, {1, 1, 1});
                    add_model(scene, Model::Hero, body, tint);
                    add_model(scene, Model::Sword, body * sword_local(e));
                    break;
                }
                case Kind::Husk: {
                    const HuskBrain& b = *world_.try_get<HuskBrain>(e);
                    if (b.state == HuskState::WindUp && h.flash == 0) tint = {1.8f, 1.1f, 0.8f, 1.0f};
                    add_model(scene, Model::Husk, trs(pos, rot, {1, 1, 1}), tint);
                    break;
                }
                case Kind::Wisp: {
                    const WispBrain& b = *world_.try_get<WispBrain>(e);
                    const f32 bob = 0.12f * std::sin(t * 2.5f + f32(e.index));
                    if (b.state == WispState::Cast && h.flash == 0) tint = {2.5f, 1.6f, 1.0f, 1.0f};
                    add_model(scene, Model::Wisp, trs(pos + vec3{0.0f, bob, 0.0f}, rot, {1, 1, 1}), tint);
                    PointLight l;  // the burning core
                    l.position = pos + vec3{0.0f, 0.35f + bob, 0.0f};
                    l.color = {1.0f, 0.5f, 0.15f};
                    l.intensity = b.state == WispState::Cast ? 2.5f : 1.0f;
                    l.radius = 4.0f;
                    scene.add_point_light(l);
                    break;
                }
                case Kind::Brute: {
                    const BruteBrain& b = *world_.try_get<BruteBrain>(e);
                    if (h.flash == 0) {
                        if (b.state == BruteState::WindUp) tint = {2.4f, 0.9f, 0.5f, 1.0f};
                        else if (b.state == BruteState::Stunned) tint = {0.6f, 0.6f, 0.9f, 1.0f};
                    }
                    f32 lean = 0.0f;
                    if (b.state == BruteState::Charge) lean = 0.3f;
                    if (b.state == BruteState::Stunned) lean = -0.25f + 0.05f * std::sin(t * 12.0f);
                    const quat r = rot * quat_from_axis_angle({1, 0, 0}, lean);
                    add_model(scene, Model::Brute, trs(pos, r, {1, 1, 1}), tint);
                    break;
                }
            }
        });
        world_.query<const Orb&>().each([&](const Orb& o) {
            const vec3 p = math::lerp(o.prev_pos, o.pos, alpha);
            add_model(scene, Model::Orb, translation(p), {1, 1, 1, 1}, false);
            PointLight l;
            l.position = p;
            l.color = {1.0f, 0.45f, 0.1f};
            l.intensity = 2.0f;
            l.radius = 3.5f;
            scene.add_point_light(l);
        });
        world_.query<const Heart&>().each([&](Entity e, const Heart& h) {
            const f32 bob = 0.1f * std::sin(t * 2.0f + f32(e.index));
            add_model(scene, Model::Heart, trs(h.pos + vec3{0.0f, bob, 0.0f}, quat_from_axis_angle({0, 1, 0}, t * 1.5f), {1, 1, 1}));
            PointLight l;
            l.position = h.pos + vec3{0.0f, 0.2f, 0.0f};
            l.color = {1.0f, 0.2f, 0.15f};
            l.intensity = 1.2f;
            l.radius = 3.0f;
            scene.add_point_light(l);
        });
        scene.build();
    }

    // The sword in the hero's model space: held up at the right side; a swing sweeps it across
    // the front, blade level, over the first half of the swing.
    mat4 sword_local(Entity player) {
        const PlayerCtl* pc = world_.try_get<PlayerCtl>(player);
        f32 sweep_angle = -0.3f, tilt = 0.35f;
        if (pc && pc->swing > 0) {
            const f32 u = clamp(f32(pc->swing) / (f32(kSwingTicks) * 0.5f), 0.0f, 1.0f);
            const f32 ease = u * u * (3.0f - 2.0f * u);
            sweep_angle = math::lerp(-1.5f, 1.4f, ease);  // from the right (-X) across to the left
            tilt = 1.45f;                                  // blade level, pointing forward
        }
        const quat r = quat_from_axis_angle({0, 1, 0}, sweep_angle) * quat_from_axis_angle({1, 0, 0}, tilt);
        return trs({-0.38f, 0.0f, 0.12f}, r, {1, 1, 1});
    }

    void record_hud(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::Extent2D bb) {
        const CourtState& gs = world_.resource<CourtState>();
        const Health* h = world_.try_get<Health>(gs.player);
        const i32 hp = h ? max(h->hp, 0) : 0;
        char bar[24] = {};
        const i32 filled = (hp * 20 + 99) / 100;
        for (i32 i = 0; i < 20; ++i) bar[i] = i < filled ? '#' : '-';
        const u32 secs = static_cast<u32>(gs.ticks / 60);
        char line[640];
        std::snprintf(line, sizeof(line),
                      "ASHEN COURT\nHP [%s] %d\nFOES %u/%u   KILLS %u   TIME %u:%02u\n"
                      "TRIS %u  LIGHTS %u  CPU SCENE %.2f MS  GPU %.2f MS",
                      bar, hp, gs.enemies, gs.enemies_total, gs.kills, secs / 60, secs % 60, scene.stats().triangles,
                      scene.stats().point_lights, last_scene_, gpu_.total);
        const char* banner = nullptr;
        switch (gs.mode) {
            case Mode::Title: banner = "ASHEN COURT\n\nCLEAR THE COURT OF ASH\n\nPRESS ENTER OR A"; break;
            case Mode::Paused: banner = "PAUSED\n\nESC OR START TO RESUME"; break;
            case Mode::Dead: banner = "YOU FELL\n\nENTER OR A TO RISE AGAIN"; break;
            case Mode::Victory: banner = "THE COURT IS CLEARED\n\nENTER OR A TO PLAY AGAIN"; break;
            case Mode::Playing: break;
        }
        SpriteBatch hud(renderer_.sprite_sets(), mem::frame());
        hud.begin(compute_screen_view(bb), PassId::Screen2D);
        text(hud, line, vec2(16, 14), 1.0f);
        if (banner) text(hud, banner, vec2(f32(bb.width) * 0.5f - 200.0f, f32(bb.height) * 0.38f), 2.0f);
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

    void text(SpriteBatch& hud, const char* s, vec2 at, f32 scale) {
        TextLayout layout(mem::frame());
        layout_text(font_.face, s, {}, layout);
        TextPlacement p;
        p.units_per_pixel = scale;
        p.origin = at + vec2(2, 2);
        p.tint = Color{0.02f, 0.01f, 0.01f, 0.85f};
        hud.add_text(font_, layout, p);
        p.origin = at;
        p.tint = Color{1.0f, 0.86f, 0.62f, 1};
        hud.add_text(font_, layout, p);
    }

    void capture(const char* path, rhi::Extent2D size) {
        Array<u8> px;
        px.resize(usize(size.width) * size.height * 4);
        if (!device_->read_pixels({}, rhi::Rect2D{0, 0, size.width, size.height}, px.span())) return;
        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            MCE_LOG_ERROR("05_ashen_court: cannot write '{}'", path);
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", size.width, size.height);
        for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
        MCE_LOG_INFO("05_ashen_court: wrote {}", path);
    }

    static void on_event(const platform::Event& e, void* user) { static_cast<CourtApp*>(user)->input_.push_event(e); }

    platform::Window* window_;
    rhi::RenderDevice* device_;
    audio::AudioDevice& audio_;
    bool autoplay_;
    bool interactive_ = false;
    Renderer renderer_;
    asset::AssetSystem assets_;
    AssetUploadSink sink_;
    MeshStore cpu_meshes_;
    MeshStore* meshes_ = nullptr;
    Array<asset::AnyHandle> handles_;
    u32 keys_[static_cast<u32>(Model::Count)] = {};
    Layout layout_;
    u32 level_tris_ = 0;
    Clips clips_;
    Font font_;
    World world_;
    u64 input_hash_ = kFnv64Offset;
    u64 ticks_ = 0;
    f32 time_ = 0.0f, prev_time_ = 0.0f;
    f64 sim_ms_ = 0.0, last_scene_ = 0.0;
    Forward3DGpuTimes gpu_;
    Budget budget_;
    Scene3D::Stats stats_;
};

int fail(const char* what, const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    FixedString<640> text;
    text.append(what);
    text.append(": ");
    text.append(msg.view());
    platform::show_error_box("05_ashen_court", text.c_str());
    return 1;
}

int run_headless() {
    audio::AudioDesc ad;
    ad.backend = audio::Backend::Silent;
    auto audio = audio::AudioDevice::create(ad);
    if (!audio) return 1;
    CourtApp app(nullptr, nullptr, *audio.value(), true);
    app::AppDesc desc;
    desc.name = "05_ashen_court";
    desc.headless = true;
    desc.max_ticks = static_cast<u64>(cv_ticks.get_int() > 0 ? cv_ticks.get_int() : 1800);
    const int rc = app::run(app, desc);
    if (rc == 0) app.report();
    return rc;
}

int run_windowed() {
    platform::WindowDesc wd;
    wd.title = "MCE - Ashen Court";
    wd.width = 1280;
    wd.height = 720;
    auto window = platform::Window::create(wd);
    if (!window) return fail("window creation failed", window.error());
    rhi::DeviceDesc dd;
    dd.debug_layer = cv_gl_debug.get_bool();
    auto device = rhi::create_default_device(*window.value(), dd);
    if (!device) return fail("render device creation failed", device.error());
    auto audio = audio::AudioDevice::create({});  // falls back to Silent when no output device opens
    if (!audio) return fail("audio init failed", audio.error());

    CourtApp app(window.value().get(), device.value().get(), *audio.value(), cv_autoplay.get_bool());
    app::AppDesc desc;
    desc.name = "05_ashen_court";
    desc.platform = &app;
    desc.fences = device.value().get();
    desc.input_source = &app.input_;
    app::RunStats stats;
    int rc = app::run(app, desc, &stats);
    app.report();
    app.report_budget();
    const u32 gl_errors = device.value()->validation_error_count();
    std::printf("05_ashen_court: %llu frames, %u GL errors\n", static_cast<unsigned long long>(stats.frames), gl_errors);
    if (rc == 0 && gl_errors != 0) rc = 2;
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    log::LogDesc log_desc;
    log_desc.app_name = "05_ashen_court";
    log::init(log_desc);
    console::parse_command_line(argc, argv);

    const ConVar* headless = ConVar::find(StringId("headless"));
    int rc = 1;
    if (headless && headless->get_bool()) {
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
