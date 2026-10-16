#include "mce/kit/ai.h"

#include <cstring>

#include "kit3d_internal.h"
#include "mce/ecs/transform.h"
#include "mce/hash.h"
#include "mce/kit/perception.h"
#include "mce/log.h"

namespace mce::kit {

// ai_tasks.cpp: the stock tasks (install_ai registers them).
void register_stock_tasks(BtRegistry& reg);

using asset::BtAbort;
using asset::BtKey;
using asset::BtKeyType;
using asset::BtNode;
using asset::BtNodeKind;
using asset::BtOp;
using asset::BtSetOp;

namespace {

// AiBrain::nodes: bits 0-1 the status, bits 2-7 a composite's current child.
BtStatus status_of(u8 s) { return static_cast<BtStatus>(s & 3u); }
u32 cursor_of(u8 s) { return s >> 2; }
u8 pack(BtStatus st, u32 cursor) { return static_cast<u8>((cursor << 2) | static_cast<u8>(st)); }

bool numeric(BtKeyType t) { return t == BtKeyType::Bool || t == BtKeyType::Int || t == BtKeyType::Float; }

// The `set` bit of a key's slot.
u32 set_bit(const BtKey& k) {
    const u32 base = numeric(k.type) ? 0u : k.type == BtKeyType::Vec3 ? asset::kBtNumbers : asset::kBtNumbers + asset::kBtVectors;
    return 1u << (base + k.slot);
}

bool key_set(World& w, const Blackboard* bb, const BtKey& k) {
    if (!bb || !(bb->set & set_bit(k))) return false;
    return k.type != BtKeyType::Entity || w.alive(bb->entities[k.slot]);
}

// Within a thousandth of a tick counts as reached: the sum of N ticks of dt must complete an
// N-tick wait whatever the float rounding (the sequences' rule).
bool reached(f32 elapsed, f32 seconds, f32 dt) { return elapsed + dt * 1e-3f >= seconds; }

void write_defaults(const BtRegistry::Def& d, Blackboard& bb) {
    for (const BtKey& k : d.keys) {
        if (!k.has_default || (bb.set & set_bit(k))) continue;
        if (k.type == BtKeyType::Vec3) bb.vectors[k.slot] = {k.value[0], k.value[1], k.value[2]};
        else bb.numbers[k.slot] = k.value[0];
        bb.set |= set_bit(k);
    }
}

struct Run {
    Frame& f;
    World& w;
    BtRegistry& reg;
    const BtRegistry::Def& d;
    Entity e;
    AiBrain& b;
    Blackboard* bb;

    const BtNode& node(u32 i) const { return d.nodes[i]; }
    BtStatus status(u32 i) const { return status_of(b.nodes[i]); }
    void set_status(u32 i, BtStatus s) { b.nodes[i] = pack(s, cursor_of(b.nodes[i])); }

    // Child `k` of node `i` (children follow in pre-order; each next one starts at the last one's end).
    u32 child(u32 i, u32 k) const {
        u32 c = i + 1;
        while (k-- > 0) c = d.nodes[c].end;
        return c;
    }

    BtStatus call_task(u32 i, bool entering, bool halting) {
        const BtNode& n = node(i);
        BtTaskFn fn = reg.task(StringId(n.id));
        if (!fn) {
            if (!halting) reg.warn_missing(StringId(n.id), d.string(n.text));
            return BtStatus::Failure;
        }
        if (entering) b.slots[n.slot] = 0.0f;
        BtTaskCall call{f, e, bb, d, n, b.slots[n.slot], entering, halting};
        const BtStatus r = fn(call);
        return r == BtStatus::Idle ? BtStatus::Failure : r;
    }

    // Stops node `i`'s running descendants (and `i` itself): tasks hear it once, statuses go Idle.
    void halt(u32 i) {
        for (u32 j = i; j < node(i).end; ++j) {
            if (status(j) != BtStatus::Running) continue;
            if (node(j).kind == BtNodeKind::Task) call_task(j, false, true);
            b.nodes[j] = pack(BtStatus::Idle, 0);
        }
    }

    bool holds(const BtNode& n) const {
        const BtKey& k = d.keys[n.key];
        const bool set = key_set(w, bb, k);
        const BtOp op = static_cast<BtOp>(n.op);
        if (op == BtOp::IsSet) return set;
        if (op == BtOp::NotSet) return !set;
        if (!set) return false;
        const f32 x = bb->numbers[k.slot];
        switch (op) {
        case BtOp::Eq: return x == n.a;
        case BtOp::Ne: return x != n.a;
        case BtOp::Lt: return x < n.a;
        case BtOp::Le: return x <= n.a;
        case BtOp::Gt: return x > n.a;
        case BtOp::Ge: return x >= n.a;
        default: return false;
        }
    }

