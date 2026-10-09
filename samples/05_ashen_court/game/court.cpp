#include "court.h"

#include <cmath>

namespace court {

using namespace mce;
using namespace mce::math;
using physics::ControllerFlags3D;
using physics::ControllerParams3D;
using physics::MoveResult3D;
using physics::World3D;

namespace {

// ---- tuning (metres, seconds) ---------------------------------------------------------------------
constexpr f32 kGravity = 18.0f;  // snappier than 9.81 for a melee game
constexpr f32 kRunSpeed = 5.0f;
constexpr f32 kSwingMoveSpeed = 1.5f;
constexpr f32 kJumpSpeed = 6.0f;
constexpr f32 kTurnRate = 12.0f;
constexpr u32 kSwingCooldown = 4;
constexpr f32 kSwingRange = 2.3f;
constexpr f32 kSwingCosArc = 0.25f;  // ~75 degrees either side
constexpr i32 kSwingDamage = 34;
constexpr u32 kPlayerInvuln = 40;
constexpr f32 kKillY = -10.0f;

constexpr f32 kHuskSpeed = 3.2f;
constexpr f32 kHuskReach = 1.7f;
constexpr i32 kHuskDamage = 12;
constexpr f32 kWispSpeed = 2.4f;
constexpr f32 kWispNear = 6.0f, kWispFar = 9.0f;
constexpr f32 kOrbSpeed = 9.0f;
constexpr u32 kOrbLife = 150;
constexpr i32 kOrbDamage = 10;
constexpr f32 kBruteSpeed = 2.6f;
constexpr f32 kChargeSpeed = 11.0f;
constexpr i32 kChargeDamage = 25;
constexpr f32 kLoseRange = 24.0f;

constexpr u8 kLayerWorld = 0, kLayerActor = 1;

f32 sight_range(Kind k) { return k == Kind::Wisp ? 16.0f : k == Kind::Brute ? 14.0f : 12.0f; }

ControllerParams3D params_for(Kind k) {
    ControllerParams3D p;
    p.layer = kLayerActor;
    p.mask = 1u << kLayerWorld;
    p.step_height = 0.45f;  // the dais steps are 0.4 m
    switch (k) {
        case Kind::Player: p.radius = 0.3f; p.half_height = 0.55f; break;
        case Kind::Husk: p.radius = 0.32f; p.half_height = 0.45f; break;
        case Kind::Wisp: p.radius = 0.4f; p.half_height = 0.5f; break;
        case Kind::Brute: p.radius = 0.55f; p.half_height = 0.35f; break;
    }
    return p;
}
f32 standing_height(Kind k) {
    const ControllerParams3D p = params_for(k);
    return p.half_height + p.radius + p.padding;
}

vec3 flat(vec3 v) { return {v.x, 0.0f, v.z}; }
vec3 facing(f32 yaw) { return {std::sin(yaw), 0.0f, std::cos(yaw)}; }
f32 yaw_of(vec3 dir) { return std::atan2(dir.x, dir.z); }
f32 wrap_angle(f32 a) {
    while (a > kPi) a -= kTau;
    while (a < -kPi) a += kTau;
    return a;
}
f32 turn_toward(f32 from, f32 to, f32 max_step) { return from + clamp(wrap_angle(to - from), -max_step, max_step); }

Entity static_id(u32 i) { return Entity{0x100000u + i, 1}; }

void play(Frame& f, audio::ClipHandle clip, vec3 at, f32 gain_db = 0.0f) {
    if (clip.is_null()) return;
    audio::PlaySfx ev;
    ev.clip_handle = clip;
    ev.gain_db = gain_db;
    ev.attenuation_id = kAttenuationCombat;
    ev.position = at;
    f.events().push(ev);
}

// One tick of gravity + the controller.
MoveResult3D move_actor(World3D& pw, Entity e, Actor& a, vec3 horizontal, f32 dt) {
    if (a.grounded && a.vy < 0.0f) a.vy = 0.0f;
    a.vy -= kGravity * dt;
    const MoveResult3D r = pw.move_character(e, {horizontal.x * dt, a.vy * dt, horizontal.z * dt}, params_for(a.kind));
    a.pos = r.position;
    a.grounded = (r.flags & ControllerFlags3D::Ground) ? 1u : 0u;
    if (r.hit_ceiling && a.vy > 0.0f) a.vy = 0.0f;
    return r;
}

// ---- spawning -------------------------------------------------------------------------------------

struct CastSpot {
    Kind kind;
    vec3 at;
};
constexpr CastSpot kCast[] = {
    {Kind::Husk, {-5.0f, 0, 6.0f}},    {Kind::Husk, {5.0f, 0, 3.0f}},     {Kind::Husk, {-14.0f, 0, -8.0f}},
    {Kind::Husk, {13.0f, 0, -6.0f}},   {Kind::Wisp, {0.0f, 1.2f, -8.0f}}, {Kind::Wisp, {-17.0f, 0, -15.0f}},
    {Kind::Wisp, {17.0f, 0, -14.0f}},  {Kind::Brute, {-7.0f, 0, -2.0f}},  {Kind::Brute, {11.0f, 0, 11.0f}},
};
constexpr vec3 kHearts[] = {{-18.0f, 0, 18.0f}, {18.0f, 0, -2.0f}, {-2.0f, 0, -14.0f}, {-18.0f, 0, -2.0f}};
constexpr vec3 kPlayerStart{0.0f, 0.0f, 17.0f};

// Spawns through the World outside a tick (install) and through Commands inside one (restart).
struct Spawner {
    World& world;
    Commands* cmd;
    template <class... Ts>
    Entity operator()(const Ts&... cs) {
        return cmd ? cmd->spawn(cs...) : world.spawn_with(cs...);
    }
};

Entity spawn_actor(Spawner& w, World3D& pw, Kind kind, vec3 at, i32 hp) {
    Actor a;
    a.kind = kind;
    a.pos = a.prev_pos = a.home = at + vec3{0.0f, standing_height(kind), 0.0f};
    a.yaw = a.prev_yaw = kind == Kind::Player ? kPi : 0.0f;  // the player faces into the court (-Z)
    Health h;
    h.hp = h.max = hp;
    Entity e;
    switch (kind) {
        case Kind::Player: e = w(a, h, PlayerCtl{}); break;
        case Kind::Husk: e = w(a, h, Senses{}, HuskBrain{}); break;
        case Kind::Wisp: {
            WispBrain b;
            b.orbit = at.x < 0.0f ? 1.0f : -1.0f;
            e = w(a, h, Senses{}, b);
            break;
        }
        case Kind::Brute: e = w(a, h, Senses{}, BruteBrain{}); break;
    }
    MCE_VERIFY(pw.create_character(e, a.pos, params_for(kind)));
    return e;
}

// Despawns every actor, orb and heart and spawns the cast afresh (start and restart). Inside a
// tick (`cmd` set) the structural changes are deferred to the phase sync; the physics bodies go now.
void populate(World& w, Commands* cmd, CourtState& gs) {
    World3D& pw = w.resource<Physics>().world;
    Array<Entity> doomed;
    w.query<const Actor&>().each([&](Entity e, const Actor&) { doomed.push_back(e); });
    w.query<const Orb&>().each([&](Entity e, const Orb&) { doomed.push_back(e); });
    w.query<const Heart&>().each([&](Entity e, const Heart&) { doomed.push_back(e); });
    for (Entity e : doomed) {
        pw.destroy_body(e);
        if (cmd) cmd->destroy(e);
        else w.destroy(e);
    }
    Spawner sp{w, cmd};
    gs.player = spawn_actor(sp, pw, Kind::Player, kPlayerStart, 100);
    gs.enemies = gs.enemies_total = 0;
    for (const CastSpot& c : kCast) {
        const i32 hp = c.kind == Kind::Husk ? 60 : c.kind == Kind::Wisp ? 40 : 150;
        spawn_actor(sp, pw, c.kind, c.at, hp);
        ++gs.enemies;
        ++gs.enemies_total;
    }
    for (vec3 h : kHearts) {
        Heart heart;
        heart.pos = h + vec3{0.0f, 0.8f, 0.0f};
        sp(heart);
    }
    gs.kills = 0;
    gs.ticks = 0;
    gs.damage_taken = 0;
    gs.cam_yaw = gs.prev_cam_yaw = 0.0f;  // behind the player, looking into the court (-Z)
    gs.boom = gs.prev_boom = kBoomLength;
    ++gs.serial;
}

// ---- guards and actions (fsm.h): code, never serialized --------------------------------------------

const Senses& senses(Frame& f, Entity e) { return *f.world().try_get<Senses>(e); }
bool sees(Frame& f, Entity e) { return senses(f, e).sees != 0; }
bool lost(Frame& f, Entity e) { return senses(f, e).lost != 0; }
bool in_reach(Frame& f, Entity e) { return senses(f, e).in_reach != 0; }
bool bumped(Frame& f, Entity e) { return senses(f, e).bumped != 0; }
bool wisp_ready(Frame& f, Entity e) {
    return senses(f, e).in_reach != 0 && f.world().try_get<WispBrain>(e)->state_time >= 1.8f;
}

vec3 player_pos(Frame& f) {
    const CourtState& gs = f.resource<CourtState>();
    const Actor* p = f.world().try_get<Actor>(gs.player);
    return p ? p->pos : vec3{};
}

void husk_strike(Frame& f, Entity e) {
    const Actor& a = *f.world().try_get<Actor>(e);
    AttackEvent ev;
    ev.source = e;
    ev.origin = a.pos;
    ev.dir = facing(a.yaw);
    ev.range = kHuskReach + 0.3f;
    ev.cos_arc = 0.2f;
    ev.damage = kHuskDamage;
    ev.hits_player = 1;
    f.events().push(ev);
    play(f, f.resource<CourtState>().sounds.swing, a.pos, -6.0f);
}

void wisp_fire(Frame& f, Entity e) {
    const Actor& a = *f.world().try_get<Actor>(e);
    const vec3 from = a.pos + vec3{0.0f, 0.35f, 0.0f};
    vec3 to = player_pos(f) + vec3{0.0f, 0.3f, 0.0f};
    const vec3 d = to - from;
    const f32 len = length(d);
    if (len < 1e-3f) return;
    Orb orb;
    orb.pos = orb.prev_pos = from + d * (0.5f / len);
    orb.vel = d * (kOrbSpeed / len);
    orb.life = kOrbLife;
    f.commands().spawn(orb);
    play(f, f.resource<CourtState>().sounds.cast, from, -2.0f);
}

void brute_lock(Frame& f, Entity e) {
    BruteBrain& b = *f.world().try_get_mut<BruteBrain>(e);
    const Actor& a = *f.world().try_get<Actor>(e);
    vec3 d = flat(player_pos(f) - a.pos);
    const f32 len = length(d);
    b.charge_dir = len > 1e-3f ? d * (1.0f / len) : facing(a.yaw);
    b.charge_hit = 0;
    play(f, f.resource<CourtState>().sounds.roar, a.pos);
}

constexpr fsm::Trans<HuskState> kHuskFsm[] = {
    {HuskState::Idle, HuskState::Chase, &sees},
    {HuskState::Chase, HuskState::WindUp, &in_reach},
    {HuskState::Chase, HuskState::Idle, &lost},
    {HuskState::WindUp, HuskState::Recover, nullptr, 0.45f, nullptr, &husk_strike},  // the blow lands on exit
    {HuskState::Recover, HuskState::Chase, nullptr, 0.6f},
    {HuskState::Stagger, HuskState::Chase, nullptr, 0.35f},
};
constexpr fsm::Trans<WispState> kWispFsm[] = {
    {WispState::Idle, WispState::Hover, &sees},
    {WispState::Hover, WispState::Cast, &wisp_ready},
    {WispState::Hover, WispState::Idle, &lost},
    {WispState::Cast, WispState::Cooldown, nullptr, 0.5f, nullptr, &wisp_fire},
    {WispState::Cooldown, WispState::Hover, nullptr, 1.0f},
    {WispState::Stagger, WispState::Hover, nullptr, 0.3f},
};
constexpr fsm::Trans<BruteState> kBruteFsm[] = {
    {BruteState::Idle, BruteState::Chase, &sees},
    {BruteState::Chase, BruteState::WindUp, &in_reach, -1.0f, &brute_lock},
    {BruteState::Chase, BruteState::Idle, &lost},
    {BruteState::WindUp, BruteState::Charge, nullptr, 0.7f},
    {BruteState::Charge, BruteState::Stunned, &bumped},
    {BruteState::Charge, BruteState::Recover, nullptr, 1.0f},
    {BruteState::Stunned, BruteState::Chase, nullptr, 1.8f},
    {BruteState::Recover, BruteState::Chase, nullptr, 0.6f},
};

// ---- systems ----------------------------------------------------------------------------------------

void ac_flow(Frame& f) {
    CourtState& gs = f.resource<CourtState>();
    const input::InputState& in = f.resource<input::InputState>();
    f.query<Actor&>().each([](Actor& a) {
        a.prev_pos = a.pos;
        a.prev_yaw = a.yaw;
    });
    f.query<Orb&>().each([](Orb& o) { o.prev_pos = o.pos; });
    gs.prev_boom = gs.boom;
    gs.prev_cam_yaw = gs.cam_yaw;
    if (gs.settle) --gs.settle;
    gs.shake = gs.shake > f.dt * 2.0f ? gs.shake - f.dt * 2.0f : 0.0f;
    switch (gs.mode) {
        case Mode::Title:
            if (in.confirm_pressed || in.jump_pressed) gs.mode = Mode::Playing;
            break;
        case Mode::Playing:
            if (in.pause_pressed) gs.mode = Mode::Paused;
            else ++gs.ticks;
            break;
        case Mode::Paused:
            if (in.pause_pressed || in.confirm_pressed) gs.mode = Mode::Playing;
            break;
        case Mode::Dead:
        case Mode::Victory:
            if (in.confirm_pressed) {
                populate(f.world(), &f.commands(), gs);
                gs.mode = Mode::Playing;
                gs.settle = 1;  // the new cast lands at the phase sync; the sim systems sit this tick out
            }
            break;
    }
}

bool playing(Frame& f) {
    const CourtState& gs = f.resource<CourtState>();
    return gs.mode == Mode::Playing && gs.settle == 0;
}

void ac_player(Frame& f) {
    if (!playing(f)) return;
    CourtState& gs = f.resource<CourtState>();
    const input::InputState& in = f.resource<input::InputState>();
    World3D& pw = f.resource<Physics>().world;
    const f32 dt = f.dt;

    // Camera: look input turns the boom (mouse delta or the right stick through the host's map).
    if (gs.autopilot == 0) {
        gs.cam_yaw = wrap_angle(gs.cam_yaw - in.look.x * 0.0045f);
        gs.cam_pitch = clamp(gs.cam_pitch + in.look.y * 0.003f, 0.05f, 1.1f);
    }

    Actor* a = f.world().try_get_mut<Actor>(gs.player);
    PlayerCtl* pc = f.world().try_get_mut<PlayerCtl>(gs.player);
    if (!a || !pc) return;
    const vec3 cam_fwd{-std::sin(gs.cam_yaw), 0.0f, -std::cos(gs.cam_yaw)};
    const vec3 cam_right{std::cos(gs.cam_yaw), 0.0f, -std::sin(gs.cam_yaw)};
    vec3 dir = cam_fwd * clamp(in.move.y, -1.0f, 1.0f) + cam_right * clamp(in.move.x, -1.0f, 1.0f);
    const f32 len = length(dir);
    if (len > 1.0f) dir = dir * (1.0f / len);

    if (pc->cooldown > 0) --pc->cooldown;
    if (pc->swing == 0 && pc->cooldown == 0 && in.fire_pressed) {
        pc->swing = 1;
        ++pc->swings;
        if (len > 0.1f) a->yaw = yaw_of(dir);  // swing where the stick points
        play(f, gs.sounds.swing, a->pos, -3.0f);
    } else if (pc->swing > 0) {
        ++pc->swing;
    }
    if (pc->swing == kSwingHitTick) {
        AttackEvent ev;
        ev.source = gs.player;
        ev.origin = a->pos;
        ev.dir = facing(a->yaw);
        ev.range = kSwingRange;
        ev.cos_arc = kSwingCosArc;
        ev.damage = kSwingDamage;
        f.events().push(ev);
    }
    if (pc->swing >= kSwingTicks) {
        pc->swing = 0;
        pc->cooldown = kSwingCooldown;
    }

    const f32 speed = pc->swing > 0 ? kSwingMoveSpeed : kRunSpeed;
    if (len > 0.1f && pc->swing == 0) a->yaw = turn_toward(a->yaw, yaw_of(dir), kTurnRate * dt);
    if (a->grounded && in.jump_pressed && pc->swing == 0) {
        a->vy = kJumpSpeed;
        a->grounded = 0;
    }
    move_actor(pw, gs.player, *a, dir * speed, dt);
    if (gs.autopilot) {  // the bot's camera trails the heading
        gs.cam_yaw = wrap_angle(gs.cam_yaw + wrap_angle(a->yaw + kPi - gs.cam_yaw) * min(1.0f, 3.0f * dt));
        gs.cam_pitch = 0.32f;
    }
    if (a->pos.y < kKillY) {
        pw.teleport(gs.player, kPlayerStart + vec3{0.0f, standing_height(Kind::Player), 0.0f}, quat::identity());
        a->pos = a->prev_pos = kPlayerStart + vec3{0.0f, standing_height(Kind::Player), 0.0f};
        a->vy = 0.0f;
    }
}

void ac_senses(Frame& f) {
    if (!playing(f)) return;
    const CourtState& gs = f.resource<CourtState>();
    const World3D& pw = f.resource<Physics>().world;
    const Actor* p = f.world().try_get<Actor>(gs.player);
    const Health* ph = f.world().try_get<Health>(gs.player);
    const bool target = p && ph && ph->hp > 0;
    physics::QueryFilter3D walls;
    walls.layer_mask = 1u << kLayerWorld;
    f.query<Senses&, const Actor&>().each([&](Senses& s, const Actor& a) {
        s.sees = s.lost = s.in_reach = 0;
        s.distance = 1e9f;
        if (!target) return;
        const vec3 d = flat(p->pos - a.pos);
        s.distance = length(d);
        s.lost = s.distance > kLoseRange ? 1u : 0u;
        if (s.distance <= sight_range(a.kind)) {
            const vec3 eye = a.pos + vec3{0.0f, 0.5f, 0.0f};
            const vec3 to = p->pos + vec3{0.0f, 0.5f, 0.0f} - eye;
            const f32 dist = length(to);
            s.sees = (dist < 1e-3f || !pw.raycast(eye, to, dist, walls)) ? 1u : 0u;
        }
        switch (a.kind) {
            case Kind::Husk: s.in_reach = s.distance <= kHuskReach ? 1u : 0u; break;
            case Kind::Wisp: s.in_reach = (s.sees && s.distance <= 14.0f) ? 1u : 0u; break;
            case Kind::Brute: s.in_reach = (s.sees && s.distance <= 10.0f && s.distance >= 3.0f) ? 1u : 0u; break;
            case Kind::Player: break;
        }
    });
}

void ac_husk_fsm(Frame& f) {
    if (playing(f)) fsm::run<HuskBrain>(f, Span<const fsm::Trans<HuskState>>(kHuskFsm));
}
void ac_wisp_fsm(Frame& f) {
    if (playing(f)) fsm::run<WispBrain>(f, Span<const fsm::Trans<WispState>>(kWispFsm));
}
void ac_brute_fsm(Frame& f) {
    if (playing(f)) fsm::run<BruteBrain>(f, Span<const fsm::Trans<BruteState>>(kBruteFsm));
}

void ac_enemies(Frame& f) {
    if (!playing(f)) return;
    CourtState& gs = f.resource<CourtState>();
    World3D& pw = f.resource<Physics>().world;
    World& w = f.world();
    const f32 dt = f.dt;
    const vec3 target = player_pos(f);

    f.query<Actor&, Senses&>().each([&](Entity e, Actor& a, Senses& s) {
        if (a.kind == Kind::Player) return;
        const vec3 to = flat(target - a.pos);
        const f32 dist = length(to);
        const vec3 toward = dist > 1e-3f ? to * (1.0f / dist) : facing(a.yaw);
        vec3 v{};
        s.bumped = 0;
        switch (a.kind) {
            case Kind::Husk: {
                const HuskState st = w.try_get<HuskBrain>(e)->state;
                if (st == HuskState::Chase) v = toward * kHuskSpeed;
                if (st == HuskState::Chase || (st == HuskState::WindUp && w.try_get<HuskBrain>(e)->state_time < 0.25f))
                    a.yaw = turn_toward(a.yaw, yaw_of(toward), 8.0f * dt);
                break;
            }
            case Kind::Wisp: {
                const WispBrain& b = *w.try_get<WispBrain>(e);
                if (b.state == WispState::Hover) {
                    const f32 radial = dist < kWispNear ? -1.0f : dist > kWispFar ? 1.0f : 0.0f;
                    const vec3 tangent{-toward.z * b.orbit, 0.0f, toward.x * b.orbit};
                    v = (toward * radial + tangent * 0.6f) * kWispSpeed;
                }
                if (b.state != WispState::Idle) a.yaw = turn_toward(a.yaw, yaw_of(toward), 6.0f * dt);
                break;
            }
            case Kind::Brute: {
                BruteBrain& b = *w.try_get_mut<BruteBrain>(e);
                if (b.state == BruteState::Chase) {
                    v = toward * kBruteSpeed;
                    a.yaw = turn_toward(a.yaw, yaw_of(toward), 4.0f * dt);
                } else if (b.state == BruteState::WindUp) {
                    a.yaw = turn_toward(a.yaw, yaw_of(b.charge_dir), 10.0f * dt);
                } else if (b.state == BruteState::Charge) {
                    v = b.charge_dir * kChargeSpeed;
                    a.yaw = yaw_of(b.charge_dir);
                    if (!b.charge_hit && dist < 1.3f) {
                        AttackEvent ev;
                        ev.source = e;
                        ev.origin = a.pos;
                        ev.dir = b.charge_dir;
                        ev.range = 1.6f;
                        ev.cos_arc = -2.0f;  // a sphere
                        ev.damage = kChargeDamage;
                        ev.hits_player = 1;
                        f.events().push(ev);
                        b.charge_hit = 1;
                    }
                }
                break;
            }
            case Kind::Player: break;
        }
        const MoveResult3D r = move_actor(pw, e, a, v, dt);
        if (a.kind == Kind::Brute && w.try_get<BruteBrain>(e)->state == BruteState::Charge &&
            (r.flags & ControllerFlags3D::Wall)) {
            s.bumped = 1;
            gs.shake = 0.6f;
            play(f, gs.sounds.hit, a.pos, 2.0f);
        }
    });
}

void ac_orbs(Frame& f) {
    if (!playing(f)) return;
    const World3D& pw = f.resource<Physics>().world;
    physics::QueryFilter3D walls;
    walls.layer_mask = 1u << kLayerWorld;
    const vec3 target = player_pos(f);
    f.query<Orb&>().each([&](Entity e, Orb& o) {
        const vec3 step = o.vel * f.dt;
        const f32 len = length(step);
        bool done = --o.life == 0;
        if (!done && len > 0.0f && pw.raycast(o.pos, step, len, walls)) done = true;
        o.pos = o.pos + step;
        if (!done && length(o.pos - (target + vec3{0.0f, 0.3f, 0.0f})) < 0.7f) {
            AttackEvent ev;
            ev.source = e;
            ev.origin = o.pos;
            ev.dir = o.vel * (1.0f / kOrbSpeed);
            ev.range = 1.0f;
            ev.cos_arc = -2.0f;
            ev.damage = kOrbDamage;
            ev.hits_player = 1;
            f.events().push(ev);
            done = true;
        }
        if (done) f.commands().destroy(e);
    });
}

void ac_physics(Frame& f) {
    CourtState& gs = f.resource<CourtState>();
    World3D& pw = f.resource<Physics>().world;
    if (gs.mode == Mode::Playing) pw.step(f.dt);
    // The boom: sweep a sphere from the head to the wanted eye; a hit pulls the camera in front of
    // the wall (UC-B: the camera never clips geometry).
    const Actor* p = f.world().try_get<Actor>(gs.player);
    if (!p) return;
    const vec3 pivot = p->pos + kHeadOffset;
    const f32 cp = std::cos(gs.cam_pitch);
    const vec3 back{std::sin(gs.cam_yaw) * cp, std::sin(gs.cam_pitch), std::cos(gs.cam_yaw) * cp};
    const vec3 want = pivot + back * kBoomLength;
    gs.boom = kBoomLength;
    if (std::optional<physics::ShapeHit3D> hit = pw.camera_sweep(pivot, want, kBoomRadius)) {
        gs.boom = length(hit->position - pivot);
        if (gs.boom < kBoomLength - 0.05f) ++gs.camera_hits;
        gs.closest_boom = min(gs.closest_boom, gs.boom);
    }
}

void ac_combat(Frame& f) {
    CourtState& gs = f.resource<CourtState>();
    World& w = f.world();
    World3D& pw = f.resource<Physics>().world;
    for (const AttackEvent& ev : f.events().read<AttackEvent>()) {
        f.query<Actor&, Health&>().each([&](Entity e, Actor& a, Health& h) {
            if (e == ev.source || h.hp <= 0) return;
            if ((a.kind == Kind::Player) != (ev.hits_player != 0)) return;
            const vec3 d = flat(a.pos - ev.origin);
            const f32 dist = length(d);
            const f32 radius = params_for(a.kind).radius;
            if (dist > ev.range + radius) return;
            if (ev.cos_arc > -1.0f && dist > 1e-3f && dot(d * (1.0f / dist), ev.dir) < ev.cos_arc) return;
            if (h.invuln > 0) return;
            i32 dmg = ev.damage;
            if (const BruteBrain* b = w.try_get<BruteBrain>(e); b && b->state == BruteState::Stunned) dmg *= 2;
            h.hp -= dmg;
            h.flash = 8;
            Damaged d_ev{e, a.kind, dmg, a.pos};
            f.events().push(d_ev);
            if (a.kind == Kind::Player) {
                h.invuln = kPlayerInvuln;
                gs.damage_taken += static_cast<u32>(dmg);
                gs.shake = 0.5f;
                play(f, gs.sounds.hurt, a.pos);
            } else {
                if (PlayerCtl* pc = w.try_get_mut<PlayerCtl>(gs.player)) ++pc->hits;
                play(f, gs.sounds.hit, a.pos);
                // Light enemies stagger (the FSM routes them back); a knock-back shove.
                if (HuskBrain* hb = w.try_get_mut<HuskBrain>(e)) {
                    hb->state = HuskState::Stagger;
                    hb->state_time = 0.0f;
                }
                if (WispBrain* wb = w.try_get_mut<WispBrain>(e)) {
                    wb->state = WispState::Stagger;
                    wb->state_time = 0.0f;
                }
                if (a.kind != Kind::Brute && dist > 1e-3f) {
                    const MoveResult3D r = pw.move_character(e, d * (0.35f / dist), params_for(a.kind));
                    a.pos = r.position;
                }
            }
            if (h.hp <= 0) {
                f.events().push(Died{e, a.kind, a.pos});
                play(f, gs.sounds.death, a.pos, a.kind == Kind::Player ? 0.0f : -2.0f);
                if (a.kind == Kind::Player) {
                    gs.mode = Mode::Dead;
                    ++gs.deaths;
                } else {
                    ++gs.kills;
                    --gs.enemies;
                    pw.destroy_body(e);
                    f.commands().destroy(e);
                }
            }
        });
    }
    if (gs.mode == Mode::Playing && gs.enemies == 0 && gs.enemies_total > 0) gs.mode = Mode::Victory;
    f.query<Health&>().each([](Health& h) {
        if (h.invuln > 0) --h.invuln;
        if (h.flash > 0) --h.flash;
    });
}

void ac_pickups(Frame& f) {
    if (!playing(f)) return;
    CourtState& gs = f.resource<CourtState>();
    Actor* p = f.world().try_get_mut<Actor>(gs.player);
    Health* h = f.world().try_get_mut<Health>(gs.player);
    if (!p || !h) return;
    f.query<const Heart&>().each([&](Entity e, const Heart& heart) {
        if (h->hp >= h->max || length(heart.pos - p->pos) > 1.1f) return;
        h->hp = min(h->max, h->hp + static_cast<i32>(heart.heal));
        play(f, gs.sounds.heal, heart.pos);
        f.commands().destroy(e);
    });
}

void add_box(World3D& pw, u32& id, vec3 center, vec3 half, f32 yaw = 0.0f) {
    physics::BodyDesc3D d;
    d.shape = physics::Shape3D::box(half);
    d.position = center;
    d.rotation = quat_from_axis_angle({0, 1, 0}, yaw);
    d.layer = kLayerWorld;
    MCE_VERIFY(pw.create_body(static_id(id++), d));
}

}  // namespace

// ---- the court ------------------------------------------------------------------------------------

void build_layout(Layout& out) {
    out.statics.clear();
    out.braziers.clear();
    auto put = [&](Model m, vec3 p, f32 yaw = 0.0f, vec3 s = {1, 1, 1}) { out.statics.push_back(Placement{m, p, yaw, s}); };
    put(Model::Floor, {0, 0, 0});
    // Perimeter: 11 segments a side, inner faces at +-21.5.
    for (i32 k = 0; k < 11; ++k) {
        const f32 t = -20.0f + 4.0f * static_cast<f32>(k);
        put(Model::Wall, {t, 0, -22.0f});
        put(Model::Wall, {t, 0, 22.0f}, kPi);
        put(Model::Wall, {-22.0f, 0, t}, 0.5f * kPi);
        put(Model::Wall, {22.0f, 0, t}, -0.5f * kPi);
    }
    // Colonnade: two rows of seven, every third one fallen.
    for (i32 k = 0; k < 7; ++k) {
        const f32 z = -12.0f + 4.0f * static_cast<f32>(k);
        for (f32 x : {-9.0f, 9.0f}) {
            const bool broken = (k + (x < 0 ? 0 : 1)) % 3 == 2;
            put(broken ? Model::BrokenColumn : Model::Column, {x, 0, z}, 0.37f * static_cast<f32>(k));
        }
    }
    put(Model::Arch, {-6.0f, 0, -18.0f});
    put(Model::Arch, {6.0f, 0, -18.0f});
    put(Model::Dais, {0, 0, -8.0f});
    const vec3 braziers[] = {{-16, 0, -16}, {16, 0, -16}, {-16, 0, 16}, {16, 0, 16}, {-6, 0, -1.5f}, {6, 0, -1.5f}};
    for (vec3 b : braziers) {
        put(Model::Brazier, b);
        out.braziers.push_back(Light{b + vec3{0.0f, 1.9f, 0.0f}, {1.0f, 0.55f, 0.2f}, 3.0f, 10.0f});
    }
    const vec3 rubble[] = {{14, 0, 4}, {-13, 0, -3}, {5, 0, 12}, {-6, 0, 10}, {16, 0, -10}, {-16, 0, 9}, {2, 0, -19}, {-3, 0, 15}};
    for (u32 i = 0; i < sizeof(rubble) / sizeof(rubble[0]); ++i) put(Model::Rubble, rubble[i], 0.9f * static_cast<f32>(i));
}

namespace {
void build_colliders(World3D& pw, const Layout& lay) {
    u32 id = 0;
    add_box(pw, id, {0, -0.5f, 0}, {24.0f, 0.5f, 24.0f});  // the floor slab
    add_box(pw, id, {0, 2.5f, -22.0f}, {22.5f, 2.5f, 0.5f});
    add_box(pw, id, {0, 2.5f, 22.0f}, {22.5f, 2.5f, 0.5f});
    add_box(pw, id, {-22.0f, 2.5f, 0}, {0.5f, 2.5f, 22.5f});
    add_box(pw, id, {22.0f, 2.5f, 0}, {0.5f, 2.5f, 22.5f});
    for (const Placement& p : lay.statics) {
        switch (p.model) {
            case Model::Column: add_box(pw, id, p.position + vec3{0, 2.55f, 0}, {0.45f, 2.55f, 0.45f}, p.yaw); break;
            case Model::BrokenColumn: add_box(pw, id, p.position + vec3{0, 1.1f, 0}, {0.45f, 1.1f, 0.45f}, p.yaw); break;
            case Model::Arch:
                for (f32 px : {-2.0f, 2.0f}) add_box(pw, id, p.position + vec3{px, 1.8f, 0}, {0.5f, 1.8f, 0.6f});
                break;
            case Model::Dais:
                for (i32 k = 0; k < 3; ++k) {
                    const f32 half = 5.0f - 0.8f * static_cast<f32>(k);
                    add_box(pw, id, p.position + vec3{0, 0.2f + 0.4f * static_cast<f32>(k), 0}, {half, 0.2f, half});
                }
                break;
            case Model::Brazier: add_box(pw, id, p.position + vec3{0, 0.62f, 0}, {0.5f, 0.62f, 0.5f}); break;
            case Model::Rubble: add_box(pw, id, p.position + vec3{0, 0.35f, 0}, {1.0f, 0.35f, 0.8f}, p.yaw); break;
            default: break;
        }
    }
}
}  // namespace

ResultV install(World& world) {
    if (!world.try_resource<input::InputState>())
        return Error(Errc::InvalidArgument, "court.install", "the host installs input first");
    world.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);
    world.register_event<AttackEvent>(Phase::FixedUpdate_Pre);
    world.register_event<Damaged>(Phase::FixedUpdate_Post);
    world.register_event<Died>(Phase::FixedUpdate_Post);

