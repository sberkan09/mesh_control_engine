#include "mce/net/replication.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "mce/hash.h"

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

// Smallest possible entry: id + kind varints, a 40-byte transform, empty extra and field blobs.
constexpr usize kMinEntryBytes = 1 + 1 + 40 + 1 + 1;

bool replicated(const refl::FieldRecord& f) { return f.has(refl::FieldFlags::Replicated); }

// Writes e's Replicated fields: varu32 component count, then per component its type id, layout
// and the field bytes (length-prefixed). Entity fields go as the target's NetId (0: none).
void write_fields(World& world, Entity e, ByteWriter& out, ByteWriter& one) {
    ByteWriter blocks;
    u32 count = 0;
    for (usize ci = 0; ci < world.component_count(); ++ci) {
        const auto c = static_cast<ecs::ComponentId>(ci);
        const ecs::ComponentInfo& info = world.component(c);
        if (info.refl_id == 0 || info.is_tag) continue;
        const void* row = world.get_raw(e, c);
        if (!row) continue;
        const refl::TypeRecord* type = refl::find_type(StringId(info.refl_id));
        const u32 layout = type ? replicated_layout(*type) : 0u;
        if (layout == 0) continue;
        one.clear();
        for (const refl::FieldRecord& f : type->fields) {
            if (!replicated(f)) continue;
            const u8* at = static_cast<const u8*>(row) + f.offset;
            if (f.kind == refl::FieldKind::EntityRef) {
                for (u32 i = 0; i < f.count; ++i) {
                    Entity target;
                    std::memcpy(&target, at + usize(i) * f.size, sizeof(Entity));
                    const NetId* id = world.alive(target) ? world.try_get<NetId>(target) : nullptr;
                    one.write_varu32(id ? id->id : 0u);
                }
            } else {
                one.append(Span<const u8>(at, usize(f.size) * f.count));
            }
        }
        blocks.write_u64(info.refl_id);
        blocks.write_u32(layout);
        blocks.write_bytes(one.data());
        ++count;
    }
    out.write_varu32(count);
    out.append(blocks.data());
}

}  // namespace

u32 replicated_layout(const refl::TypeRecord& type) {
    Hasher64 h;
    u32 n = 0;
    for (const refl::FieldRecord& f : type.fields) {
        if (!replicated(f)) continue;
        h.u64v(f.name.value);
        h.u32v(static_cast<u32>(f.kind));
        h.u32v(f.size);
        h.u32v(f.count);
        ++n;
    }
    if (n == 0) return 0;
    const u32 v = static_cast<u32>(h.value());
    return v ? v : 1u;
}

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

u32 ReplicationServer::assign_tagged(World& world) {
    Array<Entity> todo;
    world.query<const NetReplicated>().each([&](Entity e, const NetReplicated&) {
        if (!world.has<NetId>(e)) todo.push_back(e);
    });
    for (Entity e : todo) assign(world, e, world.try_get<NetReplicated>(e)->kind);
    return static_cast<u32>(todo.size());
}

u32 ReplicationServer::assign_tagged(Commands& cmd, World& world) {
    u32 n = 0;
    world.query<const NetReplicated>().each([&](Entity e, const NetReplicated& r) {
        if (world.has<NetId>(e)) return;
        assign(cmd, e, r.kind);
        ++n;
    });
    return n;
}