    void apply_set(const BtNode& n) {
        if (!bb) return;
        const BtKey& k = d.keys[n.key];
        const u32 bit = set_bit(k);
        switch (static_cast<BtSetOp>(n.op)) {
        case BtSetOp::Assign:
            if (k.type == BtKeyType::Vec3) bb->vectors[k.slot] = {n.v[0], n.v[1], n.v[2]};
            else bb->numbers[k.slot] = n.a;
            bb->set |= bit;
            break;
        case BtSetOp::Add:
            bb->numbers[k.slot] = ((bb->set & bit) ? bb->numbers[k.slot] : 0.0f) + n.a;
            bb->set |= bit;
            break;
        case BtSetOp::Clear: bb->set &= ~bit; break;
        case BtSetOp::Copy: {
            const BtKey& from = d.keys[n.key2];
            if (!(bb->set & set_bit(from))) {
                bb->set &= ~bit;
                break;
            }
            if (numeric(k.type)) bb->numbers[k.slot] = bb->numbers[from.slot];
            else if (k.type == BtKeyType::Vec3) bb->vectors[k.slot] = bb->vectors[from.slot];
            else bb->entities[k.slot] = bb->entities[from.slot];
            bb->set |= bit;
            break;
        }
        case BtSetOp::Self:
            bb->entities[k.slot] = e;
            bb->set |= bit;
            break;
        }
    }

    BtStatus tick(u32 i) {
        const BtNode& n = node(i);
        const bool entering = status(i) != BtStatus::Running;
        if (entering) {
            // A fresh start: the subtree's results from an earlier run clear (nothing in it runs).
            for (u32 j = i + 1; j < n.end; ++j) b.nodes[j] = 0;
            b.nodes[i] = pack(BtStatus::Running, 0);
        }
        const BtStatus r = run(i, n, entering);
        b.nodes[i] = pack(r, r == BtStatus::Running ? cursor_of(b.nodes[i]) : 0);
        return r;
    }

