#include "mce/net/replication.h"

#include <algorithm>
#include <cmath>

namespace mce::net {
namespace {

void write_transform(ByteWriter& w, const LocalTransform& t) {
    w.write_f32(t.position.x);
    w.write_f32(t.position.y);
    w.write_f32(t.position.z);
    w.write_f32(t.rotation.x);
    w.write_f32(t.rotation.y);
    w.write_f32(t.rotation.z);
    w.write_f32(t.rotation.w);
    w.write_f32(t.scale.x);
    w.write_f32(t.scale.y);
    w.write_f32(t.scale.z);
}

LocalTransform read_transform(ByteReader& r) {
    LocalTransform t;
    t.position.x = r.read_f32();
    t.position.y = r.read_f32();
    t.position.z = r.read_f32();
    t.rotation.x = r.read_f32();
    t.rotation.y = r.read_f32();
    t.rotation.z = r.read_f32();
    t.rotation.w = r.read_f32();
    t.scale.x = r.read_f32();
    t.scale.y = r.read_f32();
    t.scale.z = r.read_f32();
    return t;
}

// Smallest possible entry: id + kind varints, a 40-byte transform, an empty extra blob.
constexpr usize kMinEntryBytes = 1 + 1 + 40 + 1;

}  // namespace

// ---- server ---------------------------------------------------------------------------------

u32 ReplicationServer::assign(Commands& cmd, Entity e, u32 kind) {
    const u32 id = next_id_++;
    cmd.add(e, NetId{id, kind});
    return id;
}

u32 ReplicationServer::assign(World& world, Entity e, u32 kind) {
    const u32 id = next_id_++;
    world.add(e, NetId{id, kind});
    return id;
}

void ReplicationServer::write_snapshot(World& world, u32 tick, ByteWriter& out) const {
    struct Item {
        u32 id;
        Entity e;
    };
    Array<Item> items;
    world.query<const NetId, const LocalTransform>().each(
        [&](Entity e, const NetId& id, const LocalTransform&) { items.push_back(Item{id.id, e}); });
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.id < b.id; });

    out.append(Span<const u8>(kSnapshotMagic, 3));
    out.write_u8(kSnapshotVersion);
    out.write_u32(tick);
    out.write_varu32(static_cast<u32>(items.size()));
    ByteWriter extra;
    for (const Item& it : items) {
        const NetId& id = *world.try_get<NetId>(it.e);
        out.write_varu32(id.id);
        out.write_varu32(id.kind);
        write_transform(out, *world.try_get<LocalTransform>(it.e));
        extra.clear();
        if (hooks_.write_extra) hooks_.write_extra(hooks_.user, world, it.e, extra);
        out.write_bytes(extra.data());
    }
}

// ---- client ---------------------------------------------------------------------------------

