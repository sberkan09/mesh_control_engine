// Procedural level generators (M-PROCGEN 1). See mce/kit/procgen.h for the contract: pure functions
// of (parameters, seed), integer-only, one connected floor region inside a wall border.

#include "mce/kit/procgen.h"

namespace mce::kit::procgen {

u32 ProcGrid::floor_count() const {
    u32 n = 0;
    for (usize i = 0; i < cells.size(); ++i) n += cells[i] == kFloor ? 1u : 0u;
    return n;
}

u64 ProcGrid::hash() const {
    Hasher64 h;
    h.u32v(width);
    h.u32v(height);
    h.bytes(cells.data(), cells.size());
    h.u32v(static_cast<u32>(start.x));
    h.u32v(static_cast<u32>(start.y));
    h.u32v(static_cast<u32>(exit.x));
    h.u32v(static_cast<u32>(exit.y));
    return h.value();
}

void ProcGrid::to_ascii(Array<char>& out) const {
    out.clear();
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const i32 ix = static_cast<i32>(x), iy = static_cast<i32>(y);
            char c = at(ix, iy) == kFloor ? '.' : '#';
            if (start == Cell{ix, iy}) c = 'S';
            if (exit == Cell{ix, iy}) c = 'E';
            out.push_back(c);
        }
        if (y + 1 < height) out.push_back('\n');
    }
}

namespace {

constexpr usize idx(u32 w, i32 x, i32 y) { return static_cast<usize>(y) * w + static_cast<usize>(x); }

Result<ProcGrid> fail(const char* stage) { return Error(Errc::InvalidArgument, stage); }

bool size_ok(u32 w, u32 h) { return w >= kMinSize && h >= kMinSize && w <= kMaxSize && h <= kMaxSize; }

ProcGrid blank(u32 w, u32 h, u8 fill) {
    ProcGrid g;
    g.width = w;
    g.height = h;
    g.cells.resize(static_cast<usize>(w) * h);
    for (usize i = 0; i < g.cells.size(); ++i) g.cells[i] = fill;
    return g;
}

void wall_border(ProcGrid& g) {
    for (u32 x = 0; x < g.width; ++x) {
        g.cells[idx(g.width, static_cast<i32>(x), 0)] = kWall;
        g.cells[idx(g.width, static_cast<i32>(x), static_cast<i32>(g.height) - 1)] = kWall;
    }
    for (u32 y = 0; y < g.height; ++y) {
        g.cells[idx(g.width, 0, static_cast<i32>(y))] = kWall;
        g.cells[idx(g.width, static_cast<i32>(g.width) - 1, static_cast<i32>(y))] = kWall;
    }
}

// Breadth-first search over 4-connected floor from `from`; fills `dist` (-1 = not reached) and
// returns the number of cells reached. The queue is the visit order: the same on every machine.
u32 flood(const ProcGrid& g, Cell from, Array<i32>& dist, Array<u32>& queue) {
    dist.resize(g.cells.size());
    for (usize i = 0; i < dist.size(); ++i) dist[i] = -1;
    queue.clear();
    dist[idx(g.width, from.x, from.y)] = 0;
    queue.push_back(static_cast<u32>(idx(g.width, from.x, from.y)));
    static constexpr i32 dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (usize head = 0; head < queue.size(); ++head) {
        const u32 cur = queue[head];
        const i32 cx = static_cast<i32>(cur % g.width), cy = static_cast<i32>(cur / g.width);
        for (int d = 0; d < 4; ++d) {
            const i32 nx = cx + dx[d], ny = cy + dy[d];
            if (g.at(nx, ny) != kFloor) continue;
            const usize ni = idx(g.width, nx, ny);
            if (dist[ni] >= 0) continue;
            dist[ni] = dist[cur] + 1;
            queue.push_back(static_cast<u32>(ni));
        }
    }
    return static_cast<u32>(queue.size());
}

// Keeps the largest floor region (the first one found on a tie), checks the floor target, picks
// the start from the "start" stream and the exit as the farthest cell by steps. False: this draw
// does not make a level.
bool finalize(ProcGrid& g, u64 seed, u32 attempt, u32 min_floor_percent) {
    wall_border(g);
    Array<i32> dist;
    Array<u32> queue;
    Array<u8> seen;
    seen.resize(g.cells.size());
    for (usize i = 0; i < seen.size(); ++i) seen[i] = 0;
    usize best_first = g.cells.size();
    u32 best_size = 0;
    for (usize i = 0; i < g.cells.size(); ++i) {
        if (g.cells[i] != kFloor || seen[i]) continue;
        const u32 n = flood(g, {static_cast<i32>(i % g.width), static_cast<i32>(i / g.width)}, dist, queue);
        for (usize q = 0; q < queue.size(); ++q) seen[queue[q]] = 1;
        if (n > best_size) {
            best_size = n;
            best_first = i;
        }
    }
    if (best_size == 0) return false;
    // Fill every other pocket with wall.
    flood(g, {static_cast<i32>(best_first % g.width), static_cast<i32>(best_first / g.width)}, dist, queue);
    for (usize i = 0; i < g.cells.size(); ++i)
        if (g.cells[i] == kFloor && dist[i] < 0) g.cells[i] = kWall;
    if (static_cast<u64>(best_size) * 100 < static_cast<u64>(min_floor_percent) * g.cells.size()) return false;

    Rng pick = Rng::child(seed, "start", attempt);
    const u32 which = pick.below(best_size);
    u32 seen_floor = 0;
    for (usize i = 0; i < g.cells.size(); ++i) {
        if (g.cells[i] != kFloor) continue;
        if (seen_floor++ == which) {
            g.start = {static_cast<i32>(i % g.width), static_cast<i32>(i / g.width)};
            break;
        }
    }
    flood(g, g.start, dist, queue);
    i32 far = -1;
    usize far_at = idx(g.width, g.start.x, g.start.y);
    for (usize i = 0; i < dist.size(); ++i)
        if (dist[i] > far) {
            far = dist[i];
            far_at = i;
        }
    g.exit = {static_cast<i32>(far_at % g.width), static_cast<i32>(far_at / g.width)};
    return true;
}

// ---- rooms -------------------------------------------------------------------------------------

struct Rect {
    i32 x, y, w, h;
};

struct RoomsBuild {
    ProcGrid& g;
    const RoomsParams& p;
    Rng& rng;

