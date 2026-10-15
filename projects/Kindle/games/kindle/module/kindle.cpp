#include "kindle.h"

#include <cmath>
#include <cstdio>

#include "mce/input/actions.h"
#include "mce/kit/captions.h"

namespace kindle {

using namespace mce;
using namespace mce::physics;

namespace {

// Ember Step's feel (samples/03_platformer/game/platformer.cpp); the speeds and gravity are
// authored on PlayerController, the timings below are not.
constexpr StringId kDashAction = MCE_SID("Dash");
constexpr f32 kGroundAccel = 70.0f;
constexpr f32 kAirAccel = 40.0f;
constexpr f32 kLowJumpGravityScale = 2.5f;
constexpr u32 kCoyoteTicks = 6;
constexpr u32 kJumpBufferTicks = 6;
constexpr u32 kInvulnTicks = 60;
constexpr f32 kWallSlideSpeed = 3.0f;
constexpr f32 kWallJumpX = 8.0f;
constexpr f32 kWallJumpY = 12.5f;
constexpr u32 kWallCoyoteTicks = 6;
constexpr u32 kWallLockTicks = 10;
constexpr u32 kDashTicks = 10;
constexpr u32 kRunFrameTicks = 6;
constexpr u8 kWorldLayer = 0;  // the layer the player's sweeps collide with

const math::Color kRedTint{1.0f, 0.42f, 0.36f, 1.0f};
const math::Color kBlueTint{0.38f, 0.62f, 1.0f, 1.0f};

vec2 position_of(const World2D& pw, Entity e, const LocalTransform& fallback) {
    BodyState s;
    if (pw.body_state(e, s)) return s.position;
    return {fallback.position.x, fallback.position.y};
}

vec2 half_extents(const World& w, Entity e) {
    if (const Collider2D* c = w.try_get<Collider2D>(e); c && c->shape.type == ShapeType::Box) return c->shape.half;
    return {0.5f, 0.5f};
}

// A sound and its caption (M-ACCESS): the caption shows when the player turns captions on
// (access.captions); neither reaches the state hash.
void play(Frame& f, const asset::AssetRef<asset::Sfx>& clip, const char* caption, f32 gain_db = 0.0f) {
    if (clip.guid.is_null()) return;
    audio::PlaySfx ev;
    ev.clip = clip.guid;
    ev.gain_db = gain_db;
    f.events().push(ev);
    kit::ShowCaption cap;
    cap.text = caption;
    cap.seconds = 1.5f;
    cap.kind = kit::CaptionKind::Sound;
    f.events().push(cap);
}

const char* color_name(BlockColor c) { return c == BlockColor::Red ? "RED" : "BLUE"; }

// ---- FixedUpdate_Pre ---------------------------------------------------------------------------

void kindle_setup(Frame& f) {
    KindleState& ks = f.resource<KindleState>();
    if (ks.setup == 0) {
        ks.setup = 1;
        f.query<const PlayerController&>().each([&](Entity e, const PlayerController&) {
            if (ks.player.is_null()) ks.player = e;
        });
        f.query<const Coin&>().each([&](const Coin& c) { ks.coins_total += c.value; });
        bool first = true;
        f.query<const ColorSwitch&>().each([&](const ColorSwitch& s) {
            if (first) ks.active = s.start_color;
            first = false;
        });
    }
    if (ks.won == 0) ++ks.ticks;
}

f32 mover_wave(const Mover& m, f32 t) {
    const f32 period = m.period > 0.0f ? m.period : 1.0f;
    return std::sin(math::kTau * t / period + m.phase);
}

void kindle_movers(Frame& f) {
    const KindleState& ks = f.resource<KindleState>();
    World2D& pw = f.resource<World2D>();
    const f32 t = static_cast<f32>(ks.ticks) * f.dt;
    f.query<Mover&, const LocalTransform&>().each([&](Entity e, Mover& m, const LocalTransform& lt) {
        if (m.started == 0) {
            m.origin = {lt.position.x, lt.position.y};
            m.started = 1;
        }
        // The velocity that lands exactly on the path at the end of this step (no drift).
        const vec2 target = m.origin + m.extent * mover_wave(m, t);
        pw.set_linear_velocity(e, (target - position_of(pw, e, lt)) * (1.0f / f.dt));
    });
}

bool player_overlaps(const World2D& pw, const World& w, Entity player, vec2 center, vec2 half) {
    BodyState ps;
    if (player.is_null() || !pw.body_state(player, ps)) return false;
    const vec2 ph = half_extents(w, player);
    return std::fabs(ps.position.x - center.x) < half.x + ph.x && std::fabs(ps.position.y - center.y) < half.y + ph.y;
}

void kindle_crumble(Frame& f) {
    const KindleState& ks = f.resource<KindleState>();
    World2D& pw = f.resource<World2D>();
    const World& w = f.world();
    f.query<Crumble&, const LocalTransform&>().each([&](Entity e, Crumble& c, const LocalTransform& lt) {
        if (c.state == CrumbleState::Solid) return;
        if (c.timer > 0) {
            --c.timer;
            if (c.timer > 0) return;
        }
        if (c.state == CrumbleState::Shaking) {
            pw.set_enabled(e, false);
            c.state = CrumbleState::Gone;
            c.timer = c.gone_ticks > 0 ? c.gone_ticks : 1;
            return;
        }
        // Grows back only when the player is clear of it.
        if (player_overlaps(pw, w, ks.player, {lt.position.x, lt.position.y}, half_extents(w, e))) {
            c.timer = 1;
            return;
        }
        pw.set_enabled(e, true);
        c.state = CrumbleState::Solid;
    });
}

// The new mechanic: blocks of the active colour are solid. A block turning solid around the player
// waits until they step out (like a crumbling block growing back).
void kindle_blocks(Frame& f) {
    const KindleState& ks = f.resource<KindleState>();
    World2D& pw = f.resource<World2D>();
    const World& w = f.world();
    f.query<SwitchBlock&, const LocalTransform&>().each([&](Entity e, SwitchBlock& b, const LocalTransform& lt) {
        if (!pw.has_body(e)) return;  // bodies appear at the first physics step
        const u32 want = b.color == ks.active ? 1u : 0u;
        if (want == b.solid) return;
        if (want == 1 && player_overlaps(pw, w, ks.player, {lt.position.x, lt.position.y}, half_extents(w, e))) return;
        pw.set_enabled(e, want == 1);
        b.solid = want;
    });
}

void respawn(Frame& f, KindleState& ks, World2D& pw, Entity e, PlayerController& p) {
    pw.teleport(e, p.spawn, 0.0f);
    p.vel = {};
    p.coyote = p.jump_buffer = 0;
    p.wall = 0;
    p.wall_coyote = p.wall_lock = p.dash_ticks = 0;
    p.dash_ready = 1;
    p.invuln = kInvulnTicks;
    ++ks.deaths;
    play(f, p.hurt_sound, "ouch");
}

void kindle_player(Frame& f) {
    KindleState& ks = f.resource<KindleState>();
    World2D& pw = f.resource<World2D>();
    const input::InputState& in = f.resource<input::InputState>();
    const f32 dt = f.dt;
    const bool dash_pressed = in.has_action(kDashAction) ? in.pressed(kDashAction) : in.fire_pressed;

    f.query<PlayerController&, const LocalTransform&>().each([&](Entity e, PlayerController& p, const LocalTransform& lt) {
        if (e != ks.player || !pw.has_body(e)) return;
        if (p.started == 0) {
            p.spawn = {lt.position.x, lt.position.y};
            p.started = 1;
        }
        if (p.invuln > 0) --p.invuln;
        const bool control = ks.won == 0;
        const f32 mx = control ? math::clamp(in.move.x, -1.0f, 1.0f) : 0.0f;

        if (p.grounded) p.coyote = kCoyoteTicks;
        else if (p.coyote > 0) --p.coyote;
        if (p.wall != 0) p.wall_coyote = kWallCoyoteTicks;
        else if (p.wall_coyote > 0) --p.wall_coyote;
        if (p.grounded || p.wall != 0) p.dash_ready = 1;
        if (p.wall_lock > 0) --p.wall_lock;
        if (control && in.jump_pressed) p.jump_buffer = kJumpBufferTicks;
        else if (p.jump_buffer > 0) --p.jump_buffer;

        if (p.wall_lock == 0) {
            const f32 target = mx * p.run_speed;
            const f32 accel = (p.grounded ? kGroundAccel : kAirAccel) * dt;
            if (p.vel.x < target) p.vel.x = math::min(p.vel.x + accel, target);
            else if (p.vel.x > target) p.vel.x = math::max(p.vel.x - accel, target);
            if (mx > 0.1f) p.facing = 1;
            if (mx < -0.1f) p.facing = -1;
        }

        if (p.jump_buffer > 0 && p.coyote > 0) {
            p.vel.y = p.jump_speed;
            p.jump_buffer = p.coyote = p.wall_coyote = 0;
            p.dash_ticks = 0;
            play(f, p.jump_sound, "jump", -4.0f);
        } else if (p.jump_buffer > 0 && p.wall_coyote > 0 && !p.grounded) {
            const i32 away = -p.last_wall;  // kick off the wall last touched
            p.vel = {static_cast<f32>(away) * kWallJumpX, kWallJumpY};
            p.facing = away;
            p.jump_buffer = p.wall_coyote = 0;
            p.wall_lock = kWallLockTicks;
            p.dash_ticks = 0;
            play(f, p.jump_sound, "jump", -4.0f);
        }

        if (control && dash_pressed && p.dash_ready && p.dash_ticks == 0) {
            p.dash_dir = mx > 0.1f ? 1 : mx < -0.1f ? -1 : p.facing;
            p.facing = p.dash_dir;
            p.dash_ticks = kDashTicks;
            p.dash_ready = 0;
            p.wall_lock = 0;
            play(f, p.jump_sound, "dash", -10.0f);
        }

        if (p.dash_ticks > 0) {
            p.vel = {static_cast<f32>(p.dash_dir) * p.dash_speed, 0.0f};
            if (--p.dash_ticks == 0) p.vel.x = static_cast<f32>(p.dash_dir) * p.run_speed;
        } else {
            const bool rising_held = p.vel.y > 0.0f && in.jump_held && control;
            p.vel.y -= p.gravity * (rising_held || p.vel.y <= 0.0f ? 1.0f : kLowJumpGravityScale) * dt;
            if (p.vel.y < -p.max_fall) p.vel.y = -p.max_fall;
            const bool into_wall = p.wall != 0 && mx * static_cast<f32>(p.wall) > 0.1f;
            if (into_wall && !p.grounded && p.vel.y < -kWallSlideSpeed) p.vel.y = -kWallSlideSpeed;
        }

        QueryFilter filter;
        filter.layer_mask = static_cast<u16>(1u << kWorldLayer);
        const MoveResult2D r = pw.move_and_collide(e, p.vel * dt, filter);
        p.grounded = (r.flags & ControllerFlags::Ground) ? 1u : 0u;
        if (p.grounded && p.vel.y < 0.0f) p.vel.y = 0.0f;
        if (r.hit_ceiling && p.vel.y > 0.0f) p.vel.y = 0.0f;
        const bool wall_left = (r.flags & ControllerFlags::WallLeft) != 0;
        const bool wall_right = (r.flags & ControllerFlags::WallRight) != 0;
        if (wall_left && p.vel.x < 0.0f) p.vel.x = 0.0f;
        if (wall_right && p.vel.x > 0.0f) p.vel.x = 0.0f;
        if ((wall_left || wall_right) && p.dash_ticks > 0) p.dash_ticks = 0;  // a dash ends on a wall
        p.wall = p.grounded ? 0 : wall_left ? -1 : wall_right ? 1 : 0;
        if (p.wall != 0) p.last_wall = p.wall;
        ++p.anim_ticks;

        // Standing on a crumbling block starts it shaking.
        if (p.grounded && !r.ground_body.is_null()) {
            if (Crumble* c = f.world().try_get_mut<Crumble>(r.ground_body); c && c->state == CrumbleState::Solid) {
                c->state = CrumbleState::Shaking;
                c->timer = c->shake_ticks > 0 ? c->shake_ticks : 1;
            }
        }

        if (r.position.y < p.kill_y) respawn(f, ks, pw, e, p);
    });
}

// ---- FixedUpdate_Post --------------------------------------------------------------------------

void kindle_triggers(Frame& f) {
    KindleState& ks = f.resource<KindleState>();
    World& w = f.world();
    World2D& pw = f.resource<World2D>();
    for (const TriggerEnter& ev : f.events().read<TriggerEnter>()) {
        if (ev.other != ks.player) continue;
        if (const Coin* coin = w.try_get<Coin>(ev.sensor)) {
            ks.coins += coin->value;
            play(f, coin->sound, "coin", -3.0f);
            f.commands().destroy(ev.sensor);
        } else if (w.has<Hazard>(ev.sensor)) {
            PlayerController* p = w.try_get_mut<PlayerController>(ks.player);
            if (p && p->invuln == 0 && ks.won == 0) respawn(f, ks, pw, ks.player, *p);
        } else if (ColorSwitch* s = w.try_get_mut<ColorSwitch>(ev.sensor)) {
            if (ks.won != 0) continue;
            ks.active = ks.active == BlockColor::Red ? BlockColor::Blue : BlockColor::Red;
            ++ks.toggles;
            ++s->presses;
            play(f, s->sound, "switch clicks");
        } else if (Goal* g = w.try_get_mut<Goal>(ev.sensor); g && ks.won == 0) {
            g->reached = 1;
            ks.won = 1;
            ks.won_at = ks.ticks;
            play(f, g->sound, "flag fanfare");
        }
    }
}

void kindle_camera(Frame& f) {
    KindleState& ks = f.resource<KindleState>();
    const LocalTransform* target = f.world().try_get<LocalTransform>(ks.player);
    if (!target) return;
    const bool snap = ks.camera_snapped == 0;
    ks.camera_snapped = 1;
    f.query<const CameraFollow&, LocalTransform&>().each([&](const CameraFollow& c, LocalTransform& t) {
        vec2 want{target->position.x + c.offset.x, target->position.y + c.offset.y};
        want.x = math::clamp(want.x, c.min.x, c.max.x);
        want.y = math::clamp(want.y, c.min.y, c.max.y);
        const f32 k = snap ? 1.0f : math::clamp(c.smoothing, 0.01f, 1.0f);
        t.position.x += (want.x - t.position.x) * k;
        t.position.y += (want.y - t.position.y) * k;
    });
}

void set_texture(kit::Sprite& s, const asset::AssetRef<asset::Texture>& tex) {
    if (tex.guid.is_null() || s.texture.guid == tex.guid) return;
    s.texture = asset::AssetRef<asset::Texture>{tex.guid, {}};
}

void kindle_present(Frame& f) {
    const KindleState& ks = f.resource<KindleState>();
    const math::Color active_tint = ks.active == BlockColor::Red ? kRedTint : kBlueTint;

    f.query<const PlayerController&, kit::Sprite&>().each([&](const PlayerController& p, kit::Sprite& s) {
        if (p.dash_ticks > 0) set_texture(s, p.dash);
        else if (!p.grounded) set_texture(s, p.jump);
        else if (std::fabs(p.vel.x) > 0.5f) set_texture(s, (p.anim_ticks / kRunFrameTicks) % 2 ? p.run_b : p.run_a);
        else set_texture(s, p.idle);
        s.flip_x = p.facing < 0;
        // Blink while invulnerable after a respawn.
        s.tint.a = (p.invuln > 0 && (p.invuln / 4) % 2) ? 0.35f : 1.0f;
    });
    f.query<const ColorSwitch&, kit::Sprite&>().each([&](const ColorSwitch&, kit::Sprite& s) { s.tint = active_tint; });
    f.query<const SwitchBlock&, kit::Sprite&>().each([&](const SwitchBlock& b, kit::Sprite& s) {
        const bool on = b.color == ks.active && b.solid;
        s.tint.a = on ? 1.0f : b.ghost_alpha;
    });

    char line[64];
    f.query<const HudText&, kit::TextLabel&>().each([&](const HudText& h, kit::TextLabel& label) {
        if (h.kind == HudKind::Status) {
            std::snprintf(line, sizeof(line), "COINS %u/%u   SWITCH %s", ks.coins, ks.coins_total, color_name(ks.active));
        } else if (ks.won != 0) {
            const u64 tenths = ks.won_at * 10u / 60u;  // 60 Hz ticks -> tenths of a second
            std::snprintf(line, sizeof(line), "LIT! %llu.%llu s   COINS %u/%u", static_cast<unsigned long long>(tenths / 10u),
                          static_cast<unsigned long long>(tenths % 10u), ks.coins, ks.coins_total);
        } else {
            return;  // the banner keeps its authored text until the flag
        }
        label.text.assign(StringView(line));
    });
}

}  // namespace

ResultV install(World& world) {
    if (!world.try_resource<World2D>()) return Error(Errc::InvalidArgument, "kindle.install", "the host installs physics2d first");
    if (!world.try_resource<input::InputState>()) input::install(world);
    // The audio bridge registers it too (idempotent); a host without audio drops the events.
    world.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);
    world.register_event<kit::ShowCaption>(Phase::FixedUpdate_Pre);
    world.register_components<SwitchBlock, ColorSwitch, PlayerController, Coin, Hazard, Goal, Crumble, Mover, CameraFollow,
                              HudText>();

    world.add_system(Phase::FixedUpdate_Pre, "kindle_setup", &kindle_setup);
    world.add_system(Phase::FixedUpdate_Pre, "kindle_movers", &kindle_movers, "kindle_setup");
    world.add_system(Phase::FixedUpdate_Pre, "kindle_crumble", &kindle_crumble, "kindle_movers");
    world.add_system(Phase::FixedUpdate_Pre, "kindle_blocks", &kindle_blocks, "kindle_crumble");
    world.add_system(Phase::FixedUpdate_Pre, "kindle_player", &kindle_player, "kindle_blocks");
    world.add_system(Phase::FixedUpdate_Post, "kindle_triggers", &kindle_triggers);
    world.add_system(Phase::FixedUpdate_Post, "kindle_camera", &kindle_camera, "kindle_triggers");
    world.add_system(Phase::FixedUpdate_Post, "kindle_present", &kindle_present, "kindle_camera");
    world.set_resource(KindleState{});
    return ok();
}

}  // namespace kindle
