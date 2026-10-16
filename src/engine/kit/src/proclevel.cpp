#include "mce/kit/proclevel.h"

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/hash.h"
#include "mce/kit/components.h"
#include "mce/kit/procgen.h"
#include "mce/log.h"
#include "mce/physics/components.h"

namespace mce::kit {

namespace {

namespace pg = procgen;

struct Rect {
    u32 x0, x1;  // cells [x0, x1)
    u32 y0, y1;  // rows [y0, y1), row 0 at the top
};

Result<pg::ProcGrid> generate(const ProcLevel& l) {
    switch (l.kind) {
        case ProcKind::Caves: {
            pg::CavesParams p;
            if (l.param) p.fill_percent = l.param;
            return pg::generate_caves(l.width, l.height, l.seed, p);
        }
        case ProcKind::Walk: {
            pg::WalkParams p;
            if (l.param) p.coverage_percent = l.param;
            return pg::generate_walk(l.width, l.height, l.seed, p);
        }
        case ProcKind::Noise: {
            pg::NoiseParams p;
            if (l.param) p.floor_percent = l.param;
            return pg::generate_noise(l.width, l.height, l.seed, p);
        }
        case ProcKind::Rooms: break;
    }
    return pg::generate_rooms(l.width, l.height, l.seed);
}

// Wall runs per row, then runs of equal extent in consecutive rows merge into one rectangle.
void merge_walls(const pg::ProcGrid& g, Array<Rect>& out) {
    out.clear();
    Array<u32> open;  // indices into `out` of rectangles that end at the previous row
    Array<u32> next;
    for (u32 y = 0; y < g.height; ++y) {
        next.clear();
        u32 x = 0;
        while (x < g.width) {
            if (g.at(x, y) != pg::kWall) {
                ++x;
                continue;
            }
            u32 e = x;
            while (e < g.width && g.at(e, y) == pg::kWall) ++e;
            bool merged = false;
            for (u32 i : open) {
                Rect& r = out[i];
                if (r.x0 == x && r.x1 == e && r.y1 == y) {
                    r.y1 = y + 1;
                    next.push_back(i);
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                out.push_back(Rect{x, e, y, y + 1});
                next.push_back(static_cast<u32>(out.size() - 1));
            }
            x = e;
        }
        open.clear();
        for (u32 i : next) open.push_back(i);
    }
}

// A shortest 4-neighbour walk over floor from start to exit: the cells after the start up to and including the exit.
// Breadth-first with a fixed neighbour order, so the same grid always gives the same path.
void shortest_path(const pg::ProcGrid& g, Array<pg::Cell>& out) {
    out.clear();
    const usize n = static_cast<usize>(g.width) * g.height;
    Array<i32> from;  // index of the cell we came from; -2 unseen, -1 the start
    from.resize(n);
    for (i32& f : from) f = -2;
    Array<u32> queue;
    const auto idx = [&](i32 x, i32 y) { return static_cast<u32>(y) * g.width + static_cast<u32>(x); };
    const u32 s = idx(g.start.x, g.start.y), e = idx(g.exit.x, g.exit.y);
    from[s] = -1;
    queue.push_back(s);
    const i32 dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (usize head = 0; head < queue.size() && from[e] == -2; ++head) {
        const u32 i = queue[head];
        const i32 x = static_cast<i32>(i % g.width), y = static_cast<i32>(i / g.width);
        for (u32 d = 0; d < 4; ++d) {
            const i32 nx = x + dx[d], ny = y + dy[d];
            if (nx < 0 || ny < 0 || nx >= static_cast<i32>(g.width) || ny >= static_cast<i32>(g.height)) continue;
            const u32 j = idx(nx, ny);
            if (g.at(static_cast<u32>(nx), static_cast<u32>(ny)) != pg::kFloor || from[j] != -2) continue;
            from[j] = static_cast<i32>(i);
            queue.push_back(j);
        }
    }
    if (from[e] == -2) return;  // the generators guarantee a walk; no path markers if one ever fails
    for (i32 i = static_cast<i32>(e); i != static_cast<i32>(s); i = from[static_cast<u32>(i)])
        out.push_back(pg::Cell{static_cast<i32>(static_cast<u32>(i) % g.width), static_cast<i32>(static_cast<u32>(i) / g.width)});
    for (usize a = 0, b = out.size(); a + 1 < b; ++a, --b) {  // reverse: start side first
        const pg::Cell tmp = out[a];
        out[a] = out[b - 1];
        out[b - 1] = tmp;
    }
}

u64 level_hash(const ProcLevel& l, math::vec3 pos) {
    Hasher64 h;
    h.u32v(static_cast<u32>(l.kind));
    h.u32v(l.path_markers ? 1 : 0);
    h.u32v(l.width);
    h.u32v(l.height);
    h.u32v(l.seed);
    h.u32v(l.param);
    h.f32v(l.tile_size);
    h.f32v(pos.x);
    h.f32v(pos.y);
    h.f32v(pos.z);
    h.f32v(l.wall_color.r);
    h.f32v(l.wall_color.g);
    h.f32v(l.wall_color.b);
    h.f32v(l.wall_color.a);
    h.f32v(l.floor_color.r);
    h.f32v(l.floor_color.g);
    h.f32v(l.floor_color.b);
    h.f32v(l.floor_color.a);
    h.u32v(static_cast<u32>(static_cast<i32>(l.layer)));
    const u64 v = h.value();
    return v ? v : 1;  // 0 means "nothing built"
}

void destroy_owned(World& world, Frame* f, Entity owner) {
    Array<Entity> doomed;
    world.query<const ProcLevelOwned>().each([&](Entity e, const ProcLevelOwned& o) {
        if (o.owner == owner) doomed.push_back(e);
    });
    for (Entity e : doomed) {
        if (f) f->commands().destroy(e);
        else if (world.in_phase()) world.commands().destroy(e);
        else world.destroy(e);
    }
}

void build(Frame& f, Entity owner, const ProcLevel& l, math::vec3 pos, const pg::ProcGrid& g) {
    const f32 ts = l.tile_size;
    const auto cell_x = [&](f32 x) { return pos.x + x * ts; };
    const auto cell_y = [&](f32 y) { return pos.y + (static_cast<f32>(g.height) - y) * ts; };  // grid row 0 is the top
    Commands& cmd = f.commands();

    // The floor: one quad under everything.
    {
        Sprite s;
        s.tint = l.floor_color;
        s.size = {static_cast<f32>(g.width) * ts, static_cast<f32>(g.height) * ts};
        s.order = 0;
        s.layer = l.layer;
        LocalTransform t;
        t.position = {cell_x(static_cast<f32>(g.width) * 0.5f), cell_y(static_cast<f32>(g.height) * 0.5f), pos.z};
        cmd.spawn(t, s, ProcLevelOwned{owner});
    }

    Array<Rect> rects;
    merge_walls(g, rects);
    for (const Rect& r : rects) {
        const f32 w = static_cast<f32>(r.x1 - r.x0) * ts, h = static_cast<f32>(r.y1 - r.y0) * ts;
        LocalTransform t;
        t.position = {cell_x(static_cast<f32>(r.x0 + r.x1) * 0.5f), cell_y(static_cast<f32>(r.y0 + r.y1) * 0.5f), pos.z};
        Sprite s;
        s.tint = l.wall_color;
        s.size = {w, h};
        s.order = 1;
        s.layer = l.layer;
        physics::RigidBody2D rb;
        rb.type = physics::BodyType::Static;
        physics::Collider2D col;
        col.shape = physics::ColliderShape::box({w * 0.5f, h * 0.5f});
        col.density = 0.0f;
        cmd.spawn(t, s, rb, col, ProcLevelOwned{owner});
    }

    const pg::Cell marks[2] = {g.start, g.exit};
    for (u32 i = 0; i < 2; ++i) {
        LocalTransform t;
        t.position = {cell_x(static_cast<f32>(marks[i].x) + 0.5f), cell_y(static_cast<f32>(marks[i].y) + 0.5f), pos.z};
        ProcMarker m;
        m.kind = i == 0 ? ProcMarkerKind::Start : ProcMarkerKind::Exit;
        cmd.spawn(t, m, ProcLevelOwned{owner});
    }

    if (!l.path_markers) return;
    Array<pg::Cell> path;
    shortest_path(g, path);
    for (usize i = 0; i < path.size(); ++i) {
        LocalTransform t;
        t.position = {cell_x(static_cast<f32>(path[i].x) + 0.5f), cell_y(static_cast<f32>(path[i].y) + 0.5f), pos.z};
        ProcMarker m;
        m.kind = ProcMarkerKind::Path;
        m.index = static_cast<u32>(i + 1);
        cmd.spawn(t, m, ProcLevelOwned{owner});
    }
}

void kit_proclevel(Frame& f) {
    World& world = f.world();
    Array<Entity> levels;
    world.query<const ProcLevel>().each([&](Entity e, const ProcLevel&) { levels.push_back(e); });
    for (Entity e : levels) {
        ProcLevel* l = world.try_get_mut<ProcLevel>(e);
        if (!l || !l->enabled) continue;
        if (l->reroll) {  // read and cleared here, in a fixed phase and order
            l->reroll = false;
            l->seed += 1;
        }
        math::vec3 pos{};
        if (const LocalTransform* t = world.try_get<LocalTransform>(e)) pos = t->position;
        const u64 h = level_hash(*l, pos);
        if (h == l->built) continue;
        l->built = h;
        destroy_owned(world, &f, e);
        Result<pg::ProcGrid> g = generate(*l);
        if (!g.has_value()) {
            MCE_LOG_WARN("kit: ProcLevel {}x{} seed {} cannot be generated ({}); nothing is built", l->width, l->height, l->seed, g.error().stage);
            continue;
        }
        build(f, e, *l, pos, g.value());
    }
}

// The level's entities go with it.
void on_destroy(World& world, Entity e) {
    if (!world.has<ProcLevel>(e)) return;
    destroy_owned(world, nullptr, e);
}

}  // namespace

void install_proclevel(World& world) {
    if (world.find_component(kTypeHash<ProcLevel>) != ecs::kInvalidComponent) return;
    world.register_components<ProcLevel, ProcLevelOwned, ProcMarker, LocalTransform, Sprite, physics::RigidBody2D, physics::Collider2D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_proclevel", &kit_proclevel);
    world.add_destroy_hook(&on_destroy);
}

NavGrid proclevel_nav_grid(const ProcLevel& level, math::vec2 origin) { return NavGrid(level.width, level.height, level.tile_size, origin, 1); }

u32 proclevel_max_wall_rects(u32 width, u32 height) {
    // A rectangle needs a run start on its first row: at most ceil(width / 2) runs per row.
    return ((width + 1) / 2) * height;
}

}  // namespace mce::kit