    void carve(i32 x, i32 y) {
        if (x > 0 && y > 0 && x < static_cast<i32>(g.width) - 1 && y < static_cast<i32>(g.height) - 1) g.cells[idx(g.width, x, y)] = kFloor;
    }
    void carve_h(i32 x0, i32 x1, i32 y) {
        if (x0 > x1) {
            const i32 t = x0;
            x0 = x1;
            x1 = t;
        }
        for (i32 x = x0; x <= x1; ++x)
            for (u32 c = 0; c < p.corridor; ++c) carve(x, y + static_cast<i32>(c));
    }
    void carve_v(i32 y0, i32 y1, i32 x) {
        if (y0 > y1) {
            const i32 t = y0;
            y0 = y1;
            y1 = t;
        }
        for (i32 y = y0; y <= y1; ++y)
            for (u32 c = 0; c < p.corridor; ++c) carve(x + static_cast<i32>(c), y);
    }
    Cell room(const Rect& r) {
        i32 rw = r.w * rng.range(50, 90) / 100, rh = r.h * rng.range(50, 90) / 100;
        rw = rw < static_cast<i32>(p.room_min) ? static_cast<i32>(p.room_min) : rw;
        rh = rh < static_cast<i32>(p.room_min) ? static_cast<i32>(p.room_min) : rh;
        if (rw > r.w - 2) rw = r.w - 2;
        if (rh > r.h - 2) rh = r.h - 2;
        const i32 rx = r.x + 1 + static_cast<i32>(rng.below(static_cast<u32>(r.w - 2 - rw + 1)));
        const i32 ry = r.y + 1 + static_cast<i32>(rng.below(static_cast<u32>(r.h - 2 - rh + 1)));
        for (i32 y = ry; y < ry + rh; ++y)
            for (i32 x = rx; x < rx + rw; ++x) carve(x, y);
        return {rx + rw / 2, ry + rh / 2};
    }
    // Builds the subtree and returns a cell inside its first room, for the parent to join to.
    Cell build(const Rect& r, u32 depth) {
        const i32 lm = static_cast<i32>(p.leaf_min);
        const bool can_v = r.w >= 2 * lm, can_h = r.h >= 2 * lm;
        if ((!can_v && !can_h) || depth >= 10) return room(r);
        bool vertical = can_v;  // a vertical cut splits the width
        if (can_v && can_h) vertical = r.w * 4 > r.h * 5 ? true : (r.h * 4 > r.w * 5 ? false : rng.chance(50));
        Rect a = r, b = r;
        if (vertical) {
            const i32 cut = lm + static_cast<i32>(rng.below(static_cast<u32>(r.w - 2 * lm + 1)));
            a.w = cut;
            b.x = r.x + cut;
            b.w = r.w - cut;
        } else {
            const i32 cut = lm + static_cast<i32>(rng.below(static_cast<u32>(r.h - 2 * lm + 1)));
            a.h = cut;
            b.y = r.y + cut;
            b.h = r.h - cut;
        }
        const Cell ca = build(a, depth + 1), cb = build(b, depth + 1);
        if (rng.chance(50)) {
            carve_h(ca.x, cb.x, ca.y);
            carve_v(ca.y, cb.y, cb.x);
        } else {
            carve_v(ca.y, cb.y, ca.x);
            carve_h(ca.x, cb.x, cb.y);
        }
        return ca;
    }
};

}  // namespace

