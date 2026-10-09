#include "prism_break.h"

#include <cmath>
#include <cstdio>

#include "mce/input/actions.h"

namespace prism_break {

using namespace mce;
using namespace mce::physics;

namespace {

constexpr u32 kAutoServeTicks = 45;    // autopilot serves after this many ticks
constexpr u32 kMaxSweeps = 4;          // bounces resolved per ball per tick
constexpr u32 kMaxExcluded = 4;
constexpr f32 kSkin = 0.002f;          // metres kept between the ball and what it hit
constexpr f32 kMinVertical = 0.28f;    // |vy| / speed never drops below this (no endless rallies)
constexpr f32 kServeSide = 0.35f;      // serve direction: (side, 1) normalized

const math::Color kSteelTint{0.55f, 0.58f, 0.64f, 1.0f};
const math::Color kHpTints[] = {
    {0.36f, 0.86f, 1.0f, 1.0f},   // 1 hit left: cyan
    {0.96f, 0.42f, 0.86f, 1.0f},  // 2: magenta
    {1.0f, 0.80f, 0.30f, 1.0f},   // 3: gold
    {1.0f, 0.52f, 0.30f, 1.0f},   // 4: orange
    {0.95f, 0.32f, 0.32f, 1.0f},  // 5: red
};

void play(Frame& f, const asset::AssetRef<asset::Sfx>& clip, f32 gain_db = 0.0f) {
    if (clip.guid.is_null()) return;
    audio::PlaySfx ev;
    ev.clip = clip.guid;
    ev.gain_db = gain_db;
    f.events().push(ev);
}

vec2 body_pos(const World2D& pw, const World& w, Entity e) {
    BodyState s;
    if (pw.body_state(e, s)) return s.position;
    if (const LocalTransform* t = w.try_get<LocalTransform>(e)) return {t->position.x, t->position.y};
    return {};
}

vec2 half_extents(const World& w, Entity e) {
    if (const Collider2D* c = w.try_get<Collider2D>(e); c && c->shape.type == ShapeType::Box) return c->shape.half;
    return {0.5f, 0.5f};
}

math::Color rainbow(u64 ticks) {
    const f32 t = static_cast<f32>(ticks % 120u) / 120.0f * math::kTau;
    return {0.5f + 0.5f * std::sin(t), 0.5f + 0.5f * std::sin(t + math::kTau / 3.0f),
            0.5f + 0.5f * std::sin(t + 2.0f * math::kTau / 3.0f), 1.0f};
}

// Keeps the ball's speed and a minimum vertical share, so it never rallies sideways forever.
vec2 steady(vec2 v, f32 speed) {
    if (math::length(v) < 1e-5f) v = {0.0f, 1.0f};
    v = math::normalize(v);
    if (std::fabs(v.y) < kMinVertical) {
        v.y = v.y < 0.0f ? -kMinVertical : kMinVertical;
        v.x = (v.x < 0.0f ? -1.0f : 1.0f) * std::sqrt(1.0f - kMinVertical * kMinVertical);
    }
    return v * speed;
}

void arm_bricks(Frame& f, PrismState& ps, World2D& pw) {
    ps.bricks_total = 0;
    f.query<Brick&>().each([&](Entity e, Brick& b) {
        b.hp = b.max_hp > 0 ? b.max_hp : 1u;
        b.alive = 1;
        if (pw.has_body(e)) pw.set_enabled(e, true);
        if (b.kind != BrickKind::Steel) ++ps.bricks_total;
    });
    ps.bricks_left = ps.bricks_total;
}

// ---- FixedUpdate_Pre ---------------------------------------------------------------------------

void prism_setup(Frame& f) {
    PrismState& ps = f.resource<PrismState>();
    World2D& pw = f.resource<World2D>();
    if (ps.setup == 0) {
        ps.setup = 1;
        f.query<const Rules&>().each([&](Entity e, const Rules& r) {
            if (!ps.rules.is_null()) return;
            ps.rules = e;
            ps.lives = r.lives;
        });
        f.query<const Paddle&>().each([&](Entity e, const Paddle&) {
            if (ps.paddle.is_null()) ps.paddle = e;
        });
        arm_bricks(f, ps, pw);
    }

    // After a win or a loss, Jump (or Fire) starts over: bricks back, split balls gone, full lives.
    const input::InputState& in = f.resource<input::InputState>();
    const bool again = in.jump_pressed || in.fire_pressed;
    if ((ps.phase == RunPhase::Won || ps.phase == RunPhase::Lost) && again) {
        arm_bricks(f, ps, pw);
        bool first = true;
        f.query<Ball&>().each([&](Entity e, Ball& b) {
            if (b.spawned) {
                f.commands().destroy(e);
                return;
            }
            b.active = first ? 1u : 0u;
            b.launched = 0;
            b.vel = {};
            first = false;
        });
        const Rules* r = f.world().try_get<Rules>(ps.rules);
        ps.lives = r ? r->lives : 3u;
        ps.score = ps.balls_lost = ps.splits = ps.serve_ticks = ps.bounces = 0;
        ps.ticks = ps.ended_at = 0;
        ps.phase = RunPhase::Serve;
        ps.restarted = 1;  // the press that restarted must not also serve
        return;
    }
    ps.restarted = 0;
    if (ps.phase == RunPhase::Serve || ps.phase == RunPhase::Play) ++ps.ticks;
}

// Where on the paddle the autopilot takes the next ball, in [-0.8, 0.8] of its half width: a
// fresh pick after every paddle bounce (a fixed aim loops forever off a steel brick).
f32 autopilot_aim(u32 bounces) {
    const u32 h = (bounces + 1u) * 2654435761u;
    return (static_cast<f32>((h >> 8) % 1000u) / 999.0f) * 1.6f - 0.8f;
}

// The ball the autopilot tracks: the lowest one coming down, else the lowest one.
bool autopilot_target(Frame& f, f32 aim, f32& x) {
    f32 best_y = 1e9f;
    bool falling = false, found = false;
    f.query<const Ball&, const LocalTransform&>().each([&](const Ball& b, const LocalTransform& t) {
        if (!b.active || !b.launched) return;
        const bool down = b.vel.y < 0.0f;
        if (found && falling && !down) return;
        if (!found || (down && !falling) || t.position.y < best_y) {
            best_y = t.position.y;
            x = t.position.x - aim;
            falling = down;
            found = true;
        }
    });
    return found;
}

void prism_paddle(Frame& f) {
    PrismState& ps = f.resource<PrismState>();
    World2D& pw = f.resource<World2D>();
    const input::InputState& in = f.resource<input::InputState>();
    const Paddle* p = f.world().try_get<Paddle>(ps.paddle);
    if (!p || !pw.has_body(ps.paddle)) return;
    const f32 dt = f.dt;
    const vec2 pos = body_pos(pw, f.world(), ps.paddle);

    f32 dir = 0.0f;
    if (ps.phase == RunPhase::Serve || ps.phase == RunPhase::Play) {
        if (p->autopilot) {
            f32 target = pos.x;
            const f32 aim = autopilot_aim(ps.bounces) * half_extents(f.world(), ps.paddle).x;
            if (autopilot_target(f, aim, target)) dir = math::clamp((target - pos.x) / (p->speed * dt), -1.0f, 1.0f);
        } else {
            dir = math::clamp(in.move.x, -1.0f, 1.0f);
        }
    }
    const f32 want = math::clamp(pos.x + dir * p->speed * dt, p->min_x, p->max_x);
    pw.set_linear_velocity(ps.paddle, {(want - pos.x) / dt, 0.0f});
}

struct Sweep {
    Frame& f;
    PrismState& ps;
    World2D& pw;
    const Rules* rules;
    const Paddle* paddle;
    vec2 paddle_at;
    vec2 paddle_half;
};

// A brick took a hit: lose a point of hp, break at zero (a prism splits the ball that broke it).
void hit_brick(Sweep& s, Entity e, Brick& br, vec2 at, const Ball& ball, Entity ball_entity) {
    if (!br.alive || br.kind == BrickKind::Steel) {
        if (s.rules) play(s.f, s.rules->paddle_sound, -8.0f);
        return;
    }
    if (br.hp > 0) --br.hp;
    if (s.rules) play(s.f, s.rules->brick_sound, br.hp == 0 ? -2.0f : -6.0f);
    if (br.hp > 0) return;
    br.alive = 0;
    s.pw.set_enabled(e, false);
    s.ps.score += br.points;
    if (s.ps.bricks_left > 0) --s.ps.bricks_left;
    if (br.kind != BrickKind::Prism) return;

    // The split: a twin of the ball, mirrored in x, from where the ball is now.
    Ball twin = ball;
    twin.vel = steady({-ball.vel.x, ball.vel.y}, math::length(ball.vel));
    if (std::fabs(ball.vel.x) < 0.5f) twin.vel = steady({ball.vel.y > 0.0f ? 0.6f : -0.6f, ball.vel.y}, math::length(ball.vel));
    twin.launched = 1;
    twin.active = 1;
    twin.spawned = 1;
    LocalTransform lt;
    lt.position = {at.x, at.y, 0.0f};
    const kit::Sprite* look = s.f.world().try_get<kit::Sprite>(ball_entity);
    s.f.commands().spawn(twin, lt, look ? *look : kit::Sprite{});
    ++s.ps.splits;
}

// Moves one ball through the world for this tick: sweep, reflect, repeat.
void sweep_ball(Sweep& s, Entity e, Ball& b, LocalTransform& t) {
    const ColliderShape shape = ColliderShape::circle(b.radius);
    Entity excluded[kMaxExcluded];
    u32 n_excluded = 0;
    vec2 p{t.position.x, t.position.y};
    vec2 d = b.vel * s.f.dt;
    for (u32 i = 0; i < kMaxSweeps; ++i) {
        const f32 len = math::length(d);
        if (len < 1e-6f) break;
        QueryFilter filter;
        filter.exclude = Span<const Entity>(excluded, n_excluded);
        const std::optional<RayHit2D> hit = s.pw.shape_cast(shape, p, p + d, filter);
        if (!hit) {
            p = p + d;
            break;
        }
        const bool overlap = hit->t <= 0.0f && math::dot(hit->normal, d) < 0.0f && hit->entity != s.ps.paddle &&
                             math::length(hit->point - p) < 1e-6f;
        if (overlap) {  // started inside something (it moved onto the ball): pass through it
            if (n_excluded < kMaxExcluded) excluded[n_excluded++] = hit->entity;
            continue;
        }
        const f32 frac = math::max(0.0f, hit->t - kSkin / len);
        p = p + d * frac;
        const f32 rest = len * (1.0f - frac);
        const f32 speed = math::length(b.vel);
        if (hit->entity == s.ps.paddle && s.paddle) {
            // Where it lands on the paddle sets the angle; it always leaves upwards.
            const f32 off = math::clamp((p.x - s.paddle_at.x) / (s.paddle_half.x + b.radius), -1.0f, 1.0f);
            const f32 a = math::radians(s.paddle->bounce_angle) * off;
            b.vel = steady({std::sin(a), std::cos(a)}, speed);
            if (n_excluded < kMaxExcluded) excluded[n_excluded++] = s.ps.paddle;
            ++s.ps.bounces;
            if (s.rules) play(s.f, s.rules->paddle_sound, -6.0f);
        } else {
            const f32 vn = math::dot(b.vel, hit->normal);
            if (vn < 0.0f) b.vel = b.vel - hit->normal * (2.0f * vn);
            if (Brick* br = s.f.world().try_get_mut<Brick>(hit->entity)) {
                hit_brick(s, hit->entity, *br, p, b, e);
                if (br->kind != BrickKind::Steel) b.vel = b.vel * (math::min(speed + b.speedup, b.max_speed) / speed);
            }
            b.vel = steady(b.vel, math::length(b.vel));
        }
        d = math::normalize(b.vel) * rest;
    }
    t.position.x = p.x;
    t.position.y = p.y;
}

void prism_balls(Frame& f) {
    PrismState& ps = f.resource<PrismState>();
    World2D& pw = f.resource<World2D>();
    const input::InputState& in = f.resource<input::InputState>();
    World& w = f.world();
    const Paddle* paddle = w.try_get<Paddle>(ps.paddle);
    const Rules* rules = w.try_get<Rules>(ps.rules);
    const f32 kill_y = rules ? rules->kill_y : -8.0f;
    // Where the paddle will be after this tick's physics step (it moves by velocity).
    const vec2 paddle_now = body_pos(pw, w, ps.paddle);
    const vec2 paddle_at = paddle_now + pw.linear_velocity(ps.paddle) * f.dt;
    const vec2 paddle_half = half_extents(w, ps.paddle);

    bool serve = false;
    if (ps.phase == RunPhase::Serve) {
        ++ps.serve_ticks;
        serve = ps.restarted == 0 && (in.jump_pressed || in.fire_pressed || (paddle && paddle->autopilot && ps.serve_ticks >= kAutoServeTicks));
    }

    Sweep s{f, ps, pw, rules, paddle, paddle_at, paddle_half};
    u32 in_play = 0;
    f.query<Ball&, LocalTransform&>().each([&](Entity e, Ball& b, LocalTransform& t) {
        if (!b.active) return;
        if (!b.launched) {
            t.position.x = paddle_at.x;
            t.position.y = paddle_at.y + paddle_half.y + b.radius + 0.02f;
            if (serve) {
                b.launched = 1;
                const f32 side = (ps.ticks / 7u) % 2u ? kServeSide : -kServeSide;
                b.vel = steady({side, 1.0f}, b.speed);
            }
            ++in_play;
            return;
        }
        if (ps.phase != RunPhase::Play) return;  // frozen once the run is over
        sweep_ball(s, e, b, t);
        if (t.position.y < kill_y) {
            b.active = 0;
            b.launched = 0;
            b.vel = {};
            ++ps.balls_lost;
            if (b.spawned) f.commands().destroy(e);
            return;
        }
        ++in_play;
    });
    if (serve) ps.phase = RunPhase::Play;

    if (ps.phase == RunPhase::Play && ps.bricks_left == 0 && ps.bricks_total > 0) {
        ps.phase = RunPhase::Won;
        ps.ended_at = ps.ticks;
        MCE_LOG_INFO("prism_break: cleared at tick {}: score {}, lives {}, splits {}, balls lost {}", ps.ticks, ps.score,
                     ps.lives, ps.splits, ps.balls_lost);
        if (rules) play(f, rules->win_sound);
        return;
    }
    if (ps.phase == RunPhase::Play && in_play == 0) {
        if (rules) play(f, rules->lose_sound);
        if (ps.lives > 0) --ps.lives;
        if (ps.lives == 0) {
            ps.phase = RunPhase::Lost;
            ps.ended_at = ps.ticks;
            MCE_LOG_INFO("prism_break: game over at tick {}: score {}, bricks left {}/{}", ps.ticks, ps.score,
                         ps.bricks_left, ps.bricks_total);
            return;
        }
        MCE_LOG_INFO("prism_break: ball lost at tick {} ({} lives left, {} bricks left)", ps.ticks, ps.lives, ps.bricks_left);
        // Serve again with the scene's own ball; split balls are gone already.
        bool first = true;
        f.query<Ball&>().each([&](Ball& b) {
            if (!first || b.spawned) return;
            b.active = 1;
            first = false;
        });
        ps.phase = RunPhase::Serve;
        ps.serve_ticks = 0;
    }
}

// ---- FixedUpdate_Post --------------------------------------------------------------------------

void prism_present(Frame& f) {
    const PrismState& ps = f.resource<PrismState>();
    const math::Color prism = rainbow(ps.ticks);
    f.query<const Brick&, kit::Sprite&>().each([&](const Brick& b, kit::Sprite& s) {
        if (b.kind == BrickKind::Steel) s.tint = kSteelTint;
        else if (b.kind == BrickKind::Prism) s.tint = prism;
        else s.tint = kHpTints[math::clamp(b.hp, 1u, 5u) - 1u];
        s.tint.a = b.alive ? 1.0f : 0.0f;
    });
    f.query<const Ball&, kit::Sprite&>().each([&](const Ball& b, kit::Sprite& s) { s.tint.a = b.active ? 1.0f : 0.0f; });

    char line[64];
    f.query<const HudText&, kit::TextLabel&>().each([&](const HudText& h, kit::TextLabel& label) {
        if (h.kind == HudKind::Status) {
            std::snprintf(line, sizeof(line), "SCORE %u   LIVES %u   BRICKS %u/%u", ps.score, ps.lives, ps.bricks_left,
                          ps.bricks_total);
        } else if (ps.phase == RunPhase::Won) {
            const u64 tenths = ps.ended_at * 10u / 60u;  // 60 Hz ticks -> tenths of a second
            std::snprintf(line, sizeof(line), "PRISM BROKEN! %llu.%llu s - SPACE: again",
                          static_cast<unsigned long long>(tenths / 10u), static_cast<unsigned long long>(tenths % 10u));
        } else if (ps.phase == RunPhase::Lost) {
            std::snprintf(line, sizeof(line), "GAME OVER - SPACE: again");
        } else if (ps.phase == RunPhase::Serve) {
            std::snprintf(line, sizeof(line), "SPACE: serve");
        } else {
            line[0] = '\0';
        }
        label.text.assign(StringView(line));
    });
}

}  // namespace

ResultV install(World& world) {
    if (!world.try_resource<World2D>()) return Error(Errc::InvalidArgument, "prism_break.install", "the host installs physics2d first");
    if (!world.try_resource<input::InputState>()) input::install(world);
    // The audio bridge registers it too (idempotent); a host without audio drops the events.
    world.register_event<audio::PlaySfx>(mce::Phase::FixedUpdate_Pre);
    world.register_components<Rules, Paddle, Ball, Brick, HudText>();

    world.add_system(mce::Phase::FixedUpdate_Pre, "prism_setup", &prism_setup);
    world.add_system(mce::Phase::FixedUpdate_Pre, "prism_paddle", &prism_paddle, "prism_setup");
    world.add_system(mce::Phase::FixedUpdate_Pre, "prism_balls", &prism_balls, "prism_paddle");
    world.add_system(mce::Phase::FixedUpdate_Post, "prism_present", &prism_present);
    world.set_resource(PrismState{});
    return ok();
}

}  // namespace prism_break
