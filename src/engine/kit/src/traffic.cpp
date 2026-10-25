#include "mce/kit/traffic.h"

#include <algorithm>
#include <cmath>

#include "kit3d_internal.h"
#include "mce/asset/asset_system.h"
#include "mce/ecs/prefab.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/crowd.h"
#include "mce/kit/roads.h"
#include "mce/kit/vehicles3d.h"
#include "mce/log.h"

namespace mce::kit {

using asset::kRoadNone;
using asset::RoadLane;
using asset::RoadLaneKind;

// Padding-free: World::state_hash hashes component bytes.
static_assert(sizeof(TrafficDriver3D) == 104);
static_assert(sizeof(TrafficSpawner3D) == 88);

namespace {

constexpr f32 kDeg = 0.017453292519943295f;
constexpr f32 kCornerAccel = 4.0f;  // m/s^2 sideways the steering allows
constexpr f32 kStopLine = 1.0f;     // m short of a connector's start a car stops
constexpr f32 kStandAtStop = 1.0f;  // s a car stands at a stop line before it may go
constexpr f32 kWalkerClear = 1.2f;  // m beside its lane a crossing pedestrian still stops a car
constexpr f32 kWalkerGap = 3.0f;    // m short of a crossing pedestrian a car stops
constexpr f32 kWalkerStep = 1.5f;   // m between the points of its lanes a car checks

struct TrafficAssets {
    asset::AssetSystem* assets = nullptr;
};

u32 mix(u32 a, u32 b) {
    u32 h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

math::vec3 flat(math::vec3 v) { return {v.x, 0.0f, v.z}; }

// Projects world point `p` onto `lane` (flat): s along it and the distance off it.
void project(const RoadNet& net, u32 lane, math::vec3 p, f32& s, f32& dist) {
    const RoadRegistry::Def& d = *net.def;
    const RoadLane& l = d.lanes[lane];
    const math::vec3 q = p - net.origin;
    f32 best = 1e30f;
    s = 0.0f;
    for (u32 k = 0; k + 1 < l.point_count; ++k) {
        const asset::RoadPoint& a = d.points[l.first_point + k];
        const asset::RoadPoint& b = d.points[l.first_point + k + 1];
        const f32 dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz;
        f32 t = l2 > 1e-12f ? ((q.x - a.x) * dx + (q.z - a.z) * dz) / l2 : 0.0f;
        // Beyond the lane's ends the projection runs on along the end segments, so s can pass the length.
        if (k > 0) t = math::max(t, 0.0f);
        if (k + 2 < l.point_count) t = math::min(t, 1.0f);
        const f32 ex = a.x + dx * t - q.x, ez = a.z + dz * t - q.z;
        const f32 e = std::sqrt(ex * ex + ez * ez);
        if (e < best) {
            best = e;
            s = a.s + (b.s - a.s) * t;
        }
    }
    dist = best;
}

// The successor of `lane` a driver takes: its only one, or one picked by the driver's hash, leaving
// U-turns out while another way exists.
u32 choose(const RoadRegistry::Def& d, u32 lane, TrafficDriver3D& t, Entity e) {
    const RoadLane& l = d.lanes[lane];
    if (l.next_count == 0) return kRoadNone;
    if (l.next_count == 1) return l.first_next;
    u32 options[16];
    u32 n = 0;
    for (u32 k = 0; k < l.next_count && n < 16; ++k)
        if (d.lanes[l.first_next + k].index != u8(asset::RoadTurn::UTurn)) options[n++] = l.first_next + k;
    if (n == 0) return l.first_next;
    const u32 pick = options[mix(mix(t.seed, e.index), t.turns) % n];
    ++t.turns;
    return pick;
}

struct Car {
    Entity e;
    TrafficDriver3D* d = nullptr;
    Vehicle3D* v = nullptr;
    const RoadNet* net = nullptr;
    math::vec3 pos;
    math::quat rot;
    f32 speed = 0.0f;
};

// A car on a lane, for the occupancy table.
struct Slot {
    u32 lane;
    f32 s;
    u32 car;
};

bool slot_less(const Slot& a, const Slot& b) {
    if (a.lane != b.lane) return a.lane < b.lane;
    if (a.s != b.s) return a.s < b.s;
    return a.car < b.car;
}

struct Occupancy {
    Array<Slot> slots;  // sorted by lane, then s
    // The first car on `lane` with s above `after` (and not `self`); null when none.
    const Slot* ahead(u32 lane, f32 after, u32 self) const {
        const Slot key{lane, after, 0};
        const Slot* it = std::upper_bound(slots.begin(), slots.end(), key, [](const Slot& a, const Slot& b) {
            return a.lane < b.lane || (a.lane == b.lane && a.s < b.s);
        });
        for (; it != slots.end() && it->lane == lane; ++it)
            if (it->car != self) return it;
        return nullptr;
    }
    bool any(u32 lane, u32 self) const { return ahead(lane, -1e30f, self) != nullptr; }
};

// The world position of the car's entity.
bool pose_of(World& w, Entity e, math::vec3& pos, math::quat& rot) {
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    if (!t) return false;
    math::vec3 scale;
    world_pose(w, e, *t, pos, rot, scale);
    return true;
}

// Re-snaps a car to the nearest lane running its way.
bool snap(Car& c) {
    RoadNearestQuery q;
    q.max_distance = math::max(c.d->lost_distance * 3.0f, 10.0f);
    q.heading = flat(math::rotate(c.rot, math::vec3{0.0f, 0.0f, -1.0f}));
    RoadSpot spot;
    if (!road_nearest(*c.net, c.pos, spot, q)) return false;
    c.d->lane = spot.lane;
    c.d->s = spot.s;
    c.d->next = c.d->after = kRoadNone;
    return true;
}

// Places the car on its lanes: projects it, moves on to the next lane once it passed the end.
void locate(Car& c) {
    TrafficDriver3D& d = *c.d;
    const RoadRegistry::Def& def = *c.net->def;
    if (d.lane >= def.lanes.size() || (d.next != kRoadNone && d.next >= def.lanes.size()) ||
        (d.after != kRoadNone && d.after >= def.lanes.size())) {
        d.lane = kRoadNone;
        if (!snap(c)) {
            d.state = TrafficState::Lost;
            return;
        }
    }
    f32 s = 0.0f, dist = 0.0f;
    project(*c.net, d.lane, c.pos, s, dist);
    for (u32 hop = 0; hop < 3 && d.next != kRoadNone && s >= def.lanes[d.lane].length - 0.05f; ++hop) {
        f32 s2 = 0.0f, dist2 = 0.0f;
        project(*c.net, d.next, c.pos, s2, dist2);
        if (s2 < 0.0f && s < def.lanes[d.lane].length + 0.5f) break;  // not onto the next lane yet
        d.lane = d.next;
        d.next = d.after;
        d.after = kRoadNone;
        d.stopped_for = 0.0f;
        s = s2;
        dist = dist2;
    }
    if (dist > d.lost_distance) {
        if (!snap(c)) {
            d.state = TrafficState::Lost;
            return;
        }
        project(*c.net, d.lane, c.pos, s, dist);
    }
    d.s = math::clamp(s, 0.0f, def.lanes[d.lane].length);
    if (d.state == TrafficState::Lost || d.state == TrafficState::Idle) d.state = TrafficState::Driving;
}

// IDM acceleration toward desired speed v0 behind an obstacle `gap` metres ahead moving at `vl`.
f32 idm(const TrafficDriver3D& d, f32 v, f32 v0, f32 gap, f32 vl) {
    const f32 free = 1.0f - std::pow(math::max(v, 0.0f) / math::max(v0, 0.1f), 4.0f);
    if (gap >= 1e8f) return d.accel * free;
    const f32 s_star = d.min_gap + math::max(0.0f, v * d.headway + v * (v - vl) / (2.0f * std::sqrt(d.accel * d.decel)));
    const f32 g = math::max(gap, 0.1f);
    return d.accel * (free - (s_star / g) * (s_star / g));
}

struct Drive {
    World& w;
    f32 dt;
    Array<Car>& cars;
    const Occupancy& occ;
    const Array<math::vec3>& walkers;  // pedestrians crossing a road now (B3), world positions
    Array<Slot> claimed;               // lanes cars changed into this tick (the occupancy table is from before), M-TRAFFICAVOID

    // How far down its lanes (`lanes`, starting `start` metres ahead) car `ci` first passes a crossing
    // pedestrian, within `reach` metres; a negative number when none.
    f32 walker_ahead(u32 ci, const u32* lanes, const f32* start, f32 reach) const {
        const Car& c = cars[ci];
        const RoadRegistry::Def& d = *c.net->def;
        // Only the pedestrians near enough to matter.
        u32 near[32];
        u32 n = 0;
        for (u32 k = 0; k < walkers.size() && n < 32; ++k)
            if (math::length(flat(walkers[k] - c.pos)) < reach + 10.0f) near[n++] = k;
        if (n == 0) return -1.0f;
        for (u32 i = 0; i < 3 && lanes[i] != kRoadNone; ++i) {
            const RoadLane& l = d.lanes[lanes[i]];
            const f32 half = 0.5f * l.width + kWalkerClear;
            for (f32 s = math::max(-start[i], 0.0f); s <= l.length; s += kWalkerStep) {
                const f32 ahead = start[i] + s;
                if (ahead > reach) return -1.0f;
                const math::vec3 p = road_lane_point(*c.net, lanes[i], s);
                for (u32 k = 0; k < n; ++k)
                    if (math::length(flat(walkers[near[k]] - p)) < half) return ahead;
            }
        }
        return -1.0f;
    }

    // When car `ci` would reach the start of connector `conn` from where it is, in seconds.
    f32 eta(u32 ci, u32 conn) const {
        const Car& c = cars[ci];
        const RoadRegistry::Def& d = *c.net->def;
        f32 dist = d.lanes[c.d->lane].length - c.d->s;
        if (c.d->lane == conn) return 0.0f;
        if (c.d->next != conn) {
            if (c.d->after != conn || c.d->next == kRoadNone) return 1e9f;
            dist += d.lanes[c.d->next].length;
        }
        return dist / math::max(c.speed, 1.0f);
    }

    // Whether car `ci` may not enter connector `conn`, `dist` metres ahead, now; sets its state.
    bool held(u32 ci, u32 conn, f32 dist) {
        Car& c = cars[ci];
        TrafficDriver3D& t = *c.d;
        const RoadRegistry::Def& d = *c.net->def;
        const RoadLane& l = d.lanes[conn];
        f32 left = 0.0f;
        const RoadLight light = road_light(*c.net, conn, &left);
        const f32 v = math::max(c.speed, 0.0f);
        const f32 can_stop = v * v / (2.0f * t.decel) + kStopLine;
        if (light == RoadLight::Red || (light == RoadLight::Amber && dist > can_stop)) {
            t.state = TrafficState::Waiting;
            return true;
        }
        if ((l.flags & asset::RoadLaneFlags::Stop) && dist < can_stop + 20.0f) {
            if (dist < kStopLine + 2.5f && v < 0.5f) t.stopped_for += dt;
            if (t.stopped_for < kStandAtStop) {
                t.state = TrafficState::Yielding;
                return true;
            }
        }
        // Give way only near the junction: far off, the cars there will have moved.
        if (dist > math::max(25.0f, v * t.yield_time + can_stop)) return false;
        const f32 mine = dist / math::max(v, 1.0f);
        for (u32 k = 0; k < l.conflict_count; ++k) {
            const u32 other = d.conflicts[l.first_conflict + k];
            const RoadLane& o = d.lanes[other];
            if (road_light(*c.net, other) == RoadLight::Red) continue;
            if (occ.any(other, ci)) {
                t.state = TrafficState::Yielding;
                return true;
            }
            // Cars heading for the other connector that go first.
            for (const Slot* s = occ.ahead(o.prev, -1e30f, ci); s && s->lane == o.prev; ++s) {
                if (s->car == ci) continue;
                const Car& oc = cars[s->car];
                if (oc.net != c.net || oc.d->next != other) continue;
                const f32 theirs = eta(s->car, other);
                const bool first = o.rank > l.rank || (o.rank == l.rank && (theirs < mine || (theirs == mine && oc.e.index < c.e.index)));
                if (first && theirs < mine + t.yield_time) {
                    t.state = TrafficState::Yielding;
                    return true;
                }
            }
        }
        return false;
    }

    // M-TRAFFICAVOID: the road lane beside `lane` going the same way (`side` +-1 in lane index), or kRoadNone.
    static u32 neighbour(const RoadRegistry::Def& d, u32 lane, int side) {
        const RoadLane& l = d.lanes[lane];
        if (l.kind != RoadLaneKind::Road) return kRoadNone;
        for (u32 i = 0; i < d.lanes.size(); ++i) {
            const RoadLane& o = d.lanes[i];
            if (o.kind == RoadLaneKind::Road && o.road == l.road && o.node == l.node && static_cast<int>(o.index) == static_cast<int>(l.index) + side) return i;
        }
        return kRoadNone;
    }

    // A car stuck behind a stopped car that nobody drives (a wreck, a parked car) takes a free lane beside it.
    void dodge(u32 ci) {
        Car& c = cars[ci];
        TrafficDriver3D& t = *c.d;
        const RoadRegistry::Def& d = *c.net->def;
        if (t.lane >= d.lanes.size() || d.lanes[t.lane].kind != RoadLaneKind::Road) return;
        const Slot* ahead = occ.ahead(t.lane, t.s, ci);
        if (!ahead) return;
        const Car& lc = cars[ahead->car];
        if (lc.d->drive || std::fabs(lc.speed) > 0.5f) return;
        const f32 gap = ahead->s - t.s - lc.d->length;
        const f32 v = math::max(c.speed, 0.0f);
        if (gap > math::max(30.0f, v * v / (2.0f * t.decel) + 10.0f)) return;
        for (const int side : {-1, 1}) {
            const u32 n = neighbour(d, t.lane, side);
            if (n == kRoadNone) continue;
            f32 s_n = 0.0f, dist = 0.0f;
            project(*c.net, n, c.pos, s_n, dist);
            const f32 lo = s_n - 10.0f;
            const f32 hi = s_n + (ahead->s - t.s) + 8.0f;
            const Slot* other = occ.ahead(n, lo, ci);
            if (other && other->lane == n && other->s < hi) continue;
            bool taken = false;
            for (const Slot& k : claimed) taken |= k.lane == n && k.s > lo && k.s < hi;
            if (taken) continue;
            claimed.push_back(Slot{n, s_n, ci});
            if (t.home == kRoadNone) t.home = t.lane;
            t.lane = n;
            t.s = math::clamp(s_n, 0.0f, d.lanes[n].length);
            t.next = kRoadNone;
            t.after = kRoadNone;
            t.stopped_for = 0.0f;
            return;
        }
    }

    // A car that went round a wreck returns to the lane it left once it is clear of it: nobody in the way
    // beside it, no other stopped car undriven ahead in that lane (it would only dodge again).
    void rejoin(u32 ci) {
        Car& c = cars[ci];
        TrafficDriver3D& t = *c.d;
        const RoadRegistry::Def& d = *c.net->def;
        const u32 h = t.home;
        if (h >= d.lanes.size() || t.lane >= d.lanes.size() || h == t.lane) {
            t.home = kRoadNone;
            return;
        }
        const RoadLane& l = d.lanes[t.lane];
        const RoadLane& o = d.lanes[h];
        // It drove on to another road piece, or the lane is no longer beside its own: forget the way back.
        if (l.kind != RoadLaneKind::Road || o.kind != RoadLaneKind::Road || o.road != l.road || o.node != l.node ||
            std::abs(static_cast<int>(o.index) - static_cast<int>(l.index)) != 1) {
            t.home = kRoadNone;
            return;
        }
        f32 s_h = 0.0f, dist = 0.0f;
        project(*c.net, h, c.pos, s_h, dist);
        const f32 lo = s_h - 10.0f;
        const f32 hi = s_h + 12.0f;
        const Slot* other = occ.ahead(h, lo, ci);
        if (other && other->lane == h && other->s < hi) return;
        if (other && other->lane == h && other->s < s_h + 60.0f) {
            const Car& oc = cars[other->car];
            if (oc.d->drive == 0 && std::fabs(oc.speed) <= 0.5f) return;
        }
        for (const Slot& k : claimed)
            if (k.lane == h && k.s > lo && k.s < hi) return;
        claimed.push_back(Slot{h, s_h, ci});
        t.lane = h;
        t.s = math::clamp(s_h, 0.0f, o.length);
        t.next = kRoadNone;
        t.after = kRoadNone;
        t.stopped_for = 0.0f;
        t.home = kRoadNone;
    }

    void drive(u32 ci) {
        Car& c = cars[ci];
        TrafficDriver3D& t = *c.d;
        Vehicle3D& v = *c.v;
        const RoadRegistry::Def& d = *c.net->def;
        if (t.state != TrafficState::Lost && t.lane < d.lanes.size()) {
            if (t.home != kRoadNone) rejoin(ci);
            dodge(ci);
        }
        if (t.state == TrafficState::Lost || t.lane >= d.lanes.size()) {
            t.target_speed = 0.0f;
            v.throttle = v.steer = 0.0f;
            v.brake = 1.0f;
            return;
        }
        const f32 speed = c.speed;
        const RoadLane& l0 = d.lanes[t.lane];
        const f32 limit = [&](const RoadLane& l) {
            const f32 lim = l.speed * t.speed_factor;
            return t.max_speed > 0.0f ? math::min(lim, t.max_speed) : lim;
        }(l0);
        f32 v0 = limit;
        f32 accel = idm(t, speed, v0, 1e9f, 0.0f);
        t.state = TrafficState::Driving;
        t.leader = Entity{};
        const auto obstacle = [&](f32 gap, f32 vl) { accel = math::min(accel, idm(t, speed, v0, gap, vl)); };
        // The lanes ahead: this one, the next and the one after, with the distance to each's start.
        const u32 lanes[3] = {t.lane, t.next, t.next == kRoadNone ? kRoadNone : t.after};
        f32 start[3] = {-t.s, 0.0f, 0.0f};
        start[1] = l0.length - t.s;
        start[2] = start[1] + (lanes[1] != kRoadNone ? d.lanes[lanes[1]].length : 0.0f);
        bool found_leader = false;
        bool blocked = false;
        for (u32 i = 0; i < 3 && lanes[i] != kRoadNone; ++i) {
            const RoadLane& li = d.lanes[lanes[i]];
            // A bend: the connector's cooked speed, braked to in time.
            if (li.kind == RoadLaneKind::Connector) {
                const f32 vc = li.speed * t.speed_factor;
                const f32 dist = math::max(start[i], 0.0f);
                v0 = math::min(v0, std::sqrt(vc * vc + 2.0f * t.decel * dist));
            }
            // A connector it may not enter yet: a stop line.
            if (!blocked && i > 0 && li.kind == RoadLaneKind::Connector && held(ci, lanes[i], start[i] - kStopLine)) {
                obstacle(start[i] - kStopLine, 0.0f);
                blocked = true;
            }
            // The car ahead.
            if (!found_leader) {
                const Slot* s = occ.ahead(lanes[i], i == 0 ? t.s : -1e30f, ci);
                if (s) {
                    const Car& lc = cars[s->car];
                    obstacle(start[i] + s->s - lc.d->length, math::max(lc.speed, 0.0f));
                    t.leader = lc.e;
                    found_leader = true;
                }
            }
            if (blocked) break;
        }
        // A pedestrian crossing in front (unsignalled crossings leave the giving way to the cars).
        if (!walkers.empty()) {
            const f32 can_stop = speed * speed / (2.0f * t.decel);
            const f32 at = walker_ahead(ci, lanes, start, math::max(can_stop + 15.0f, 20.0f));
            if (at >= 0.0f) {
                obstacle(at - kWalkerGap, 0.0f);
                t.state = TrafficState::Yielding;
                blocked = true;
            }
        }
        accel = math::min(accel, idm(t, speed, v0, 1e9f, 0.0f));
        if (!blocked && found_leader && accel < 0.0f) t.state = TrafficState::Following;
        t.target_speed = math::max(0.0f, speed + accel);

        // Steering: pure pursuit at a point look_ahead down the lanes.
        const f32 reach = math::max(t.look_ahead, 0.6f * std::fabs(speed));
        f32 along = t.s + reach;
        u32 aim_lane = t.lane;
        for (u32 i = 1; i < 3 && along > d.lanes[aim_lane].length && lanes[i] != kRoadNone; ++i) {
            along -= d.lanes[aim_lane].length;
            aim_lane = lanes[i];
        }
        const math::vec3 aim = road_lane_point(*c.net, aim_lane, along);
        const math::vec3 ahead = flat(math::rotate(c.rot, math::vec3{0.0f, 0.0f, -1.0f}));
        const math::vec3 right{-ahead.z, 0.0f, ahead.x};
        const math::vec3 to = flat(aim - c.pos);
        const f32 error = std::atan2(math::dot(to, right), math::dot(to, ahead));
        const f32 base = math::max(std::fabs(v.rear_axle - v.front_axle), 0.5f);
        const f32 dist = math::max(math::length(to), 1.0f);
        f32 angle = std::atan(2.0f * base * std::sin(error) / dist);
        if (speed * speed > 1.0f) {
            const f32 most = std::atan(1.5f * kCornerAccel * base / (speed * speed));
            angle = math::clamp(angle, -most, most);
        }
        v.steer = math::clamp(angle / math::max(v.max_steer * kDeg, 1e-3f), -1.0f, 1.0f);
        // Pedals from the acceleration it wants.
        v.handbrake = 0.0f;
        if (t.target_speed < 0.3f && speed < 1.0f) {
            v.throttle = 0.0f;
            v.brake = 1.0f;
        } else if (accel >= 0.0f) {
            v.throttle = math::clamp(0.25f + accel / t.accel * 0.5f, 0.0f, 1.0f) * (speed < v0 ? 1.0f : 0.0f);
            v.brake = 0.0f;
        } else {
            v.throttle = 0.0f;
            v.brake = accel < -0.3f ? math::clamp(-accel / (2.0f * t.decel), 0.0f, 1.0f) : 0.0f;
        }
    }
};

struct NetCache {
    Entity key;  // the driver's `network` (null = the first)
    RoadNet net;
    bool ok = false;
};

const RoadNet* net_for(World& w, Array<NetCache>& cache, Entity network) {
    for (NetCache& n : cache)
        if (n.key == network) return n.ok ? &n.net : nullptr;
    NetCache n;
    n.key = network;
    n.ok = network.is_null() ? road_net_first(w, n.net) : road_net(w, network, n.net);
    cache.push_back(n);
    return n.ok ? &cache.back().net : nullptr;
}

// FixedUpdate_Pre, after the bodies and ahead of the vehicles: every driver's pedals and wheel.
void traffic_drivers(Frame& f) {
    World& w = f.world();
    Array<NetCache> nets;
    nets.reserve(4);
    Array<Car> cars;
    f.query<TrafficDriver3D, Vehicle3D>().each([&](Entity e, TrafficDriver3D& d, Vehicle3D& v) {
        Car c;
        c.e = e;
        c.d = &d;
        c.v = &v;
        c.speed = v.speed;
        if (!pose_of(w, e, c.pos, c.rot)) return;
        cars.push_back(c);
    });
    // Every network is looked up before any Car points into `nets`, so its growth moves nothing a car holds.
    for (Car& c : cars) {
        const Entity key = c.d->network;
        bool known = false;
        for (const NetCache& n : nets) known |= n.key == key;
        if (!known) net_for(w, nets, key);
    }
    for (Car& c : cars) {
        for (NetCache& n : nets)
            if (n.key == c.d->network) c.net = n.ok ? &n.net : nullptr;
        if (!c.net) {
            c.d->state = TrafficState::Idle;
            continue;
        }
        locate(c);
    }
    // Their way ahead, then who is where.
    Occupancy occ;
    for (u32 i = 0; i < cars.size(); ++i) {
        Car& c = cars[i];
        if (!c.net || c.d->state == TrafficState::Lost || c.d->lane == kRoadNone) continue;
        const RoadRegistry::Def& d = *c.net->def;
        if (c.d->next == kRoadNone) c.d->next = choose(d, c.d->lane, *c.d, c.e);
        if (c.d->after == kRoadNone && c.d->next != kRoadNone) c.d->after = choose(d, c.d->next, *c.d, c.e);
        occ.slots.push_back(Slot{c.d->lane, c.d->s, i});
    }
    std::sort(occ.slots.begin(), occ.slots.end(), slot_less);
    Array<math::vec3> walkers;
    f.query<const Pedestrian3D, const LocalTransform>().each([&](Entity, const Pedestrian3D& p, const LocalTransform& lt) {
        // a fleeing pedestrian (M-PEDFLEE) runs over crossings without waiting, so its mode no longer says Cross: cars give way to it too
        if (p.mode == PedestrianMode::Cross || p.mode == PedestrianMode::Flee) walkers.push_back(lt.position);
    });
    Drive drive{w, f.dt, cars, occ, walkers, {}};
    for (u32 i = 0; i < cars.size(); ++i) {
        Car& c = cars[i];
        if (!c.net || !c.d->drive) continue;
        drive.drive(i);
    }
}

// FixedUpdate_Pre, ahead of the drivers: removes far and stuck cars, spawns new ones.
void traffic_spawners(Frame& f) {
    World& w = f.world();
    const TrafficAssets* ta = w.try_resource<TrafficAssets>();
    const TrafficSpawnHook* hook = w.try_resource<TrafficSpawnHook>();
    f.query<TrafficSpawner3D>().each([&](Entity se, TrafficSpawner3D& sp) {
        RoadNet net;
        if (!(sp.network.is_null() ? road_net_first(w, net) : road_net(w, sp.network, net))) return;
        math::vec3 focus = net.origin;
        math::quat unused;
        if (!sp.focus.is_null() && w.alive(sp.focus)) pose_of(w, sp.focus, focus, unused);
        // Its cars: remove the far and the stuck; collect where every car is for the spacing check.
        u32 alive = 0;
        Array<math::vec3> taken;
        f.query<TrafficDriver3D, LocalTransform>().each([&](Entity e, TrafficDriver3D& d, LocalTransform&) {
            math::vec3 p;
            math::quat r;
            if (!pose_of(w, e, p, r)) return;
            taken.push_back(p);
            if (!(d.spawner == se)) return;
            const f32 far = math::length(flat(p - focus));
            const Vehicle3D* v = w.try_get<Vehicle3D>(e);
            const bool stuck = d.state == TrafficState::Lost || (v && v->flipped_ticks > 0);
            d.stuck_for = stuck ? d.stuck_for + f.dt : 0.0f;
            if (far > sp.despawn_radius || (sp.stuck_after > 0.0f && d.stuck_for > sp.stuck_after && far > sp.min_radius)) {
                f.commands().destroy(e);
                ++sp.removed;
                return;
            }
            ++alive;
        });
        sp.alive = alive;
        if (!sp.active || alive >= sp.max_cars) return;
        const bool use_prefab = sp.prefab.guid != asset::Guid{} && ta && ta->assets;
        if (!use_prefab && (!hook || !hook->spawn)) return;
        // Road lanes with a point in the ring, from the grid.
        const RoadRegistry::Def& d = *net.def;
        Array<u32> lanes;
        for (u32 i = 0; i < d.lanes.size(); ++i) {
            const RoadLane& l = d.lanes[i];
            if (l.kind != RoadLaneKind::Road || l.length < 2.0f * sp.spacing) continue;
            lanes.push_back(i);
        }
        if (lanes.empty()) return;
        u32 made = 0;
        for (u32 attempt = 0; attempt < 16u * sp.per_tick && made < sp.per_tick && alive + made < sp.max_cars; ++attempt) {
            const u32 h = mix(mix(sp.seed, sp.spawned), attempt);
            const u32 lane = lanes[h % lanes.size()];
            const RoadLane& l = d.lanes[lane];
            const f32 s = sp.spacing + static_cast<f32>(mix(h, 7u) % 1000u) / 1000.0f * (l.length - 2.0f * sp.spacing);
            math::vec3 dir;
            const math::vec3 at = road_lane_point(net, lane, s, &dir);
            const f32 r = math::length(flat(at - focus));
            if (r < sp.min_radius || r > sp.spawn_radius) continue;
            bool clear = true;
            for (const math::vec3& p : taken) clear = clear && math::length(flat(p - at)) >= sp.spacing;
            if (!clear) continue;
            const math::vec3 pos = at + math::vec3{0.0f, 1.0f, 0.0f};
            const math::quat rot = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, std::atan2(-dir.x, -dir.z));
            Entity car;
            if (use_prefab) {
                const asset::AssetKey key = ta->assets->find(sp.prefab.guid);
                if (key == asset::kInvalidAssetKey) return;
                Result<asset::AssetHandle<asset::Prefab>> ph = ta->assets->load_sync<asset::Prefab>(key);
                if (!ph) return;
                const ecs::PrefabOverride o[2] = {ecs::PrefabOverride::of("LocalTransform.position", pos),
                                                  ecs::PrefabOverride::of("LocalTransform.rotation", rot)};
                ecs::SpawnParams p;
                p.overrides = Span<const ecs::PrefabOverride>(o, 2);
                Array<Entity> out;
                const Result<u32> res = ecs::instantiate(w, ta->assets->resolve(ph.value()), p, &out);
                ta->assets->release(asset::AnyHandle(ph.value()));
                if (!res || out.empty()) return;
                car = out[0];
            } else {
                car = hook->spawn(f, pos, rot, sp.spawned);
                if (car.is_null()) {
                    ++sp.spawned;
                    continue;
                }
            }
            TrafficDriver3D drv;
            drv.network = sp.network;
            drv.spawner = se;
            drv.seed = mix(sp.seed, sp.spawned);
            drv.speed_factor = 1.0f + sp.speed_spread * (static_cast<f32>(mix(drv.seed, 3u) % 2001u) / 1000.0f - 1.0f);
            drv.lane = lane;
            drv.s = s;
            f.commands().add<TrafficDriver3D>(car, drv);
            taken.push_back(at);
            ++sp.spawned;
            ++made;
        }
        sp.alive = alive + made;
    });
}

}  // namespace

void install_traffic(World& world, asset::AssetSystem* assets) {
    if (TrafficAssets* a = world.try_resource<TrafficAssets>()) {
        a->assets = assets;
        return;
    }
    install_roads(world, assets);
    install_bodies3d(world, assets);
    world.set_resource(TrafficAssets{assets});
    world.register_components<TrafficDriver3D, TrafficSpawner3D>();
    world.add_system(Phase::FixedUpdate_Pre, "traffic_spawners", &traffic_spawners, "kit3d_bodies");
    world.add_system(Phase::FixedUpdate_Pre, "traffic_drivers", &traffic_drivers, "traffic_spawners");
    install_vehicles3d(world, assets);
}

}  // namespace mce::kit