ApplyResult ReplicationClient::apply(Commands& cmd, const World& world, Span<const u8> snapshot) {
    ApplyResult result;
    ByteReader r(snapshot);
    const u8 m0 = r.read_u8(), m1 = r.read_u8(), m2 = r.read_u8();
    const u8 version = r.read_u8();
    const u32 tick = r.read_u32();
    const u32 count = r.read_varu32();
    result.tick = tick;
    if (!r.ok() || m0 != kSnapshotMagic[0] || m1 != kSnapshotMagic[1] || m2 != kSnapshotMagic[2] ||
        version != kSnapshotVersion || count > r.remaining() / kMinEntryBytes) {
        return result;
    }
    // Sequenced delivery already drops late packets; this also covers an unordered channel.
    // Serial-number comparison, so a tick counter that wraps keeps working. A new server session
    // starts its ticks again: the client calls reset() on disconnect.
    if (has_tick_ && tick != last_tick_ && static_cast<i32>(tick - last_tick_) < 0) return result;

    // Pass 1: parse and validate everything before queuing any command.
    entries_.clear();
    entries_.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        Entry en;
        en.id.id = r.read_varu32();
        en.id.kind = r.read_varu32();
        en.transform = read_transform(r);
        en.extra = r.read_bytes();
        if (!r.ok() || en.id.id == 0) return result;
        entries_.push_back(en);
    }
    if (!r.at_end()) return result;

    // A repeat of the newest snapshot says nothing about timing.
    if (interpolate_ && !(has_tick_ && tick == last_tick_)) sample_arrival(tick);

    // Pass 2: mirror.
    ++generation_;
    for (const Entry& en : entries_) {
        Mirror* m = entities_.find(en.id.id);
        if (m && (!world.alive(m->entity) || m->id.kind != en.id.kind)) {
            // Destroyed locally, or the server reused the id for another kind: rebuild it.
            if (world.alive(m->entity)) {
                if (hooks_.on_despawn) hooks_.on_despawn(hooks_.user, cmd, m->entity, m->id);
                cmd.destroy(m->entity);
            }
            entities_.erase(en.id.id);
            m = nullptr;
        }
        if (!m) {
            const Entity e = cmd.spawn(en.id, en.transform);
            Mirror fresh;
            fresh.entity = e;
            fresh.id = en.id;
            fresh.seen = generation_;
            fresh.predicted = is_predicted(en.id.id);
            fresh.latest = en.transform;
            m = entities_.try_emplace(en.id.id, fresh).first;
            if (interpolate_ && !m->predicted) m->push(tick, en.transform);
            if (hooks_.on_spawn) hooks_.on_spawn(hooks_.user, cmd, e, en.id);
            ++result.spawned;
        } else {
            m->latest = en.transform;
            if (m->predicted) {
                // The game's Predictor draws it.
            } else if (interpolate_) {
                m->push(tick, en.transform);  // update() draws it
            } else {
                cmd.set(m->entity, en.transform);
            }
            m->seen = generation_;
            ++result.updated;
        }
        if (hooks_.read_extra) {
            ByteReader extra(en.extra);
            hooks_.read_extra(hooks_.user, cmd, m->entity, extra);
        }
    }
    // Everything the snapshot no longer lists is gone on the server.
    Array<u32> gone;
    for (auto&& [net_id, m] : entities_) {
        if (m.seen != generation_) gone.push_back(net_id);
    }
    for (u32 net_id : gone) {
        const Mirror m = *entities_.find(net_id);
        entities_.erase(net_id);
        if (world.alive(m.entity)) {
            if (hooks_.on_despawn) hooks_.on_despawn(hooks_.user, cmd, m.entity, m.id);
            cmd.destroy(m.entity);
        }
        ++result.despawned;
    }

    last_tick_ = tick;
    has_tick_ = true;
    result.ok = true;
    return result;
}

void ReplicationClient::set_predicted(u32 net_id, bool predicted) {
    usize i = 0;
    while (i < predicted_.size() && predicted_[i] != net_id) ++i;
    if (predicted && i == predicted_.size()) predicted_.push_back(net_id);
    if (!predicted && i < predicted_.size()) predicted_.erase(i);
    if (Mirror* m = entities_.find(net_id)) {
        m->predicted = predicted;
        m->count = 0;  // interpolation starts over from the next snapshot
        m->head = 0;
    }
}

bool ReplicationClient::is_predicted(u32 net_id) const {
    for (u32 id : predicted_)
        if (id == net_id) return true;
    return false;
}

const LocalTransform* ReplicationClient::latest_transform(u32 net_id) const {
    const Mirror* m = entities_.find(net_id);
    return m ? &m->latest : nullptr;
}

Entity ReplicationClient::find(u32 net_id) const {
    const Mirror* m = entities_.find(net_id);
    return m ? m->entity : Entity::null;
}

void ReplicationClient::reset(Commands& cmd, const World& world) {
    for (auto&& [net_id, m] : entities_) {
        (void)net_id;
        if (!world.alive(m.entity)) continue;
        if (hooks_.on_despawn) hooks_.on_despawn(hooks_.user, cmd, m.entity, m.id);
        cmd.destroy(m.entity);
    }
    entities_.clear();
    predicted_.clear();
    has_tick_ = false;
    last_tick_ = 0;
    has_offset_ = false;
    has_render_ = false;
    jitter_s_ = 0.0;
    interval_s_ = 0.0;
}

// ---- interpolation (M-NET2 step 2) ------------------------------------------------------------

void ReplicationClient::Mirror::push(u32 tick, const LocalTransform& t) {
    if (count == kHistory) {
        head = (head + 1) % kHistory;
        --count;
    }
    Sample& s = history[(head + count) % kHistory];
    s.tick = tick;
    s.transform = t;
    ++count;
}

void ReplicationClient::set_interpolation(const InterpolationDesc& desc) {
    interp_ = desc;
    if (!(interp_.tick_rate > 0.0)) interp_.tick_rate = 60.0;
    if (interp_.max_delay_s < interp_.min_delay_s) interp_.max_delay_s = interp_.min_delay_s;
    interpolate_ = true;
}