    MCE_TRY(fsm::validate(Span<const fsm::Trans<HuskState>>(kHuskFsm), HuskState::Idle));
    MCE_TRY(fsm::validate(Span<const fsm::Trans<WispState>>(kWispFsm), WispState::Idle));
    MCE_TRY(fsm::validate(Span<const fsm::Trans<BruteState>>(kBruteFsm), BruteState::Idle));

    world.add_system(Phase::FixedUpdate_Pre, "ac_flow", &ac_flow);
    world.add_system(Phase::FixedUpdate_Pre, "ac_player", &ac_player, "ac_flow");
    world.add_system(Phase::FixedUpdate_Pre, "ac_senses", &ac_senses, "ac_player");
    world.add_system(Phase::FixedUpdate_Pre, "ac_husk_fsm", &ac_husk_fsm, "ac_senses");
    world.add_system(Phase::FixedUpdate_Pre, "ac_wisp_fsm", &ac_wisp_fsm, "ac_husk_fsm");
    world.add_system(Phase::FixedUpdate_Pre, "ac_brute_fsm", &ac_brute_fsm, "ac_wisp_fsm");
    world.add_system(Phase::FixedUpdate_Pre, "ac_enemies", &ac_enemies, "ac_brute_fsm");
    world.add_system(Phase::FixedUpdate_Pre, "ac_orbs", &ac_orbs, "ac_enemies");
    world.add_system(Phase::FixedUpdate_Physics, "ac_physics", &ac_physics);
    world.add_system(Phase::FixedUpdate_Post, "ac_combat", &ac_combat);
    world.add_system(Phase::FixedUpdate_Post, "ac_pickups", &ac_pickups, "ac_combat");

