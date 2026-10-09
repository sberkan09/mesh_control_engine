// Solstice Reach gameplay: see solstice_reach.h for the system map.
#include "solstice_reach.h"

#include <algorithm>
#include <cmath>

#include "mce/ecs/save.h"
#include "mce/hash.h"
#include "terrain.h"

namespace solstice_reach {

using namespace mce;
using namespace mce::math;
using physics::ControllerParams3D;
using physics::MoveResult3D;
using physics::World3D;

namespace {

constexpr u8 kLayerWorld = 0, kLayerKeeper = 1;
constexpr f32 kGravity = 22.0f;
constexpr f32 kWalk = 4.5f, kSprint = 7.0f, kJump = 7.4f;
constexpr f32 kBoomLength = 3.8f, kBoomRadius = 0.25f;
constexpr vec3 kHeadOffset{0.0f, 1.5f, 0.0f};
constexpr f32 kKillY = -4.0f;

ControllerParams3D keeper_params() {
    ControllerParams3D p;
    p.radius = 0.35f;
    p.half_height = 0.5f;
    p.step_height = 0.42f;
    p.max_slope_deg = 52.0f;
    p.layer = kLayerKeeper;
    p.mask = 1u << kLayerWorld;
    return p;
}
f32 standing_height() {
    const ControllerParams3D p = keeper_params();
    return p.half_height + p.radius + p.padding;
}

Entity static_id(u32 i) { return Entity{0x200000u + i, 1}; }
f32 yaw_of(vec3 dir) { return std::atan2(dir.x, dir.z); }
f32 wrap_angle(f32 a) {
    while (a > kPi) a -= kTau;
    while (a < -kPi) a += kTau;
    return a;
}
f32 flat_dist(vec3 a, vec3 b) {
    const f32 dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

// A rotation taking +Y onto `dir` (unit): the dish's bowl opens toward the light.
quat rotation_up_to(vec3 dir) {
    const vec3 up{0.0f, 1.0f, 0.0f};
    const f32 d = clamp(dot(up, dir), -1.0f, 1.0f);
    if (d > 0.9999f) return quat::identity();
    if (d < -0.9999f) return quat_from_axis_angle({1.0f, 0.0f, 0.0f}, kPi);
    const vec3 axis = normalize(cross(up, dir));
    return quat_from_axis_angle(axis, std::acos(d));
}

// ---- Startup --------------------------------------------------------------------------------------

void build_terrain_collider(World3D& pw) {
    // 1 m grid over the island; cells entirely under the sea bed are skipped (nobody walks there).
    constexpr i32 kCells = 200;
    constexpr f32 kStep = 2.0f * static_cast<f32>(terrain::kHalf) / kCells;
    Array<vec3> verts;
    Array<u32> indices;
    verts.reserve((kCells + 1) * (kCells + 1));
    for (i32 j = 0; j <= kCells; ++j)
        for (i32 i = 0; i <= kCells; ++i) {
            const f32 x = -static_cast<f32>(terrain::kHalf) + kStep * static_cast<f32>(i);
            const f32 z = -static_cast<f32>(terrain::kHalf) + kStep * static_cast<f32>(j);
            verts.push_back({x, terrain::height_f(x, z), z});
        }
    for (i32 j = 0; j < kCells; ++j)
        for (i32 i = 0; i < kCells; ++i) {
            const u32 a = static_cast<u32>(j * (kCells + 1) + i), b = a + static_cast<u32>(kCells + 1);
            if (verts[a].y < -3.0f && verts[b].y < -3.0f && verts[a + 1].y < -3.0f && verts[b + 1].y < -3.0f) continue;
            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(a + 1);
            indices.push_back(a + 1);
            indices.push_back(b);
            indices.push_back(b + 1);
        }
    physics::BodyDesc3D d;
    d.shape = physics::Shape3D::mesh(verts.span(), indices.span());
    d.layer = kLayerWorld;
    d.friction = 0.9f;
    MCE_VERIFY(pw.create_body(static_id(0), d));
    // The sea: a flat floor at the sea bed so a fall never ends below the mesh.
    physics::BodyDesc3D sea;
    sea.shape = physics::Shape3D::box({500.0f, 0.5f, 500.0f});
    sea.position = {0.0f, static_cast<f32>(terrain::kSeaFloor) - 0.5f, 0.0f};
    sea.layer = kLayerWorld;
    MCE_VERIFY(pw.create_body(static_id(1), sea));
}

void build_box_colliders(Frame& f, World3D& pw) {
    f.query<const BoxCollider3D&, const LocalTransform&>().each([&](Entity e, const BoxCollider3D& c, const LocalTransform& t) {
        physics::BodyDesc3D d;
        d.shape = physics::Shape3D::box({c.half.x * t.scale.x, c.half.y * t.scale.y, c.half.z * t.scale.z});
        d.rotation = t.rotation * quat_from_axis_angle({1.0f, 0.0f, 0.0f}, radians(c.pitch_deg));
        d.position = t.position + rotate(t.rotation, {c.offset.x * t.scale.x, c.offset.y * t.scale.y, c.offset.z * t.scale.z});
        d.layer = kLayerWorld;
        if (!pw.create_body(e, d)) MCE_LOG_WARN("solstice_reach: box collider on entity {} not created", e.index);
    });
}

u32 mesh_slot(ScatterCache& cache, const asset::Guid& g) {
    if (g.is_null()) return ~0u;
    for (u32 i = 0; i < cache.meshes.size(); ++i)
        if (cache.meshes[i] == g) return i;
    cache.meshes.push_back(g);
    return static_cast<u32>(cache.meshes.size() - 1);
}

void build_scatter(Frame& f, ScatterCache& cache) {
    cache.meshes.clear();
    cache.instances.clear();
    cache.cells.clear();
    f.query<Scatter&, const LocalTransform&>().each([&](Entity e, Scatter& s, const LocalTransform& t) {
        const u32 slots[3] = {mesh_slot(cache, s.mesh.guid), mesh_slot(cache, s.mesh_b.guid), mesh_slot(cache, s.mesh_c.guid)};
        u32 variants = 0;
        for (u32 k : slots) variants += k != ~0u ? 1u : 0u;
        s.count = 0;
        if (variants == 0 || s.density <= 0.0f) return;
        const f32 area = 4.0f * s.half_extent.x * s.half_extent.y;
        const u32 wanted = static_cast<u32>(std::min(area * s.density, 20000.0f));
        const f32 min_ny = std::cos(radians(s.max_slope_deg));
        const u32 base_seed = s.seed * 7919u + e.index * 131u;
        for (u32 k = 0; k < wanted; ++k) {
            const f32 u = static_cast<f32>(terrain::h01(static_cast<std::int64_t>(k), 1, base_seed));
            const f32 v = static_cast<f32>(terrain::h01(static_cast<std::int64_t>(k), 2, base_seed));
            const f32 x = t.position.x + (u * 2.0f - 1.0f) * s.half_extent.x;
            const f32 z = t.position.z + (v * 2.0f - 1.0f) * s.half_extent.y;
            const f32 y = terrain::height_f(x, z);
            if (y < s.min_height || y > s.max_height) continue;
            f32 n[3];
            terrain::normal(x, z, n);
            if (n[1] < min_ny) continue;
            ScatterInstance inst;
            inst.pos = {x, y, z};
            inst.scale = lerp(s.min_scale, s.max_scale, static_cast<f32>(terrain::h01(static_cast<std::int64_t>(k), 3, base_seed)));
            const f32 yaw = static_cast<f32>(terrain::h01(static_cast<std::int64_t>(k), 4, base_seed)) * kTau;
            inst.rot = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, yaw);
            if (s.align_to_ground) inst.rot = rotation_up_to({n[0], n[1], n[2]}) * inst.rot;
            u32 pick = static_cast<u32>(static_cast<f32>(terrain::h01(static_cast<std::int64_t>(k), 5, base_seed)) * static_cast<f32>(variants));
            if (pick >= variants) pick = variants - 1;
            u32 slot = ~0u;
            for (u32 c = 0, seen = 0; c < 3; ++c) {
                if (slots[c] == ~0u) continue;
                if (seen++ == pick) slot = slots[c];
            }
            inst.mesh = slot;
            cache.instances.push_back(inst);
            ++s.count;
        }
    });
    // Cells of 16 m: sort instances by cell, then record ranges with a bounding sphere each.
    constexpr f32 kCell = 16.0f;
    auto cell_key = [&](const ScatterInstance& i) {
        const i32 cx = static_cast<i32>(std::floor(i.pos.x / kCell)) + 64, cz = static_cast<i32>(std::floor(i.pos.z / kCell)) + 64;
        return static_cast<u32>(cz * 128 + cx);
    };
    std::stable_sort(cache.instances.begin(), cache.instances.end(),
                     [&](const ScatterInstance& a, const ScatterInstance& b) { return cell_key(a) < cell_key(b); });
    for (u32 i = 0; i < cache.instances.size();) {
        const u32 key = cell_key(cache.instances[i]);
        u32 j = i;
        vec3 lo = cache.instances[i].pos, hi = lo;
        while (j < cache.instances.size() && cell_key(cache.instances[j]) == key) {
            const vec3 p = cache.instances[j].pos;
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            ++j;
        }
        ScatterCell c;
        c.center = (lo + hi) * 0.5f + vec3{0.0f, 2.0f, 0.0f};
        c.radius = length(hi - lo) * 0.5f + 6.0f;  // instances are at most ~6 m tall/wide
        c.first = i;
        c.count = j - i;
        cache.cells.push_back(c);
        i = j;
    }
}

void sr_startup(Frame& f) {
    ReachState& gs = f.resource<ReachState>();
    World3D& pw = f.resource<Physics>().world;
    build_terrain_collider(pw);
    build_box_colliders(f, pw);
    build_scatter(f, f.resource<ScatterCache>());

    // The Keeper: the character at the spawn, the Keeper component on the spawn entity.
    gs.keeper = Entity::null;
    f.query<const KeeperSpawn&, const LocalTransform&>().each([&](Entity e, const KeeperSpawn& s, const LocalTransform& t) {
        if (!gs.keeper.is_null()) return;
        gs.keeper = e;
        Keeper k;
        if (const Keeper* saved = f.world().try_get<Keeper>(e)) k = *saved;  // a save restored it before the first tick
        else {
            k.pos = {t.position.x, terrain::height_f(t.position.x, t.position.z) + standing_height(), t.position.z};
            k.yaw = s.yaw;
        }
        k.prev_pos = k.pos;
        k.prev_yaw = k.yaw;
        MCE_VERIFY(pw.create_character(e, k.pos, keeper_params()));
        f.commands().set(e, k);
        gs.cam_yaw = gs.prev_cam_yaw = wrap_angle(k.yaw + kPi);
    });
    if (gs.keeper.is_null()) MCE_LOG_WARN("solstice_reach: the scene has no KeeperSpawn");

    gs.progress = Entity::null;
    f.query<const Progress&>().each([&](Entity e, const Progress&) {
        if (gs.progress.is_null()) gs.progress = e;
    });
    if (gs.progress.is_null()) gs.progress = f.commands().spawn(Progress{}, SaveKey{9001});
    gs.shards_total = static_cast<u32>(f.query<const Sunshard&>().count());
    gs.started = 1;
}

// ---- flow -------------------------------------------------------------------------------------------

Progress* progress(Frame& f, ReachState& gs) { return f.world().try_get_mut<Progress>(gs.progress); }

void sr_flow(Frame& f) {
    ReachState& gs = f.resource<ReachState>();
    const input::InputState& in = f.resource<input::InputState>();
    Progress* p = progress(f, gs);
    if (!p) return;

    switch (gs.mode) {
        case Mode::Title:
            if (in.confirm_pressed || in.fire_pressed) {
                gs.mode = Mode::Playing;
                gs.journal_line = p->mirrors;
                gs.journal_timer = 6.0f;
                ++gs.serial;
            }
            return;
        case Mode::Paused:
            if (in.pause_pressed || in.cancel_pressed) gs.mode = Mode::Playing;
            return;
        case Mode::Playing:
        case Mode::Dawn:
            if (in.pause_pressed && !gs.autopilot) {
                gs.mode = Mode::Paused;
                return;
            }
            break;
        case Mode::Finale:
            gs.finale_t += f.dt;
            if (gs.finale_t >= kFinaleSeconds) {
                gs.mode = Mode::Dawn;
                p->finale_done = 1;
                p->target_hour = kHourKeys[8];
                gs.journal_line = 8;
                gs.journal_timer = 8.0f;
            }
            break;
    }
    ++gs.ticks;
    // The hour drifts to its target at one hour per eight seconds.
    const f32 diff = p->target_hour - p->hour;
    const f32 step = kHoursPerSecond * f.dt * (gs.mode == Mode::Finale ? 0.8f : 1.0f);
    if (std::fabs(diff) <= step) p->hour = p->target_hour;
    else p->hour += diff > 0 ? step : -step;
    if (gs.journal_timer > 0.0f) gs.journal_timer = std::max(0.0f, gs.journal_timer - f.dt);
    if (gs.flare > 0.0f) gs.flare = std::max(0.0f, gs.flare - f.dt * 0.8f);
    gs.chime = 0.0f;
}

// ---- the Keeper ----------------------------------------------------------------------------------

struct Target {
    vec3 pos;
    bool valid = false;
    bool mirror = false;
};

Target bot_target(Frame& f, const ReachState& gs) {
    Target best;
    u32 best_order = ~0u;
    f.query<const Waypoint&, const LocalTransform&>().each([&](const Waypoint& w, const LocalTransform& t) {
        if (w.order < gs.waypoint_cursor || w.order >= best_order) return;
        best_order = w.order;
        best.pos = t.position;
        best.mirror = w.mirror != 0;
        best.valid = true;
    });
    return best;
}

void align_mirror(ReachState& gs, Progress& p, Heliostat& h) {
    h.aligned = 1.0f;
    h.turn = 1.0f;
    h.turning = 0;
    p.mirrors = std::min(p.mirrors + 1, kMirrors);
    p.target_hour = kHourKeys[std::min(p.mirrors, 7u)];
    gs.journal_line = p.mirrors;
    gs.journal_timer = 7.0f;
    gs.flare = 1.0f;
    gs.save_requested = 1;
    if (p.mirrors >= kMirrors && !p.finale_done) {
        gs.mode = Mode::Finale;
        gs.finale_t = 0.0f;
    }
}

void sr_keeper(Frame& f) {
    ReachState& gs = f.resource<ReachState>();
    if (gs.mode != Mode::Playing && gs.mode != Mode::Dawn) return;
    Keeper* k = f.world().try_get_mut<Keeper>(gs.keeper);
    Progress* p = progress(f, gs);
    if (!k || !p) return;
    World3D& pw = f.resource<Physics>().world;
    const input::InputState& in = f.resource<input::InputState>();
    const f32 dt = f.dt;

    k->prev_pos = k->pos;
    k->prev_yaw = k->yaw;
    gs.prev_cam_yaw = gs.cam_yaw;
    gs.prev_boom = gs.boom;

    vec3 dir;
    bool want_jump = false, sprint = false, interact = false;
    Target bot;
    if (gs.autopilot) {
        bot = bot_target(f, gs);
        if (bot.valid) {
            const vec3 to = bot.pos - k->pos;
            const f32 d = flat_dist(bot.pos, k->pos);
            if (d > 1.1f) dir = normalize(vec3{to.x, 0.0f, to.z});
            else if (!bot.mirror) ++gs.waypoint_cursor;
            else interact = true;
            sprint = d > 12.0f;
            // Something in the way (a wall, a boulder, a step too tall): hop once the walk has
            // stalled for a third of a second. Slopes alone never trigger it.
            if (d > 1.1f && k->grounded) {
                gs.bot_stuck = k->speed < 0.6f ? gs.bot_stuck + dt : 0.0f;
                if (gs.bot_stuck > 0.35f) {
                    want_jump = true;
                    gs.bot_stuck = 0.0f;
                }
            }
        }
        gs.cam_yaw = wrap_angle(gs.cam_yaw + wrap_angle(k->yaw + kPi - gs.cam_yaw) * std::min(1.0f, 2.5f * dt));
        gs.cam_pitch = 0.28f;
    } else {
        gs.cam_yaw = wrap_angle(gs.cam_yaw - in.look.x * 0.0045f);
        gs.cam_pitch = clamp(gs.cam_pitch + in.look.y * 0.003f, -0.35f, 1.15f);
        const vec3 cam_fwd{-std::sin(gs.cam_yaw), 0.0f, -std::cos(gs.cam_yaw)};
        const vec3 cam_right{std::cos(gs.cam_yaw), 0.0f, -std::sin(gs.cam_yaw)};
        dir = cam_fwd * clamp(in.move.y, -1.0f, 1.0f) + cam_right * clamp(in.move.x, -1.0f, 1.0f);
        const f32 len = length(dir);
        if (len > 1.0f) dir = dir * (1.0f / len);
        want_jump = in.jump_pressed;
        sprint = in.trigger_l > 0.5f || (in.has_action(MCE_SID("Sprint")) && in.held(MCE_SID("Sprint")));
        interact = in.fire_pressed || (in.has_action(MCE_SID("Interact")) && in.held(MCE_SID("Interact")));
        if (in.has_action(MCE_SID("Lantern")) && in.pressed(MCE_SID("Lantern"))) k->lantern_on = k->lantern_on ? 0u : 1u;
    }

    // Gravity, jumps (with coyote time), the move.
    if (k->grounded) k->coyote = 0.12f;
    else k->coyote = std::max(0.0f, k->coyote - dt);
    if (want_jump && (k->grounded || k->coyote > 0.0f) && k->vy <= 0.5f) {
        k->vy = kJump;
        k->coyote = 0.0f;
        k->grounded = 0;
    }
    k->vy -= kGravity * dt;
    k->vy = std::max(k->vy, -30.0f);
    const f32 speed = sprint ? kSprint : kWalk;
    const vec3 horizontal = dir * speed;
    const MoveResult3D r = pw.move_character(gs.keeper, {horizontal.x * dt, k->vy * dt, horizontal.z * dt}, keeper_params());
    k->pos = r.position;
    k->grounded = (r.flags & physics::ControllerFlags3D::Ground) ? 1u : 0u;
    if (k->grounded && k->vy < 0.0f) k->vy = -0.5f;
    if (r.hit_ceiling && k->vy > 0.0f) k->vy = 0.0f;
    k->speed = length(vec3{k->pos.x - k->prev_pos.x, 0.0f, k->pos.z - k->prev_pos.z}) / std::max(dt, 1e-4f);  // what the move achieved
    k->sprinting = sprint ? 1u : 0u;
    if (length(dir) > 0.05f) {
        const f32 target = yaw_of(dir);
        k->yaw = wrap_angle(k->yaw + wrap_angle(target - k->yaw) * std::min(1.0f, 14.0f * dt));
    }
    k->anim_time += dt * (k->speed > 0.3f ? std::max(0.6f, k->speed / kWalk) : 1.0f);
    if (k->pos.y < kKillY) {  // fell into the sea: back to the shore
        vec3 at{0.0f, 0.0f, 70.0f};
        f.query<const KeeperSpawn&, const LocalTransform&>().each([&](const KeeperSpawn&, const LocalTransform& t) { at = t.position; });
        k->pos = k->prev_pos = {at.x, terrain::height_f(at.x, at.z) + standing_height(), at.z};
        k->vy = 0.0f;
        pw.teleport(gs.keeper, k->pos, quat::identity());
    }

    // Heliostats: on a plate, holding Interact turns the dish.
    gs.near_mirror = 0;
    f.query<Heliostat&, const LocalTransform&>().each([&](Heliostat& h, const LocalTransform& t) {
        h.turning = 0;
        if (h.aligned >= 1.0f) return;
        const f32 d = flat_dist(t.position, k->pos);
        if (d > kInteractRange || std::fabs(k->pos.y - t.position.y) > 3.0f) return;
        gs.near_mirror = h.index + 1;
        if (!interact) return;
        h.turning = 1;
        h.turn = std::min(1.0f, h.turn + dt / kAlignSeconds);
        if (h.turn >= 1.0f) {
            align_mirror(gs, *p, h);
            if (gs.autopilot) ++gs.waypoint_cursor;
        }
    });
    // Standing on a mirror waypoint with nothing left to align there (a save already did): move on.
    if (gs.autopilot && bot.valid && bot.mirror && interact && gs.near_mirror == 0) ++gs.waypoint_cursor;

    // Sunshards: walk through one to take it.
    f.query<Sunshard&, const LocalTransform&>().each([&](Sunshard& s, const LocalTransform& t) {
        if (s.taken) return;
        const vec3 d = t.position + vec3{0.0f, 0.5f, 0.0f} - k->pos;
        if (length_sq(d) > 1.6f * 1.6f) return;
        s.taken = 1;
        ++p->shards;
        gs.chime = 1.0f;
    });
}

// ---- physics and the camera boom -------------------------------------------------------------------

void sr_physics(Frame& f) {
    ReachState& gs = f.resource<ReachState>();
    World3D& pw = f.resource<Physics>().world;
    if (gs.mode == Mode::Playing || gs.mode == Mode::Dawn || gs.mode == Mode::Finale) pw.step(f.dt);
    const Keeper* k = f.world().try_get<Keeper>(gs.keeper);
    if (!k) return;
    const vec3 pivot = k->pos + kHeadOffset;
    const f32 cp = std::cos(gs.cam_pitch);
    const vec3 back{std::sin(gs.cam_yaw) * cp, std::sin(gs.cam_pitch), std::cos(gs.cam_yaw) * cp};
    const vec3 want = pivot + back * kBoomLength;
    gs.boom = kBoomLength;
    if (std::optional<physics::ShapeHit3D> hit = pw.camera_sweep(pivot, want, kBoomRadius)) gs.boom = length(hit->position - pivot);
}

// ---- dishes ---------------------------------------------------------------------------------------

void sr_mirrors(Frame& f) {
    f.query<const Heliostat&, const LocalTransform&>().each([&](const Heliostat& h, const LocalTransform& base) {
        LocalTransform* dish = f.world().try_get_mut<LocalTransform>(h.dish);
        if (!dish) return;
        // Rest: the bowl tilted 35 degrees away from its target; aligned: the bowl bisects the
        // sun (up and behind) and the beam direction, like a real heliostat.
        vec3 target = kVaultOculus;
        if (const LocalTransform* n = f.world().try_get<LocalTransform>(h.next)) target = n->position + vec3{0.0f, 1.9f, 0.0f};
        const vec3 from = base.position + h.beam_offset;
        const vec3 beam = normalize(target - from);
        const vec3 sun = normalize(vec3{0.6f, 0.75f, 0.3f});
        const vec3 aligned = normalize(beam + sun);
        const vec3 rest = normalize(vec3{-beam.x, 2.5f, -beam.z});
        const f32 t = h.turn * h.turn * (3.0f - 2.0f * h.turn);
        dish->rotation = rotation_up_to(normalize(lerp(rest, aligned, t)));
    });
}

}  // namespace

ResultV install(World& world) {
    if (!world.try_resource<input::InputState>()) input::install(world);  // play-in-editor has no input
    world.register_components<MeshRef, Scatter, Light3D, BoxCollider3D, Animated, Heliostat, Sunshard, KeeperSpawn, Keeper, Waypoint,
                              GullPath, Progress>();
    world.add_system(Phase::Startup, "sr_startup", &sr_startup);
    world.add_system(Phase::FixedUpdate_Pre, "sr_flow", &sr_flow);
    world.add_system(Phase::FixedUpdate_Pre, "sr_keeper", &sr_keeper, "sr_flow");
    world.add_system(Phase::FixedUpdate_Physics, "sr_physics", &sr_physics);
    world.add_system(Phase::FixedUpdate_Post, "sr_mirrors", &sr_mirrors);
    physics::PhysicsConfig3D cfg;
    cfg.max_bodies = 2048;
    world.set_resource(Physics{World3D(cfg)});
    world.set_resource(ScatterCache{});
    ReachState gs;
    if (const ReachState* given = world.try_resource<ReachState>()) gs = *given;  // the host may preset autopilot / Title
    world.set_resource(gs);
    return ok();
}

u64 hash_game(World& world, u64 seed) {
    const ReachState& gs = world.resource<ReachState>();
    Hasher64 h(world.state_hash(seed));
    h.u64v(world.resource<Physics>().world.hash_state(seed));
    h.u32v(static_cast<u32>(gs.mode));
    h.u64v(gs.ticks);
    h.u32v(gs.waypoint_cursor);
    h.f32v(gs.cam_yaw);
    h.f32v(gs.boom);
    return h.value();
}

f32 current_hour(World& world) {
    const ReachState& gs = world.resource<ReachState>();
    if (const Progress* p = world.try_get<Progress>(gs.progress)) return p->hour;
    return kHourKeys[0];
}


void reset_run(World& world) {
    ReachState& gs = world.resource<ReachState>();
    if (Progress* p = world.try_get_mut<Progress>(gs.progress)) *p = Progress{};
    world.query<Heliostat&>().each([](Heliostat& h) {
        h.aligned = 0.0f;
        h.turn = 0.0f;
        h.turning = 0;
    });
    world.query<Sunshard&>().each([](Sunshard& s) { s.taken = 0; });
    if (Keeper* k = world.try_get_mut<Keeper>(gs.keeper)) {
        vec3 at{0.0f, 0.0f, 70.0f};
        f32 yaw = 0.0f;
        world.query<const KeeperSpawn&, const LocalTransform&>().each([&](const KeeperSpawn& s, const LocalTransform& t) {
            at = t.position;
            yaw = s.yaw;
        });
        k->pos = k->prev_pos = {at.x, terrain::height_f(at.x, at.z) + standing_height(), at.z};
        k->yaw = k->prev_yaw = yaw;
        k->vy = 0.0f;
        k->lantern_on = 0;
        k->anim_time = 0.0f;
        world.resource<Physics>().world.teleport(gs.keeper, k->pos, quat::identity());
        gs.cam_yaw = gs.prev_cam_yaw = wrap_angle(yaw + kPi);
    }
    gs.mode = Mode::Playing;
    gs.journal_timer = 6.0f;
    gs.journal_line = 0;
    gs.flare = 0.0f;
    gs.finale_t = 0.0f;
    gs.waypoint_cursor = 0;
    gs.save_requested = 0;
}

}  // namespace solstice_reach