    BtStatus run(u32 i, const BtNode& n, bool entering) {
        f32* slot = n.slot != asset::kBtNoSlot ? &b.slots[n.slot] : nullptr;
        switch (n.kind) {
        case BtNodeKind::Sequence:
        case BtNodeKind::Selector: {
            const BtStatus stop = n.kind == BtNodeKind::Sequence ? BtStatus::Failure : BtStatus::Success;
            u32 first = cursor_of(b.nodes[i]);
            if (n.kind == BtNodeKind::Selector && first > 0) {
                // A higher-priority guard that observes lower branches and now holds takes over.
                for (u32 k = 0, c = i + 1; k < first; ++k, c = node(c).end) {
                    const BtNode& g = node(c);
                    if (g.kind != BtNodeKind::Guard || g.abort < static_cast<u8>(BtAbort::Lower) || !holds(g)) continue;
                    halt(child(i, first));
                    first = k;
                    break;
                }
            }
            for (u32 k = first; k < n.child_count; ++k) {
                b.nodes[i] = pack(BtStatus::Running, k);
                const BtStatus r = tick(child(i, k));
                if (r == BtStatus::Running) return r;
                if (r == stop) return r;
            }
            return stop == BtStatus::Failure ? BtStatus::Success : BtStatus::Failure;
        }
        case BtNodeKind::Parallel: {
            u32 ok = 0, failed = 0;
            for (u32 k = 0, c = i + 1; k < n.child_count; ++k, c = node(c).end) {
                BtStatus r = status(c);
                if (r != BtStatus::Success && r != BtStatus::Failure) r = tick(c);
                ok += r == BtStatus::Success;
                failed += r == BtStatus::Failure;
            }
            const bool all = static_cast<asset::BtPolicy>(n.op) == asset::BtPolicy::All;
            BtStatus r = BtStatus::Running;
            if (all) r = failed > 0 ? BtStatus::Failure : ok == n.child_count ? BtStatus::Success : BtStatus::Running;
            else r = ok > 0 ? BtStatus::Success : failed == n.child_count ? BtStatus::Failure : BtStatus::Running;
            if (r != BtStatus::Running)
                for (u32 k = 0, c = i + 1; k < n.child_count; ++k, c = node(c).end) halt(c);
            return r;
        }
        case BtNodeKind::Invert: {
            const BtStatus r = tick(i + 1);
            return r == BtStatus::Success ? BtStatus::Failure : r == BtStatus::Failure ? BtStatus::Success : r;
        }
        case BtNodeKind::Succeed:
        case BtNodeKind::Fail: {
            const BtStatus r = tick(i + 1);
            if (r == BtStatus::Running) return r;
            return n.kind == BtNodeKind::Succeed ? BtStatus::Success : BtStatus::Failure;
        }
        case BtNodeKind::Repeat:
        case BtNodeKind::Retry: {
            if (entering) *slot = 0.0f;
            const BtStatus again = n.kind == BtNodeKind::Repeat ? BtStatus::Success : BtStatus::Failure;
            const BtStatus r = tick(i + 1);
            if (r != again) return r;
            *slot += 1.0f;
            if (n.a > 0.0f && *slot >= n.a) return r;
            return BtStatus::Running;  // the child starts again on the next tick
        }
        case BtNodeKind::Cooldown: {
            // The slot holds when the child may run again, on the brain's clock (it survives the
            // node finishing and the tree starting over).
            if (entering && b.time + f.dt * 1e-3f < *slot) return BtStatus::Failure;
            const BtStatus r = tick(i + 1);
            if (r != BtStatus::Running) *slot = b.time + n.a;
            return r;
        }
        case BtNodeKind::Timeout: {
            if (entering) *slot = 0.0f;
            *slot += f.dt;
            const BtStatus r = tick(i + 1);
            if (r == BtStatus::Running && reached(*slot, n.a, f.dt)) {
                halt(i + 1);
                return BtStatus::Failure;
            }
            return r;
        }
        case BtNodeKind::Guard:
            if (entering && !holds(n)) return BtStatus::Failure;
            if (!entering && (n.abort == static_cast<u8>(BtAbort::Self) || n.abort == static_cast<u8>(BtAbort::Both)) && !holds(n)) {
                halt(i + 1);
                return BtStatus::Failure;
            }
            return tick(i + 1);
        case BtNodeKind::Check: return holds(n) ? BtStatus::Success : BtStatus::Failure;
        case BtNodeKind::Wait:
            if (entering) *slot = 0.0f;
            if (n.a <= 0.0f) return BtStatus::Success;
            *slot += f.dt;
            return reached(*slot, n.a, f.dt) ? BtStatus::Success : BtStatus::Running;
        case BtNodeKind::Set: apply_set(n); return BtStatus::Success;
        case BtNodeKind::Emit: f.events().push(BtSignal{StringId(n.id), e}); return BtStatus::Success;
        case BtNodeKind::Task: return call_task(i, entering, false);
        }
        return BtStatus::Failure;
    }
};

const BtRegistry::Def* def_of(World& w, Entity e) {
    BtRegistry* reg = w.try_resource<BtRegistry>();
    const AiBrain* b = w.try_get<AiBrain>(e);
    return reg && b ? reg->resolve(*b) : nullptr;
}

// The key `name` of `e`'s tree and its Blackboard; null when either is missing.
Blackboard* key_of(World& w, Entity e, StringView name, const BtKey*& key) {
    const BtRegistry::Def* d = def_of(w, e);
    if (!d) return nullptr;
    const u16 k = d->find_key(StringId(name));
    if (k == asset::kBtNoKey) return nullptr;
    key = &d->keys[k];
    return w.try_get_mut<Blackboard>(e);
}

}  // namespace

// ------------------------------------------------------------------ registry

StringView BtRegistry::Def::string(u32 offset) const {
    if (offset >= strings.size()) return {};
    usize n = 0;
    while (offset + n < strings.size() && strings[offset + n] != '\0') ++n;
    return StringView(strings.data() + offset, n);
}

u16 BtRegistry::Def::find_key(StringId key_name) const {
    for (usize i = 0; i < keys.size(); ++i)
        if (keys[i].id == key_name.value) return static_cast<u16>(i);
    return asset::kBtNoKey;
}

Result<void> BtRegistry::copy(Def& d, asset::Guid guid, Span<const byte> payload) const {
    Result<asset::BtView> view = asset::BtView::read(payload);
    if (!view) return view.error();
    const asset::BtView& v = view.value();
    d.guid = guid;
    d.name.assign(v.name());
    d.slot_count = v.header->slot_count;
    d.hash = static_cast<u32>(fnv1a64(payload.data(), payload.size()));
    if (d.hash == 0) d.hash = 1;  // 0 is a brain that has run no tree
    d.keys.clear();
    d.nodes.clear();
    d.strings.clear();
    for (const BtKey& k : v.keys) d.keys.push_back(k);
    for (const BtNode& n : v.nodes) d.nodes.push_back(n);
    for (char c : v.strings) d.strings.push_back(c);
    return {};
}

Result<void> BtRegistry::define(asset::Guid guid, Span<const byte> payload) {
    Def d;
    if (Result<void> r = copy(d, guid, payload); !r) return r;
    for (Def& existing : defs_)
        if (existing.guid == guid) {
            existing = static_cast<Def&&>(d);
            return {};
        }
    defs_.push_back(static_cast<Def&&>(d));
    return {};
}

const BtRegistry::Def* BtRegistry::resolve(const AiBrain& b) {
    const asset::Guid guid = b.tree.guid;
    if (guid == asset::Guid{}) return nullptr;
    Def* cached = nullptr;
    for (Def& d : defs_)
        if (d.guid == guid) cached = &d;
    if (cached && (!assets_ || cached->key == asset::kInvalidAssetKey || assets_->generation(cached->key) == cached->generation))
        return cached;
    if (!assets_) return nullptr;
    const bool fresh = cached == nullptr;
    if (fresh) {
        defs_.push_back(Def{});
        cached = &defs_.back();
    }
    const asset::AssetKey key = assets_->find(guid);
    bool loaded = false;
    if (key != asset::kInvalidAssetKey) {
        if (Result<asset::AssetHandle<asset::BehaviorTree>> h = assets_->load_sync<asset::BehaviorTree>(key)) {
            loaded = static_cast<bool>(copy(*cached, guid, assets_->resolve(h.value())));
            cached->key = key;
            cached->generation = h.value().generation;
            assets_->release(asset::AnyHandle(h.value()));
        }
    }
    if (loaded) return cached;
    if (fresh) defs_.pop_back();
    bool warned = false;
    for (const asset::Guid& g : warned_) warned |= g == guid;
    if (!warned) {
        MCE_LOG_WARN("kit: behavior tree {} cannot be loaded; its brains stay idle", asset::to_string(guid).view());
        warned_.push_back(guid);
    }
    return nullptr;
}

void BtRegistry::register_task(StringView name, BtTaskFn fn) {
    const StringId id(name);
    for (Task& t : tasks_)
        if (t.id == id) {
            t.fn = fn;
            return;
        }
    tasks_.push_back(Task{id, fn});
}

BtTaskFn BtRegistry::task(StringId id) const {
    for (const Task& t : tasks_)
        if (t.id == id) return t.fn;
    return nullptr;
}

Result<void> BtRegistry::validate_tasks() const {
    for (const Def& d : defs_)
        for (const BtNode& n : d.nodes)
            if (n.kind == BtNodeKind::Task && !task(StringId(n.id))) return Error(Errc::NotFound, "kit.ai.task_unresolved");
    return {};
}

void BtRegistry::warn_missing(StringId id, StringView name) {
    for (StringId w : warned_tasks_)
        if (w == id) return;
    warned_tasks_.push_back(id);
    MCE_LOG_WARN("kit: behavior tree task '{}' is not registered; its nodes fail", name);
}

// ------------------------------------------------------------------ the system

void ai_brains(Frame& f) {
    BtRegistry* reg = f.try_resource<BtRegistry>();
    if (!reg) return;
    World& w = f.world();
    // Snapshot: tasks may spawn and destroy (Commands) and run queries of their own.
    Array<Entity> brains;
    f.query<const AiBrain>().each([&](Entity e, const AiBrain& b) {
        if (b.active || b.restart) brains.push_back(e);
    });
    for (Entity e : brains) {
        const AiBrain* cb = w.try_get<AiBrain>(e);
        if (!cb) continue;
        const BtRegistry::Def* d = reg->resolve(*cb);
        if (!d) continue;
        AiBrain& b = *w.try_get_mut<AiBrain>(e);
        Blackboard* bb = w.try_get_mut<Blackboard>(e);
        if (b.restart || b.tree_hash != d->hash) {
            // The same tree halts what runs; another tree's statuses mean nothing to this one.
            if (b.tree_hash == d->hash && b.started) Run{f, w, *reg, *d, e, b, bb}.halt(0);
            std::memset(b.nodes, 0, sizeof(b.nodes));
            std::memset(b.slots, 0, sizeof(b.slots));
            b.started = false;
            b.result = BtStatus::Idle;
            b.runs = 0;
            b.time = 0.0f;
            b.tree_hash = d->hash;
            b.restart = false;
        }
        if (!b.active) continue;
        if (!b.started) {
            if (!d->keys.empty() && !bb) {
                Blackboard fresh;
                write_defaults(*d, fresh);
                f.commands().add<Blackboard>(e, fresh);
                continue;  // starts on the next tick, with its blackboard
            }
            if (bb) write_defaults(*d, *bb);
            b.started = true;
        }
        b.time += f.dt;
        const BtStatus r = Run{f, w, *reg, *d, e, b, bb}.tick(0);
        if (r != BtStatus::Running) {
            AiBrain* after = w.try_get_mut<AiBrain>(e);
            if (!after) continue;
            after->result = r;
            ++after->runs;
        }
    }
}

void install_ai(World& world, asset::AssetSystem* assets) {
    if (BtRegistry* r = world.try_resource<BtRegistry>()) {
        r->set_assets(assets);
        return;
    }
    install_perception(world);  // first: "ai_perception" runs ahead of "ai_brains"
    world.register_components<AiBrain, Blackboard>();
    world.register_event<BtSignal>(Phase::FixedUpdate_Pre);
    world.set_resource(BtRegistry(assets));
    world.add_system(Phase::FixedUpdate_Pre, "ai_brains", &ai_brains);
    register_stock_tasks(world.resource<BtRegistry>());
}

bool bt_key_position(const BtTaskCall& call, math::vec3& out, Entity* target) {
    if (call.node.key >= call.tree.keys.size() || !call.blackboard) return false;
    const BtKey& k = call.tree.keys[call.node.key];
    World& w = call.frame.world();
    if (!key_set(w, call.blackboard, k)) return false;
    if (k.type == BtKeyType::Vec3) {
        out = call.blackboard->vectors[k.slot];
        return true;
    }
    if (k.type != BtKeyType::Entity) return false;
    const Entity e = call.blackboard->entities[k.slot];
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    if (!t) return false;
    math::quat rot;
    math::vec3 scale;
    world_pose(w, e, *t, out, rot, scale);
    if (target) *target = e;
    return true;
}

BtStatus bt_node_status(const AiBrain& b, u32 node) { return node < asset::kBtMaxNodes ? status_of(b.nodes[node]) : BtStatus::Idle; }

// ------------------------------------------------------------------ blackboard by name

bool bb_set_number(World& w, Entity e, StringView key, f32 value) {
    const BtKey* k = nullptr;
    Blackboard* bb = key_of(w, e, key, k);
    if (!bb || !numeric(k->type)) return false;
    bb->numbers[k->slot] = k->type == BtKeyType::Bool ? (value != 0.0f ? 1.0f : 0.0f) : value;
    bb->set |= set_bit(*k);
    return true;
}

bool bb_set_vector(World& w, Entity e, StringView key, math::vec3 value) {
    const BtKey* k = nullptr;
    Blackboard* bb = key_of(w, e, key, k);
    if (!bb || k->type != BtKeyType::Vec3) return false;
    bb->vectors[k->slot] = value;
    bb->set |= set_bit(*k);
    return true;
}

bool bb_set_entity(World& w, Entity e, StringView key, Entity value) {
    const BtKey* k = nullptr;
    Blackboard* bb = key_of(w, e, key, k);
    if (!bb || k->type != BtKeyType::Entity) return false;
    bb->entities[k->slot] = value;
    bb->set |= set_bit(*k);
    return true;
}

bool bb_clear(World& w, Entity e, StringView key) {
    const BtKey* k = nullptr;
    Blackboard* bb = key_of(w, e, key, k);
    if (!bb) return false;
    bb->set &= ~set_bit(*k);
    return true;
}

bool bb_number(World& w, Entity e, StringView key, f32& out) {
    const BtKey* k = nullptr;
    Blackboard* bb = key_of(w, e, key, k);
    if (!bb || !numeric(k->type) || !key_set(w, bb, *k)) return false;
    out = bb->numbers[k->slot];
    return true;
}

bool bb_vector(World& w, Entity e, StringView key, math::vec3& out) {
    const BtKey* k = nullptr;
    Blackboard* bb = key_of(w, e, key, k);
    if (!bb || k->type != BtKeyType::Vec3 || !key_set(w, bb, *k)) return false;
    out = bb->vectors[k->slot];
    return true;
}

bool bb_entity(World& w, Entity e, StringView key, Entity& out) {
    const BtKey* k = nullptr;
    Blackboard* bb = key_of(w, e, key, k);
    if (!bb || k->type != BtKeyType::Entity || !key_set(w, bb, *k)) return false;
    out = bb->entities[k->slot];
    return true;
}

}  // namespace mce::kit