    Physics phys;
    world.set_resource(std::move(phys));
    Layout lay;
    build_layout(lay);
    build_colliders(world.resource<Physics>().world, lay);

    CourtState gs;
    if (const Sounds* s = world.try_resource<Sounds>()) gs.sounds = *s;
    world.set_resource(gs);
    populate(world, nullptr, world.resource<CourtState>());
    return ok();
}

u64 hash_game(World& world, u64 seed) {
    const CourtState& gs = world.resource<CourtState>();
    Hasher64 h(world.state_hash(seed));
    h.u64v(world.resource<Physics>().world.hash_state(seed));
    h.u32v(static_cast<u32>(gs.mode));
    h.u64v(gs.ticks);
    h.u32v(gs.kills);
    h.u32v(gs.enemies);
    h.u32v(gs.deaths);
    h.u32v(gs.damage_taken);
    h.f32v(gs.cam_yaw);
    h.f32v(gs.boom);
    h.u32v(gs.camera_hits);
    return h.value();
}

// ---- the bot ---------------------------------------------------------------------------------------

input::InputState autopilot(World& world, u64 tick) {
    input::InputState s;
    s.tick = tick;
    CourtState& gs = world.resource<CourtState>();
    gs.autopilot = 1;
    if (gs.mode == Mode::Title || gs.mode == Mode::Dead || gs.mode == Mode::Victory) {
        s.confirm_pressed = tick % 120 == 0;  // restart after a beat
        return s;
    }
    const Actor* p = world.try_get<Actor>(gs.player);
    const Health* ph = world.try_get<Health>(gs.player);
    const PlayerCtl* pc = world.try_get<PlayerCtl>(gs.player);
    if (!p || !ph || !pc) return s;

    // Goal: a heart when hurt, else the nearest enemy (a brute winding up is dodged sideways).
    vec3 goal = p->pos;
    f32 best = 1e9f;
    if (ph->hp < 45) {
        world.query<const Heart&>().each([&](const Heart& h) {
            const f32 d = length(flat(h.pos - p->pos));
            if (d < best) {
                best = d;
                goal = h.pos;
            }
        });
    }
    bool dodge = false;
    vec3 dodge_dir{};
    Entity foe;
    if (best == 1e9f) {
        world.query<const Actor&>().each([&](Entity e, const Actor& a) {
            if (a.kind == Kind::Player) return;
            const f32 d = length(flat(a.pos - p->pos));
            if (d < best) {
                best = d;
                goal = a.pos;
                foe = e;
            }
            if (const BruteBrain* b = world.try_get<BruteBrain>(e);
                b && (b->state == BruteState::WindUp || b->state == BruteState::Charge) && d < 12.0f) {
                dodge = true;
                const vec3 cd = b->state == BruteState::Charge ? b->charge_dir : flat(p->pos - a.pos) * (1.0f / max(d, 1e-3f));
                dodge_dir = vec3{-cd.z, 0.0f, cd.x};
            }
        });
    }
    vec3 to = flat(goal - p->pos);
    const f32 dist = length(to);
    vec3 want = dist > 1e-3f ? to * (1.0f / dist) : vec3{};
    if (dodge) want = dodge_dir;
    // Close in to swing range, then swing while facing.
    const bool attack_range = !foe.is_null() && dist < 2.0f;
    if (attack_range && !dodge) want = want * 0.2f;
    // Camera-relative stick from the world direction.
    const vec3 cam_fwd{-std::sin(gs.cam_yaw), 0.0f, -std::cos(gs.cam_yaw)};
    const vec3 cam_right{std::cos(gs.cam_yaw), 0.0f, -std::sin(gs.cam_yaw)};
    s.move = {dot(want, cam_right), dot(want, cam_fwd)};
    // Unstick: hop now and then when hugging something.
    s.jump_pressed = (tick % 97 == 0) && !attack_range;
    s.fire_pressed = attack_range && pc->swing == 0 && pc->cooldown == 0;
    return s;
}

}  // namespace court
