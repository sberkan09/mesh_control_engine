// Solstice Reach presentation: the game's state turned into the engine's 3D kit (the stock player
// draws it). Tick-driven and deterministic like the sim; the kit draws moving entities between
// the last two ticks, so a camera or a gull moved here is smooth at any frame rate.
//
//   FixedUpdate_Pre   sr_sounds    PlaySfx for a shard, an alignment and the finale
//   FixedUpdate_Post  sr_present   the camera, the time of day (Environment3D), lantern / sconce /
//                                  firefly glows, the beams, the Keeper's body and lantern, gulls
//   FixedUpdate_Post  sr_hud       the HUD's TextLabels under the screen camera
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mce/audio/components.h"
#include "mce/kit/components.h"
#include "solstice_reach.h"

namespace solstice_reach {

using namespace mce;
using namespace mce::math;
using kit::Environment3D;

namespace {

// ---- noise -------------------------------------------------------------------------------------

u32 hash_u32(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    return x ^ (x >> 16);
}
f32 hash01(u32 x) { return static_cast<f32>(hash_u32(x) & 0xFFFFu) / 65535.0f; }
f32 smooth_noise(f32 t, u32 seed) {
    const f32 i = std::floor(t);
    const f32 u = t - i;
    const u32 k = static_cast<u32>(static_cast<i32>(i)) * 7919u + seed * 104729u;
    const f32 s = u * u * (3.0f - 2.0f * u);
    return lerp(hash01(k), hash01(k + 7919u), s) * 2.0f - 1.0f;
}
f32 smoothstep01(f32 t) {
    t = clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ---- the time of day ---------------------------------------------------------------------------

struct SkyKey {
    f32 hour;
    f32 sun_el, sun_az;  // degrees; the sun's position (azimuth from +Z toward +X; 270 = west)
    vec3 sun_color;
    f32 sun_intensity;
    vec3 ambient_sky, ambient_ground;
    vec3 zenith, horizon, ground;
    vec3 fog_color;
    f32 fog_density;
    f32 stars;  // star-field intensity
    f32 moon_el, moon_az, moon_intensity;
    f32 exposure;
    f32 lantern;  // lantern gain
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
#define SR_LERP(field) k.field = lerp(a.field, b.field, t)
    SR_LERP(sun_el);
    SR_LERP(sun_az);
    SR_LERP(sun_color);
    SR_LERP(sun_intensity);
    SR_LERP(ambient_sky);
    SR_LERP(ambient_ground);
    SR_LERP(zenith);
    SR_LERP(horizon);
    SR_LERP(ground);
    SR_LERP(fog_color);
    SR_LERP(fog_density);
    SR_LERP(stars);
    SR_LERP(moon_el);
    SR_LERP(moon_az);
    SR_LERP(moon_intensity);
    SR_LERP(exposure);
    SR_LERP(lantern);
#undef SR_LERP
    return k;
}

vec3 sky_dir(f32 el_deg, f32 az_deg) {
    const f32 el = radians(el_deg), az = radians(az_deg);
    return {std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)};
}

// The kit's world light for an hour. `stars` is the scene's authored Environment3D star map.
void write_environment(Environment3D& env, const SkyKey& k, const asset::AssetRef<asset::Texture>& stars) {
    const vec3 sun_pos = sky_dir(k.sun_el, k.sun_az);
    const vec3 moon_pos = sky_dir(k.moon_el, k.moon_az);
    const bool moonlit = k.sun_el < -3.0f && k.moon_intensity > 0.001f;
    if (moonlit) {
        env.sun_direction = moon_pos * -1.0f;
        env.sun_color = {0.62f, 0.72f, 1.0f};
        env.sun_intensity = k.moon_intensity;
    } else {
        env.sun_direction = sun_pos * -1.0f;
        env.sun_color = k.sun_color;
        env.sun_intensity = k.sun_intensity * smoothstep01((k.sun_el + 3.0f) / 6.0f);
    }
    env.sun_shadows = 1;
    env.ambient_sky = k.ambient_sky;
    env.ambient_ground = k.ambient_ground;
    env.sky_enabled = 1;
    env.sky_zenith = k.zenith;
    env.sky_horizon = k.horizon;
    env.sky_ground = k.ground;
    env.sky_sun_intensity = moonlit ? 0.0f : 7.0f;
    env.sky_sun_size = 0.9993f;
    env.sky_specular_ambient = 0.6f;
    const bool starry = !stars.guid.is_null() && k.stars > 0.001f;
    env.sky_texture = starry ? stars : asset::AssetRef<asset::Texture>{};
    env.sky_texture_intensity = starry ? k.stars * 0.9f : 0.0f;
    env.sky_texture_additive = 1;
    env.fog_color = k.fog_color;
    env.fog_density = k.fog_density;
    env.exposure = k.exposure;
    env.bloom = 1;
    env.bloom_intensity = 0.09f;
    env.bloom_threshold = 1.1f;
    env.fxaa = 1;
    env.shadow_cascades = 4;
    env.shadow_distance = 140.0f;
    env.shadow_splits = {0.06f, 0.18f, 0.45f, 1.0f};
    env.shadow_blend = 0.12f;
}

// ---- placement helpers -------------------------------------------------------------------------

// The camera looking at `target` from `distance` away (yaw about +Y, pitch below the horizon).
void orbit(LocalTransform& t, vec3 target, f32 yaw, f32 pitch, f32 distance) {
    t.rotation = normalize(quat_from_axis_angle({0, 1, 0}, yaw) * quat_from_axis_angle({1, 0, 0}, -pitch));
    t.position = target - rotate(t.rotation, {0, 0, -1}) * distance;
}

quat rotation_z_to(vec3 dir) {
    const vec3 fwd{0.0f, 0.0f, 1.0f};
    const f32 d = clamp(dot(fwd, dir), -1.0f, 1.0f);
    if (d > 0.9999f) return quat::identity();
    if (d < -0.9999f) return quat_from_axis_angle({0.0f, 1.0f, 0.0f}, kPi);
    return quat_from_axis_angle(normalize(cross(fwd, dir)), std::acos(d));
}

// A beam model (unit length along +Z) stretched from `from` to `to`.
void place_beam(LocalTransform& t, vec3 from, vec3 to, f32 width) {
    const vec3 d = to - from;
    const f32 len = std::max(length(d), 0.01f);
    t.position = from;
    t.rotation = rotation_z_to(d * (1.0f / len));
    t.scale = {width, width, len};
}

const Progress* progress_of(World& w, const ReachState& gs) { return w.try_get<Progress>(gs.progress); }

f32 hour_of(World& w, const ReachState& gs) {
    const Progress* p = progress_of(w, gs);
    return p ? p->hour : kHourKeys[0];
}

// ---- sounds ------------------------------------------------------------------------------------

void play(Frame& f, const asset::AssetRef<asset::Sfx>& clip, f32 gain_db) {
    if (clip.guid.is_null()) return;
    audio::PlaySfx ev;
    ev.clip = clip.guid;
    ev.gain_db = gain_db;
    f.events().push(ev);
}

void sr_sounds(Frame& f) {
    const ReachState& gs = f.resource<ReachState>();
    f.query<const ReachSounds&>().each([&](const ReachSounds& s) {
        if (gs.chime > 0.0f) play(f, s.chime, -6.0f);
        if (gs.flare >= 1.0f) play(f, s.flare, -2.0f);
        if (gs.mode == Mode::Finale && gs.finale_t == 0.0f) play(f, s.organ, 0.0f);
    });
}

// ---- the 3D view -------------------------------------------------------------------------------

void present_camera(Frame& f, ReachState& gs) {
    const Keeper* k = f.world().try_get<Keeper>(gs.keeper);
    f.query<kit::Camera3DComponent&, LocalTransform&>().each([&](kit::Camera3DComponent& cam, LocalTransform& t) {
        if (!cam.primary) return;
        if (gs.mode == Mode::Finale) {
            // A slow orbit around the Vault while the beams converge and the sun rises.
            const f32 u = gs.finale_t / kFinaleSeconds;
            orbit(t, kVaultOculus - vec3{0.0f, 6.0f, 0.0f}, 0.6f + u * 2.2f, 0.22f + 0.2f * u, 46.0f - 20.0f * u);
            cam.fov_deg = 55.0f;
        } else if (gs.mode == Mode::Title) {
            orbit(t, {-6.0f, 40.0f, -20.0f}, gs.time * 0.05f, 0.18f, 150.0f);
            cam.fov_deg = 50.0f;
        } else {
            const vec3 pivot = (k ? k->pos : vec3{0.0f, 1.5f, 70.0f}) + vec3{0.0f, 1.5f, 0.0f};
            orbit(t, pivot, gs.cam_yaw, gs.cam_pitch, gs.boom);
            cam.fov_deg = 62.0f;
        }
        cam.mouse_look = gs.autopilot ? 0u : 1u;
    });
}

void present_environment(Frame& f, const SkyKey& k) {
    asset::AssetRef<asset::Texture> stars;
    bool authored = false;
    f.query<const Environment3D&>().each([&](const Environment3D& e) {
        if (authored) return;
        authored = true;
        stars = e.sky_texture;
    });
    if (Environment3D* env = f.world().try_resource<Environment3D>()) write_environment(*env, k, stars);
}

void present_glows(Frame& f, const ReachState& gs, const SkyKey& k) {
    const f32 t = gs.time;
    f.query<Glow&, kit::PointLight3D&, LocalTransform&>().each([&](Entity e, Glow& g, kit::PointLight3D& l, LocalTransform& tr) {
        const u32 seed = e.index;
        f32 gain = 1.0f;
        switch (g.kind) {
            case GlowKind::Lantern:
            case GlowKind::Sconce:
                gain = k.lantern * (1.0f - g.flicker * 0.5f + g.flicker * 0.5f * smooth_noise(t * 9.0f + static_cast<f32>(seed), seed));
                break;
            case GlowKind::Firefly: {
                if (!g.placed) {
                    g.home = tr.position;
                    g.placed = 1;
                }
                gain = clamp(k.stars, 0.0f, 1.0f) * (0.4f + 0.6f * (0.5f + 0.5f * std::sin(t * 3.0f + static_cast<f32>(seed) * 1.7f)));
                tr.position = g.home + vec3{smooth_noise(t * 0.4f, seed) * 1.8f, smooth_noise(t * 0.5f, seed + 11) * 1.0f,
                                            smooth_noise(t * 0.4f, seed + 23) * 1.8f};
                if (kit::MeshRef* body = f.world().try_get_mut<kit::MeshRef>(e)) body->visible = gain > 0.05f ? 1u : 0u;
                break;
            }
            case GlowKind::Steady: break;
        }
        l.intensity = gain > 0.01f ? g.intensity * gain : 0.0f;
    });
}

void present_beams(Frame& f, const ReachState& gs) {
    const Progress* prog = progress_of(f.world(), gs);
    const u32 mirrors = prog ? prog->mirrors : 0u;
    const f32 t = gs.time;
    // Where each heliostat's beam starts and ends, by index.
    vec3 from[kMirrors] = {}, to[kMirrors] = {};
    bool aligned[kMirrors] = {};
    f.query<const Heliostat&, const LocalTransform&>().each([&](const Heliostat& h, const LocalTransform& base) {
        if (h.index >= kMirrors) return;
        from[h.index] = base.position + h.beam_offset;
        to[h.index] = kVaultOculus;
        if (const LocalTransform* n = f.world().try_get<LocalTransform>(h.next)) to[h.index] = n->position + vec3{0.0f, 1.9f, 0.0f};
        aligned[h.index] = h.aligned >= 1.0f;
    });
    const bool finale = gs.mode == Mode::Finale || (prog && prog->finale_done);
    f.query<const Beam&, kit::MeshRef&, LocalTransform&>().each([&](Entity e, const Beam& b, kit::MeshRef& m, LocalTransform& tr) {
        kit::PointLight3D* glow = f.world().try_get_mut<kit::PointLight3D>(e);
        if (b.mirror >= kMirrors) {  // the finale's shaft from the oculus, kept faint afterwards
            const f32 u = gs.mode == Mode::Finale ? smoothstep01(gs.finale_t / 6.0f) : 0.25f;
            m.visible = finale ? 1u : 0u;
            place_beam(tr, kVaultOculus, kVaultOculus + vec3{0.0f, 140.0f, 0.0f}, 1.2f + 2.0f * u);
            m.tint.w = 0.3f * u;
            if (glow) {  // below the oculus; the offset is in the (stretched, turned) entity's space
                glow->offset = rotate(conjugate(tr.rotation), vec3{0.0f, -3.0f, 0.0f});
                glow->intensity = finale ? 40.0f * u : 0.0f;
                glow->radius = 30.0f;
            }
            return;
        }
        const bool on = aligned[b.mirror];
        const bool latest = mirrors == b.mirror + 1;
        m.visible = on ? 1u : 0u;
        const f32 flare = latest ? gs.flare : 0.0f;
        place_beam(tr, from[b.mirror], to[b.mirror], 0.22f + 0.3f * flare);
        const f32 pulse = 0.85f + 0.15f * std::sin(t * 2.0f + static_cast<f32>(b.mirror));
        m.tint.w = 0.35f * pulse + 0.5f * flare;
        if (glow) {  // the dish glows at the beam's start (the entity's origin)
            glow->offset = {};
            glow->intensity = on ? 1.5f + 12.0f * flare : 0.0f;
            glow->radius = 9.0f;
        }
    });
}

void present_keeper(Frame& f, const ReachState& gs, const SkyKey& k) {
    const Keeper* kp = f.world().try_get<Keeper>(gs.keeper);
    f.query<const KeeperModel&, LocalTransform&>().each([&](Entity e, const KeeperModel& km, LocalTransform& tr) {
        kit::MeshRef* m = f.world().try_get_mut<kit::MeshRef>(e);
        if (m) m->visible = (kp && gs.mode != Mode::Title) ? 1u : 0u;
        if (!kp) return;
        tr.position = kp->pos - vec3{0.0f, km.lift, 0.0f};
        tr.rotation = quat_from_axis_angle({0, 1, 0}, kp->yaw);
        if (kit::Animator* a = f.world().try_get_mut<kit::Animator>(e)) {
            const bool walking = kp->speed > 0.3f;
            a->clip.assign(walking ? StringView("walk") : StringView("idle"));
            a->speed = walking ? std::max(0.6f, kp->speed / 4.5f) : 1.0f;
        }
        if (kit::PointLight3D* l = f.world().try_get_mut<kit::PointLight3D>(e)) {
            const bool lit = kp->lantern_on && gs.mode != Mode::Title;
            l->intensity = lit ? 1.2f + 2.3f * clamp(k.lantern, 0.0f, 1.0f) * (0.92f + 0.08f * smooth_noise(gs.time * 11.0f, 999)) : 0.0f;
        }
    });
}

void present_gulls(Frame& f, const ReachState& gs, const SkyKey& k) {
    const f32 t = gs.time;
    f.query<GullPath&, LocalTransform&>().each([&](Entity e, GullPath& g, LocalTransform& tr) {
        if (!g.placed) {
            g.center = tr.position;
            g.placed = 1;
        }
        const f32 a = t * g.speed + g.phase;
        tr.position = g.center + vec3{std::cos(a) * g.radius, g.height + std::sin(a * 2.3f) * 1.5f, std::sin(a) * g.radius};
        vec3 fwd = normalize(vec3{-std::sin(a), 0.0f, std::cos(a)});
        if (g.speed < 0.0f) fwd = fwd * -1.0f;
        tr.rotation = rotation_z_to(fwd) * quat_from_axis_angle({0, 0, 1}, -0.35f);
        if (kit::MeshRef* m = f.world().try_get_mut<kit::MeshRef>(e)) m->visible = k.stars > 0.6f ? 0u : 1u;  // they roost at night
    });
}

void sr_present(Frame& f) {
    ReachState& gs = f.resource<ReachState>();
    gs.time += f.dt;
    gs.save_requested = 0;  // the stock player writes the -save slot at every clean quit
    const SkyKey k = eval_sky(hour_of(f.world(), gs));
    present_camera(f, gs);
    present_environment(f, k);
    present_glows(f, gs, k);
    present_beams(f, gs);
    present_keeper(f, gs, k);
    present_gulls(f, gs, k);
}

// ---- the HUD -----------------------------------------------------------------------------------

// The journal line `index` split at a space into two rows that fit a TextLabel.
void journal_row(u32 index, u32 row, FixedString<64>& out) {
    out.clear();
    if (index >= 9) return;
    const StringView line(kJournal[index]);
    usize split = line.size();
    if (line.size() > 60) {
        split = line.size() / 2;
        while (split < line.size() && line[split] != ' ') ++split;
    }
    const StringView part = row == 0 ? line.substr(0, split) : (split < line.size() ? line.substr(split + 1) : StringView());
    out.assign(part.substr(0, 63));
}

void sr_hud(Frame& f) {
    const ReachState& gs = f.resource<ReachState>();
    const Progress* p = progress_of(f.world(), gs);
    f32 w = 1280.0f, h = 720.0f;
    if (const kit::ViewInfo* v = f.world().try_resource<kit::ViewInfo>()) {
        w = static_cast<f32>(v->framebuffer_width);
        h = static_cast<f32>(v->framebuffer_height);
    }
    // The screen camera draws one world unit per device pixel, origin at the centre, +y up.
    const f32 left = -0.5f * w + 18.0f, top = 0.5f * h - 22.0f;
    char line[64];
    f.query<const HudText&, kit::TextLabel&, LocalTransform&>().each([&](const HudText& hud, kit::TextLabel& label, LocalTransform& t) {
        label.text.clear();
        switch (hud.kind) {
            case HudKind::Status:
                t.position = {left, top - 28.0f * static_cast<f32>(hud.line), 0.0f};
                if (gs.mode == Mode::Title) break;
                if (hud.line == 0) {
                    label.text.assign(StringView(gs.attract ? "SOLSTICE REACH   ENTER TO PLAY" : "SOLSTICE REACH"));
                } else {
                    const f32 hour = p ? p->hour : kHourKeys[0];
                    const u32 hh = static_cast<u32>(std::fmod(hour, 24.0f));
                    const u32 mm = static_cast<u32>((hour - std::floor(hour)) * 60.0f);
                    std::snprintf(line, sizeof line, "MIRRORS %u/7   SHARDS %u/%u   %02u:%02u", p ? p->mirrors : 0u, p ? p->shards : 0u,
                                  gs.shards_total, hh, mm);
                    label.text.assign(StringView(line));
                }
                break;
            case HudKind::Journal:
                t.position = {0.0f, -0.30f * h - 30.0f * static_cast<f32>(hud.line), 0.0f};
                if (gs.journal_timer > 0.0f && gs.mode != Mode::Title) journal_row(gs.journal_line, hud.line, label.text);
                break;
            case HudKind::Prompt:
                t.position = {0.0f, -0.20f * h, 0.0f};
                if (gs.near_mirror && gs.mode == Mode::Playing && !gs.autopilot) label.text.assign(StringView("HOLD F TO ALIGN THE MIRROR"));
                break;
            case HudKind::Banner:
                t.position = {0.0f, 0.16f * h - (hud.line == 0 ? 0.0f : 30.0f + 40.0f * static_cast<f32>(hud.line)), 0.0f};
                if (gs.mode != Mode::Title) break;
                if (hud.line == 0) label.text.assign(StringView("SOLSTICE REACH"));
                else if (hud.line == 1) label.text.assign(StringView("THE LAST LIGHT OF THE LONGEST DAY"));
                else label.text.assign(StringView(gs.continued ? "ENTER  CONTINUE      N  NEW GAME" : "ENTER  BEGIN"));
                break;
        }
    });
}

}  // namespace

void install_present(World& world) {
    world.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);  // the audio bridge registers it too (idempotent)
    world.add_system(Phase::FixedUpdate_Pre, "sr_sounds", &sr_sounds, "sr_keeper");
    world.add_system(Phase::FixedUpdate_Post, "sr_present", &sr_present, "sr_mirrors");
    world.add_system(Phase::FixedUpdate_Post, "sr_hud", &sr_hud, "sr_present");
}

}  // namespace solstice_reach
