// The Solstice Reach host (docs/design/solstice-reach.md §7): the engine's 3D forward renderer
// around the game module. The stock player draws only the 2D kit, so this host mounts the pack,
// instantiates the start scene, runs the game's install() and draws the MeshRef / Scatter /
// Light3D components it finds, under a sky that follows the game's hour.
//
//   solstice_reach                          play (title -> the island -> finale -> dawn)
//   solstice_reach -autoplay 1              the bot walks the route (attract mode)
//   solstice_reach -frames 600 -autoplay 1  run 600 frames, then print the frame-budget table
//   solstice_reach -frames 120 -capture /abs/shot.ppm   also write the last frame
//   solstice_reach -photo 1 -hour 22        start in photo mode at a given hour (free camera)
//   solstice_reach -headless 1 -ticks N     determinism run: the bot, no GL, prints the hash
//
// Controls: WASD / left stick move, mouse / Q E / right stick look, Space jump, Shift sprint,
// E or J or left mouse interact (hold on a mirror's plate), L lantern, Esc pause, Enter confirm.
// Photo mode (P in pause, or after the finale): WASD R F fly, Shift fast, 1-8 pick an hour,
// B bloom, G fog, T anti-aliasing, H HUD, C capture a PPM to the save folder.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "hud_font.h"
#include "mce/asset/asset_system.h"
#include "mce/audio/audio.h"
#include "mce/core.h"
#include "mce/ecs/save.h"
#include "mce/hash.h"
#include "mce/input/ecs.h"
#include "mce/input/input_map.h"
#include "mce/input/input_system.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/asset_upload.h"
#include "mce/render/renderer.h"
#include "mce/rhi/default_device.h"
#include "mce/scene/scene.h"
#include "solstice_reach.h"
#include "terrain.h"