void ReplicationClient::sample_arrival(u32 tick) {
    const f64 server_s = static_cast<f64>(tick) / interp_.tick_rate;
    const f64 offset = server_s - clock_s_;
    if (!has_offset_) {
        offset_s_ = offset;
        jitter_s_ = 0.0;
        interval_s_ = 1.0 / interp_.tick_rate;
        has_offset_ = true;
        return;
    }
    // The earliest arrivals give the offset (a late packet only says the path was slow); it drifts
    // down slowly so a lasting rise in latency is followed.
    if (offset > offset_s_) {
        offset_s_ = offset;
    } else {
        offset_s_ += (offset - offset_s_) * 0.01;
    }
    // Jitter: how late arrivals run behind the earliest, as a slowly decaying peak (rises fast,
    // falls slowly), so the buffer covers the late tail and not just the average.
    const f64 late = offset_s_ - offset;
    jitter_s_ += (late - jitter_s_) * (late > jitter_s_ ? 0.25 : 0.005);
    if (has_tick_) {
        const i32 ticks = static_cast<i32>(tick - last_tick_);
        if (ticks > 0) interval_s_ += (static_cast<f64>(ticks) / interp_.tick_rate - interval_s_) * 0.1;
    }
}

LocalTransform ReplicationClient::sample(const Mirror& m, f64 render_tick, InterpolationStats& stats) const {
    const Sample& newest = m.at(m.count - 1);
    if (render_tick >= static_cast<f64>(newest.tick)) {
        const f64 over = render_tick - static_cast<f64>(newest.tick);
        if (over <= 0.0) {
            ++stats.interpolated;
            return newest.transform;
        }
        if (m.count >= 2 && over / interp_.tick_rate <= interp_.max_extrapolation_s) {
            const Sample& prev = m.at(m.count - 2);
            const f64 span = static_cast<f64>(newest.tick) - static_cast<f64>(prev.tick);
            if (span > 0.0) {
                LocalTransform t = newest.transform;
                const f32 k = static_cast<f32>(over / span);
                t.position = newest.transform.position + (newest.transform.position - prev.transform.position) * k;
                ++stats.extrapolated;
                return t;
            }
        }
        ++stats.held;
        return newest.transform;
    }
    const Sample& oldest = m.at(0);
    if (render_tick <= static_cast<f64>(oldest.tick)) {
        ++stats.held;
        return oldest.transform;
    }
    u32 i = m.count - 1;
    while (i > 0 && static_cast<f64>(m.at(i).tick) > render_tick) --i;
    const Sample& a = m.at(i);
    const Sample& b = m.at(i + 1);
    const f32 t = static_cast<f32>((render_tick - static_cast<f64>(a.tick)) /
                                   (static_cast<f64>(b.tick) - static_cast<f64>(a.tick)));
    LocalTransform out;
    out.position = math::lerp(a.transform.position, b.transform.position, t);
    out.rotation = math::slerp(a.transform.rotation, b.transform.rotation, t);
    out.scale = math::lerp(a.transform.scale, b.transform.scale, t);
    ++stats.interpolated;
    return out;
}

InterpolationStats ReplicationClient::update(Commands& cmd, const World& world, f64 dt_s) {
    stats_ = InterpolationStats{};
    if (!interpolate_) return stats_;
    clock_s_ += dt_s > 0.0 ? dt_s : 0.0;
    if (!has_offset_) return stats_;
    const f64 delay = interp_.delay_s > 0.0
                          ? interp_.delay_s
                          : std::min(interp_.max_delay_s, std::max(interp_.min_delay_s, 2.0 * interval_s_ + jitter_s_));
    // The render time runs at the client's pace and is steered toward its target by at most 10% of
    // dt (a slight speed-up or slow-down), so a change in the measured offset or jitter never makes
    // entities jump. Far off (the first snapshot, a long stall): it snaps.
    const f64 target = clock_s_ + offset_s_ - delay;
    const f64 dt = dt_s > 0.0 ? dt_s : 0.0;
    if (!has_render_ || std::fabs(target - (render_s_ + dt)) > 0.25) {
        render_s_ = target;
        has_render_ = true;
    } else {
        render_s_ += dt;
        const f64 err = target - render_s_;
        const f64 step = 0.1 * dt;
        render_s_ += std::min(step, std::max(-step, err));
    }
    const f64 render_tick = render_s_ * interp_.tick_rate;
    stats_.delay_s = static_cast<f32>(clock_s_ + offset_s_ - render_s_);
    stats_.jitter_s = static_cast<f32>(jitter_s_);
    stats_.interval_s = static_cast<f32>(interval_s_);
    for (auto&& [net_id, m] : entities_) {
        (void)net_id;
        if (m.predicted) {
            ++stats_.predicted;
            continue;
        }
        if (m.count == 0 || !world.alive(m.entity)) continue;
        cmd.set(m.entity, sample(m, render_tick, stats_));
    }
    return stats_;
}

}  // namespace mce::net