namespace {

// One entity's canonical entry: the bytes a full snapshot carries for it.
void encode_entry(World& world, Entity e, const ReplicationHooks& hooks, ByteWriter& out, ByteWriter& extra,
                  ByteWriter& one) {
    const NetId& id = *world.try_get<NetId>(e);
    out.write_varu32(id.id);
    out.write_varu32(id.kind);
    write_transform(out, *world.try_get<LocalTransform>(e));
    extra.clear();
    if (hooks.write_extra) hooks.write_extra(hooks.user, world, e, extra);
    out.write_bytes(extra.data());
    extra.clear();
    write_fields(world, e, extra, one);
    out.write_bytes(extra.data());
}

u64 content_hash_of(u32 tick, Span<const u8> entries) {
    Hasher64 h;
    h.u32v(tick);
    h.bytes(entries.data(), entries.size());
    return h.value();
}

// The parts of one canonical entry.
struct Parsed {
    u32 id = 0;
    u32 kind = 0;
    Span<const u8> transform;  // 40 bytes
    Span<const u8> extra;
    Span<const u8> fields;
};

bool parse_entry(ByteReader& r, Parsed& p) {
    p.id = r.read_varu32();
    p.kind = r.read_varu32();
    p.transform = r.read_raw(40);
    p.extra = r.read_bytes();
    p.fields = r.read_bytes();
    return r.ok() && p.id != 0;
}

bool same(Span<const u8> a, Span<const u8> b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

enum : u8 { kDeltaNew = 1, kDeltaTransform = 2, kDeltaExtra = 4, kDeltaFields = 8 };

// Fills `out` (tick, refs, hash) from canonical entry bytes already in out.bytes.
bool index_canonical(ReplicationServer::Canonical& out, u32 tick, u32 count) {
    out.tick = tick;
    out.refs.clear();
    out.refs.reserve(count);
    ByteReader r(out.bytes.span());
    u32 prev = 0;
    for (u32 i = 0; i < count; ++i) {
        const usize at = out.bytes.size() - r.remaining();
        Parsed p;
        if (!parse_entry(r, p) || (i > 0 && p.id <= prev)) return false;
        prev = p.id;
        const usize end = out.bytes.size() - r.remaining();
        out.refs.push_back({p.id, static_cast<u32>(at), static_cast<u32>(end - at)});
    }
    if (!r.at_end()) return false;
    out.hash = content_hash_of(tick, out.bytes.span());
    return true;
}

void copy_canonical(ReplicationServer::Canonical& to, const ReplicationServer::Canonical& from) {
    to.tick = from.tick;
    to.hash = from.hash;
    to.bytes.resize(from.bytes.size());
    if (!from.bytes.empty()) std::memcpy(to.bytes.data(), from.bytes.data(), from.bytes.size());
    to.refs.resize(from.refs.size());
    for (usize i = 0; i < from.refs.size(); ++i) to.refs[i] = from.refs[i];
}

}  // namespace

const ReplicationServer::Canonical::Ref* ReplicationServer::Canonical::find(u32 id) const {
    usize lo = 0, hi = refs.size();
    while (lo < hi) {
        const usize mid = (lo + hi) / 2;
        if (refs[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < refs.size() && refs[lo].id == id ? &refs[lo] : nullptr;
}

// The relevant entities for `peer` (all of them for kInvalidPeer), in id order, as canonical entries.
void ReplicationServer::build(World& world, u32 tick, PeerId peer, const Client* cl, Canonical& out) const {
    struct Item {
        u32 id;
        Entity e;
    };
    Array<math::vec3> views;  // where the peer's own entities are, and its camera
    if (cl && cl->sees_all) peer = kInvalidPeer;
    if (cl && cl->has_view) views.push_back(cl->view);
    if (peer != kInvalidPeer) {
        world.query<const NetOwner, const LocalTransform>().each([&](const NetOwner& o, const LocalTransform& t) {
            if (o.peer == peer) views.push_back(t.position);
        });
    }
    Array<Item> items;
    world.query<const NetId, const LocalTransform>().each([&](Entity e, const NetId& id, const LocalTransform& t) {
        if (peer != kInvalidPeer) {
            const NetRelevance* rel = world.try_get<NetRelevance>(e);
            const NetOwner* own = world.try_get<NetOwner>(e);
            if (rel && rel->radius > 0.0f && !(own && own->peer == peer)) {
                bool near = false;
                const f32 r2 = rel->radius * rel->radius;
                for (const math::vec3& v : views) {
                    const math::vec3 d = t.position - v;
                    if (d.x * d.x + d.y * d.y + d.z * d.z <= r2) {
                        near = true;
                        break;
                    }
                }
                if (!near) return;
            }
        }
        items.push_back(Item{id.id, e});
    });
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.id < b.id; });

    ByteWriter entries, extra, one;
    out.refs.clear();
    out.refs.reserve(items.size());
    for (const Item& it : items) {
        const usize at = entries.size();
        encode_entry(world, it.e, hooks_, entries, extra, one);
        out.refs.push_back({it.id, static_cast<u32>(at), static_cast<u32>(entries.size() - at)});
    }
    out.tick = tick;
    out.bytes.resize(entries.size());
    if (entries.size()) std::memcpy(out.bytes.data(), entries.data().data(), entries.size());
    out.hash = content_hash_of(tick, entries.data());
}

namespace {

void write_full(const ReplicationServer::Canonical& c, ByteWriter& out) {
    out.append(Span<const u8>(kSnapshotMagic, 3));
    out.write_u8(kSnapshotVersion);
    out.write_u32(c.tick);
    out.write_varu32(static_cast<u32>(c.refs.size()));
    out.append(c.bytes.span());
}

void write_delta(const ReplicationServer::Canonical& c, const ReplicationServer::Canonical& base, ByteWriter& out) {
    out.append(Span<const u8>(kDeltaMagic, 3));
    out.write_u8(kDeltaVersion);
    out.write_u32(c.tick);
    out.write_u64(c.hash);
    out.write_u32(base.tick);
    out.write_u64(base.hash);
    out.write_varu32(static_cast<u32>(c.refs.size()));
    u32 prev = 0;
    for (const ReplicationServer::Canonical::Ref& ref : c.refs) {
        const Span<const u8> mine(c.bytes.data() + ref.offset, ref.size);
        ByteReader rm(mine);
        Parsed now;
        parse_entry(rm, now);
        out.write_varu32(ref.id - prev);
        prev = ref.id;
        const ReplicationServer::Canonical::Ref* old_ref = base.find(ref.id);
        Parsed old;
        if (old_ref) {
            ByteReader ro(Span<const u8>(base.bytes.data() + old_ref->offset, old_ref->size));
            parse_entry(ro, old);
        }
        if (!old_ref || old.kind != now.kind) {
            out.write_u8(kDeltaNew);
            ByteReader skip(mine);
            skip.read_varu32();  // the id is already written
            out.append(mine.subspan(mine.size() - skip.remaining(), skip.remaining()));
            continue;
        }
        u16 mask = 0;
        for (u32 f = 0; f < 10; ++f)
            if (std::memcmp(now.transform.data() + f * 4, old.transform.data() + f * 4, 4) != 0) mask |= u16(1u << f);
        const bool extra = !same(now.extra, old.extra), fields = !same(now.fields, old.fields);
        out.write_u8(static_cast<u8>((mask ? kDeltaTransform : 0) | (extra ? kDeltaExtra : 0) | (fields ? kDeltaFields : 0)));
        if (mask) {
            out.write_u16(mask);
            for (u32 f = 0; f < 10; ++f)
                if (mask & (1u << f)) out.append(now.transform.subspan(f * 4, 4));
        }
        if (extra) out.write_bytes(now.extra);
        if (fields) out.write_bytes(now.fields);
    }
}

}  // namespace

u64 ReplicationServer::write_snapshot(World& world, u32 tick, ByteWriter& out) const {
    Canonical c;
    build(world, tick, kInvalidPeer, nullptr, c);
    write_full(c, out);
    return c.hash;
}

ReplicationServer::Client* ReplicationServer::client(PeerId peer) {
    for (auto& c : clients_)
        if (c->peer == peer) return c.get();
    return nullptr;
}

ReplicationServer::Client& ReplicationServer::add_client(PeerId peer) {
    if (Client* cl = client(peer)) return *cl;
    clients_.push_back(mem::make_unique<Client>(mem::heap()));
    clients_.back()->peer = peer;
    return *clients_.back();
}

void ReplicationServer::set_view(PeerId peer, const math::vec3* point) {
    Client& cl = add_client(peer);
    cl.has_view = point != nullptr;
    if (point) cl.view = *point;
}

void ReplicationServer::set_sees_all(PeerId peer, bool all) { add_client(peer).sees_all = all; }

u64 ReplicationServer::write_snapshot_for(World& world, u32 tick, PeerId peer, ByteWriter& out) {
    Client* cl = &add_client(peer);
    Canonical& slot = cl->sent[cl->head];
    if (cl->baseline == static_cast<i32>(cl->head)) cl->baseline = -1;  // about to be overwritten
    build(world, tick, peer, cl, slot);
    const usize before = out.size();
    if (cl->baseline >= 0) {
        write_delta(slot, cl->sent[cl->baseline], out);
    } else {
        write_full(slot, out);
        ++cl->stats.full;
    }
    ++cl->stats.snapshots;
    cl->stats.entities = static_cast<u32>(slot.refs.size());
    cl->stats.bytes += out.size() - before;
    cl->head = (cl->head + 1) % kBaselines;
    if (cl->count < kBaselines) ++cl->count;
    return slot.hash;
}

bool ReplicationServer::read_ack(PeerId peer, ByteReader& in) {
    const u32 tick = in.read_u32();
    const u64 hash = in.read_u64();
    if (!in.ok()) return false;
    Client* cl = client(peer);
    if (!cl) return true;
    if (tick == 0 && hash == 0) {  // the client has nothing (reset): full snapshots again
        cl->baseline = -1;
        return true;
    }
    // Newest first, so a resend of the same tick finds the snapshot the client applied.
    for (u32 i = 1; i <= cl->count; ++i) {
        const u32 s = (cl->head + kBaselines - i) % kBaselines;
        const Canonical& c = cl->sent[s];
        if (c.tick != tick || c.hash != hash) continue;
        // Only forward: an older acknowledgement arriving late changes nothing.
        if (cl->baseline >= 0 && static_cast<i32>(tick - cl->sent[cl->baseline].tick) < 0) return true;
        cl->baseline = static_cast<i32>(s);
        return true;
    }
    return true;  // too old, or never sent: keep the current baseline
}

void ReplicationServer::forget_client(PeerId peer) {
    for (usize i = 0; i < clients_.size(); ++i)
        if (clients_[i]->peer == peer) {
            clients_.erase(i);
            return;
        }
}

const ReplicationServer::ClientStats* ReplicationServer::client_stats(PeerId peer) const {
    for (const auto& c : clients_)
        if (c->peer == peer) return &c->stats;
    return nullptr;
}

// ---- client ---------------------------------------------------------------------------------

ApplyResult ReplicationClient::apply(Commands& cmd, const World& world, Span<const u8> snapshot) {
    ApplyResult result;
    ByteReader r(snapshot);
    const u8 m0 = r.read_u8(), m1 = r.read_u8(), m2 = r.read_u8();
    const u8 version = r.read_u8();
    const u32 tick = r.read_u32();
    result.tick = tick;
    const bool full = m0 == kSnapshotMagic[0] && m1 == kSnapshotMagic[1] && m2 == kSnapshotMagic[2] &&
                      version == kSnapshotVersion;
    const bool delta =
        m0 == kDeltaMagic[0] && m1 == kDeltaMagic[1] && m2 == kDeltaMagic[2] && version == kDeltaVersion;
    if (!r.ok() || (!full && !delta)) return result;
    // Sequenced delivery already drops late packets; this also covers an unordered channel.
    // Serial-number comparison, so a tick counter that wraps keeps working. A new server session
    // starts its ticks again: the client calls reset() on disconnect.
    if (has_tick_ && tick != last_tick_ && static_cast<i32>(tick - last_tick_) < 0) return result;

    // Rebuild the canonical entries (a delta onto its baseline) before queuing any command.
    canon_.bytes.clear();
    u32 count = 0;
    if (full) {
        count = r.read_varu32();
        if (!r.ok() || count > r.remaining() / kMinEntryBytes) return result;
        const Span<const u8> rest = r.read_raw(r.remaining());
        canon_.bytes.resize(rest.size());
        if (!rest.empty()) std::memcpy(canon_.bytes.data(), rest.data(), rest.size());
        if (!index_canonical(canon_, tick, count)) return result;
    } else {
        result.delta = true;
        const u64 hash = r.read_u64();
        const u32 base_tick = r.read_u32();
        const u64 base_hash = r.read_u64();
        count = r.read_varu32();
        if (!r.ok() || count > r.remaining() / 2) return result;
        const ReplicationServer::Canonical* base = nullptr;
        for (u32 i = 1; i <= history_count_ && !base; ++i) {
            const ReplicationServer::Canonical& h = history_[(history_head_ + ReplicationServer::kBaselines - i) %
                                                             ReplicationServer::kBaselines];
            if (h.tick == base_tick && h.hash == base_hash) base = &h;
        }
        if (!base) {
            result.missing_baseline = true;
            return result;
        }
        ByteWriter w;
        u32 id = 0;
        for (u32 i = 0; i < count; ++i) {
            const u32 gap = r.read_varu32();
            const u8 flags = r.read_u8();
            if (!r.ok() || gap == 0 || id + gap < id) return result;
            id += gap;
            w.write_varu32(id);
            if (flags & kDeltaNew) {
                if (flags != kDeltaNew) return result;
                const u32 kind = r.read_varu32();
                const Span<const u8> t = r.read_raw(40);
                const Span<const u8> extra = r.read_bytes(), fields = r.read_bytes();
                if (!r.ok()) return result;
                w.write_varu32(kind);
                w.append(t);
                w.write_bytes(extra);
                w.write_bytes(fields);
                continue;
            }
            if (flags & ~u8(kDeltaTransform | kDeltaExtra | kDeltaFields)) return result;
            const ReplicationServer::Canonical::Ref* ref = base->find(id);
            if (!ref) return result;
            ByteReader rb(Span<const u8>(base->bytes.data() + ref->offset, ref->size));
            Parsed old;
            if (!parse_entry(rb, old)) return result;
            w.write_varu32(old.kind);
            u8 t[40];
            std::memcpy(t, old.transform.data(), 40);
            if (flags & kDeltaTransform) {
                const u16 mask = r.read_u16();
                if (mask >> 10) return result;
                for (u32 f = 0; f < 10; ++f)
                    if (mask & (1u << f)) {
                        const Span<const u8> v = r.read_raw(4);
                        if (!r.ok()) return result;
                        std::memcpy(t + f * 4, v.data(), 4);
                    }
            }
            w.append(Span<const u8>(t, 40));
            w.write_bytes((flags & kDeltaExtra) ? r.read_bytes() : old.extra);
            w.write_bytes((flags & kDeltaFields) ? r.read_bytes() : old.fields);
            if (!r.ok()) return result;
        }
        if (!r.at_end()) return result;
        canon_.bytes.resize(w.size());
        if (w.size()) std::memcpy(canon_.bytes.data(), w.data().data(), w.size());
        if (!index_canonical(canon_, tick, count) || canon_.hash != hash) return result;
    }

    // Pass 1: the entries, validated above.
    entries_.clear();
    entries_.reserve(count);
    ByteReader er(canon_.bytes.span());
    for (u32 i = 0; i < count; ++i) {
        Parsed p;
        parse_entry(er, p);
        Entry en;
        en.id.id = p.id;
        en.id.kind = p.kind;
        ByteReader tr(p.transform);
        en.transform = read_transform(tr);
        en.extra = p.extra;
        en.fields = p.fields;
        entries_.push_back(en);
    }

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
    // Pass 3: Replicated fields, once every mirror exists (an Entity field may name one spawned
    // by this snapshot).
    for (const Entry& en : entries_)
        if (!en.fields.empty()) apply_fields(cmd, world, entities_.find(en.id.id)->entity, en.fields, result);
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

    // Kept to decode deltas against. The entries point into canon_, and this copy is separate.
    ReplicationServer::Canonical& slot = history_[history_head_];
    copy_canonical(slot, canon_);
    history_head_ = (history_head_ + 1) % ReplicationServer::kBaselines;
    if (history_count_ < ReplicationServer::kBaselines) ++history_count_;
    content_hash_ = canon_.hash;

    last_tick_ = tick;
    has_tick_ = true;
    result.ok = true;
    return result;
}

void ReplicationClient::write_ack(ByteWriter& out) const {
    out.write_u32(has_tick_ ? last_tick_ : 0u);
    out.write_u64(has_tick_ ? content_hash_ : 0u);
}

void ReplicationClient::apply_fields(Commands& cmd, const World& world, Entity e, Span<const u8> fields,
                                     ApplyResult& result) {
    ByteReader r(fields);
    const u32 count = r.read_varu32();
    for (u32 i = 0; i < count && r.ok(); ++i) {
        const u64 refl_id = r.read_u64();
        const u32 layout = r.read_u32();
        const Span<const u8> bytes = r.read_bytes();
        if (!r.ok()) break;
        const refl::TypeRecord* type = refl::find_type(StringId(refl_id));
        if (!type || type->size == 0 || replicated_layout(*type) != layout) {
            ++result.skipped_components;
            continue;
        }
        ecs::ComponentId c = world.find_component(type->type_hash);
        if (c == ecs::kInvalidComponent) {
            // First sight of this type here: registering it is not a change to any entity.
            ecs::ComponentInfo info;
            info.type_hash = type->type_hash;
            info.name = StringView(type->name);
            info.refl_id = type->id.value;
            info.size = type->size;
            info.align = type->align;
            c = const_cast<World&>(world).register_component_info(info);
        }
        // Start from the mirror's own value, so fields that are not replicated keep theirs.
        scratch_.resize(type->size);
        if (const void* now = world.alive(e) ? world.get_raw(e, c) : nullptr) {
            std::memcpy(scratch_.data(), now, type->size);
        } else {
            std::memset(scratch_.data(), 0, type->size);
            if (type->construct_default) type->construct_default(scratch_.data());
        }
        ByteReader in(bytes);
        for (const refl::FieldRecord& f : type->fields) {
            if (!f.has(refl::FieldFlags::Replicated)) continue;
            u8* at = scratch_.data() + f.offset;
            if (f.kind == refl::FieldKind::EntityRef) {
                for (u32 k = 0; k < f.count; ++k) {
                    const u32 net_id = in.read_varu32();
                    const Mirror* target = net_id ? entities_.find(net_id) : nullptr;
                    const Entity mapped = target ? target->entity : Entity::null;
                    std::memcpy(at + usize(k) * f.size, &mapped, sizeof(Entity));
                }
            } else {
                const Span<const u8> raw = in.read_raw(usize(f.size) * f.count);
                if (in.ok()) std::memcpy(at, raw.data(), raw.size());
            }
        }
        if (!in.ok() || !in.at_end()) {
            ++result.skipped_components;
            continue;
        }
        cmd.set_raw(e, c, scratch_.data());
        ++result.components;
    }
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

f64 ReplicationClient::render_tick() const {
    if (interpolate_ && has_render_) return render_s_ * interp_.tick_rate;
    return static_cast<f64>(last_tick_);
}

f64 ReplicationClient::drawn_tick(u32 net_id) const {
    const Mirror* m = entities_.find(net_id);
    if (!m) return 0.0;
    if (!interpolate_ || m->predicted || m->count == 0) return static_cast<f64>(last_tick_);
    return m->drawn_tick;
}

Entity ReplicationClient::find(u32 net_id) const {
    const Mirror* m = entities_.find(net_id);
    return m ? m->entity : Entity::null;
}

void ReplicationClient::release(Array<Released>& out) {
    out.clear();
    for (auto&& [net_id, m] : entities_) {
        (void)net_id;
        out.push_back(Released{m.entity, m.id, m.latest});
    }
    std::sort(out.begin(), out.end(), [](const Released& a, const Released& b) { return a.id.id < b.id.id; });
    entities_.clear();
    predicted_.clear();
    history_count_ = 0;
    history_head_ = 0;
    content_hash_ = 0;
    has_tick_ = false;
    last_tick_ = 0;
    has_offset_ = false;
    has_render_ = false;
    jitter_s_ = 0.0;
    interval_s_ = 0.0;
}

u32 ReplicationServer::adopt(World& world, ReplicationClient& client) {
    const u32 tick = client.last_tick();
    Array<ReplicationClient::Released> mirrors;
    client.release(mirrors);
    for (const auto& m : mirrors) {
        if (!world.alive(m.entity)) continue;
        world.set(m.entity, m.latest);
        if (!world.has<NetId>(m.entity)) world.add(m.entity, m.id);
        next_id_ = std::max(next_id_, m.id.id + 1);
    }
    clients_.clear();
    return tick;
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
    history_count_ = 0;
    history_head_ = 0;
    content_hash_ = 0;
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

LocalTransform ReplicationClient::sample(const Mirror& m, f64 render_tick, InterpolationStats& stats,
                                         f64& drawn_tick) const {
    const Sample& newest = m.at(m.count - 1);
    drawn_tick = render_tick;
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
        drawn_tick = static_cast<f64>(newest.tick);
        return newest.transform;
    }
    const Sample& oldest = m.at(0);
    if (render_tick <= static_cast<f64>(oldest.tick)) {
        ++stats.held;
        drawn_tick = static_cast<f64>(oldest.tick);
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
        cmd.set(m.entity, sample(m, render_tick, stats_, m.drawn_tick));
    }
    return stats_;
}

}  // namespace mce::net