Result<ProcGrid> generate_rooms(u32 width, u32 height, u64 seed, const RoomsParams& p) {
    if (!size_ok(width, height)) return fail("procgen.rooms: size must be 8..512 on each side");
    if (p.leaf_min < 6 || p.room_min < 3 || p.room_min + 2 > p.leaf_min || (p.corridor != 1 && p.corridor != 2) || p.min_floor_percent > 90)
        return fail("procgen.rooms: need leaf_min >= 6, 3 <= room_min <= leaf_min - 2, corridor 1 or 2, min_floor_percent <= 90");
    for (u32 attempt = 0; attempt < kMaxAttempts; ++attempt) {
        ProcGrid g = blank(width, height, kWall);
        Rng rng = Rng::child(seed, "rooms", attempt);
        RoomsBuild b{g, p, rng};
        b.build({1, 1, static_cast<i32>(width) - 2, static_cast<i32>(height) - 2}, 0);
        if (finalize(g, seed, attempt, p.min_floor_percent)) return g;
    }
    return fail("procgen.rooms: no draw reached min_floor_percent; lower it or use a larger grid");
}

Result<ProcGrid> generate_caves(u32 width, u32 height, u64 seed, const CavesParams& p) {
    if (!size_ok(width, height)) return fail("procgen.caves: size must be 8..512 on each side");
    if (p.fill_percent > 100 || p.iterations > 32 || p.min_floor_percent > 90) return fail("procgen.caves: fill_percent <= 100, iterations <= 32, min_floor_percent <= 90");
    for (u32 attempt = 0; attempt < kMaxAttempts; ++attempt) {
        ProcGrid g = blank(width, height, kWall);
        Rng rng = Rng::child(seed, "caves", attempt);
        for (u32 y = 1; y + 1 < height; ++y)
            for (u32 x = 1; x + 1 < width; ++x) g.cells[idx(width, static_cast<i32>(x), static_cast<i32>(y))] = rng.chance(p.fill_percent) ? kWall : kFloor;
        ProcGrid next = blank(width, height, kWall);
        for (u32 it = 0; it < p.iterations; ++it) {
            for (u32 y = 1; y + 1 < height; ++y)
                for (u32 x = 1; x + 1 < width; ++x) {
                    u32 walls = 0;
                    for (i32 dy = -1; dy <= 1; ++dy)
                        for (i32 dx = -1; dx <= 1; ++dx)
                            if ((dx || dy) && g.at(static_cast<i32>(x) + dx, static_cast<i32>(y) + dy) == kWall) ++walls;
                    next.cells[idx(width, static_cast<i32>(x), static_cast<i32>(y))] = walls > 4 ? kWall : (walls < 4 ? kFloor : g.cells[idx(width, static_cast<i32>(x), static_cast<i32>(y))]);
                }
            for (usize i = 0; i < g.cells.size(); ++i) g.cells[i] = next.cells[i];
        }
        if (finalize(g, seed, attempt, p.min_floor_percent)) return g;
    }
    return fail("procgen.caves: no draw reached min_floor_percent; lower it or the fill");
}

