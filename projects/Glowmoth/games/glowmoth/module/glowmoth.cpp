#include "glowmoth.h"

#include <cmath>
#include <cstdio>

#include "mce/input/actions.h"
#include "mce/log.h"
#include "mce/scene/scene.h"

namespace glowmoth {

using namespace mce;
using namespace mce::physics;

namespace {

constexpr f32 kGroundAccel = 60.0f;
constexpr f32 kAirAccel = 35.0f;
constexpr f32 kLowJumpGravityScale = 2.2f;
constexpr u32 kCoyoteTicks = 6;
constexpr u32 kJumpBufferTicks = 6;
constexpr u32 kInvulnTicks = 90;
constexpr u32 kGateDelayTicks = 40;  // the enter sound plays before the next scene loads
constexpr u8 kWorldLayer = 0;

vec2 half_extents(const World& w, Entity e) {
    if (const Collider2D* c = w.try_get<Collider2D>(e); c && c->shape.type == ShapeType::Box) return c->shape.half;
    return {0.5f, 0.5f};
}

void play(Frame& f, const asset::AssetRef<asset::Sfx>& clip, f32 gain_db = 0.0f) {
    if (clip.guid.is_null()) return;
    audio::PlaySfx ev;
    ev.clip = clip.guid;
    ev.gain_db = gain_db;
    f.events().push(ev);
}

void set_texture(kit::Sprite& s, const asset::AssetRef<asset::Texture>& tex) {
    if (tex.guid.is_null() || s.texture.guid == tex.guid) return;
    s.texture = asset::AssetRef<asset::Texture>{tex.guid, {}};
}

void go_to(Frame& f, GlowState& gs, StringView scene_path) {
    if (gs.leaving != 0 || scene_path.size() == 0) return;
    gs.leaving = 1;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.*s", static_cast<int>(scene_path.size()), scene_path.data());
    MCE_LOG_INFO("glowmoth: going to {}", static_cast<const char*>(buf));
    scene::request_change(f.world(), scene_path);
}

// ---- FixedUpdate_Pre ---------------------------------------------------------------------------

void glowmoth_setup(Frame& f) {
    GlowState& gs = f.resource<GlowState>();
    const scene::SceneChange* sc = f.world().try_resource<scene::SceneChange>();
    const u32 serial = sc ? sc->changes : 0u;
    if (gs.scene_serial != serial) {
        gs.scene_serial = serial;
        gs.lanterns = gs.lanterns_total = gs.gate_open = gs.leaving = 0;
        gs.ticks = 0;
        gs.moth = Entity{};
        f.query<const MothController&>().each([&](Entity e, const MothController&) {
            if (gs.moth.is_null()) gs.moth = e;
        });
        f.query<const Lantern&>().each([&](const Lantern&) { ++gs.lanterns_total; });
        f.query<const MenuScreen&>().each([&](const MenuScreen& s) {
            if (s.reset_run != 0) {
                gs.hits = 0;
                gs.levels_done = 0;
            }
        });
    }
    ++gs.ticks;
}

void glowmoth_screens(Frame& f) {
    GlowState& gs = f.resource<GlowState>();
    const input::InputState& in = f.resource<input::InputState>();
    const bool go = in.jump_pressed || in.confirm_pressed;
    f.query<const MenuScreen&>().each([&](const MenuScreen& s) {
        if (go && gs.ticks > s.min_ticks && gs.leaving == 0) {
            play(f, s.sound);
            go_to(f, gs, s.next_scene.view());
        }
    });
}

void glowmoth_bats(Frame& f) {
    const GlowState& gs = f.resource<GlowState>();
    const f32 t = static_cast<f32>(gs.ticks) * f.dt;
    f.query<Bat&, LocalTransform&, kit::Sprite&>().each([&](Bat& b, LocalTransform& lt, kit::Sprite& s) {
        if (b.started == 0) {
            b.origin = {lt.position.x, lt.position.y};
            b.started = 1;
        }
        const f32 period = b.period > 0.0f ? b.period : 1.0f;
        const f32 a = math::kTau * t / period + b.phase;
        // Across between the flowers, swooping down to them at both ends: high in the middle.
        const vec2 p{b.origin.x + b.extent.x * std::sin(a), b.origin.y + b.extent.y * std::cos(2.0f * a)};
        // Face the way it flies (the derivative of sin is cos).
        const f32 dx = b.extent.x * std::cos(a);
        if (dx > 0.01f) s.flip_x = false;
        if (dx < -0.01f) s.flip_x = true;
        lt.position.x = p.x;
        lt.position.y = p.y;
    });
}

void send_back(Frame& f, GlowState& gs, World2D& pw, Entity e, MothController& m, bool hit) {
    pw.teleport(e, m.spawn, 0.0f);
    m.vel = {};
    m.coyote = m.jump_buffer = 0;
    m.invuln = kInvulnTicks;
    play(f, m.hurt_sound);
    if (!hit) return;
    ++gs.hits;
    MCE_LOG_INFO("glowmoth: hit by a bat ({}/{})", gs.hits, kMaxHits);
    if (gs.hits >= kMaxHits) go_to(f, gs, "scenes/lose.mcscene");
}

void glowmoth_moth(Frame& f) {
    GlowState& gs = f.resource<GlowState>();
    World2D& pw = f.resource<World2D>();
    const input::InputState& in = f.resource<input::InputState>();
    const f32 dt = f.dt;

    f.query<MothController&, const LocalTransform&>().each([&](Entity e, MothController& m, const LocalTransform& lt) {
        if (e != gs.moth || !pw.has_body(e)) return;
        if (m.started == 0) {
            m.spawn = {lt.position.x, lt.position.y};
            m.started = 1;
            m.flaps_left = m.air_flaps;
        }
        if (m.invuln > 0) --m.invuln;
        const bool control = gs.leaving == 0;
        const f32 mx = control ? math::clamp(in.move.x, -1.0f, 1.0f) : 0.0f;

        if (m.grounded) {
            m.coyote = kCoyoteTicks;
            m.flaps_left = m.air_flaps;
        } else if (m.coyote > 0) {
            --m.coyote;
        }
        if (control && in.jump_pressed) m.jump_buffer = kJumpBufferTicks;
        else if (m.jump_buffer > 0) --m.jump_buffer;

        const f32 target = mx * m.run_speed;
        const f32 accel = (m.grounded ? kGroundAccel : kAirAccel) * dt;
        if (m.vel.x < target) m.vel.x = math::min(m.vel.x + accel, target);
        else if (m.vel.x > target) m.vel.x = math::max(m.vel.x - accel, target);
        if (mx > 0.1f) m.facing = 1;
        if (mx < -0.1f) m.facing = -1;

        if (m.jump_buffer > 0 && m.coyote > 0) {
            m.vel.y = m.jump_speed;
            m.jump_buffer = m.coyote = 0;
            play(f, m.jump_sound, -4.0f);
        } else if (control && in.jump_pressed && !m.grounded && m.coyote == 0 && m.flaps_left > 0) {
            m.vel.y = m.flap_speed;
            --m.flaps_left;
            m.jump_buffer = 0;
            play(f, m.flap_sound, -6.0f);
        }

        const bool held = in.jump_held && control;
        const bool rising_held = m.vel.y > 0.0f && held;
        m.vel.y -= m.gravity * (rising_held || m.vel.y <= 0.0f ? 1.0f : kLowJumpGravityScale) * dt;
        const f32 max_fall = (held && m.vel.y < 0.0f) ? m.glide_fall : m.max_fall;
        if (m.vel.y < -max_fall) m.vel.y = -max_fall;

        QueryFilter filter;
        filter.layer_mask = static_cast<u16>(1u << kWorldLayer);
        const MoveResult2D r = pw.move_and_collide(e, m.vel * dt, filter);
        m.grounded = (r.flags & ControllerFlags::Ground) ? 1u : 0u;
        if (m.grounded && m.vel.y < 0.0f) m.vel.y = 0.0f;
        if (r.hit_ceiling && m.vel.y > 0.0f) m.vel.y = 0.0f;
        if ((r.flags & ControllerFlags::WallLeft) && m.vel.x < 0.0f) m.vel.x = 0.0f;
        if ((r.flags & ControllerFlags::WallRight) && m.vel.x > 0.0f) m.vel.x = 0.0f;

        if (r.position.y < m.kill_y) send_back(f, gs, pw, e, m, false);
    });
}

// ---- FixedUpdate_Post --------------------------------------------------------------------------

bool moth_touches(const World2D& pw, const World& w, Entity moth, vec2 center, vec2 half) {
    BodyState ps;
    if (moth.is_null() || !pw.body_state(moth, ps)) return false;
    const vec2 mh = half_extents(w, moth);
    return std::fabs(ps.position.x - center.x) < half.x + mh.x && std::fabs(ps.position.y - center.y) < half.y + mh.y;
}

void glowmoth_triggers(Frame& f) {
    GlowState& gs = f.resource<GlowState>();
    World& w = f.world();
    World2D& pw = f.resource<World2D>();

    for (const TriggerEnter& ev : f.events().read<TriggerEnter>()) {
        if (ev.other != gs.moth) continue;
        if (Lantern* l = w.try_get_mut<Lantern>(ev.sensor); l && l->lit == 0) {
            l->lit = 1;
            ++gs.lanterns;
            play(f, l->sound, -2.0f);
            MCE_LOG_INFO("glowmoth: lantern lit ({}/{})", gs.lanterns, gs.lanterns_total);
        }
    }

    // Bats: a box overlap with the moth (they have no bodies).
    MothController* m = w.try_get_mut<MothController>(gs.moth);
    if (m && m->invuln == 0 && gs.leaving == 0) {
        bool hit = false;
        f.query<const Bat&, const LocalTransform&>().each([&](const Bat& b, const LocalTransform& lt) {
            if (!hit && moth_touches(pw, w, gs.moth, {lt.position.x, lt.position.y}, b.half)) hit = true;
        });
        if (hit) send_back(f, gs, pw, gs.moth, *m, true);
    }

    // The gate opens once every lantern is lit; touching it then leaves the level.
    f.query<MoonGate&, const LocalTransform&>().each([&](Entity e, MoonGate& g, const LocalTransform& lt) {
        if (g.open == 0 && gs.lanterns >= gs.lanterns_total) {
            g.open = 1;
            gs.gate_open = 1;
            play(f, g.open_sound);
            MCE_LOG_INFO("glowmoth: the moon gate is open");
        }
        if (g.open != 0 && g.entered == 0 && gs.leaving == 0 &&
            moth_touches(pw, w, gs.moth, {lt.position.x, lt.position.y}, half_extents(w, e))) {
            g.entered = 1;
            g.timer = kGateDelayTicks;
            play(f, g.enter_sound);
            MCE_LOG_INFO("glowmoth: level complete");
        }
        if (g.entered != 0 && g.timer > 0 && --g.timer == 0) {
            ++gs.levels_done;
            go_to(f, gs, g.next_scene.view());
        }
    });
}

void glowmoth_present(Frame& f) {
    const GlowState& gs = f.resource<GlowState>();
    f.query<const MothController&, kit::Sprite&>().each([&](const MothController& m, kit::Sprite& s) {
        s.flip_x = m.facing < 0;
        s.tint.a = (m.invuln > 0 && (m.invuln / 5) % 2) ? 0.3f : 1.0f;
    });
    f.query<const Lantern&, kit::Sprite&>().each([&](const Lantern& l, kit::Sprite& s) {
        if (l.lit != 0) set_texture(s, l.lit_texture);
    });
    f.query<const MoonGate&, kit::Sprite&>().each([&](const MoonGate& g, kit::Sprite& s) {
        if (g.open != 0) set_texture(s, g.open_texture);
    });

    char line[64];
    const u32 lives = gs.hits >= kMaxHits ? 0u : kMaxHits - gs.hits;
    f.query<const HudText&, kit::TextLabel&>().each([&](const HudText& h, kit::TextLabel& label) {
        if (h.kind == HudKind::Status) {
            std::snprintf(line, sizeof(line), "LANTERNS %u/%u   LIVES %u", gs.lanterns, gs.lanterns_total, lives);
        } else if (gs.gate_open != 0) {
            std::snprintf(line, sizeof(line), "THE MOON GATE IS OPEN!");
        } else {
            return;  // the banner keeps its authored text until the gate opens
        }
        label.text.assign(StringView(line));
    });
}

void spin(Frame& f) {
    f.query<const Spin&, LocalTransform&>().each([&](const Spin& s, LocalTransform& t) {
        const f32 radians = s.degrees_per_second * (math::kPi / 180.0f) * f.dt;
        t.rotation = math::quat_from_axis_angle({0, 0, 1}, radians) * t.rotation;
    });
}

void bob(Frame& f) {
    f.query<Bob&, LocalTransform&>().each([&](Bob& b, LocalTransform& t) {
        if (!b.started) {
            b.base_y = t.position.y;
            b.started = true;
        }
        b.time += f.dt;
        const f32 period = b.period > 0.0f ? b.period : 1.0f;
        t.position.y = b.base_y + b.height * std::sin(b.time * (2.0f * math::kPi / period));
    });
}

}  // namespace

ResultV install(World& world) {
    if (!world.try_resource<World2D>()) return Error(Errc::InvalidArgument, "glowmoth.install", "the host installs physics2d first");
    if (!world.try_resource<input::InputState>()) input::install(world);
    world.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);
    world.register_components<Spin, Bob, MothController, Lantern, MoonGate, Bat, HudText, MenuScreen>();

    world.add_system(Phase::FixedUpdate_Pre, "glowmoth_setup", &glowmoth_setup);
    world.add_system(Phase::FixedUpdate_Pre, "glowmoth_screens", &glowmoth_screens, "glowmoth_setup");
    world.add_system(Phase::FixedUpdate_Pre, "glowmoth_spin", &spin, "glowmoth_screens");
    world.add_system(Phase::FixedUpdate_Pre, "glowmoth_bob", &bob, "glowmoth_spin");
    world.add_system(Phase::FixedUpdate_Pre, "glowmoth_bats", &glowmoth_bats, "glowmoth_bob");
    world.add_system(Phase::FixedUpdate_Pre, "glowmoth_moth", &glowmoth_moth, "glowmoth_bats");
    world.add_system(Phase::FixedUpdate_Post, "glowmoth_triggers", &glowmoth_triggers);
    world.add_system(Phase::FixedUpdate_Post, "glowmoth_present", &glowmoth_present, "glowmoth_triggers");
    world.set_resource(GlowState{});
    MCE_LOG_INFO("glowmoth: game systems installed");
    return ok();
}

}  // namespace glowmoth
