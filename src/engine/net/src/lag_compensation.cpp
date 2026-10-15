#include "mce/net/lag_compensation.h"

#include <algorithm>
#include <cmath>

#include "mce/net/replication.h"

namespace mce::net {

void LagCompensator::init(const LagCompensationDesc& desc) {
    desc_ = desc;
    if (!(desc_.tick_rate > 0.0)) desc_.tick_rate = 60.0;
    if (!(desc_.max_rewind_s >= 0.0)) desc_.max_rewind_s = 0.0;
    // Every tick the rewind can reach, plus the one it blends with.
    const usize slots = static_cast<usize>(std::ceil(desc_.max_rewind_s * desc_.tick_rate)) + 2;
    frames_.clear();
    frames_.resize(slots);
    head_ = 0;
    count_ = 0;
}

void LagCompensator::clear() {
    head_ = 0;
    count_ = 0;
}

u32 LagCompensator::newest_tick() const { return count_ ? at(count_ - 1).tick : 0; }

void LagCompensator::record(World& world, u32 tick) {
    if (frames_.empty()) init(desc_);
    Frame* f = nullptr;
    if (count_ && at(count_ - 1).tick == tick) {
        f = &frames_[(head_ + count_ - 1) % frames_.size()];
    } else {
        if (count_ == frames_.size()) {
            head_ = (head_ + 1) % frames_.size();
            --count_;
        }
        f = &frames_[(head_ + count_) % frames_.size()];
        ++count_;
    }
    f->tick = tick;
    f->records.clear();
    world.query<const NetId, const LocalTransform, const HitBox>().each(
        [&](Entity e, const NetId& id, const LocalTransform& t, const HitBox& box) {
            f->records.push_back(Record{id.id, e, t, box});
        });
    std::sort(f->records.begin(), f->records.end(),
              [](const Record& a, const Record& b) { return a.net_id < b.net_id; });
}

f64 LagCompensator::clamp_view(f64 view_tick) const {
    if (!count_) return 0.0;
    const f64 newest = static_cast<f64>(at(count_ - 1).tick);
    const f64 oldest = std::max(static_cast<f64>(at(0).tick), newest - desc_.max_rewind_s * desc_.tick_rate);
    if (!(view_tick == view_tick)) return newest;  // NaN from a client
    return std::min(newest, std::max(oldest, view_tick));
}

const LagCompensator::Record* LagCompensator::find(const Frame& f, u32 net_id) {
    const Record* it = std::lower_bound(f.records.begin(), f.records.end(), net_id,
                                        [](const Record& r, u32 id) { return r.net_id < id; });
    return it != f.records.end() && it->net_id == net_id ? it : nullptr;
}

void LagCompensator::bracket(f64 tick, const Frame*& a, const Frame*& b, f32& t) const {
    usize i = count_ - 1;
    while (i > 0 && static_cast<f64>(at(i).tick) > tick) --i;
    a = &at(i);
    b = a;
    t = 0.0f;
    if (i + 1 < count_ && static_cast<f64>(a->tick) < tick) {
        b = &at(i + 1);
        // The same blend ReplicationClient draws with.
        t = static_cast<f32>((tick - static_cast<f64>(a->tick)) /
                             (static_cast<f64>(b->tick) - static_cast<f64>(a->tick)));
    }
}

namespace {

LocalTransform blend(const LocalTransform& a, const LocalTransform& b, f32 t) {
    if (t <= 0.0f) return a;
    LocalTransform out;
    out.position = math::lerp(a.position, b.position, t);
    out.rotation = math::slerp(a.rotation, b.rotation, t);
    out.scale = math::lerp(a.scale, b.scale, t);
    return out;
}

}  // namespace

bool LagCompensator::transform_at(u32 net_id, f64 view_tick, LocalTransform& out, HitBox* box) const {
    if (!count_) return false;
    const Frame* a = nullptr;
    const Frame* b = nullptr;
    f32 t = 0.0f;
    bracket(clamp_view(view_tick), a, b, t);
    const Record* ra = find(*a, net_id);
    const Record* rb = b != a ? find(*b, net_id) : ra;
    if (!ra && !rb) return false;
    if (!ra || !rb) {
        // Spawned or gone in between: the side that has it.
        const Record* r = ra ? ra : rb;
        out = r->transform;
        if (box) *box = r->box;
        return true;
    }
    out = blend(ra->transform, rb->transform, t);
    if (box) *box = ra->box;
    return true;
}

bool LagCompensator::raycast(f64 view_tick, math::vec3 origin, math::vec3 direction, f32 max_distance, RewindHit& hit,
                             u32 ignore_net_id) const {
    if (!count_) return false;
    const f64 tick = clamp_view(view_tick);
    const Frame* a = nullptr;
    const Frame* b = nullptr;
    f32 t = 0.0f;
    bracket(tick, a, b, t);
    bool found = false;
    f32 best = max_distance;
    for (const Record& ra : a->records) {
        if (ra.net_id == ignore_net_id) continue;
        const Record* rb = b != a ? find(*b, ra.net_id) : &ra;
        const LocalTransform tr = rb ? blend(ra.transform, rb->transform, t) : ra.transform;
        f32 d = 0.0f;
        if (ray_hits_box(tr, ra.box, origin, direction, best, d) && (!found || d < best)) {
            found = true;
            best = d;
            hit.net_id = ra.net_id;
            hit.entity = ra.entity;
            hit.distance = d;
            hit.point = origin + direction * d;
            hit.tick = tick;
        }
    }
    return found;
}

bool ray_hits_box(const LocalTransform& t, const HitBox& box, math::vec3 origin, math::vec3 direction, f32 max_distance,
                  f32& distance) {
    // Into the box's frame: unrotated, unscaled.
    const math::quat inv = math::conjugate(t.rotation);
    const math::vec3 center = t.position + math::rotate(t.rotation, box.offset * t.scale);
    const math::vec3 o = math::rotate(inv, origin - center);
    const math::vec3 d = math::rotate(inv, direction);
    const f32 half[3] = {box.half_extents.x * std::fabs(t.scale.x), box.half_extents.y * std::fabs(t.scale.y),
                         box.half_extents.z * std::fabs(t.scale.z)};
    const f32 oo[3] = {o.x, o.y, o.z};
    const f32 dd[3] = {d.x, d.y, d.z};
    f32 enter = 0.0f, leave = max_distance;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(dd[axis]) < 1e-12f) {
            if (oo[axis] < -half[axis] || oo[axis] > half[axis]) return false;
            continue;
        }
        f32 t0 = (-half[axis] - oo[axis]) / dd[axis];
        f32 t1 = (half[axis] - oo[axis]) / dd[axis];
        if (t0 > t1) std::swap(t0, t1);
        enter = std::max(enter, t0);
        leave = std::min(leave, t1);
        if (enter > leave) return false;
    }
    distance = enter;
    return true;
}

}  // namespace mce::net