namespace {

using namespace mce;
using namespace mce::math;
using namespace mce::render;
using namespace solstice_reach;

MCE_CVAR(cv_frames, "frames", 0, "Quit after N frames (0 = run until the window closes)", CVarFlags::None);
MCE_CVAR(cv_capture, "capture", "", "Write the last frame (with -frames) to this .ppm path", CVarFlags::None);
MCE_CVAR(cv_ticks, "ticks", 1800, "Headless: ticks to simulate", CVarFlags::None);
MCE_CVAR(cv_autoplay, "autoplay", false, "The bot plays (attract mode)", CVarFlags::None);
MCE_CVAR(cv_trace, "trace", 0, "Headless: print the Keeper and the bot every N ticks (0 = off)", CVarFlags::None);
MCE_CVAR(cv_pack, "pack", "", "Cooked pack to mount (default: assets.mce next to the executable)", CVarFlags::None);
MCE_CVAR(cv_scene, "scene", "", "Scene GUID to play (default: the project's start scene)", CVarFlags::None);
MCE_CVAR(cv_gl_debug, "r.gl_debug", true, "Enable the GL_KHR_debug validation callback", CVarFlags::RequiresRestart);
MCE_CVAR(cv_msaa, "r.msaa", 4, "3D MSAA samples (1 = off)", CVarFlags::RequiresRestart);
MCE_CVAR(cv_photo, "photo", false, "Start in photo mode (free camera, hour keys)", CVarFlags::None);
MCE_CVAR(cv_cam, "cam", "", "Photo mode: the camera as \"x,y,z,yaw,pitch\" (radians; prefix with @ when x is negative)", CVarFlags::None);
MCE_CVAR(cv_hour, "hour", -1.0f, "Photo mode: the hour to start at (17.5 .. 30.5; -1 = the game's)", CVarFlags::None);
MCE_CVAR(cv_save, "save", "autosave", "Save slot: loaded at start when it exists, written at every mirror", CVarFlags::None);
MCE_CVAR(cv_newgame, "newgame", false, "Ignore the save slot at start", CVarFlags::None);
MCE_CVAR(cv_scatter, "r.scatter", true, "Draw the scatter volumes (grass, flowers, rocks, trees)", CVarFlags::None);

f64 now_ms() { return platform::now_seconds() * 1000.0; }

u32 hash_u32(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    return x ^ (x >> 16);
}
f32 hash01(u32 x) { return f32(hash_u32(x) & 0xFFFFu) / 65535.0f; }
f32 smooth_noise(f32 t, u32 seed) {
    const f32 i = std::floor(t);
    const f32 u = t - i;
    const u32 k = static_cast<u32>(static_cast<i32>(i)) * 7919u + seed * 104729u;
    const f32 s = u * u * (3.0f - 2.0f * u);
    return math::lerp(hash01(k), hash01(k + 7919u), s) * 2.0f - 1.0f;
}
f32 smoothstep01(f32 t) {
    t = clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ---- the time of day ----------------------------------------------------------------------------------

struct SkyKey {
    f32 hour;
    f32 sun_el, sun_az;        // degrees; the sun's position (azimuth from +Z toward +X; 270 = west)
    vec3 sun_color;
    f32 sun_intensity;
    vec3 ambient_sky, ambient_ground;
    vec3 zenith, horizon, ground;
    vec3 fog_color;
    f32 fog_density;
    f32 stars;                 // star-field intensity
    f32 moon_el, moon_az, moon_intensity;
    f32 exposure;
    f32 lantern;               // lantern gain
};

// Design §2.3, one key per regime (the finale key is the dawn's start).
constexpr SkyKey kKeys[] = {
    {17.5f, 22.0f, 255.0f, {1.0f, 0.92f, 0.78f}, 3.4f, {0.42f, 0.52f, 0.72f}, {0.30f, 0.28f, 0.22f}, {0.17f, 0.36f, 0.76f}, {0.72f, 0.80f, 0.92f}, {0.24f, 0.23f, 0.21f}, {0.74f, 0.80f, 0.92f}, 0.0020f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f},
    {18.67f, 10.0f, 262.0f, {1.0f, 0.74f, 0.46f}, 2.8f, {0.34f, 0.40f, 0.60f}, {0.30f, 0.22f, 0.15f}, {0.14f, 0.27f, 0.60f}, {0.95f, 0.62f, 0.36f}, {0.20f, 0.16f, 0.13f}, {0.90f, 0.62f, 0.42f}, 0.0035f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.25f},
    {19.5f, 1.5f, 268.0f, {1.0f, 0.46f, 0.20f}, 2.2f, {0.24f, 0.25f, 0.45f}, {0.25f, 0.14f, 0.10f}, {0.08f, 0.12f, 0.36f}, {0.95f, 0.36f, 0.16f}, {0.12f, 0.08f, 0.07f}, {0.70f, 0.32f, 0.22f}, 0.005f, 0.0f, 0.0f, 0.0f, 0.0f, 1.1f, 0.8f},
    {20.25f, -6.0f, 272.0f, {0.6f, 0.4f, 0.4f}, 0.0f, {0.12f, 0.16f, 0.35f}, {0.06f, 0.06f, 0.10f}, {0.03f, 0.05f, 0.18f}, {0.25f, 0.30f, 0.55f}, {0.04f, 0.04f, 0.06f}, {0.20f, 0.25f, 0.45f}, 0.008f, 0.3f, 12.0f, 110.0f, 0.05f, 1.4f, 1.0f},
    {22.0f, -20.0f, 285.0f, {0.0f, 0.0f, 0.0f}, 0.0f, {0.030f, 0.040f, 0.090f}, {0.015f, 0.015f, 0.020f}, {0.005f, 0.008f, 0.030f}, {0.030f, 0.040f, 0.090f}, {0.010f, 0.010f, 0.015f}, {0.02f, 0.03f, 0.07f}, 0.004f, 1.0f, 35.0f, 130.0f, 0.12f, 2.0f, 1.0f},
    {26.0f, -35.0f, 20.0f, {0.0f, 0.0f, 0.0f}, 0.0f, {0.020f, 0.028f, 0.070f}, {0.010f, 0.010f, 0.016f}, {0.003f, 0.005f, 0.020f}, {0.020f, 0.028f, 0.065f}, {0.008f, 0.008f, 0.012f}, {0.015f, 0.02f, 0.05f}, 0.0035f, 1.2f, 55.0f, 190.0f, 0.10f, 2.2f, 1.0f},
    {28.5f, -8.0f, 85.0f, {0.0f, 0.0f, 0.0f}, 0.0f, {0.08f, 0.11f, 0.22f}, {0.04f, 0.04f, 0.05f}, {0.03f, 0.05f, 0.14f}, {0.38f, 0.44f, 0.32f}, {0.05f, 0.05f, 0.06f}, {0.22f, 0.26f, 0.30f}, 0.006f, 0.4f, 25.0f, 245.0f, 0.06f, 1.5f, 0.8f},
    {29.83f, 1.0f, 92.0f, {1.0f, 0.70f, 0.45f}, 1.8f, {0.25f, 0.32f, 0.50f}, {0.22f, 0.18f, 0.15f}, {0.12f, 0.25f, 0.55f}, {0.95f, 0.70f, 0.45f}, {0.15f, 0.14f, 0.13f}, {0.85f, 0.70f, 0.55f}, 0.0045f, 0.1f, 10.0f, 260.0f, 0.02f, 1.1f, 0.4f},
    {30.5f, 7.0f, 98.0f, {1.0f, 0.86f, 0.66f}, 2.6f, {0.40f, 0.50f, 0.70f}, {0.30f, 0.25f, 0.20f}, {0.20f, 0.40f, 0.78f}, {0.90f, 0.86f, 0.76f}, {0.22f, 0.21f, 0.20f}, {0.86f, 0.86f, 0.86f}, 0.003f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f},
};

SkyKey eval_sky(f32 hour) {
    constexpr u32 n = sizeof(kKeys) / sizeof(kKeys[0]);
    if (hour <= kKeys[0].hour) return kKeys[0];
    if (hour >= kKeys[n - 1].hour) return kKeys[n - 1];
    u32 i = 0;
    while (i + 2 < n && hour > kKeys[i + 1].hour) ++i;
    const SkyKey& a = kKeys[i];
    const SkyKey& b = kKeys[i + 1];
    const f32 t = smoothstep01((hour - a.hour) / (b.hour - a.hour));
    SkyKey k;
    k.hour = hour;
#define SR_LERP(field) k.field = math::lerp(a.field, b.field, t)
    SR_LERP(sun_el); SR_LERP(sun_az); SR_LERP(sun_color); SR_LERP(sun_intensity); SR_LERP(ambient_sky); SR_LERP(ambient_ground);
    SR_LERP(zenith); SR_LERP(horizon); SR_LERP(ground); SR_LERP(fog_color); SR_LERP(fog_density); SR_LERP(stars);
    SR_LERP(moon_el); SR_LERP(moon_az); SR_LERP(moon_intensity); SR_LERP(exposure); SR_LERP(lantern);
#undef SR_LERP
    return k;
}

vec3 sky_dir(f32 el_deg, f32 az_deg) {
    const f32 el = radians(el_deg), az = radians(az_deg);
    return {std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)};
}

// ---- synthesized SFX (mono, 48 kHz) -------------------------------------------------------------
constexpr f32 kRate = static_cast<f32>(audio::kSampleRate);

void sweep(Array<f32>& out, f32 seconds, f32 hz0, f32 hz1, f32 gain, f32 noise, f32 decay) {
    out.resize(static_cast<usize>(seconds * kRate));
    f32 phase = 0.0f;
    u32 rng = 0x1234567u;
    for (usize i = 0; i < out.size(); ++i) {
        const f32 t = f32(i) / f32(out.size());
        phase += (hz0 + (hz1 - hz0) * t) / kRate;
        rng = rng * 1664525u + 1013904223u;
        const f32 n = f32(rng >> 9) / f32(1u << 23) * 2.0f - 1.0f;
        const f32 env = std::exp(-decay * t) * min(1.0f, f32(i) / 96.0f);
        out[i] = gain * env * ((1.0f - noise) * std::sin(phase * kTau) + noise * n);
    }
}
void chord(Array<f32>& out, f32 seconds, const f32* hz, u32 count, f32 gain, f32 decay) {
    out.resize(static_cast<usize>(seconds * kRate));
    for (usize i = 0; i < out.size(); ++i) {
        const f32 t = f32(i) / kRate;
        f32 v = 0.0f;
        for (u32 k = 0; k < count; ++k) v += std::sin(t * hz[k] * kTau) * (1.0f / f32(count));
        out[i] = gain * v * std::exp(-decay * t / seconds) * min(1.0f, f32(i) / 400.0f);
    }
}

struct Clips {
    Array<f32> chime, ratchet, flare, organ;
    audio::ClipHandle h_chime, h_ratchet, h_flare, h_organ;
};

struct Budget {
    f64 sim = 0, scene = 0, record = 0, submit = 0;
    f64 gpu_shadow = 0, gpu_main = 0, gpu_post = 0, gpu_total = 0;
    u64 frames = 0, gpu_frames = 0;
};

struct PhotoCam {
    bool active = false;
    vec3 pos{0.0f, 20.0f, 60.0f};
    f32 yaw = 0.0f, pitch = -0.2f;
    f32 hour = -1.0f;  // < 0: the game's hour
    bool bloom = true, fog = true, aa = true, hud = false;
};

class ReachApp final : public app::Application, public app::IPlatformHooks {
public:
    ReachApp(platform::Window* window, rhi::RenderDevice* device, audio::AudioDevice& audio, bool autoplay)
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
        // The pack: -pack, else assets.mce beside the executable (shipped), else the build tree's.
        FixedString<1024> path;
        if (!cv_pack.get_string().empty()) path.append(cv_pack.get_string());
        else {
            path.append(platform::base_path());
            path.append("assets.mce");
            std::FILE* f = std::fopen(path.c_str(), "rb");
            if (f) std::fclose(f);
            else {
                path.clear();
                path.append(MCE_PROJECT_PACK);
            }
        }
        const StringView roots[1] = {path.view()};
        asset::MountOptions mo;
        mo.verify_payload_crc = false;
        MCE_TRY(assets_.mount(Span<const StringView>(roots, 1), mo));
        for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k) {
            if (assets_.info(k).type != asset::ChunkType::Texture) continue;
            MCE_TRY_ASSIGN(asset::AssetHandle<asset::Texture> h, assets_.load_sync<asset::Texture>(k));
            handles_.push_back(asset::AnyHandle(h));
            if (assets_.info(k).path == "textures/stars_equirect.png") stars_key_ = k;
        }
        for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k) {
            if (assets_.info(k).type != asset::ChunkType::Mesh) continue;
            MCE_TRY_ASSIGN(asset::AssetHandle<asset::Mesh> h, assets_.load_sync<asset::Mesh>(k));
            handles_.push_back(asset::AnyHandle(h));
            if (!device_) MCE_TRY(cpu_meshes_.upload(k, assets_.generation(k), assets_.resolve(h)));
            const StringView p = assets_.info(k).path;
            if (p == "models/beam.glb") beam_key_ = k;
            else if (p == "models/keeper.glb") keeper_key_ = k;
            else if (p == "models/gull.glb") gull_key_ = k;
        }

        // Sound: a few synthesized clips; the listener rides the camera.
        make_clips();
        audio::AttenuationCurve curve;
        curve.model = audio::AttenuationCurve::Model::Inverse;
        curve.min_distance = 3.0f;
        curve.max_distance = 80.0f;
        MCE_TRY(audio_.set_attenuation(1, curve));

        input::install(world_);
        audio::install(world_, audio_);
        ReachState preset;
        preset.autopilot = autoplay_ ? 1u : 0u;
        interactive_ = window_ && !autoplay_;
        if (interactive_) preset.mode = Mode::Title;
        world_.set_resource(preset);
        MCE_TRY(solstice_reach::install(world_));

        // The scene, then the save on top of it (before the first tick, so Startup sees both).
        asset::Guid scene_guid;
        const StringView want = !cv_scene.get_string().empty() ? cv_scene.get_string() : StringView(MCE_PROJECT_START_SCENE);
        if (Result<asset::Guid> g = asset::parse_guid(want)) scene_guid = g.value();
        if (scene_guid.is_null()) {
            for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k)
                if (assets_.info(k).type == asset::ChunkType::Scene) {
                    scene_guid = assets_.info(k).guid;
                    break;
                }
        }
        if (scene_guid.is_null()) return Error(Errc::NotFound, "solstice_reach: no scene in the pack");
        MCE_TRY(scene::load(world_, assets_, scene_guid, &entities_));
        scene_guid_ = scene_guid;
        if (interactive_ && !cv_newgame.get_bool() && !cv_save.get_string().empty() && save::exists(cv_save.get_string())) {
            save::SaveInfo info;
            if (Result<save::LoadStats> st = save::load_game(world_, cv_save.get_string(), &info, 1)) {
                MCE_LOG_INFO("solstice_reach: save '{}' loaded ({} entities)", cv_save.get_string(), st.value().entities);
                continued_ = true;
            }
        }

        if (window_) {
            Array<byte> map;
            build_input_map(map);
            MCE_TRY(input_.load_map(map.span()));
            window_->set_event_handler(&ReachApp::on_event, this);
        }
        if (device_) {
            samples::HudFont hud;
            MCE_TRY_ASSIGN(font_, renderer_.create_font(hud.metrics, hud.glyphs.span(), {}, std::as_bytes(hud.atlas.span()),
                                                        "solstice_reach.hud_font"));
        }
        poses_.reserve(16384);
        if (cv_photo.get_bool()) {
            photo_.active = true;
            photo_.hud = true;
            photo_.hour = static_cast<f32>(cv_hour.get_float());
            photo_.pos = {0.0f, 24.0f, 96.0f};
            photo_.yaw = kPi;
            photo_.pitch = -0.18f;
            if (!cv_cam.get_string().empty()) {
                float v[5] = {};
                const char* spec = cv_cam.get_string().data();
                if (*spec == '@') ++spec;  // a leading '-' would read as a flag on the command line
                if (std::sscanf(spec, "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5) {
                    photo_.pos = {v[0], v[1], v[2]};
                    photo_.yaw = v[3];
                    photo_.pitch = v[4];
                }
            }
            if (interactive_) world_.resource<ReachState>().mode = Mode::Playing;
        }
        return ok();
    }

    void on_tick(const app::TickContext& t) override {
        const f64 t0 = now_ms();
        input::InputState in;
        if (t.actions) in = *t.actions;
        handle_photo_keys(in);
        if (photo_.active) in = input::InputState{};  // the island stands still under a free camera
        input_hash_ = hash_combine(input_hash_, input::hash_state(in));
        input::publish(world_, in);
        world_.tick(t.dt, t.tick_index);
        prev_time_ = time_;
        time_ += static_cast<f32>(t.dt);
        ++ticks_;
        sim_ms_ += now_ms() - t0;
        ReachState& gs = world_.resource<ReachState>();
        if (gs.chime > 0.0f) play(clips_.h_chime, keeper_pos(), -6.0f);
        if (gs.flare >= 0.999f && gs.save_requested) play(clips_.h_flare, keeper_pos(), -2.0f);
        if (gs.mode == Mode::Finale && gs.finale_t < static_cast<f32>(t.dt) * 1.5f) play(clips_.h_organ, kVaultOculus, 0.0f);
        if (gs.save_requested) {
            gs.save_requested = 0;
            if (interactive_ && !cv_save.get_string().empty()) {
                save::SaveInfo info;
                info.scene = scene_guid_;
                info.tick = ticks_;
                if (ResultV r = save::save_game(world_, cv_save.get_string(), info); !r) MCE_LOG_WARN("solstice_reach: autosave failed");
            }
        }
        if (!device_) audio_.update();
        if (const i32 every = cv_trace.get_int(); every > 0 && ticks_ % static_cast<u64>(every) == 0) {
            const vec3 kp = keeper_pos();
            const Keeper* k = world_.try_get<Keeper>(gs.keeper);
            std::printf("trace tick %llu keeper (%.1f %.1f %.1f) ground %.1f grounded %u speed %.2f cursor %u mirrors %u mode %u\n",
                        static_cast<unsigned long long>(ticks_), kp.x, kp.y, kp.z, terrain::height_f(kp.x, kp.z),
                        k ? k->grounded : 0u, k ? k->speed : 0.0f, gs.waypoint_cursor, current_mirrors(), static_cast<u32>(gs.mode));
        }
    }

    u32 current_mirrors() {
        const ReachState& gs = world_.resource<ReachState>();
        const Progress* p = world_.try_get<Progress>(gs.progress);
        return p ? p->mirrors : 0u;
    }

    void on_render(const app::RenderContext& rc) override {
        audio_.update();
        if (!device_) return;
        device_->begin_frame(rc.frame_index);
        const rhi::Extent2D bb = device_->backbuffer_size();
        const f64 t0 = now_ms();
        Scene3D scene(*meshes_, mem::frame());
        poses_.clear();
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
        if (rc.frame_index >= 10) {
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
        if (capture_requested_) {
            capture_requested_ = false;
            FixedString<512> p;
            p.append(platform::base_path());
            char name[64];
            std::snprintf(name, sizeof name, "solstice_reach_%u.ppm", ++captures_);
            p.append(name);
            capture(p.c_str(), bb);
        }
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
        const ReachState& gs = world_.resource<ReachState>();
        const Progress* p = world_.try_get<Progress>(gs.progress);
        static constexpr const char* kModes[] = {"title", "playing", "paused", "finale", "dawn"};
        std::printf("solstice_reach: %s, hour %.2f, mirrors %u/7, shards %u/%u, scatter %u instances in %u cells | sim %.3f ms/tick\n",
                    kModes[static_cast<u32>(gs.mode)], f64(p ? p->hour : 0.0f), p ? p->mirrors : 0u, p ? p->shards : 0u, gs.shards_total,
                    static_cast<u32>(world_.resource<ScatterCache>().instances.size()),
                    static_cast<u32>(world_.resource<ScatterCache>().cells.size()), sim_ms_ / f64(max<u64>(ticks_, 1)));
        // The project's standard headless line (<game>_headless_300 matches it).
        std::printf("solstice_reach: %llu ticks, %u entities, state_hash %016llx\n", static_cast<unsigned long long>(ticks_),
                    static_cast<u32>(world_.metrics().entities), static_cast<unsigned long long>(state_hash()));
    }

    void report_budget() const {
        if (budget_.frames == 0) return;
        const f64 n = f64(budget_.frames);
        std::printf("solstice_reach: frame budget over %llu frames (%u tris drawn, %u point lights, %u instances)\n",
                    static_cast<unsigned long long>(budget_.frames), stats_.triangles, stats_.point_lights, stats_.instances);
        std::printf("  | stage | CPU ms | GPU ms |\n  |---|---|---|\n");
        std::printf("  | gameplay tick (ECS + World3D) | %.3f | - |\n", sim_ms_ / f64(max<u64>(ticks_, 1)));
        std::printf("  | scene build (scatter cells + cull + sort + cascades + clusters) | %.3f | - |\n", budget_.scene / n);
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
    void make_clips() {
        const f32 penta[] = {523.25f, 659.25f, 783.99f, 1046.5f};
        chord(clips_.chime, 0.9f, penta, 4, 0.35f, 5.0f);
        sweep(clips_.ratchet, 0.12f, 220.0f, 180.0f, 0.3f, 0.6f, 10.0f);
        sweep(clips_.flare, 1.2f, 300.0f, 1400.0f, 0.4f, 0.1f, 3.0f);
        const f32 organ[] = {130.81f, 196.0f, 261.63f, 329.63f, 392.0f};
        chord(clips_.organ, 9.0f, organ, 5, 0.5f, 1.5f);
        clips_.h_chime = register_clip(clips_.chime);
        clips_.h_ratchet = register_clip(clips_.ratchet);
        clips_.h_flare = register_clip(clips_.flare);
        clips_.h_organ = register_clip(clips_.organ);
    }
    audio::ClipHandle register_clip(const Array<f32>& pcm) {
        audio::ClipPayload p;
        p.frames = pcm.data();
        p.frame_count = static_cast<u32>(pcm.size());
        auto h = audio_.register_clip(p);
        return h ? h.value() : audio::ClipHandle{};
    }
    void play(audio::ClipHandle clip, vec3 at, f32 gain_db) {
        if (clip.is_null()) return;
        audio::PlayParams s;
        s.gain_db = gain_db;
        s.attenuation_id = 1;
        s.position = at;
        audio_.play(clip, s);
    }
    vec3 keeper_pos() {
        const ReachState& gs = world_.resource<ReachState>();
        if (const Keeper* k = world_.try_get<Keeper>(gs.keeper)) return k->pos;
        return {};
    }

    static void build_input_map(Array<byte>& out) {
        using platform::GamepadAxis;
        using platform::GamepadButton;
        using platform::Key;
        input::InputMapBuilder b;
        b.map(MCE_SID("SolsticeReach"));
        b.action(MCE_SID("Move"), input::ActionType::Axis2D)
            .key(Key::D, 0, +1.f).key(Key::A, 0, -1.f).key(Key::W, 1, +1.f).key(Key::S, 1, -1.f)
            .key(Key::Right, 0, +1.f).key(Key::Left, 0, -1.f).key(Key::Up, 1, +1.f).key(Key::Down, 1, -1.f)
            .gamepad_axis(GamepadAxis::LeftX, 0, +1.f).gamepad_axis(GamepadAxis::LeftY, 1, -1.f);
        b.action(MCE_SID("Look"), input::ActionType::Axis2D)
            .mouse_delta(0).mouse_delta(1)
            .key(Key::Q, 0, -9.f).key(Key::E, 0, +9.f)
            .gamepad_axis(GamepadAxis::RightX, 0, +9.f).gamepad_axis(GamepadAxis::RightY, 1, +14.f);
        b.action(MCE_SID("Jump")).key(Key::Space).gamepad_button(GamepadButton::South);
        b.action(MCE_SID("Fire")).key(Key::J).mouse_button(platform::MouseButton::Left).gamepad_button(GamepadButton::West);
        b.action(MCE_SID("Interact")).key(Key::F).key(Key::J).mouse_button(platform::MouseButton::Left).gamepad_button(GamepadButton::West);
        b.action(MCE_SID("Sprint")).key(Key::LShift).gamepad_button(GamepadButton::LeftStick);
        b.action(MCE_SID("Lantern")).key(Key::L).gamepad_button(GamepadButton::North);
        b.action(MCE_SID("Pause")).key(Key::Escape).gamepad_button(GamepadButton::Start);
        b.action(MCE_SID("Confirm")).key(Key::Return).key(Key::Space).gamepad_button(GamepadButton::South);
        b.action(MCE_SID("Cancel")).key(Key::Backspace).gamepad_button(GamepadButton::East);
        b.action(MCE_SID("Photo")).key(Key::P).gamepad_button(GamepadButton::Back);
        b.action(MCE_SID("FlyUp")).key(Key::R);
        b.action(MCE_SID("FlyDown")).key(Key::F);
        b.action(MCE_SID("Bloom")).key(Key::B);
        b.action(MCE_SID("FogToggle")).key(Key::G);
        b.action(MCE_SID("Aa")).key(Key::T);
        b.action(MCE_SID("Hud")).key(Key::H);
        b.action(MCE_SID("Capture")).key(Key::C);
        b.action(MCE_SID("NewGame")).key(Key::N);
        for (u32 i = 0; i < 8; ++i) {
            char name[8];
            std::snprintf(name, sizeof name, "Hour%u", i + 1);
            b.action(StringId(name)).key(static_cast<Key>(static_cast<u32>(Key::Num1) + i));
        }
        b.encode(out);
    }

    // Photo mode and the title's shortcuts: host-side, never in the sim.
    void handle_photo_keys(const input::InputState& in) {
        ReachState& gs = world_.resource<ReachState>();
        if (!interactive_) return;
        if (gs.mode == Mode::Title && in.pressed(MCE_SID("NewGame"))) {
            // A fresh run: drop the save file and reset every Save field in place.
            if (!cv_save.get_string().empty()) std::remove(save::path_of(cv_save.get_string()).c_str());
            reset_run(world_);
        }
        const Progress* p = world_.try_get<Progress>(gs.progress);
        const bool unlocked = (p && p->finale_done) || cv_photo.get_bool();
        if (in.pressed(MCE_SID("Photo")) && (gs.mode == Mode::Paused || gs.mode == Mode::Dawn || photo_.active) && (unlocked || gs.mode == Mode::Paused)) {
            photo_.active = !photo_.active;
            if (photo_.active) {
                const Camera3D c = game_camera(1.0f, nullptr);
                photo_.pos = c.position;
                photo_.yaw = gs.cam_yaw;
                photo_.pitch = -gs.cam_pitch;
                photo_.hour = -1.0f;
                gs.mode = Mode::Paused;
            } else {
                gs.mode = (p && p->finale_done) ? Mode::Dawn : Mode::Paused;
            }
        }
        if (!photo_.active) return;
        const f32 dt = 1.0f / 60.0f;
        photo_.yaw = photo_.yaw - in.look.x * 0.0045f;
        photo_.pitch = clamp(photo_.pitch - in.look.y * 0.003f, -1.5f, 1.5f);
        const f32 cp = std::cos(photo_.pitch);
        const vec3 fwd{-std::sin(photo_.yaw) * cp, std::sin(photo_.pitch), -std::cos(photo_.yaw) * cp};
        const vec3 right{std::cos(photo_.yaw), 0.0f, -std::sin(photo_.yaw)};
        const f32 speed = in.held(MCE_SID("Sprint")) ? 40.0f : 10.0f;
        photo_.pos = photo_.pos + (fwd * in.move.y + right * in.move.x) * speed * dt;
        if (in.held(MCE_SID("FlyUp"))) photo_.pos.y += speed * dt;
        if (in.held(MCE_SID("FlyDown"))) photo_.pos.y -= speed * dt;
        for (u32 i = 0; i < 8; ++i) {
            char name[8];
            std::snprintf(name, sizeof name, "Hour%u", i + 1);
            if (in.pressed(StringId(name))) photo_.hour = kHourKeys[i == 7 ? 8 : i];
        }
        if (in.pressed(MCE_SID("Bloom"))) photo_.bloom = !photo_.bloom;
        if (in.pressed(MCE_SID("FogToggle"))) photo_.fog = !photo_.fog;
        if (in.pressed(MCE_SID("Aa"))) photo_.aa = !photo_.aa;
        if (in.pressed(MCE_SID("Hud"))) photo_.hud = !photo_.hud;
        if (in.pressed(MCE_SID("Capture"))) capture_requested_ = true;
    }

    // ---- rendering ------------------------------------------------------------------------------------

    Camera3D game_camera(f32 alpha, vec3* pivot_out) {
        const ReachState& gs = world_.resource<ReachState>();
        const f32 t = math::lerp(prev_time_, time_, alpha);
        Camera3D c;
        if (gs.mode == Mode::Finale) {
            // The finale: a slow orbit around the Vault while the beams converge and the sun rises.
            const f32 u = gs.finale_t / kFinaleSeconds;
            const vec3 pivot = kVaultOculus - vec3{0.0f, 6.0f, 0.0f};
            const f32 yaw = 0.6f + u * 2.2f;
            c = orbit_camera(pivot, yaw, 0.22f + 0.2f * u, 46.0f - 20.0f * u, radians(55.0f));
            if (pivot_out) *pivot_out = pivot;
        } else if (gs.mode == Mode::Title) {
            const vec3 pivot{-6.0f, 40.0f, -20.0f};
            c = orbit_camera(pivot, t * 0.05f, 0.18f, 150.0f, radians(50.0f));
            if (pivot_out) *pivot_out = pivot;
        } else {
            vec3 pivot{0.0f, 3.0f, 70.0f};
            if (const Keeper* k = world_.try_get<Keeper>(gs.keeper)) pivot = math::lerp(k->prev_pos, k->pos, alpha) + vec3{0.0f, 1.5f, 0.0f};
            f32 dyaw = gs.cam_yaw - gs.prev_cam_yaw;
            while (dyaw > kPi) dyaw -= kTau;
            while (dyaw < -kPi) dyaw += kTau;
            c = orbit_camera(pivot, gs.prev_cam_yaw + dyaw * alpha, gs.cam_pitch, math::lerp(gs.prev_boom, gs.boom, alpha), radians(62.0f));
            if (pivot_out) *pivot_out = pivot;
        }
        c.near_z = 0.08f;
        c.far_z = 600.0f;
        return c;
    }

    Camera3D camera(f32 alpha, vec3* pivot_out) {
        if (!photo_.active) return game_camera(alpha, pivot_out);
        Camera3D c;
        c.position = photo_.pos;
        c.orientation = quat_from_axis_angle({0, 1, 0}, photo_.yaw) * quat_from_axis_angle({1, 0, 0}, photo_.pitch);
        c.fov_y = radians(60.0f);
        c.near_z = 0.08f;
        c.far_z = 600.0f;
        if (pivot_out) *pivot_out = photo_.pos + rotate(c.orientation, {0.0f, 0.0f, -10.0f});
        return c;
    }

    u32 key_of(const asset::Guid& g) const {
        if (g.is_null()) return ~0u;
        const asset::AssetKey k = assets_.find(g);
        return k == asset::kInvalidAssetKey ? ~0u : k;
    }

    void add_mesh(Scene3D& scene, u32 key, const mat4& world, vec4 tint = {1, 1, 1, 1}, bool shadows = true, Span<const mat4> pose = {}) {
        if (key == ~0u) return;
        MeshInstance i;
        i.mesh = key;
        i.world = world;
        i.tint = tint;
        i.cast_shadows = shadows;
        i.pose = pose;
        scene.add(i);
    }

    Span<const mat4> pose_for(u32 key, const char* clip_name, f32 t) {
        const GpuMesh* mesh = meshes_->get(key);
        if (!mesh) return {};
        const u32 clip = mesh->find_clip(clip_name);
        if (clip == ~0u || mesh->nodes.empty()) return {};
        const usize first = poses_.size();
        if (first + mesh->nodes.size() > poses_.capacity()) return {};  // the reserve covers the island; never reallocate a span away
        poses_.resize(first + mesh->nodes.size());
        pose_nodes(*mesh, clip, t, Span<mat4>(poses_.data() + first, mesh->nodes.size()));
        return Span<const mat4>(poses_.data() + first, mesh->nodes.size());
    }

    static const char* clip_name(ClipKind k) {
        switch (k) {
            case ClipKind::Turn: return "turn";
            case ClipKind::Flutter: return "flutter";
            case ClipKind::Gust: return "gust";
            case ClipKind::Glide: return "glide";
            case ClipKind::Walk: return "walk";
            case ClipKind::Idle: return "idle";
            case ClipKind::None: break;
        }
        return nullptr;
    }

    static quat rotation_z_to(vec3 dir) {
        const vec3 fwd{0.0f, 0.0f, 1.0f};
        const f32 d = clamp(dot(fwd, dir), -1.0f, 1.0f);
        if (d > 0.9999f) return quat::identity();
        if (d < -0.9999f) return quat_from_axis_angle({0.0f, 1.0f, 0.0f}, kPi);
        return quat_from_axis_angle(normalize(cross(fwd, dir)), std::acos(d));
    }

    void add_beam(Scene3D& scene, vec3 from, vec3 to, f32 width, f32 alpha) {
        const vec3 d = to - from;
        const f32 len = length(d);
        if (len < 0.1f) return;
        const mat4 world = translation(from) * trs({}, rotation_z_to(d * (1.0f / len)), {width, width, len});
        add_mesh(scene, beam_key_, world, {1.0f, 1.0f, 1.0f, alpha}, false);
    }

    void fill_scene(Scene3D& scene, f32 alpha, rhi::Extent2D viewport) {
        const ReachState& gs = world_.resource<ReachState>();
        const Progress* prog = world_.try_get<Progress>(gs.progress);
        const f32 t = math::lerp(prev_time_, time_, alpha);
        vec3 pivot;
        const Camera3D cam = camera(alpha, &pivot);
        scene.begin(cam, viewport);
        const View3D& view = scene.view();

        audio::Listener ear;
        ear.position = cam.position;
        ear.forward = normalize(pivot - cam.position);
        audio_.set_listener(ear);

        // The sky at this hour.
        const f32 hour = photo_.active && photo_.hour > 0.0f ? photo_.hour : (prog ? prog->hour : kHourKeys[0]);
        const SkyKey k = eval_sky(hour);
        const vec3 sun_pos = sky_dir(k.sun_el, k.sun_az);
        const vec3 moon_pos = sky_dir(k.moon_el, k.moon_az);
        const bool moonlit = k.sun_el < -3.0f && k.moon_intensity > 0.001f;
        DirectionalLight light;
        if (moonlit) {
            light.direction = moon_pos * -1.0f;
            light.color = {0.62f, 0.72f, 1.0f};
            light.intensity = k.moon_intensity;
        } else {
            light.direction = sun_pos * -1.0f;
            light.color = k.sun_color;
            light.intensity = k.sun_intensity * smoothstep01((k.sun_el + 3.0f) / 6.0f);
        }
        light.ambient_sky = k.ambient_sky;
        light.ambient_ground = k.ambient_ground;
        light.shadows = true;
        scene.set_light(light);
        Sky sky;
        sky.zenith = k.zenith;
        sky.horizon = k.horizon;
        sky.ground = k.ground;
        sky.sun_intensity = moonlit ? 0.0f : 7.0f;
        sky.sun_size = 0.9993f;
        sky.specular_ambient = 0.6f;
        if (stars_key_ != ~0u && k.stars > 0.001f) {
            sky.texture = stars_key_;
            sky.texture_intensity = k.stars * 0.9f;
            sky.texture_additive = true;
        }
        scene.set_sky(sky);
        Fog fog;
        fog.color = k.fog_color;
        fog.density = (photo_.active && !photo_.fog) ? 0.0f : k.fog_density;
        scene.set_fog(fog);
        scene.set_exposure(k.exposure);
        PostFx post;
        post.bloom = !(photo_.active && !photo_.bloom);
        post.bloom_intensity = 0.09f;
        post.bloom_threshold = 1.1f;
        post.fxaa = !(photo_.active && !photo_.aa);
        scene.set_post(post);
        ShadowSettings shadows;
        shadows.cascades = 4;
        shadows.distance = 140.0f;
        shadows.splits[0] = 0.06f;
        shadows.splits[1] = 0.18f;
        shadows.splits[2] = 0.45f;
        shadows.splits[3] = 1.0f;
        shadows.blend = 0.12f;
        scene.set_shadows(shadows);
        lantern_gain_ = k.lantern;

        // Placed models (with their clips).
        world_.query<const MeshRef&, const LocalTransform&>().each([&](Entity e, const MeshRef& m, const LocalTransform& tr) {
            if (m.cull_radius > 0.0f && !sphere_visible(view, tr.position, m.cull_radius * std::max(tr.scale.x, std::max(tr.scale.y, tr.scale.z)))) return;
            const u32 key = key_of(m.mesh.guid);
            Span<const mat4> pose;
            if (const Animated* a = world_.try_get<Animated>(e))
                if (const char* name = clip_name(a->clip)) pose = pose_for(key, name, t * a->speed + a->phase);
            vec4 tint = m.tint;
            add_mesh(scene, key, trs(tr.position, tr.rotation, tr.scale), tint, m.cast_shadows != 0, pose);
        });
        // Dishes flash when just aligned (the dish entity is referenced by its heliostat).
        world_.query<const Heliostat&, const LocalTransform&>().each([&](const Heliostat& h, const LocalTransform& base) {
            if (h.aligned < 1.0f) return;
            vec3 target = kVaultOculus;
            if (const LocalTransform* n = world_.try_get<LocalTransform>(h.next)) target = n->position + vec3{0.0f, 1.9f, 0.0f};
            const vec3 from = base.position + h.beam_offset;
            const f32 pulse = 0.85f + 0.15f * std::sin(t * 2.0f + f32(h.index));
            f32 a = 0.35f * pulse;
            if (prog && prog->mirrors == h.index + 1) a += 0.5f * gs.flare;
            add_beam(scene, from, target, 0.22f + 0.3f * gs.flare * (prog && prog->mirrors == h.index + 1 ? 1.0f : 0.0f), a);
            PointLight l;  // the dish glows
            l.position = from;
            l.color = {1.0f, 0.85f, 0.55f};
            l.intensity = 1.5f + 12.0f * (prog && prog->mirrors == h.index + 1 ? gs.flare : 0.0f);
            l.radius = 9.0f;
            scene.add_point_light(l);
        });
        // The finale's shaft from the oculus, kept faint afterwards.
        if (gs.mode == Mode::Finale || (prog && prog->finale_done)) {
            const f32 u = gs.mode == Mode::Finale ? smoothstep01(gs.finale_t / 6.0f) : 0.25f;
            add_beam(scene, kVaultOculus, kVaultOculus + vec3{0.0f, 140.0f, 0.0f}, 1.2f + 2.0f * u, 0.3f * u);
            PointLight l;
            l.position = kVaultOculus - vec3{0.0f, 3.0f, 0.0f};
            l.color = {1.0f, 0.85f, 0.6f};
            l.intensity = 40.0f * u;
            l.radius = 30.0f;
            scene.add_point_light(l);
        }

        // Lights.
        world_.query<const Light3D&, const LocalTransform&>().each([&](Entity e, const Light3D& l, const LocalTransform& tr) {
            vec3 pos = tr.position + rotate(tr.rotation, l.offset);
            f32 gain = 1.0f;
            const u32 seed = e.index;
            switch (l.kind) {
                case LightKind::Lantern:
                case LightKind::Sconce:
                    gain = k.lantern * (1.0f - l.flicker * 0.5f + l.flicker * 0.5f * smooth_noise(t * 9.0f + f32(seed), seed));
                    break;
                case LightKind::Firefly: {
                    gain = clamp(k.stars, 0.0f, 1.0f) * (0.4f + 0.6f * (0.5f + 0.5f * std::sin(t * 3.0f + f32(seed) * 1.7f)));
                    pos = pos + vec3{smooth_noise(t * 0.4f, seed) * l.radius * 0.6f, smooth_noise(t * 0.5f, seed + 11) * 1.0f,
                                     smooth_noise(t * 0.4f, seed + 23) * l.radius * 0.6f};
                    break;
                }
                case LightKind::Flare:
                case LightKind::Moonbeam: gain = 1.0f; break;
            }
            if (gain <= 0.01f) return;
            if (!sphere_visible(view, pos, l.radius)) return;
            PointLight pl;
            pl.position = pos;
            pl.color = {l.color.r, l.color.g, l.color.b};
            pl.intensity = l.intensity * gain;
            pl.radius = l.kind == LightKind::Firefly ? 2.5f : l.radius;
            if (l.spot_outer > 0.0f) {
                pl.direction = normalize(rotate(tr.rotation, l.direction));
                pl.spot_outer = radians(l.spot_outer);
                pl.spot_inner = radians(l.spot_inner);
            }
            scene.add_point_light(pl);
            if (l.kind == LightKind::Firefly) {  // a tiny glowing body
                add_mesh(scene, key_of_shard(), translation(pos) * scaling({0.06f, 0.06f, 0.06f}), {1.0f, 1.0f, 0.6f, 1.0f}, false);
            }
        });

        // The Keeper and the lantern on the staff.
        if (const Keeper* kp = world_.try_get<Keeper>(gs.keeper); kp && gs.mode != Mode::Title) {
            const vec3 pos = math::lerp(kp->prev_pos, kp->pos, alpha) - vec3{0.0f, 0.95f, 0.0f};
            f32 dyaw = kp->yaw - kp->prev_yaw;
            while (dyaw > kPi) dyaw -= kTau;
            while (dyaw < -kPi) dyaw += kTau;
            const quat rot = quat_from_axis_angle({0, 1, 0}, kp->prev_yaw + dyaw * alpha);
            const bool walking = kp->speed > 0.3f;
            const Span<const mat4> pose = pose_for(keeper_key_, walking ? "walk" : "idle", kp->anim_time);
            add_mesh(scene, keeper_key_, trs(pos, rot, {1, 1, 1}), {1, 1, 1, 1}, true, pose);
            if (kp->lantern_on) {
                PointLight l;
                l.position = pos + rotate(rot, {0.36f, 1.75f, 0.25f});
                l.color = {1.0f, 0.68f, 0.35f};
                l.intensity = 1.2f + 2.3f * clamp(k.lantern, 0.0f, 1.0f) * (0.92f + 0.08f * smooth_noise(t * 11.0f, 999));
                l.radius = 9.0f;
                scene.add_point_light(l);
            }
        }

        // Gulls.
        world_.query<const GullPath&, const LocalTransform&>().each([&](Entity e, const GullPath& g, const LocalTransform& tr) {
            if (k.stars > 0.6f) return;  // gulls roost at night
            const f32 a = t * g.speed + g.phase;
            const vec3 pos = tr.position + vec3{std::cos(a) * g.radius, g.height + std::sin(a * 2.3f) * 1.5f, std::sin(a) * g.radius};
            const vec3 fwd = normalize(vec3{-std::sin(a), 0.0f, std::cos(a)});
            const quat rot = rotation_z_to(fwd) * quat_from_axis_angle({0, 0, 1}, -0.35f);
            add_mesh(scene, gull_key_, trs(pos, rot, {1, 1, 1}), {1, 1, 1, 1}, true, pose_for(gull_key_, "glide", t + f32(e.index)));
        });

        // The scatter cache, by cell.
        if (cv_scatter.get_bool()) {
            const ScatterCache& cache = world_.resource<ScatterCache>();
            Array<u32> keys;
            for (const asset::Guid& g : cache.meshes) keys.push_back(key_of(g));
            scatter_submitted_ = 0;
            for (const ScatterCell& c : cache.cells) {
                if (!sphere_visible(view, c.center, c.radius)) continue;
                const f32 dist = length(c.center - cam.position);
                for (u32 i = c.first; i < c.first + c.count; ++i) {
                    const ScatterInstance& inst = cache.instances[i];
                    // Small things (tufts, flowers) fade with distance: every other one past 60 m, none past 120 m.
                    if (inst.scale < 1.6f && dist > 60.0f && ((i & 1u) || dist > 120.0f)) continue;
                    add_mesh(scene, keys[inst.mesh], trs(inst.pos, inst.rot, {inst.scale, inst.scale, inst.scale}), {1, 1, 1, 1},
                             inst.scale >= 1.6f);
                    ++scatter_submitted_;
                }
            }
        }
        scene.build();
    }

    u32 key_of_shard() {
        if (shard_key_ == ~0u)
            for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k)
                if (assets_.info(k).path == "models/sunshard.glb") shard_key_ = k;
        return shard_key_;
    }

    void record_hud(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::Extent2D bb) {
        const ReachState& gs = world_.resource<ReachState>();
        const Progress* p = world_.try_get<Progress>(gs.progress);
        SpriteBatch hud(renderer_.sprite_sets(), mem::frame());
        hud.begin(compute_screen_view(bb), PassId::Screen2D);
        const bool show = !photo_.active || photo_.hud;
        if (show) {
            const f32 hour = photo_.active && photo_.hour > 0.0f ? photo_.hour : (p ? p->hour : 17.5f);
            const u32 hh = static_cast<u32>(std::fmod(hour, 24.0f));
            const u32 mm = static_cast<u32>((hour - std::floor(hour)) * 60.0f);
            char line[512];
            std::snprintf(line, sizeof line,
                          "SOLSTICE REACH\nMIRRORS %u/7   SHARDS %u/%u   %02u:%02u\n"
                          "TRIS %u  LIGHTS %u  INST %u  SCATTER %u  CPU %.2f MS  GPU %.2f MS",
                          p ? p->mirrors : 0u, p ? p->shards : 0u, gs.shards_total, hh, mm, scene.stats().triangles,
                          scene.stats().point_lights, scene.stats().instances, scatter_submitted_, last_scene_, gpu_.total);
            text(hud, line, vec2(16, 14), 1.0f);
            if (gs.journal_timer > 0.0f && gs.journal_line < 9 && gs.mode != Mode::Title)
                text(hud, kJournal[gs.journal_line], vec2(f32(bb.width) * 0.5f - 7.0f * 6.0f * 0.5f * f32(std::strlen(kJournal[gs.journal_line])) * 0.5f,
                                                        f32(bb.height) * 0.80f), 1.0f);
            if (gs.near_mirror && gs.mode == Mode::Playing) text(hud, "HOLD F TO ALIGN THE MIRROR", vec2(f32(bb.width) * 0.5f - 160.0f, f32(bb.height) * 0.70f), 1.5f);
            if (photo_.active)
                text(hud, "PHOTO MODE  WASD R F FLY  SHIFT FAST  1-8 HOUR  B BLOOM  G FOG  T AA  H HUD  C CAPTURE  P BACK",
                     vec2(16, f32(bb.height) - 30.0f), 1.0f);
        }
        const char* banner = nullptr;
        char cont[128];
        switch (gs.mode) {
            case Mode::Title:
                std::snprintf(cont, sizeof cont, "SOLSTICE REACH\n\nTHE LAST LIGHT OF THE LONGEST DAY\n\n%s\nN  NEW GAME",
                              continued_ ? "ENTER  CONTINUE" : "ENTER  BEGIN");
                banner = cont;
                break;
            case Mode::Paused: banner = photo_.active ? nullptr : "PAUSED\n\nESC RESUME   P PHOTO MODE"; break;
            case Mode::Finale: break;
            case Mode::Dawn:
            case Mode::Playing: break;
        }
        if (banner) text(hud, banner, vec2(f32(bb.width) * 0.5f - 220.0f, f32(bb.height) * 0.36f), 2.0f);
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
        p.tint = Color{1.0f, 0.9f, 0.7f, 1};
        hud.add_text(font_, layout, p);
    }

    void capture(const char* path, rhi::Extent2D size) {
        Array<u8> px;
        px.resize(usize(size.width) * size.height * 4);
        if (!device_->read_pixels({}, rhi::Rect2D{0, 0, size.width, size.height}, px.span())) return;
        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            MCE_LOG_ERROR("solstice_reach: cannot write '{}'", path);
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", size.width, size.height);
        for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
        MCE_LOG_INFO("solstice_reach: wrote {}", path);
    }

    static void on_event(const platform::Event& e, void* user) { static_cast<ReachApp*>(user)->input_.push_event(e); }

    platform::Window* window_;
    rhi::RenderDevice* device_;
    audio::AudioDevice& audio_;
    bool autoplay_;
    bool interactive_ = false;
    bool continued_ = false;
    bool capture_requested_ = false;
    u32 captures_ = 0;
    Renderer renderer_;
    asset::AssetSystem assets_;
    AssetUploadSink sink_;
    MeshStore cpu_meshes_;
    MeshStore* meshes_ = nullptr;
    Array<asset::AnyHandle> handles_;
    Array<Entity> entities_;
    Array<mat4> poses_;
    asset::Guid scene_guid_;
    u32 stars_key_ = ~0u, beam_key_ = ~0u, keeper_key_ = ~0u, gull_key_ = ~0u, shard_key_ = ~0u;
    u32 scatter_submitted_ = 0;
    f32 lantern_gain_ = 0.0f;
    Clips clips_;
    Font font_;
    PhotoCam photo_;
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
    platform::show_error_box("Solstice Reach", text.c_str());
    return 1;
}

