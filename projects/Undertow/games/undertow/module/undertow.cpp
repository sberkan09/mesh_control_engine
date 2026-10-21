#include "undertow.h"

#include <cmath>
#include <cstdio>

#include "mce/input/actions.h"
#include "mce/kit/particles.h"
#include "mce/kit/sequence.h"

namespace undertow {

using namespace mce;
using namespace mce::physics;

namespace {

constexpr u32 kInvulnTicks = 50;
constexpr u32 kBannerTicks = 150;
constexpr u32 kHintTicks = 600;
constexpr f32 kDroneDensity = 1.0f;  // the authored density of the drone's circle (force = thrust * mass)

const char* kDeliverSeq = "deliver_fanfare";
const char* kWinSeq = "win_cue";

vec2 body_pos(const World2D& pw, Entity e, const LocalTransform* fallback = nullptr) {
    BodyState s;
    if (pw.body_state(e, s)) return s.position;
    return fallback ? vec2{fallback->position.x, fallback->position.y} : vec2{};
}

f32 body_mass(const World& w, Entity e) {
    const Collider2D* c = w.try_get<Collider2D>(e);
    if (!c) return 1.0f;
    const f32 d = c->density > 0.0f ? c->density : 1.0f;
    if (c->shape.type == ShapeType::Circle) return d * math::kPi * c->shape.radius * c->shape.radius;
    return d * 4.0f * c->shape.half.x * c->shape.half.y;
}

void play(Frame& f, const asset::AssetRef<asset::Sfx>& clip, f32 gain_db = 0.0f) {
    if (clip.guid.is_null()) return;
    audio::PlaySfx ev;
    ev.clip = clip.guid;
    ev.gain_db = gain_db;
    f.events().push(ev);
}

void banner(UndertowState& gs, const char* text, u32 ticks = kBannerTicks) {
    gs.banner.assign(StringView(text));
    gs.banner_ticks = ticks;
}

void burst_at(Frame& f, Entity emitter, vec2 at, u32 count) {
    if (emitter.is_null()) return;
    if (LocalTransform* t = f.world().try_get_mut<LocalTransform>(emitter)) t->position = {at.x, at.y, 0.0f};
    if (kit::ParticleSystem* ps = f.try_resource<kit::ParticleSystem>()) ps->burst(emitter, count);
}

void shake(Frame& f, f32 strength) {
    f.query<CameraRig&>().each([&](CameraRig& c) { c.shake_strength = math::max(c.shake_strength, strength); });
}

void release_tether(Frame& f, UndertowState& gs, World2D& pw, bool sound) {
    if (gs.tethered.is_null()) return;
    if (!gs.tether.is_null()) pw.destroy_joint(gs.tether);
    gs.tether = {};
    if (Cargo* c = f.world().try_get_mut<Cargo>(gs.tethered); c && c->state == CargoState::Tethered) c->state = CargoState::Loose;
    gs.tethered = {};
    if (sound) {
        if (const Drone* d = f.world().try_get<Drone>(gs.drone)) play(f, d->tether_off_sound, -6.0f);
    }
}

// The sequence hooks (kit::SequenceRegistry call table): stable names, part of the module's contract.
void seq_dock_flash(Frame& f, Entity) {
    UndertowState& gs = f.resource<UndertowState>();
    gs.flash = math::max(gs.flash, 0.6f);
    shake(f, 0.12f);
}

void seq_win_zoom(Frame& f, Entity) {
    UndertowState& gs = f.resource<UndertowState>();
    gs.flash = 1.0f;
    shake(f, 0.3f);
    if (!gs.boom_emitter.is_null()) {
        const vec2 at = body_pos(f.resource<World2D>(), gs.drone);
        burst_at(f, gs.boom_emitter, at, 90);
    }
}

void seq_win_banner(Frame& f, Entity) {
    UndertowState& gs = f.resource<UndertowState>();
    const u32 secs = gs.won_at / 60u;
    char line[64];
    std::snprintf(line, sizeof(line), "SIGNAL RESTORED  %u:%02u  HULL LOST %u", secs / 60u, secs % 60u, gs.deaths);
    banner(gs, line, 0xFFFFFFFu);
}

// ---- FixedUpdate_Pre ---------------------------------------------------------------------------

void undertow_setup(Frame& f) {
    UndertowState& gs = f.resource<UndertowState>();
    if (gs.setup != 0) return;
    gs.setup = 1;
    f.query<const Drone&>().each([&](Entity e, const Drone&) {
        if (gs.drone.is_null()) gs.drone = e;
    });
    f.query<const DroneVisual&>().each([&](Entity e, const DroneVisual&) {
        if (gs.visual.is_null()) gs.visual = e;
    });
    f.query<const Cargo&>().each([&](const Cargo&) { ++gs.cores_total; });
    // The one-shot emitters the game moves around: named by their preset use, found by component
    // on entities without a Thruster. The first is the explosion, the second the sparks.
    f.query<const kit::ParticleEmitter&>().each([&](Entity e, const kit::ParticleEmitter& em) {
        if (em.playing || f.world().has<Thruster>(e)) return;
        if (gs.boom_emitter.is_null()) gs.boom_emitter = e;
        else if (gs.spark_emitter.is_null()) gs.spark_emitter = e;
    });
    f.query<Flicker&, const kit::Light2D&>().each([&](Flicker& fl, const kit::Light2D& l) { fl.base = l.intensity; });

    // Code-defined sequences (ADR-0032): the dock fanfare and the win cue. Their sounds come from
    // the authored Pad, so they are defined here, once the scene is loaded.
    if (kit::SequenceRegistry* seqs = f.try_resource<kit::SequenceRegistry>()) {
        seqs->register_call("dock_flash", &seq_dock_flash);
        seqs->register_call("win_zoom", &seq_win_zoom);
        seqs->register_call("win_banner", &seq_win_banner);
        asset::Guid deliver, win;
        f.query<const Pad&>().each([&](const Pad& p) {
            if (p.kind == PadKind::Dock && deliver.is_null()) deliver = p.deliver_sound.guid;
        });
        f.query<const Drone&>().each([&](const Drone& d) { win = d.explode_sound.guid; });
        using R = kit::SequenceRegistry;
        const asset::SequenceStep fanfare[] = {R::call_step("dock_flash"), R::wait(0.18f), R::call_step("dock_flash"),
                                               R::wait(0.18f), R::call_step("dock_flash")};
        (void)seqs->define(kDeliverSeq, Span<const asset::SequenceStep>(fanfare, 5));
        const asset::SequenceStep cue[] = {R::wait(0.4f), R::call_step("win_zoom"), R::wait(0.6f), R::call_step("win_zoom"),
                                           R::wait(0.6f), R::call_step("win_zoom"), R::wait(0.8f), R::call_step("win_banner")};
        (void)seqs->define(kWinSeq, Span<const asset::SequenceStep>(cue, 8));
    }
    banner(gs, "UNDERTOW", 0xFFFFFFFu);
}

f32 mover_wave(const Mover& m, f32 t) {
    const f32 period = m.period > 0.0f ? m.period : 1.0f;
    return std::sin(math::kTau * t / period + m.phase);
}

void undertow_movers(Frame& f) {
    World2D& pw = f.resource<World2D>();
    const f32 t = static_cast<f32>(f.tick) * f.dt;
    f.query<Mover&, const LocalTransform&>().each([&](Entity e, Mover& m, const LocalTransform& lt) {
        if (m.started == 0) {
            m.origin = {lt.position.x, lt.position.y};
            m.started = 1;
        }
        if (!pw.has_body(e)) return;
        const vec2 target = m.origin + m.extent * mover_wave(m, t);
        pw.set_linear_velocity(e, (target - body_pos(pw, e, &lt)) * (1.0f / f.dt));
    });
}

void undertow_gates(Frame& f) {
    UndertowState& gs = f.resource<UndertowState>();
    World2D& pw = f.resource<World2D>();
    for (u32& t : gs.gate_ticks)
        if (t > 0) --t;
    f.query<Gate&>().each([&](Entity e, Gate& g) {
        if (!pw.has_body(e)) return;
        const u32 want_closed = gs.gate_ticks[g.channel & 7] == 0 ? 1u : 0u;
        if (want_closed == g.closed) return;
        pw.set_enabled(e, want_closed == 1);
        g.closed = want_closed;
    });
}

void undertow_currents(Frame& f) {
    UndertowState& gs = f.resource<UndertowState>();
    World2D& pw = f.resource<World2D>();
    const World& w = f.world();
    f.query<const Current&, const LocalTransform&>().each([&](const Current& c, const LocalTransform& lt) {
        const vec2 center{lt.position.x, lt.position.y};
        auto inside = [&](vec2 p) { return std::fabs(p.x - center.x) < c.half.x && std::fabs(p.y - center.y) < c.half.y; };
        if (!gs.drone.is_null() && pw.has_body(gs.drone) && inside(body_pos(pw, gs.drone)))
            pw.apply_force(gs.drone, c.force * body_mass(w, gs.drone));
        f.query<const Cargo&>().each([&](Entity e, const Cargo& cargo) {
            if (cargo.state == CargoState::Delivered || !pw.has_body(e)) return;
            if (inside(body_pos(pw, e))) pw.apply_force(e, c.force * (c.pod_scale * body_mass(w, e)));
        });
    });
}

void respawn(World2D& pw, Entity e, Drone& d) {
    pw.set_enabled(e, true);
    pw.teleport(e, d.spawn, 0.0f);
    pw.set_linear_velocity(e, {});
    d.prev_vel = {};
    d.hull = d.hull_max;
    d.energy = d.energy_max;
    d.invuln = kInvulnTicks * 2;
    d.dead_ticks = 0;
}

void destroy_drone(Frame& f, UndertowState& gs, World2D& pw, Entity e, Drone& d) {
    release_tether(f, gs, pw, false);
    burst_at(f, gs.boom_emitter, body_pos(pw, e), 70);
    play(f, d.explode_sound);
    shake(f, 0.9f);
    gs.aberration_pulse = 1.0f;
    gs.flash = 0.8f;
    pw.set_enabled(e, false);
    d.hull = 0;
    d.dead_ticks = d.respawn_ticks > 0 ? d.respawn_ticks : 1;
    d.thrust_bits = 0;
    d.boosting = 0;
    ++gs.deaths;
    banner(gs, "HULL BREACH");
}

void damage(Frame& f, UndertowState& gs, World2D& pw, Entity e, Drone& d, u32 amount) {
    if (amount == 0 || d.dead_ticks > 0 || d.invuln > 0 || gs.phase == RunPhase::Won) return;
    if (amount >= d.hull) {
        destroy_drone(f, gs, pw, e, d);
        return;
    }
    d.hull -= amount;
    d.invuln = kInvulnTicks;
    play(f, d.hit_sound);
    burst_at(f, gs.spark_emitter, body_pos(pw, e), 22);
    shake(f, 0.45f);
    gs.aberration_pulse = 0.7f;
    banner(gs, d.hull == 1 ? "HULL CRITICAL" : "HULL DAMAGED", 90);
}

// What the drone is told to do this tick: the player's input, or the autopilot's.
struct Command {
    vec2 move;
    bool boost = false;
    bool tether = false;
    bool any = false;
};

Command autopilot_command(Autopilot& ap, const World2D& pw, Entity drone, bool hauling) {
    Command c;
    if (ap.count == 0 || ap.done != 0) return c;
    const vec2* wps = &ap.w0;
    const vec2 pos = body_pos(pw, drone);
    const vec2 vel = pw.linear_velocity(drone);
    vec2 target = wps[ap.index % 8];
    if (math::length(target - pos) < ap.reach) {
        if (ap.tether_mask & (1u << (ap.index % 8))) c.tether = true;
        ++ap.index;
        if (ap.index >= ap.count) {
            if (ap.loop != 0) ap.index = 0;
            else ap.done = 1;
        }
        target = wps[ap.index % 8];
    }
    const vec2 to = target - pos;
    const f32 dist = math::length(to);
    const vec2 want = dist > 0.001f ? to * (math::min(ap.cruise, dist * 1.6f) / dist) : vec2{};
    const vec2 err = want - vel;
    c.move.x = err.x > 0.35f ? 1.0f : err.x < -0.35f ? -1.0f : 0.0f;
    c.move.y = err.y > 0.35f ? 1.0f : err.y < -0.35f ? -1.0f : 0.0f;
    c.boost = hauling || dist > 6.0f;
    c.any = true;
    return c;
}

void undertow_drone(Frame& f) {
    UndertowState& gs = f.resource<UndertowState>();
    World2D& pw = f.resource<World2D>();
    World& w = f.world();
    const input::InputState& in = f.resource<input::InputState>();
    const f32 dt = f.dt;

    f.query<Drone&, const LocalTransform&>().each([&](Entity e, Drone& d, const LocalTransform& lt) {
        if (e != gs.drone || !pw.has_body(e)) return;
        if (d.started == 0) {
            d.spawn = {lt.position.x, lt.position.y};
            d.hull = d.hull_max;
            d.energy = d.energy_max;
            d.started = 1;
        }
        if (d.invuln > 0) --d.invuln;

        Command cmd;
        if (Autopilot* ap = w.try_get_mut<Autopilot>(e)) cmd = autopilot_command(*ap, pw, e, !gs.tethered.is_null());
        if (!cmd.any) {
            cmd.move = {math::clamp(in.move.x, -1.0f, 1.0f), math::clamp(in.move.y, -1.0f, 1.0f)};
            cmd.boost = in.jump_held;
            cmd.tether = in.fire_pressed;
            cmd.any = std::fabs(cmd.move.x) > 0.1f || std::fabs(cmd.move.y) > 0.1f || in.jump_pressed || in.fire_pressed;
        }
        if (gs.phase == RunPhase::Intro && cmd.any) {
            gs.phase = RunPhase::Playing;
            banner(gs, "RECOVER THE CORES", 200);
        }
        if (gs.phase != RunPhase::Playing) cmd = {};

        if (d.dead_ticks > 0) {
            if (--d.dead_ticks == 0) respawn(pw, e, d);
            d.prev_vel = pw.linear_velocity(e);
            return;
        }

        // Tether: throw it to the nearest loose pod in range, or drop it.
        if (cmd.tether) {
            if (!gs.tethered.is_null()) {
                release_tether(f, gs, pw, true);
            } else {
                const vec2 pos = body_pos(pw, e);
                Entity best;
                f32 best_d = d.tether_range;
                f.query<Cargo&>().each([&](Entity pe, Cargo& c) {
                    if (c.state != CargoState::Loose || !pw.has_body(pe)) return;
                    const f32 dist = math::length(body_pos(pw, pe) - pos);
                    if (dist < best_d) {
                        best_d = dist;
                        best = pe;
                    }
                });
                if (!best.is_null()) {
                    DistanceJointDesc jd;
                    jd.a = e;
                    jd.b = best;
                    jd.length = d.tether_length;
                    jd.min_length = 0.3f;
                    jd.max_length = d.tether_length;
                    gs.tether = pw.create_distance_joint(jd);
                    if (!gs.tether.is_null()) {
                        gs.tethered = best;
                        w.try_get_mut<Cargo>(best)->state = CargoState::Tethered;
                        play(f, d.tether_on_sound, -4.0f);
                        burst_at(f, gs.spark_emitter, body_pos(pw, best), 10);
                    }
                } else {
                    play(f, d.tether_off_sound, -14.0f);
                }
            }
        }

        // Thrust: a force per axis; boost multiplies it while energy lasts.
        const bool boosting = cmd.boost && d.energy > 0.0f && (std::fabs(cmd.move.x) > 0.1f || std::fabs(cmd.move.y) > 0.1f);
        d.boosting = boosting ? 1u : 0u;
        if (boosting) d.energy = math::max(0.0f, d.energy - d.boost_drain * dt);
        else d.energy = math::min(d.energy_max, d.energy + d.energy_regen * dt);
        const f32 mass = body_mass(w, e) / kDroneDensity * kDroneDensity;
        const f32 force = d.thrust * mass * (boosting ? d.boost_mult : 1.0f);
        vec2 push{cmd.move.x, cmd.move.y};
        if (math::length(push) > 1.0f) push = math::normalize(push);
        pw.apply_force(e, push * force);
        d.thrust_bits = (cmd.move.x > 0.1f ? 1u : 0u) | (cmd.move.x < -0.1f ? 2u : 0u) | (cmd.move.y > 0.1f ? 4u : 0u) |
                        (cmd.move.y < -0.1f ? 8u : 0u);
        // Speed cap, so boosting is quick but not a wall-breaker.
        const vec2 v = pw.linear_velocity(e);
        const f32 cap = boosting ? d.boost_max_speed : d.max_speed;
        if (math::length(v) > cap) pw.set_linear_velocity(e, math::normalize(v) * cap);
        d.prev_vel = pw.linear_velocity(e);
    });

    // Delivered pods stay on their slot; loose ones keep the world's weak gravity.
    if (gs.phase == RunPhase::Playing) ++gs.ticks;
}

// ---- FixedUpdate_Post --------------------------------------------------------------------------

void undertow_impacts(Frame& f) {
    UndertowState& gs = f.resource<UndertowState>();
    World2D& pw = f.resource<World2D>();
    World& w = f.world();
    Drone* d = gs.drone.is_null() ? nullptr : w.try_get_mut<Drone>(gs.drone);
    if (!d || !pw.has_body(gs.drone) || d->dead_ticks > 0) return;

    // Lasers and crushers: contact with a closed Gate or a Hazard body.
    for (const ContactBegin2D& ev : f.events().read<ContactBegin2D>()) {
        const Entity other = ev.a == gs.drone ? ev.b : ev.b == gs.drone ? ev.a : Entity::null;
        if (other.is_null()) continue;
        u32 dmg = 0;
        if (const Gate* g = w.try_get<Gate>(other); g && g->closed) dmg = 1;
        if (const Hazard* h = w.try_get<Hazard>(other)) dmg = math::max(dmg, h->damage);
        if (dmg > 0) {
            const vec2 away = ev.a == gs.drone ? -ev.normal : ev.normal;
            pw.apply_impulse(gs.drone, away * (6.0f * body_mass(w, gs.drone)), body_pos(pw, gs.drone));
            damage(f, gs, pw, gs.drone, *d, dmg);
            if (d->dead_ticks > 0) return;
        }
    }
    // Hard landings: the velocity jump across the physics step.
    const vec2 v = pw.linear_velocity(gs.drone);
    const f32 jump = math::length(v - d->prev_vel);
    if (jump > d->impact_speed) {
        damage(f, gs, pw, gs.drone, *d, 1);
    } else if (jump > d->bump_speed) {
        play(f, d->bump_sound, -10.0f + math::min(10.0f, jump));
        burst_at(f, gs.spark_emitter, body_pos(pw, gs.drone), 6);
        shake(f, 0.12f);
    }
    d->prev_vel = v;
}

void deliver(Frame& f, UndertowState& gs, World2D& pw, Entity pad_entity, Pad& pad, Entity pod, Cargo& cargo) {
    if (pod == gs.tethered) release_tether(f, gs, pw, false);
    cargo.state = CargoState::Delivered;
    // Park it on the dock: slots 1.1 m apart along the pad, above its surface.
    const LocalTransform* pt = f.world().try_get<LocalTransform>(pad_entity);
    const vec2 base = pt ? vec2{pt->position.x, pt->position.y} : body_pos(pw, pod);
    const f32 slot = (static_cast<f32>(gs.cores) - static_cast<f32>(gs.cores_total > 0 ? gs.cores_total - 1 : 0) * 0.5f) * 1.1f;
    pw.teleport(pod, {base.x + slot, base.y + 0.9f}, 0.0f);
    pw.set_linear_velocity(pod, {});
    pw.set_enabled(pod, false);
    ++gs.cores;
    play(f, pad.deliver_sound);
    burst_at(f, gs.boom_emitter, {base.x + slot, base.y + 0.9f}, 40);
    shake(f, 0.25f);
    gs.flash = 0.5f;
    char line[64];
    std::snprintf(line, sizeof(line), "CORE %u/%u RECOVERED", gs.cores, gs.cores_total);
    banner(gs, line, 180);
    kit::SequencePlayer sp;
    sp.name.assign(StringView(kDeliverSeq));
    f.commands().set(pad_entity, sp);
    if (gs.cores >= gs.cores_total && gs.phase == RunPhase::Playing) {
        gs.phase = RunPhase::Won;
        gs.won_at = gs.ticks;
        banner(gs, "SIGNAL RESTORED", 0xFFFFFFFu);
        if (Drone* d = f.world().try_get_mut<Drone>(gs.drone)) {
            d->thrust_bits = 0;
            d->boosting = 0;
        }
        kit::SequencePlayer win;
        win.name.assign(StringView(kWinSeq));
        f.commands().set(gs.drone, win);
    }
}

void undertow_triggers(Frame& f) {
    UndertowState& gs = f.resource<UndertowState>();
    World& w = f.world();
    World2D& pw = f.resource<World2D>();
    Drone* d = gs.drone.is_null() ? nullptr : w.try_get_mut<Drone>(gs.drone);
    for (const TriggerEnter& ev : f.events().read<TriggerEnter>()) {
        if (Pad* pad = w.try_get_mut<Pad>(ev.sensor)) {
            if (Cargo* cargo = w.try_get_mut<Cargo>(ev.other); cargo && pad->kind == PadKind::Dock && cargo->state != CargoState::Delivered) {
                deliver(f, gs, pw, ev.sensor, *pad, ev.other, *cargo);
            } else if (ev.other == gs.drone && d && d->dead_ticks == 0) {
                const LocalTransform* pt = w.try_get<LocalTransform>(ev.sensor);
                if (pt) d->spawn = {pt->position.x, pt->position.y + 1.2f};
                const bool heal = d->hull < d->hull_max || d->energy < d->energy_max * 0.5f;
                d->hull = d->hull_max;
                d->energy = d->energy_max;
                if (pad->visited == 0 || heal) {
                    play(f, pad->sound, -4.0f);
                    banner(gs, pad->kind == PadKind::Dock ? "DOCK  HULL RESTORED" : "RELAY  CHECKPOINT", 120);
                }
                pad->visited = 1;
            }
        } else if (Switch* s = w.try_get_mut<Switch>(ev.sensor)) {
            if (ev.other != gs.drone || gs.phase != RunPhase::Playing) continue;
            gs.gate_ticks[s->channel & 7] = static_cast<u32>(s->open_seconds * 60.0f);
            ++s->presses;
            play(f, s->sound);
            burst_at(f, gs.spark_emitter, body_pos(pw, ev.sensor), 12);
            banner(gs, "GATE OPEN", 90);
        } else if (Mine* m = w.try_get_mut<Mine>(ev.sensor)) {
            if (ev.other != gs.drone || !d || d->dead_ticks > 0) continue;
            const vec2 at = body_pos(pw, ev.sensor);
            const vec2 dp = body_pos(pw, gs.drone);
            const vec2 dir = math::length(dp - at) > 0.01f ? math::normalize(dp - at) : vec2{0.0f, 1.0f};
            pw.apply_impulse(gs.drone, dir * (m->knockback * body_mass(w, gs.drone)), dp);
            play(f, m->sound);
            burst_at(f, gs.boom_emitter, at, 50);
            shake(f, 0.7f);
            d->invuln = 0;
            damage(f, gs, pw, gs.drone, *d, m->damage);
            f.commands().destroy(ev.sensor);
        }
    }
}

void undertow_camera(Frame& f) {
    UndertowState& gs = f.resource<UndertowState>();
    World2D& pw = f.resource<World2D>();
    if (gs.drone.is_null() || !pw.has_body(gs.drone)) return;
    const vec2 pos = body_pos(pw, gs.drone);
    const vec2 vel = pw.linear_velocity(gs.drone);
    const bool snap = gs.camera_snapped == 0;
    gs.camera_snapped = 1;
    gs.aberration_pulse *= 0.9f;
    gs.flash *= 0.85f;
    const f32 t = static_cast<f32>(f.tick);
    f.query<CameraRig&, LocalTransform&>().each([&](Entity e, CameraRig& c, LocalTransform& tr) {
        vec2 want = pos + vel * c.lookahead;
        want.x = math::clamp(want.x, c.min.x, c.max.x);
        want.y = math::clamp(want.y, c.min.y, c.max.y);
        const f32 k = snap ? 1.0f : math::clamp(c.smoothing, 0.01f, 1.0f);
        vec2 centre{tr.position.x - c.shake.x, tr.position.y - c.shake.y};
        centre += (want - centre) * k;
        c.shake_strength *= 0.86f;
        if (c.shake_strength < 0.005f) c.shake_strength = 0.0f;
        c.shake = {std::sin(t * 37.1f) * c.shake_strength, std::cos(t * 53.7f) * c.shake_strength};
        tr.position = {centre.x + c.shake.x, centre.y + c.shake.y, tr.position.z};
        if (kit::Camera2DComponent* cam = f.world().try_get_mut<kit::Camera2DComponent>(e)) {
            const f32 zoom_want = gs.phase == RunPhase::Won ? c.win_zoom : 1.0f;
            cam->zoom += (zoom_want - cam->zoom) * 0.03f;
        }
        if (kit::Post2DComponent* post = f.world().try_get_mut<kit::Post2DComponent>(e)) {
            post->chromatic_aberration = gs.aberration_pulse > 0.03f;
            post->aberration = 0.002f + gs.aberration_pulse * 0.018f;
            const Drone* d = f.world().try_get<Drone>(gs.drone);
            const bool critical = d && d->hull == 1 && d->dead_ticks == 0;
            const f32 vignette = critical ? 0.7f + 0.15f * std::sin(t * 0.2f) : 0.45f;
            post->vignette_intensity += (vignette - post->vignette_intensity) * 0.1f;
        }
    });
}

void undertow_present(Frame& f) {
    UndertowState& gs = f.resource<UndertowState>();
    World2D& pw = f.resource<World2D>();
    World& w = f.world();
    const Drone* d = gs.drone.is_null() ? nullptr : w.try_get<Drone>(gs.drone);
    const vec2 dpos = d ? body_pos(pw, gs.drone, w.try_get<LocalTransform>(gs.drone)) : vec2{};
    const bool alive = d && d->dead_ticks == 0;
    const f32 t = static_cast<f32>(f.tick) * f.dt;

    // The drone's look follows the body; the sprite tilts into sideways thrust and blinks while invulnerable.
    f.query<DroneVisual&, LocalTransform&>().each([&](Entity e, DroneVisual& v, LocalTransform& tr) {
        if (!d) return;
        const f32 side = (d->thrust_bits & 1u ? 1.0f : 0.0f) - (d->thrust_bits & 2u ? 1.0f : 0.0f);
        v.tilt += (-side * v.tilt_max - v.tilt) * v.tilt_rate;
        tr.position = {dpos.x, dpos.y, tr.position.z};
        tr.rotation = rotation_z(v.tilt);
        if (kit::Sprite* s = w.try_get_mut<kit::Sprite>(e)) {
            s->tint.a = !alive ? 0.0f : (d->invuln > 0 && (d->invuln / 4) % 2) ? 0.35f : 1.0f;
        }
        if (kit::Light2D* l = w.try_get_mut<kit::Light2D>(e)) {
            const f32 want = !alive ? 0.0f : d->boosting ? 2.8f : 2.0f;
            l->intensity += (want - l->intensity) * 0.2f;
        }
    });
    // Thruster plumes sit on the body and emit while that direction is pushed.
    f.query<const Thruster&, LocalTransform&, kit::ParticleEmitter&>().each(
        [&](const Thruster& th, LocalTransform& tr, kit::ParticleEmitter& em) {
            tr.position = {dpos.x + th.offset.x, dpos.y + th.offset.y, tr.position.z};
            const u32 bit = 1u << static_cast<u32>(th.dir);
            em.playing = alive && (d->thrust_bits & bit) != 0;
        });
    // The engine hum: loud while thrusting, louder boosting, quiet otherwise.
    if (audio::AudioSource* hum = alive ? w.try_get_mut<audio::AudioSource>(gs.drone) : nullptr) {
        const f32 want = d->thrust_bits == 0 ? -34.0f : d->boosting ? -4.0f : -10.0f;
        hum->gain_db += (want - hum->gain_db) * 0.15f;
    } else if (audio::AudioSource* dead = (!alive && d) ? w.try_get_mut<audio::AudioSource>(gs.drone) : nullptr) {
        dead->gain_db = -60.0f;
    }
    // Pods: tethered ones glow, delivered ones go steady and bright.
    f.query<const Cargo&, kit::Sprite&>().each([&](Entity e, const Cargo& c, kit::Sprite& s) {
        const f32 pulse = 0.5f + 0.5f * std::sin(t * 4.0f + static_cast<f32>(c.index));
        if (c.state == CargoState::Delivered) s.tint = {0.75f, 1.0f, 0.9f, 1.0f};
        else if (c.state == CargoState::Tethered) s.tint = {1.0f, 0.95f, 0.7f, 1.0f};
        else s.tint = {0.55f + 0.3f * pulse, 0.85f, 1.0f, 1.0f};
        if (kit::Light2D* l = w.try_get_mut<kit::Light2D>(e))
            l->intensity = c.state == CargoState::Delivered ? 1.8f : 1.0f + 0.6f * pulse;
    });
    // Beacons light up as cores come home.
    f.query<Beacon&, kit::Light2D&, kit::Sprite&>().each([&](Beacon& b, kit::Light2D& l, kit::Sprite& s) {
        const bool on = gs.cores > b.index;
        b.lit = on ? 1u : 0u;
        const f32 want = on ? b.on_intensity : 0.0f;
        l.intensity += (want - l.intensity) * 0.08f;
        s.tint = on ? math::Color{0.6f, 1.0f, 0.85f, 1.0f} : math::Color{0.25f, 0.3f, 0.35f, 1.0f};
    });
    // Gates: solid red beams while closed; faint and blinking in the last two seconds of being open.
    f.query<const Gate&, kit::Sprite&>().each([&](Entity e, const Gate& g, kit::Sprite& s) {
        const u32 left = gs.gate_ticks[g.channel & 7];
        const bool blink = left > 0 && left < 120 && (left / 8) % 2 == 0;
        s.tint.a = g.closed ? 1.0f : blink ? 0.6f : g.open_alpha;
        if (kit::Light2D* l = w.try_get_mut<kit::Light2D>(e)) l->intensity = g.closed ? 1.2f : 0.15f;
    });
    f.query<const Switch&, kit::Sprite&>().each([&](const Switch& sw, kit::Sprite& s) {
        const bool open = gs.gate_ticks[sw.channel & 7] > 0;
        s.tint = open ? math::Color{0.6f, 1.0f, 0.7f, 1.0f} : math::Color{1.0f, 0.75f, 0.45f, 1.0f};
    });
    f.query<const Flicker&, kit::Light2D&>().each([&](const Flicker& fl, kit::Light2D& l) {
        const f32 phase = t * fl.speed * math::kTau + static_cast<f32>(fl.seed) * 1.7f;
        l.intensity = fl.base * (1.0f + fl.amount * (0.6f * std::sin(phase) + 0.4f * std::sin(phase * 2.7f)));
    });
    // The rope and the screen flash.
    f.query<const TetherVisual&, LocalTransform&, kit::Sprite&>().each([&](const TetherVisual& tv, LocalTransform& tr, kit::Sprite& s) {
        if (gs.tethered.is_null() || !alive) {
            s.tint.a = 0.0f;
            return;
        }
        const vec2 to = body_pos(pw, gs.tethered) - dpos;
        const f32 len = math::length(to);
        tr.position = {dpos.x, dpos.y, tr.position.z};
        tr.rotation = rotation_z(std::atan2(to.y, to.x));
        s.size = {math::max(len, 0.05f), tv.thickness};
        s.pivot = {0.0f, 0.5f};
        const f32 strain = d ? math::clamp((len - d->tether_length * 0.9f) / (d->tether_length * 0.15f), 0.0f, 1.0f) : 0.0f;
        s.tint = {1.0f, 1.0f - 0.5f * strain, 0.8f - 0.6f * strain, 0.9f};
    });
    f.query<const ScreenFlash&, kit::Sprite&>().each([&](const ScreenFlash& sf, kit::Sprite& s) { s.tint.a = gs.flash * sf.max_alpha; });
    if (gs.banner_ticks > 0 && gs.banner_ticks < 0xFFFFFFFu) --gs.banner_ticks;

    char line[80];
    f.query<const HudText&, kit::TextLabel&>().each([&](const HudText& h, kit::TextLabel& label) {
        switch (h.kind) {
            case HudKind::Status: {
                if (!d) return;
                char hull[8] = {};
                for (u32 i = 0; i < d->hull_max && i < 7; ++i) hull[i] = i < d->hull ? '#' : '-';
                char energy[12] = {};
                const u32 bars = static_cast<u32>(math::clamp(d->energy / d->energy_max, 0.0f, 1.0f) * 10.0f + 0.5f);
                for (u32 i = 0; i < 10; ++i) energy[i] = i < bars ? '|' : '.';
                const u32 secs = (gs.phase == RunPhase::Won ? gs.won_at : gs.ticks) / 60u;
                std::snprintf(line, sizeof(line), "HULL %s  ENERGY %s  CORES %u/%u  %u:%02u", hull, energy, gs.cores,
                              gs.cores_total, secs / 60u, secs % 60u);
                label.text.assign(StringView(line));
                break;
            }
            case HudKind::Banner:
                label.text.assign(gs.banner_ticks > 0 ? gs.banner.view() : StringView(""));
                break;
            case HudKind::Hint:
                if (gs.phase == RunPhase::Intro) label.text.assign(StringView("WASD / ARROWS THRUST   SPACE BOOST   J TETHER"));
                else if (gs.phase == RunPhase::Playing && gs.ticks < kHintTicks)
                    label.text.assign(StringView("HAUL THE CORES TO THE DOCK   PADS RESTORE HULL"));
                else if (gs.phase == RunPhase::Won) label.text.assign(StringView("ESC TO QUIT"));
                else label.text.assign(StringView(""));
                break;
            case HudKind::Timer: {
                u32 best = 0;
                for (u32 ch = 0; ch < 8; ++ch) best = math::max(best, gs.gate_ticks[ch]);
                if (best == 0) {
                    label.text.assign(StringView(""));
                } else {
                    std::snprintf(line, sizeof(line), "GATE %u.%us", best / 60u, (best % 60u) * 10u / 60u);
                    label.text.assign(StringView(line));
                }
                break;
            }
        }
    });
    f.query<RunStats&>().each([&](RunStats& r) {
        r.energy = d ? d->energy : 0.0f;
        r.hull = d ? d->hull : 0;
        r.cores = gs.cores;
        r.cores_total = gs.cores_total;
        r.deaths = gs.deaths;
        r.won = gs.phase == RunPhase::Won ? 1u : 0u;
        r.ticks = gs.ticks;
        r.started = gs.phase != RunPhase::Intro ? 1u : 0u;
        r.tethered = gs.tethered.is_null() ? 0u : 1u;
        u32 open = 0;
        for (u32 ch = 0; ch < 8; ++ch) open |= gs.gate_ticks[ch] > 0 ? (1u << ch) : 0u;
        r.gates_open = open;
    });
}

}  // namespace

ResultV install(World& world) {
    if (!world.try_resource<World2D>()) return Error(Errc::InvalidArgument, "undertow.install", "the host installs physics2d first");
    if (!world.try_resource<input::InputState>()) input::install(world);
    world.register_event<audio::PlaySfx>(mce::Phase::FixedUpdate_Pre);
    world.register_components<Drone, DroneVisual, Thruster, Cargo, Pad, Beacon, Switch, Gate, Hazard, Mine, Current, Mover,
                              Flicker, TetherVisual, ScreenFlash, CameraRig, HudText, RunStats, Autopilot>();

    world.add_system(mce::Phase::FixedUpdate_Pre, "undertow_setup", &undertow_setup);
    world.add_system(mce::Phase::FixedUpdate_Pre, "undertow_movers", &undertow_movers, "undertow_setup");
    world.add_system(mce::Phase::FixedUpdate_Pre, "undertow_gates", &undertow_gates, "undertow_movers");
    world.add_system(mce::Phase::FixedUpdate_Pre, "undertow_currents", &undertow_currents, "undertow_gates");
    world.add_system(mce::Phase::FixedUpdate_Pre, "undertow_drone", &undertow_drone, "undertow_currents");
    world.add_system(mce::Phase::FixedUpdate_Post, "undertow_impacts", &undertow_impacts);
    world.add_system(mce::Phase::FixedUpdate_Post, "undertow_triggers", &undertow_triggers, "undertow_impacts");
    world.add_system(mce::Phase::FixedUpdate_Post, "undertow_camera", &undertow_camera, "undertow_triggers");
    world.add_system(mce::Phase::FixedUpdate_Post, "undertow_present", &undertow_present, "undertow_camera");
    world.set_resource(UndertowState{});
    return ok();
}

}  // namespace undertow
