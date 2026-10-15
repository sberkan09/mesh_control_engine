#pragma once
// Lag compensation for hit checks (M-NET2 step 4).
//
// A client draws remote entities in the past (the jitter buffer, plus the trip the snapshot took),
// so when its player fires at what it sees, the target has already moved on the server. Checked
// against the server's present, a shot that looked dead on misses. The server keeps a short history
// of every hit box and checks the shot against the moment the client was drawing:
//
//   server, after each tick (the tick number its snapshot carries):
//     lag.record(world, tick);
//   client, with the shot:
//     send(shot{origin, direction, client_rep.drawn_tick(target_net_id)});
//   server, on the shot:
//     net::RewindHit hit;
//     if (lag.raycast(shot.view_tick, shot.origin, shot.direction, range, hit, shooter_id)) ...
//
// Entities take part when they have NetId, LocalTransform and HitBox (an oriented box in the
// entity's local space, scaled with it). The history is interpolated the way ReplicationClient
// draws it, so a shot at the drawn centre of an entity hits it exactly. The rewind is limited to
// max_rewind_s: a client claiming an older view (a slow link, or a cheat) is checked at the oldest
// moment allowed, and a view in the future is checked at the newest recorded tick.

#include "mce/containers/array.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"

namespace mce::net {

// The volume a hit check tests, server-side: a box of half_extents around offset, in the entity's
// local space (rotated and scaled with its LocalTransform).
struct HitBox {
    math::vec3 half_extents{0.5f, 0.5f, 0.5f};
    math::vec3 offset{};
};

struct LagCompensationDesc {
    f64 tick_rate = 60.0;     // server ticks per second
    f64 max_rewind_s = 0.25;  // how far back a client's view is honoured
};

struct RewindHit {
    u32 net_id = 0;
    Entity entity;
    f32 distance = 0.0f;  // along the ray
    math::vec3 point{};
    f64 tick = 0.0;  // the moment the check ran at, after clamping
};

class LagCompensator {
public:
    void init(const LagCompensationDesc& desc);

    // Records every hit box as of `tick` (call after the tick, with the number its snapshot carries).
    // Ticks must increase; a repeat of the newest replaces it.
    void record(World& world, u32 tick);
    // Forgets the history (a new session).
    void clear();

    // The moment a check for a client's view runs at: within [newest - max_rewind, newest].
    f64 clamp_view(f64 view_tick) const;
    // An entity's transform and box at `view_tick` (clamped), interpolated between recorded ticks.
    bool transform_at(u32 net_id, f64 view_tick, LocalTransform& out, HitBox* box = nullptr) const;
    // The nearest hit box the ray (unit `direction`) crosses within max_distance at `view_tick`
    // (clamped), skipping `ignore_net_id` (the shooter).
    bool raycast(f64 view_tick, math::vec3 origin, math::vec3 direction, f32 max_distance, RewindHit& hit,
                 u32 ignore_net_id = 0) const;

    usize frames() const { return count_; }
    u32 newest_tick() const;

private:
    struct Record {
        u32 net_id;
        Entity entity;
        LocalTransform transform;
        HitBox box;
    };
    struct Frame {
        u32 tick = 0;
        Array<Record> records;  // by net id
    };
    const Frame& at(usize i) const { return frames_[(head_ + i) % frames_.size()]; }
    static const Record* find(const Frame& f, u32 net_id);
    // The frames around `tick` and the blend between them (a == b when outside or exact).
    void bracket(f64 tick, const Frame*& a, const Frame*& b, f32& t) const;

    LagCompensationDesc desc_;
    Array<Frame> frames_;  // a ring, oldest at head_
    usize head_ = 0;
    usize count_ = 0;
};

// Ray against an oriented box: the entry distance in [0, max_distance], or false.
bool ray_hits_box(const LocalTransform& t, const HitBox& box, math::vec3 origin, math::vec3 direction, f32 max_distance,
                  f32& distance);

}  // namespace mce::net