int run_headless() {
    audio::AudioDesc ad;
    ad.backend = audio::Backend::Silent;
    auto audio = audio::AudioDevice::create(ad);
    if (!audio) return 1;
    ReachApp app(nullptr, nullptr, *audio.value(), true);
    app::AppDesc desc;
    desc.name = "solstice_reach";
    desc.headless = true;
    desc.max_ticks = static_cast<u64>(cv_ticks.get_int() > 0 ? cv_ticks.get_int() : 1800);
    const int rc = app::run(app, desc);
    if (rc == 0) app.report();
    return rc;
}

int run_windowed() {
    platform::WindowDesc wd;
    wd.title = MCE_PROJECT_TITLE;
    wd.width = 1280;
    wd.height = 720;
    auto window = platform::Window::create(wd);
    if (!window) return fail("window creation failed", window.error());
    rhi::DeviceDesc dd;
    dd.debug_layer = cv_gl_debug.get_bool();
    auto device = rhi::create_default_device(*window.value(), dd);
    if (!device) return fail("render device creation failed", device.error());
    auto audio = audio::AudioDevice::create({});
    if (!audio) return fail("audio init failed", audio.error());

    ReachApp app(window.value().get(), device.value().get(), *audio.value(), cv_autoplay.get_bool());
    app::AppDesc desc;
    desc.name = "solstice_reach";
    desc.platform = &app;
    desc.fences = device.value().get();
    desc.input_source = &app.input_;
    app::RunStats stats;
    int rc = app::run(app, desc, &stats);
    app.report();
    app.report_budget();
    const u32 gl_errors = device.value()->validation_error_count();
    std::printf("solstice_reach: %llu frames, %u GL errors\n", static_cast<unsigned long long>(stats.frames), gl_errors);
    if (rc == 0 && gl_errors != 0) rc = 2;
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    log::LogDesc log_desc;
    log_desc.app_name = "solstice_reach";
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