Result<ProcGrid> generate_walk(u32 width, u32 height, u64 seed, const WalkParams& p) {
    if (!size_ok(width, height)) return fail("procgen.walk: size must be 8..512 on each side");
    if (p.coverage_percent < 1 || p.coverage_percent > 80 || p.turn_percent > 100) return fail("procgen.walk: coverage_percent 1..80, turn_percent <= 100");
    const u64 target = static_cast<u64>(width - 2) * (height - 2) * p.coverage_percent / 100;
    const u64 max_steps = p.max_steps ? p.max_steps : static_cast<u64>(width) * height * 10;
    static constexpr i32 dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (u32 attempt = 0; attempt < kMaxAttempts; ++attempt) {
        ProcGrid g = blank(width, height, kWall);
        Rng rng = Rng::child(seed, "walk", attempt);
        i32 x = static_cast<i32>(width) / 2, y = static_cast<i32>(height) / 2;
        u32 dir = rng.below(4);
        u64 open = 0;
        for (u64 step = 0; step < max_steps && open < target; ++step) {
            u8& c = g.cells[idx(width, x, y)];
            if (c != kFloor) {
                c = kFloor;
                ++open;
            }
            if (rng.chance(p.turn_percent)) dir = rng.below(4);
            const i32 nx = x + dx[dir], ny = y + dy[dir];
            if (nx < 1 || ny < 1 || nx >= static_cast<i32>(width) - 1 || ny >= static_cast<i32>(height) - 1) {
                dir = rng.below(4);
                continue;
            }
            x = nx;
            y = ny;
        }
        if (finalize(g, seed, attempt, p.coverage_percent / 2 > 5 ? p.coverage_percent / 2 : 5)) return g;
    }
    return fail("procgen.walk: no draw reached half its coverage; raise max_steps or turn_percent");
}

namespace {

// One lattice value, 0..65535, for octave `o` at lattice point (ix, iy).
u32 lattice(u64 seed, u32 o, i32 ix, i32 iy) {
    return static_cast<u32>(mix64(hash_combine(hash_combine(hash_combine(seed, o), static_cast<u64>(static_cast<u32>(ix))), static_cast<u64>(static_cast<u32>(iy)))) >> 48);
}

u32 smooth(u32 f) {  // f in 0..65535 -> 3f^2 - 2f^3, same range
    const u64 f2 = (static_cast<u64>(f) * f) >> 16;
    return static_cast<u32>((f2 * ((3ull << 16) - 2ull * f)) >> 16);
}

u32 mix(u32 a, u32 b, u32 s) {  // a + (b - a) * s, s in 0..65535
    return static_cast<u32>(static_cast<i64>(a) + ((static_cast<i64>(b) - static_cast<i64>(a)) * static_cast<i64>(s) >> 16));
}

// Value noise of one octave at cell (x, y) with a lattice period of `period` cells.
u32 octave(u64 seed, u32 o, u32 period, u32 x, u32 y) {
    const i32 gx = static_cast<i32>(x / period), gy = static_cast<i32>(y / period);
    const u32 fx = smooth(static_cast<u32>((static_cast<u64>(x % period) << 16) / period)), fy = smooth(static_cast<u32>((static_cast<u64>(y % period) << 16) / period));
    const u32 top = mix(lattice(seed, o, gx, gy), lattice(seed, o, gx + 1, gy), fx);
    const u32 bottom = mix(lattice(seed, o, gx, gy + 1), lattice(seed, o, gx + 1, gy + 1), fx);
    return mix(top, bottom, fy);
}

}  // namespace

Result<ProcGrid> generate_noise(u32 width, u32 height, u64 seed, const NoiseParams& p) {
    if (!size_ok(width, height)) return fail("procgen.noise: size must be 8..512 on each side");
    if (p.scale < 2 || p.scale > 256 || p.octaves < 1 || p.octaves > 4 || p.floor_percent < 1 || p.floor_percent > 99 || p.min_floor_percent > 90)
        return fail("procgen.noise: scale 2..256, octaves 1..4, floor_percent 1..99, min_floor_percent <= 90");
    for (u32 attempt = 0; attempt < kMaxAttempts; ++attempt) {
        ProcGrid g = blank(width, height, kWall);
        const u64 s = hash_combine(hash_combine(seed, fnv1a64("noise")), attempt);
        const u64 total_weight = (1ull << p.octaves) - 1;
        for (u32 y = 1; y + 1 < height; ++y)
            for (u32 x = 1; x + 1 < width; ++x) {
                u64 sum = 0;
                for (u32 o = 0; o < p.octaves; ++o) {
                    u32 period = p.scale >> o;
                    period = period < 1 ? 1 : period;
                    sum += static_cast<u64>(octave(s, o, period, x, y)) << (p.octaves - 1 - o);
                }
                const u64 v = sum / total_weight;  // 0..65535
                g.cells[idx(width, static_cast<i32>(x), static_cast<i32>(y))] = v * 100 < static_cast<u64>(p.floor_percent) * 65536 ? kFloor : kWall;
            }
        if (finalize(g, seed, attempt, p.min_floor_percent)) return g;
    }
    return fail("procgen.noise: no draw reached min_floor_percent; raise floor_percent");
}

}  // namespace mce::kit::procgen
