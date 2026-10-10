#include "mce/net/replication.h"

#include <algorithm>

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
            m = entities_.try_emplace(en.id.id, Mirror{e, en.id, generation_}).first;
            if (hooks_.on_spawn) hooks_.on_spawn(hooks_.user, cmd, e, en.id);
            ++result.spawned;
        } else {
            cmd.set(m->entity, en.transform);
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
    has_tick_ = false;
    last_tick_ = 0;
}

}  // namespace mce::net
