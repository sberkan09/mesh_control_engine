#include "mce/kit/pathfinding.h"

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/physics/components.h"
#include "mce/physics/world2d.h"

namespace mce::kit {

namespace {

// Expansion order is part of the determinism contract: orthogonal first, then diagonal.
constexpr i32 kDx[8] = {1, -1, 0, 0, 1, -1, 1, -1};
constexpr i32 kDy[8] = {0, 0, 1, -1, 1, 1, -1, -1};
constexpr u64 kStraight = 10;
constexpr u64 kDiagonal = 14;

i32 iabs(i32 v) { return v < 0 ? -v : v; }

u64 heuristic(GridCell a, GridCell b, bool diagonal) {
    const u64 dx = u64(iabs(a.x - b.x));
    const u64 dy = u64(iabs(a.y - b.y));
    if (!diagonal) return kStraight * (dx + dy);  // Manhattan: tight for 4-neighbour moves
    const u64 lo = dx < dy ? dx : dy;
    const u64 hi = dx < dy ? dy : dx;
    return kStraight * hi + (kDiagonal - kStraight) * lo;  // octile
}

// Float -> cell coordinate without overflow on far-away points.
i32 to_cell(f32 v) {
    const f32 c = v < -1.0e9f ? -1.0e9f : (v > 1.0e9f ? 1.0e9f : v);
    return static_cast<i32>(std::floor(c));
}

math::vec2 rotate2(math::vec2 v, f32 angle) {
    const f32 c = std::cos(angle), s = std::sin(angle);
    return {c * v.x - s * v.y, s * v.x + c * v.y};
}

void grow(math::aabb2& box, math::vec2 p) {
    box.min.x = p.x < box.min.x ? p.x : box.min.x;
    box.min.y = p.y < box.min.y ? p.y : box.min.y;
    box.max.x = p.x > box.max.x ? p.x : box.max.x;
    box.max.y = p.y > box.max.y ? p.y : box.max.y;
}

// The world AABB of a collider on a body at `pos` turned by `angle`.
math::aabb2 shape_aabb(const physics::ColliderShape& s, math::vec2 pos, f32 angle) {
    auto to_world = [&](math::vec2 local) { return pos + rotate2(local, angle); };
    math::aabb2 box{{1e30f, 1e30f}, {-1e30f, -1e30f}};
    switch (s.type) {
    case physics::ShapeType::Box:
        for (i32 i = 0; i < 4; ++i) {
            const math::vec2 corner{(i & 1) ? s.half.x : -s.half.x, (i & 2) ? s.half.y : -s.half.y};
            grow(box, to_world(s.offset + rotate2(corner, s.rotation)));
        }
        break;
    case physics::ShapeType::Circle: {
        const math::vec2 c = to_world(s.offset);
        box = {{c.x - s.radius, c.y - s.radius}, {c.x + s.radius, c.y + s.radius}};
        break;
    }
    case physics::ShapeType::Convex:
        for (u32 i = 0; i < s.vertex_count && i < physics::kMaxPolygonVertices; ++i) grow(box, to_world(s.vertices[i]));
        if (s.vertex_count == 0) box = {pos, pos};
        break;
    }
    return box;
}

}  // namespace

// ------------------------------------------------------------------ grid

NavGrid::NavGrid(u32 width, u32 height, f32 cell_size, math::vec2 origin, u8 cost)
    : width_(width), height_(height), cell_size_(cell_size > 0.0f ? cell_size : 1.0f), origin_(origin) {
    costs_.resize(usize(width) * height, cost);
}

void NavGrid::set_cost(GridCell c, u8 cost) {
    if (contains(c)) costs_[index(c)] = cost;
}

void NavGrid::fill(u8 cost) {
    for (u8& c : costs_) c = cost;
}

GridCell NavGrid::cell_at(math::vec2 p) const {
    return {to_cell((p.x - origin_.x) / cell_size_), to_cell((p.y - origin_.y) / cell_size_)};
}

math::vec2 NavGrid::cell_center(GridCell c) const {
    return {origin_.x + (static_cast<f32>(c.x) + 0.5f) * cell_size_, origin_.y + (static_cast<f32>(c.y) + 0.5f) * cell_size_};
}

math::aabb2 NavGrid::cell_bounds(GridCell c) const {
    const math::vec2 lo{origin_.x + static_cast<f32>(c.x) * cell_size_, origin_.y + static_cast<f32>(c.y) * cell_size_};
    return {lo, {lo.x + cell_size_, lo.y + cell_size_}};
}

u32 NavGrid::mark_aabb(math::aabb2 box, u8 cost) {
    if (width_ == 0 || height_ == 0) return 0;
    // Cells [x0, x1] x [y0, y1] overlap the box's interior; an edge exactly on a cell line stays out.
    i32 x0 = to_cell((box.min.x - origin_.x) / cell_size_);
    i32 y0 = to_cell((box.min.y - origin_.y) / cell_size_);
    i32 x1 = -to_cell(-(box.max.x - origin_.x) / cell_size_) - 1;  // ceil - 1
    i32 y1 = -to_cell(-(box.max.y - origin_.y) / cell_size_) - 1;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= i32(width_) ? i32(width_) - 1 : x1;
    y1 = y1 >= i32(height_) ? i32(height_) - 1 : y1;
    u32 n = 0;
    for (i32 y = y0; y <= y1; ++y)
        for (i32 x = x0; x <= x1; ++x) {
            costs_[index({x, y})] = cost;
            ++n;
        }
    return n;
}

u64 NavGrid::hash(u64 seed) const {
    Hasher64 h(seed);
    h.bytes(&width_, sizeof(width_));
    h.bytes(&height_, sizeof(height_));
    h.bytes(&cell_size_, sizeof(cell_size_));
    h.bytes(&origin_, sizeof(origin_));
    h.bytes(costs_.data(), costs_.size());
    return h.value();
}

NavGrid nav_grid_from_tilemap(const asset::TilemapView& map, math::vec2 origin, Span<const u32> solid_tiles) {
    if (!map.header) return {};
    NavGrid grid(map.header->width, map.header->height, map.header->tile_size, origin, 1);
    for (u32 i = 0; i < grid.cell_count() && i < map.tiles.size(); ++i) {
        const u32 tile = map.tiles[i];
        if (tile == 0) continue;
        bool solid = solid_tiles.empty();
        for (u32 s : solid_tiles) solid |= s == tile;
        if (solid) grid.set_cost(grid.cell_of_index(i), NavGrid::kBlocked);  // tiles are row-major, row 0 at the bottom
    }
    return grid;
}

u32 mark_static_colliders(NavGrid& grid, World& world, f32 inflate) {
    u32 n = 0;
    world.query<const physics::RigidBody2D, const physics::Collider2D, const LocalTransform>().each(
        [&](const physics::RigidBody2D& rb, const physics::Collider2D& c, const LocalTransform& t) {
            if (rb.type != physics::BodyType::Static || c.is_sensor || c.one_way) return;
            math::aabb2 box = shape_aabb(c.shape, t.position.xy(), angle_z(t.rotation));
            box.min = box.min - math::vec2(inflate);
            box.max = box.max + math::vec2(inflate);
            n += grid.mark_aabb(box);
        });
    return n;
}

math::aabb2 static_collider_bounds(World& world) {
    math::aabb2 all{{1e30f, 1e30f}, {-1e30f, -1e30f}};
    world.query<const physics::RigidBody2D, const physics::Collider2D, const LocalTransform>().each(
        [&](const physics::RigidBody2D& rb, const physics::Collider2D& c, const LocalTransform& t) {
            if (rb.type != physics::BodyType::Static || c.is_sensor || c.one_way) return;
            const math::aabb2 box = shape_aabb(c.shape, t.position.xy(), angle_z(t.rotation));
            grow(all, box.min);
            grow(all, box.max);
        });
    return all;
}

// ------------------------------------------------------------------ search

void Pathfinder::heap_push(Node n) {
    // Min-heap on (f, larger g, smaller index): a total order, so pops never depend on insertion history.
    auto before = [](const Node& a, const Node& b) {
        if (a.f != b.f) return a.f < b.f;
        if (a.g != b.g) return a.g > b.g;
        return a.index < b.index;
    };
    heap_.push_back(n);
    usize i = heap_.size() - 1;
    while (i > 0) {
        const usize parent = (i - 1) / 2;
        if (!before(heap_[i], heap_[parent])) break;
        const Node tmp = heap_[i];
        heap_[i] = heap_[parent];
        heap_[parent] = tmp;
        i = parent;
    }
}

Pathfinder::Node Pathfinder::heap_pop() {
    auto before = [](const Node& a, const Node& b) {
        if (a.f != b.f) return a.f < b.f;
        if (a.g != b.g) return a.g > b.g;
        return a.index < b.index;
    };
    const Node top = heap_[0];
    heap_[0] = heap_.back();
    heap_.pop_back();
    usize i = 0;
    const usize n = heap_.size();
    for (;;) {
        const usize l = 2 * i + 1, r = l + 1;
        usize best = i;
        if (l < n && before(heap_[l], heap_[best])) best = l;
        if (r < n && before(heap_[r], heap_[best])) best = r;
        if (best == i) break;
        const Node tmp = heap_[i];
        heap_[i] = heap_[best];
        heap_[best] = tmp;
        i = best;
    }
    return top;
}

bool Pathfinder::find_cells(const NavGrid& grid, GridCell start, GridCell goal, bool diagonal, Array<GridCell>& out) {
    out.clear();
    expanded_ = 0;
    if (!grid.contains(start) || !grid.walkable(goal)) return false;
    const u32 n = grid.cell_count();
    if (g_.size() != n) {
        g_.resize(n, 0);
        parent_.resize(n, 0);
        open_stamp_.resize(n, 0);
        closed_stamp_.resize(n, 0);
        for (u32& s : open_stamp_) s = 0;
        for (u32& s : closed_stamp_) s = 0;
        stamp_ = 0;
    }
    if (++stamp_ == 0) {  // wrapped: stale stamps could alias
        for (u32& s : open_stamp_) s = 0;
        for (u32& s : closed_stamp_) s = 0;
        stamp_ = 1;
    }
    heap_.clear();
    const u32 si = grid.index(start), gi = grid.index(goal);
    g_[si] = 0;
    parent_[si] = si;
    open_stamp_[si] = stamp_;
    heap_push({heuristic(start, goal, diagonal), 0, si, 0});
    const u32 dirs = diagonal ? 8u : 4u;
    while (!heap_.empty()) {
        const Node cur = heap_pop();
        if (closed_stamp_[cur.index] == stamp_ || cur.g != g_[cur.index]) continue;  // stale entry
        closed_stamp_[cur.index] = stamp_;
        ++expanded_;
        if (cur.index == gi) {
            for (u32 i = gi;; i = parent_[i]) {
                out.push_back(grid.cell_of_index(i));
                if (i == si) break;
            }
            for (usize a = 0, b = out.size() - 1; a < b; ++a, --b) {
                const GridCell tmp = out[a];
                out[a] = out[b];
                out[b] = tmp;
            }
            return true;
        }
        const GridCell c = grid.cell_of_index(cur.index);
        for (u32 k = 0; k < dirs; ++k) {
            const GridCell nc{c.x + kDx[k], c.y + kDy[k]};
            const u8 cost = grid.cost(nc);
            if (cost == NavGrid::kBlocked) continue;
            const bool diag = k >= 4;
            // No corner cutting: a diagonal step needs both cells it squeezes between open.
            if (diag && (!grid.walkable({c.x + kDx[k], c.y}) || !grid.walkable({c.x, c.y + kDy[k]}))) continue;
            const u32 ni = grid.index(nc);
            if (closed_stamp_[ni] == stamp_) continue;
            const u64 ng = cur.g + (diag ? kDiagonal : kStraight) * cost;
            if (open_stamp_[ni] == stamp_ && ng >= g_[ni]) continue;
            g_[ni] = ng;
            parent_[ni] = cur.index;
            open_stamp_[ni] = stamp_;
            heap_push({ng + heuristic(nc, goal, diagonal), ng, ni, 0});
        }
    }
    return false;
}

bool Pathfinder::line_of_sight(const NavGrid& grid, GridCell a, GridCell b, u8 max_cost) {
    auto open = [&](i32 x, i32 y) {
        const u8 c = grid.cost({x, y});
        return c != NavGrid::kBlocked && c <= max_cost;
    };
    // Supercover walk (Dedu): every cell the centre-to-centre segment touches; at an exact corner
    // crossing both side cells. `a` itself is not tested (a path may start inside a wall).
    i32 x = a.x, y = a.y;
    const i32 dx = b.x - a.x, dy = b.y - a.y;
    const i32 sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    const i32 ax = iabs(dx), ay = iabs(dy);
    const i32 ddx = 2 * ax, ddy = 2 * ay;
    if (ddx >= ddy) {
        i32 error = ax, prev = ax;
        for (i32 i = 0; i < ax; ++i) {
            x += sx;
            error += ddy;
            if (error > ddx) {
                y += sy;
                error -= ddx;
                if (error + prev < ddx) {
                    if (!open(x, y - sy)) return false;
                } else if (error + prev > ddx) {
                    if (!open(x - sx, y)) return false;
                } else if (!open(x, y - sy) || !open(x - sx, y)) {
                    return false;
                }
            }
            if (!open(x, y)) return false;
            prev = error;
        }
    } else {
        i32 error = ay, prev = ay;
        for (i32 i = 0; i < ay; ++i) {
            y += sy;
            error += ddx;
            if (error > ddy) {
                x += sx;
                error -= ddy;
                if (error + prev < ddy) {
                    if (!open(x - sx, y)) return false;
                } else if (error + prev > ddy) {
                    if (!open(x, y - sy)) return false;
                } else if (!open(x - sx, y) || !open(x, y - sy)) {
                    return false;
                }
            }
            if (!open(x, y)) return false;
            prev = error;
        }
    }
    return true;
}

void Pathfinder::smooth(const NavGrid& grid, Array<GridCell>& cells) {
    if (cells.size() <= 2) return;
    const usize last = cells.size() - 1;
    usize write = 1;
    usize anchor = 0;
    while (anchor < last) {
        usize best = anchor + 1;
        u8 max_cost = grid.cost(cells[best]);
        for (usize j = anchor + 2; j <= last; ++j) {
            const u8 c = grid.cost(cells[j]);
            max_cost = c > max_cost ? c : max_cost;
            if (!line_of_sight(grid, cells[anchor], cells[j], max_cost)) break;
            best = j;
        }
        cells[write++] = cells[best];  // write <= best: in place is safe
        anchor = best;
    }
    cells.resize(write);
}

bool Pathfinder::find_path(const NavGrid& grid, math::vec2 from, math::vec2 to, const PathOptions& opt, Array<math::vec2>& out) {
    out.clear();
    if (!find_cells(grid, grid.cell_at(from), grid.cell_at(to), opt.diagonal, cells_)) return false;
    if (opt.smooth) smooth(grid, cells_);
    for (usize i = 1; i + 1 < cells_.size(); ++i) out.push_back(grid.cell_center(cells_[i]));
    out.push_back(to);
    return true;
}

// ------------------------------------------------------------------ runner

void path_agents(Frame& f) {
    const NavGrid* grid = f.try_resource<NavGrid>();
    PathScratch* scratch = f.try_resource<PathScratch>();
    if (!grid || !scratch || grid->cell_count() == 0) return;
    physics::World2D* phys = f.try_resource<physics::World2D>();
    const f32 dt = f.dt;

    f.query<PathAgent, LocalTransform>().each([&](Entity e, PathAgent& a, LocalTransform& t) {
        physics::BodyState b;
        const bool body = phys && phys->body_state(e, b);
        const math::vec2 pos = body ? b.position : t.position.xy();
        const bool was_moving = a.status == PathStatus::Moving;

        // Arrived last tick: the body's velocity carried it onto the target; stop it now.
        if (a.status == PathStatus::Arrived && a.waypoint_count > 0) {
            if (body) phys->set_linear_velocity(e, {0.0f, 0.0f});
            a.waypoint_count = 0;
            a.waypoint = 0;
        }
        if (!a.active) {
            if (body && was_moving) phys->set_linear_velocity(e, {0.0f, 0.0f});
            a.status = PathStatus::Idle;
            a.waypoint_count = 0;
            return;
        }

        bool plan = a.status == PathStatus::Idle || !(a.planned_target == a.target);
        if (!plan && (a.status == PathStatus::Moving || a.status == PathStatus::NoPath)) {
            if (a.status == PathStatus::Moving && a.truncated && a.waypoint >= a.waypoint_count) {
                plan = true;
            } else if (a.repath_interval > 0) {
                if (a.repath_countdown > 0) --a.repath_countdown;
                plan = a.repath_countdown == 0;
            }
        }
        if (plan) {
            PathOptions opt;
            opt.diagonal = a.diagonal;
            ++scratch->plans;
            a.planned_target = a.target;
            a.repath_countdown = a.repath_interval;
            a.waypoint = 0;
            if (scratch->finder.find_path(*grid, pos, a.target, opt, scratch->points)) {
                const usize n = scratch->points.size();
                const usize keep = n < PathAgent::kMaxWaypoints ? n : PathAgent::kMaxWaypoints;
                for (usize i = 0; i < keep; ++i) a.waypoints[i] = scratch->points[i];
                a.waypoint_count = static_cast<u8>(keep);
                a.truncated = n > PathAgent::kMaxWaypoints;
                a.status = PathStatus::Moving;
            } else {
                a.waypoint_count = 0;
                a.truncated = false;
                a.status = PathStatus::NoPath;
                if (body && was_moving) phys->set_linear_velocity(e, {0.0f, 0.0f});
            }
        }
        if (a.status != PathStatus::Moving) return;

        // Walk speed x dt along the waypoints, turning corners inside the tick.
        f32 travel = a.speed * dt;
        math::vec2 p = pos;
        while (a.waypoint < a.waypoint_count) {
            const math::vec2 wp = a.waypoints[a.waypoint];
            const bool final = a.waypoint + 1 == a.waypoint_count && !a.truncated;
            const math::vec2 d = wp - p;
            const f32 dist = math::length(d);
            if (final && dist <= a.arrival_radius) {
                a.status = PathStatus::Arrived;
                break;
            }
            // Within a thousandth of the step counts as reaching it (float sums of speed x dt).
            if (dist <= travel * 1.001f) {
                p = wp;
                travel -= dist;
                ++a.waypoint;
                if (final) a.status = PathStatus::Arrived;
                if (travel <= 0.0f) break;
                continue;
            }
            p = p + d * (travel / dist);
            break;
        }
        if (body) {
            const math::vec2 v = dt > 0.0f ? (p - pos) * (1.0f / dt) : math::vec2{};
            phys->set_linear_velocity(e, v);
        } else {
            t.position.x = p.x;
            t.position.y = p.y;
        }
    });
}

void install_path_agents(World& world) {
    if (world.try_resource<PathScratch>()) return;
    world.register_components<PathAgent>();
    world.set_resource(PathScratch{});
    world.add_system(Phase::FixedUpdate_Pre, "path_agents", &path_agents);
}

void install_path_agents(World& world, NavGrid&& grid) {
    install_path_agents(world);
    world.set_resource(static_cast<NavGrid&&>(grid));
}

// ------------------------------------------------------------------ debug draw

void debug_draw_grid(const NavGrid& grid, physics::DebugDrawSink& sink, math::Color blocked, math::Color costly) {
    for (u32 i = 0; i < grid.cell_count(); ++i) {
        const u8 c = grid.costs()[i];
        if (c == 1) continue;
        const math::aabb2 r = grid.cell_bounds(grid.cell_of_index(i));
        const math::vec2 tl{r.min.x, r.max.y}, br{r.max.x, r.min.y};
        if (c == NavGrid::kBlocked) {
            sink.line(r.min, r.max, blocked);
            sink.line(tl, br, blocked);
        } else {
            sink.line(r.min, br, costly);
            sink.line(br, r.max, costly);
            sink.line(r.max, tl, costly);
            sink.line(tl, r.min, costly);
        }
    }
    const math::Color edge{0.7f, 0.7f, 0.7f, 1.0f};
    const math::aabb2 all{grid.origin(), grid.origin() + math::vec2(static_cast<f32>(grid.width()) * grid.cell_size(),
                                                                     static_cast<f32>(grid.height()) * grid.cell_size())};
    const math::vec2 tl{all.min.x, all.max.y}, br{all.max.x, all.min.y};
    sink.line(all.min, br, edge);
    sink.line(br, all.max, edge);
    sink.line(all.max, tl, edge);
    sink.line(tl, all.min, edge);
}

void debug_draw_agents(World& world, physics::DebugDrawSink& sink, math::Color color) {
    world.query<const PathAgent, const LocalTransform>().each([&](const PathAgent& a, const LocalTransform& t) {
        math::vec2 from = t.position.xy();
        for (u32 i = a.waypoint; i < a.waypoint_count; ++i) {
            sink.line(from, a.waypoints[i], color);
            from = a.waypoints[i];
        }
        sink.circle(a.target, a.arrival_radius > 0.05f ? a.arrival_radius : 0.05f, color);
    });
}

}  // namespace mce::kit
