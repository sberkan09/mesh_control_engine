#pragma once
// net_arena's rules (M-NET3 step 5): what the server, the clients' predictions and the bots share.
// The arena itself is scene data (assets/scenes/arena.mcscene): walls and spawn pads are entities
// with a LocalTransform and NetReplicated; the game decides what each is from NetReplicated::kind.
// Players are made by the server, with a NetOwner, an ArenaHealth (replicated fields) and a hit box.

#include <cmath>
#include <cstring>
#include <utility>

#include "mce/containers/array.h"
#include "mce/ecs/replicated.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/net/net.h"
#include "mce/refl/macros.h"

namespace arena {

using namespace mce;

constexpr f64 kDt = 1.0 / 60.0;
constexpr u8 kChannels = 3;
constexpr u8 kChannelControl = 0;   // reliable: JOIN, WELCOME, SHOT
constexpr u8 kChannelSnapshot = 1;  // sequenced: server -> client snapshots
constexpr u8 kChannelInput = 2;     // sequenced: client -> server inputs and acknowledgements
constexpr u8 kMsgJoin = 1;
constexpr u8 kMsgInput = 2;
constexpr u8 kMsgWelcome = 3;  // server -> client: the net id of your player
constexpr u8 kMsgShot = 4;     // client -> server: the tick the client drew, and where it aimed
constexpr u8 kMsgAck = 5;      // client -> server: the newest snapshot applied (delta snapshots)

constexpr u32 kKindPlayer = 1;
constexpr u32 kKindWall = 2;  // scene: a box of the entity's scale
constexpr u32 kKindPad = 3;   // scene: a spawn pad

constexpr f32 kSpeed = 7.0f;        // m/s
constexpr f32 kRadius = 0.45f;      // a player is a circle for walking, a box for shots
constexpr f32 kShotRange = 40.0f;
constexpr u32 kShotDamage = 25;
constexpr u32 kMaxHealth = 100;
constexpr u32 kFireTicks = 14;      // between shots (the server enforces it)
constexpr u32 kRespawnTicks = 120;  // dead for two seconds
constexpr f64 kRewindSeconds = 1.0;  // how far back a shot's view is honoured (through a relay a link is two hops)
constexpr u32 kSnapshotEvery = 2;     // ticks between snapshots: 30 Hz keeps a delta baseline for a second
constexpr u32 kMaxPlayers = 4;
constexpr f32 kViewWidth = 32.0f;   // the camera shows this much of the arena
constexpr f32 kViewHeight = 20.0f;

// Replicated fields: every client sees them, nobody but the server writes them.
struct ArenaHealth {
    u32 hp = kMaxHealth;
    u32 frags = 0;
    u32 deaths = 0;
    u32 shots = 0;  // fired so far: a client draws a beam when this goes up
};
MCE_COMPONENT(ArenaHealth, (hp, Replicated), (frags, Replicated), (deaths, Replicated), (shots, Replicated))

// What a client sends each tick, and the state it predicts. The step is shared: the server runs it
// for each input it receives, the client for each input it presses and again when it replays the
// unacknowledged ones after a snapshot.
struct Input {
    f32 move_x = 0, move_y = 0;  // -1..1
    f32 aim = 0;                 // radians, counter-clockwise from +x
};
struct State {
    LocalTransform transform;
};

struct Box {
    math::vec2 center;
    math::vec2 half;
    bool operator==(const Box& o) const { return center.x == o.center.x && center.y == o.center.y && half.x == o.half.x && half.y == o.half.y; }
};

// The arena as the rules see it: walls and spawn pads, read from the entities of a world (the
// server's, loaded from the scene; a client's, mirrored from snapshots).
struct Map {
    Array<Box> walls;
    Array<math::vec2> pads;

    // Rebuilds from the world; true when anything changed.
    bool rebuild(World& world) {
        Array<Box> w;
        Array<math::vec2> p;
        world.query<const net::NetId, const LocalTransform>().each([&](const net::NetId& id, const LocalTransform& t) {
            if (id.kind == kKindWall) w.push_back(Box{{t.position.x, t.position.y}, {0.5f * t.scale.x, 0.5f * t.scale.y}});
            if (id.kind == kKindPad) p.push_back({t.position.x, t.position.y});
        });
        // Entities iterate in storage order; the rules need the same order everywhere.
        auto by_pos = [](const auto& a, const auto& b) {
            return a.center.x != b.center.x ? a.center.x < b.center.x : a.center.y < b.center.y;
        };
        for (usize i = 1; i < w.size(); ++i)
            for (usize j = i; j > 0 && by_pos(w[j], w[j - 1]); --j) std::swap(w[j], w[j - 1]);
        for (usize i = 1; i < p.size(); ++i)
            for (usize j = i; j > 0 && (p[j].x < p[j - 1].x || (p[j].x == p[j - 1].x && p[j].y < p[j - 1].y)); --j)
                std::swap(p[j], p[j - 1]);
        bool same = w.size() == walls.size() && p.size() == pads.size();
        for (usize i = 0; same && i < w.size(); ++i) same = w[i] == walls[i];
        for (usize i = 0; same && i < p.size(); ++i) same = p[i].x == pads[i].x && p[i].y == pads[i].y;
        if (same) return false;
        walls = std::move(w);
        pads = std::move(p);
        return true;
    }
    math::vec2 pad(u32 index) const {
        return pads.empty() ? math::vec2(0, 0) : pads[index % pads.size()];
    }
};

inline math::quat aim_rotation(f32 aim) { return math::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, aim); }

// Pushes a circle out of a box (one pass).
inline void push_out(math::vec3& p, const Box& b) {
    const f32 cx = std::fmax(b.center.x - b.half.x, std::fmin(p.x, b.center.x + b.half.x));
    const f32 cy = std::fmax(b.center.y - b.half.y, std::fmin(p.y, b.center.y + b.half.y));
    f32 dx = p.x - cx, dy = p.y - cy;
    const f32 d2 = dx * dx + dy * dy;
    if (d2 >= kRadius * kRadius) return;
    if (d2 > 1e-12f) {
        const f32 d = std::sqrt(d2), k = (kRadius - d) / d;
        p.x += dx * k;
        p.y += dy * k;
        return;
    }
    // The centre is inside the box: leave through the nearest side.
    dx = p.x - b.center.x;
    dy = p.y - b.center.y;
    const f32 ox = b.half.x + kRadius - std::fabs(dx), oy = b.half.y + kRadius - std::fabs(dy);
    if (ox < oy) p.x += dx < 0 ? -ox : ox;
    else p.y += dy < 0 ? -oy : oy;
}

// The one step both sides run. `user` is the Map. The server trusts no client: NaN and long
// vectors are clamped here.
inline void step_player(void* user, State& s, const Input& in, f32 dt) {
    const Map& map = *static_cast<const Map*>(user);
    f32 mx = std::isfinite(in.move_x) ? in.move_x : 0.0f, my = std::isfinite(in.move_y) ? in.move_y : 0.0f;
    const f32 len = std::sqrt(mx * mx + my * my);
    if (len > 1.0f) {
        mx /= len;
        my /= len;
    }
    s.transform.position.x += mx * kSpeed * dt;
    s.transform.position.y += my * kSpeed * dt;
    for (int pass = 0; pass < 2; ++pass)
        for (const Box& b : map.walls) push_out(s.transform.position, b);
    s.transform.rotation = aim_rotation(std::isfinite(in.aim) ? in.aim : 0.0f);
}

}  // namespace arena
