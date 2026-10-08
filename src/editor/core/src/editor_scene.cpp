#include "mce/editor/editor_scene.h"

#include <cstdio>
#include <cstring>

#include "mce/cook/cook.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/fs.h"
#include "mce/log.h"
#include "mce/scene/scene.h"

namespace mce::editor {

namespace {

constexpr const char* kStage = "editor.scene";


}  // namespace

// ---------------------------------------------------------------- selection

void SelectionSet::set(EntityId e) {
    items_.clear();
    if (e != kNoEntity) items_.push_back(e);
}

void SelectionSet::toggle(EntityId e) {
    if (e == kNoEntity) return;
    if (contains(e)) remove(e);
    else items_.push_back(e);
}

bool SelectionSet::contains(EntityId e) const {
    for (EntityId x : items_)
        if (x == e) return true;
    return false;
}

void SelectionSet::remove(EntityId e) {
    for (usize i = 0; i < items_.size(); ++i)
        if (items_[i] == e) {
            items_.erase(i);
            return;
        }
}

// ---------------------------------------------------------------- lifecycle

EditorScene::EditorScene() { new_scene("untitled"); }

EditorScene::~EditorScene() = default;

void EditorScene::new_scene(StringView name) {
    clear_undo();
    doc_.reset(name);
    path_.clear();
    selection_.clear();
    rebuild_world();
    dirty_ = false;
    ++revision_;
}

Result<void> EditorScene::open(StringView path, cook::DiagList& diags) {
    cook::String text;
    if (!cook::fs::read_text(path, text)) {
        diags.error(Errc::FileNotFound, path, 0, 0, kStage, "cannot read the scene file");
        return Error(Errc::FileNotFound, kStage, path);
    }
    SceneDocument loaded;
    if (!loaded.parse(text.view(), path, diags)) return Error(Errc::ParseError, kStage, path);
    clear_undo();
    doc_ = std::move(loaded);
    path_.assign(path);
    selection_.clear();
    rebuild_world();
    dirty_ = false;
    ++revision_;
    return ok();
}

bool EditorScene::recover(StringView text, cook::DiagList& diags) {
    SceneDocument loaded;
    if (!loaded.parse(text, "autosave", diags)) return false;
    clear_undo();
    doc_ = std::move(loaded);
    selection_.clear();
    rebuild_world();
    touch();  // recovered work is unsaved until the user saves it
    return true;
}

Result<void> EditorScene::save(StringView path) {
    cook::String text;
    doc_.serialize(text);
    MCE_TRY(cook::fs::write_file_atomic(path, Span<const byte>(reinterpret_cast<const byte*>(text.c_str()), text.size())));
    path_.assign(path);
    dirty_ = false;
    return ok();
}

void EditorScene::assign_new_guid() {
    doc_.set_guid(cook::generate_guid());
    touch();
}

void EditorScene::touch() {
    dirty_ = true;
    ++revision_;
    sync_world_transforms();
    sync_prefab_ghosts();
}

// ---------------------------------------------------------------- preview world

Entity EditorScene::runtime_entity(EntityId id) const { return id < runtime_.size() ? runtime_[id] : Entity::null; }

EntityId EditorScene::entity_for(Entity e) const {
    if (e.is_null()) return kNoEntity;
    for (const DocEntity& d : doc_.entities())
        if (runtime_entity(d.id) == e) return d.id;
    return kNoEntity;
}

void EditorScene::mirror(EntityId id, const DocComponent& c) {
    const Entity e = runtime_entity(id);
    if (e.is_null()) return;
    Array<u64> tmp;
    tmp.resize(c.storage.size());
    std::memcpy(tmp.data(), c.storage.data(), c.storage.size() * sizeof(u64));
    visit_entity_refs(*c.type, tmp.data(), [](Entity* f, void* user) {
        *f = static_cast<const EditorScene*>(user)->runtime_entity(doc_entity_id(*f));
    }, this);
    world_->set_raw(e, scene::component_for(*world_, *c.type), tmp.data());
}

void EditorScene::spawn_mirror(const DocEntity& e) {
    if (runtime_.size() <= e.id) runtime_.resize(usize(e.id) + 1, Entity::null);
    runtime_[e.id] = world_->spawn();
}

void EditorScene::rebuild_world() {
    world_ = mem::make_unique<World>(mem::heap());
    runtime_.clear();
    // Two passes: every entity exists before any EntityRef is mapped.
    for (const DocEntity& e : doc_.entities()) spawn_mirror(e);
    for (const DocEntity& e : doc_.entities())
        for (const DocComponent& c : e.components) mirror(e.id, c);
    world_transforms_ = false;
    ghosts_.clear();
    sync_world_transforms();
    sync_prefab_ghosts();
}

void EditorScene::set_assets(asset::AssetSystem* assets) {
    assets_ = assets;
    sync_prefab_ghosts();
}

void EditorScene::sync_prefab_ghosts() {
    for (Entity g : ghosts_) world_->destroy(g);
    ghosts_.clear();
    if (!assets_) return;
    const refl::TypeRecord& child_of = refl::type_of<ChildOf>();
    const refl::TypeRecord& local_type = refl::type_of<LocalTransform>();
    const ecs::ComponentId wt = scene::component_for(*world_, refl::type_of<WorldTransform>());
    for (const DocEntity& e : doc_.entities()) {
        if (e.prefab.is_null()) continue;
        const asset::AssetKey key = assets_->find(e.prefab);
        if (key == asset::kInvalidAssetKey) continue;
        Result<asset::AssetHandle<asset::Prefab>> h = assets_->load_sync<asset::Prefab>(key);
        if (!h) continue;
        Array<Entity> spawned;
        Result<asset::SceneView> view = asset::SceneView::read(assets_->resolve(h.value()));
        const bool ok_spawn = view && scene::instantiate(*world_, view.value(), &spawned, false);
        assets_->release(asset::AnyHandle(h.value()));
        for (Entity g : spawned) ghosts_.push_back(g);
        if (!ok_spawn || spawned.empty()) continue;
        // The instance's own components override the prefab root, as the cook does; its
        // placement is the root's.
        const Entity root = spawned[0];
        for (const DocComponent& c : e.components) {
            if (c.type == &child_of) continue;
            Array<u64> tmp;
            tmp.resize(c.storage.size());
            std::memcpy(tmp.data(), c.storage.data(), c.storage.size() * sizeof(u64));
            visit_entity_refs(*c.type, tmp.data(), [](Entity* f, void* user) {
                *f = static_cast<const EditorScene*>(user)->runtime_entity(doc_entity_id(*f));
            }, this);
            world_->set_raw(root, scene::component_for(*world_, *c.type), tmp.data());
        }
        // Scene placement of every ghost: the root at the instance, the rest down their ChildOf.
        const WorldTransform at = e.find(local_type) ? world_transform(e.id) : ecs::to_world(LocalTransform{});
        Array<WorldTransform> placed;
        placed.resize(spawned.size());
        Array<u8> done;
        done.resize(spawned.size(), 0);
        placed[0] = at;
        done[0] = 1;
        for (usize pass = 0; pass < spawned.size(); ++pass) {
            bool progress = false;
            for (usize i = 1; i < spawned.size(); ++i) {
                if (done[i]) continue;
                const ChildOf* link = world_->try_get<ChildOf>(spawned[i]);
                usize parent = 0;  // a template root without ChildOf hangs off the instance too
                if (link) {
                    for (usize k = 0; k < spawned.size(); ++k)
                        if (spawned[k] == link->parent) parent = k;
                }
                if (!done[parent]) continue;
                const LocalTransform* t = world_->try_get<LocalTransform>(spawned[i]);
                placed[i] = ecs::compose(placed[parent], t ? *t : LocalTransform{});
                done[i] = 1;
                progress = true;
            }
            if (!progress) break;
        }
        for (usize i = 0; i < spawned.size(); ++i)
            if (done[i]) world_->set_raw(spawned[i], wt, &placed[i]);
        if (!world_transforms_) {  // the document's own entities draw from WorldTransform too now
            world_transforms_ = true;
            sync_world_transforms();
        }
    }
}

void EditorScene::sync_world_transforms() {
    const refl::TypeRecord& child_of = refl::type_of<ChildOf>();
    bool any = world_transforms_;
    for (const DocEntity& e : doc_.entities()) any = any || e.find(child_of) != nullptr;
    if (!any) return;
    world_transforms_ = true;
    const ecs::ComponentId wt = scene::component_for(*world_, refl::type_of<WorldTransform>());
    for (const DocEntity& e : doc_.entities()) {
        const Entity r = runtime_entity(e.id);
        if (r.is_null() || !e.find(refl::type_of<LocalTransform>())) continue;
        const WorldTransform w = world_transform(e.id);
        world_->set_raw(r, wt, &w);
    }
}

bool EditorScene::verify_world() const {
    u32 expected = 0;
    for (const DocEntity& e : doc_.entities()) {
        const Entity r = runtime_entity(e.id);
        if (!world_->alive(r)) return false;
        ++expected;
        u32 comps = 0;
        for (const DocComponent& c : e.components) {
            const ecs::ComponentId id = world_->find_component(c.type->type_hash);
            if (id == ecs::kInvalidComponent) return false;
            const void* live = world_->get_raw(r, id);
            if (!live) return false;
            Array<u64> tmp;
            tmp.resize(c.storage.size());
            std::memcpy(tmp.data(), c.storage.data(), c.storage.size() * sizeof(u64));
            visit_entity_refs(*c.type, tmp.data(), [](Entity* f, void* user) {
                *f = static_cast<const EditorScene*>(user)->runtime_entity(doc_entity_id(*f));
            }, const_cast<EditorScene*>(this));
            if (std::memcmp(live, tmp.data(), c.type->size) != 0) return false;
            ++comps;
        }
        // The mirror holds exactly the document's components.
        u32 live_comps = 0;
        const ecs::ComponentId wt = world_->find_component(refl::type_of<WorldTransform>().type_hash);
        for (usize id = 0; id < world_->component_count(); ++id)
            if (id != wt && world_->has(r, static_cast<ecs::ComponentId>(id))) ++live_comps;  // WorldTransform: derived
        if (live_comps != comps) return false;
    }
    return world_->entity_count() == expected + ghosts_.size();
}

void EditorScene::select_range(EntityId to, bool additive) {
    const EntityId anchor = selection_.anchor();
    const i32 a = doc_.index_of(anchor), b = doc_.index_of(to);
    if (b < 0) return;
    if (a < 0) {
        selection_.set(to);
        return;
    }
    Array<EntityId> keep;
    if (additive)
        for (EntityId id : selection_.items())
            if (id != anchor) keep.push_back(id);
    selection_.set(anchor);
    const i32 lo = a < b ? a : b, hi = a < b ? b : a;
    for (i32 i = lo; i <= hi; ++i) {
        const EntityId id = doc_.at(static_cast<usize>(i)).id;
        if (id != anchor) selection_.toggle(id);
    }
    for (EntityId id : keep)
        if (!selection_.contains(id)) selection_.toggle(id);
}

// ---------------------------------------------------------------- hierarchy

EntityId EditorScene::parent_of(EntityId id) const {
    const ChildOf* c = get<ChildOf>(id);
    if (!c) return kNoEntity;
    const EntityId p = doc_entity_id(c->parent);
    return p != id && doc_.find(p) ? p : kNoEntity;
}

void EditorScene::children_of(EntityId id, Array<EntityId>& out) const {
    for (const DocEntity& e : doc_.entities()) {
        if (e.id == id) continue;
        const EntityId p = parent_of(e.id);
        // An entity in a ChildOf cycle has no root above it: it is listed with the roots too.
        if (p == id || (id == kNoEntity && is_ancestor(e.id, e.id))) out.push_back(e.id);
    }
}

bool EditorScene::is_ancestor(EntityId ancestor, EntityId id) const {
    if (ancestor == kNoEntity) return false;
    EntityId p = parent_of(id);
    for (usize guard = 0; p != kNoEntity && guard <= doc_.size(); ++guard) {
        if (p == ancestor) return true;
        p = parent_of(p);
    }
    return false;
}

WorldTransform EditorScene::world_transform(EntityId id) const {
    // Root first; a cycle stops the walk where it repeats.
    EntityId chain[64];
    usize n = 0;
    for (EntityId e = id; e != kNoEntity && n < 64; e = parent_of(e)) {
        bool seen = false;
        for (usize i = 0; i < n; ++i) seen = seen || chain[i] == e;
        if (seen) break;
        chain[n++] = e;
    }
    WorldTransform w;
    for (usize i = n; i-- > 0;) {
        const LocalTransform* t = get<LocalTransform>(chain[i]);
        const LocalTransform local = t ? *t : LocalTransform{};
        w = i + 1 == n ? ecs::to_world(local) : ecs::compose(w, local);
    }
    if (n == 0) w = ecs::to_world(LocalTransform{});
    return w;
}

LocalTransform EditorScene::local_for_world(EntityId id, const WorldTransform& world) const {
    const EntityId parent = parent_of(id);
    if (parent == kNoEntity) return LocalTransform{world.position, world.rotation, world.scale};
    const WorldTransform p = world_transform(parent);
    const auto div = [](f32 a, f32 b) { return b != 0.0f ? a / b : a; };
    const math::quat inv = math::conjugate(p.rotation);
    const math::vec3 d = math::rotate(inv, world.position - p.position);
    LocalTransform out;
    out.position = {div(d.x, p.scale.x), div(d.y, p.scale.y), div(d.z, p.scale.z)};
    out.rotation = math::normalize(inv * world.rotation);
    out.scale = {div(world.scale.x, p.scale.x), div(world.scale.y, p.scale.y), div(world.scale.z, p.scale.z)};
    return out;
}

bool EditorScene::set_parent(EntityId child, EntityId parent) {
    if (!doc_.find(child) || (parent != kNoEntity && !doc_.find(parent))) return false;
    if (parent == child || is_ancestor(child, parent) || parent_of(child) == parent) return false;
    Edit edit(*this, "Reparent");
    const WorldTransform placed = world_transform(child);
    const refl::TypeRecord& child_of = refl::type_of<ChildOf>();
    if (parent == kNoEntity) {
        remove_component(child, child_of);
    } else {
        if (!get<ChildOf>(child)) add_component(child, child_of);
        set(child, ChildOf{doc_entity_ref(parent)});
    }
    if (get<LocalTransform>(child)) set(child, local_for_world(child, placed));
    if (parent != kNoEntity) {
        // After the new parent's subtree, so the file reads parent-first.
        i32 last = doc_.index_of(parent);
        for (const DocEntity& e : doc_.entities())
            if (e.id != child && is_ancestor(parent, e.id) && !is_ancestor(child, e.id)) last = doc_.index_of(e.id) > last ? doc_.index_of(e.id) : last;
        const i32 from = doc_.index_of(child);
        const i32 to = from <= last ? last : last + 1;
        if (to != from) move_entity(child, static_cast<usize>(to));
    }
    return true;
}

// ---------------------------------------------------------------- commands

EntityId EditorScene::create_entity(StringView name, bool with_transform, i32 at) {
    Edit edit(*this, "Create entity");
    const EntityId id = doc_.insert(name, at);
    note(StructOp::Insert, id, static_cast<usize>(doc_.index_of(id)));
    pending_.before.push_back(EntitySnapshot{id, -1, {}, {}, {}});
    spawn_mirror(*doc_.find(id));
    if (with_transform) {
        const refl::TypeRecord* t = refl::find_type(StringId("LocalTransform"));
        if (t) {
            const DocComponent* c = doc_.add_component(id, *t);
            mirror(id, *c);
        }
    }
    touch();
    return id;
}

EntityId EditorScene::create_prefab_instance(StringView name, asset::Guid prefab, i32 at) {
    Edit edit(*this, "Instantiate prefab");
    const EntityId id = create_entity(name, true, at);
    doc_.find(id)->prefab = prefab;
    touch();
    return id;
}

bool EditorScene::destroy_entity(EntityId id) {
    if (!doc_.find(id)) return false;
    Edit edit(*this, "Delete entity");
    Array<EntityId> kids;  // the subtree goes too, after this entity (a ChildOf cycle ends there)
    children_of(id, kids);
    record(id);
    note(StructOp::Erase, id, static_cast<usize>(doc_.index_of(id)));
    doc_.erase(id);
    world_->destroy(runtime_entity(id));
    runtime_[id] = Entity::null;
    selection_.remove(id);
    // References to it become null in the document and the mirror alike (FR-DX-050: a missing
    // target is an empty field, never a dangling handle).
    struct Ctx {
        Entity dead;
        bool hit;
        bool clear;
    };
    for (DocEntity& e : doc_.entities_mut()) {
        for (DocComponent& c : e.components) {
            Ctx ctx{doc_entity_ref(id), false, false};
            const auto visit = [](Entity* f, void* user) {
                Ctx& x = *static_cast<Ctx*>(user);
                if (*f != x.dead) return;
                x.hit = true;
                if (x.clear) *f = Entity::null;
            };
            visit_entity_refs(*c.type, c.data(), visit, &ctx);
            if (!ctx.hit) continue;
            record(e.id);  // before the reference is cleared
            ctx.clear = true;
            visit_entity_refs(*c.type, c.data(), visit, &ctx);
            mirror(e.id, c);
        }
    }
    for (EntityId k : kids) destroy_entity(k);
    touch();
    return true;
}

bool EditorScene::rename_entity(EntityId id, StringView name) {
    DocEntity* e = doc_.find(id);
    if (!e || e->name.view() == name) return false;
    Edit edit(*this, "Rename");
    record(id);
    e->name.assign(name);
    touch();
    return true;
}

bool EditorScene::move_entity(EntityId id, usize index) {
    const i32 from = doc_.index_of(id);
    if (from < 0 || static_cast<usize>(from) == index) return false;
    Edit edit(*this, "Reorder");
    record(id);
    doc_.move(id, index);
    note(StructOp::Move, id, static_cast<usize>(from), static_cast<usize>(doc_.index_of(id)));
    touch();
    return true;
}

bool EditorScene::add_component(EntityId id, const refl::TypeRecord& type) {
    DocEntity* e = doc_.find(id);
    if (!e || !type.is_component() || e->find(type)) return false;
    Edit edit(*this, "Add component");
    record(id);
    const DocComponent* c = doc_.add_component(id, type);
    mirror(id, *c);
    touch();
    return true;
}

bool EditorScene::remove_component(EntityId id, const refl::TypeRecord& type) {
    const DocEntity* e = doc_.find(id);
    if (!e || !e->find(type)) return false;
    Edit edit(*this, "Remove component");
    record(id);
    doc_.remove_component(id, type);
    const ecs::ComponentId cid = world_->find_component(type.type_hash);
    if (cid != ecs::kInvalidComponent) world_->remove_raw(runtime_entity(id), cid);
    touch();
    return true;
}

void EditorScene::copy_entities(Span<const EntityId> ids, cook::String& out) const {
    SceneDocument clip;
    clip.reset("clipboard");
    for (const DocEntity& e : doc_.entities()) {
        bool wanted = false;
        for (EntityId id : ids) wanted |= id == e.id;
        if (!wanted) continue;
        clip.insert(e.name.view(), -1, e.id);  // same ids: internal references keep resolving
        clip.find(e.id)->prefab = e.prefab;
        for (const DocComponent& c : e.components) {
            DocComponent* dst = clip.add_component(e.id, *c.type);
            std::memcpy(dst->data(), c.data(), c.type->size);
        }
    }
    out.clear();
    clip.serialize(out);
}

bool EditorScene::paste_entities(StringView text, i32 at, cook::DiagList& diags, Array<EntityId>* pasted) {
    SceneDocument clip;
    if (!clip.parse(text, "clipboard", diags) || clip.size() == 0) return false;
    // Fresh ids for every pasted entity; references inside the fragment follow the remap.
    struct Remap {
        Array<EntityId> from, to;
        EntityId map(EntityId id) const {
            for (usize i = 0; i < from.size(); ++i)
                if (from[i] == id) return to[i];
            return kNoEntity;
        }
    } remap;
    const usize base = at < 0 || static_cast<usize>(at) > doc_.size() ? doc_.size() : static_cast<usize>(at);
    Edit edit(*this, "Paste");
    for (usize i = 0; i < clip.size(); ++i) {
        const DocEntity& src = clip.at(i);
        const EntityId id = doc_.insert(src.name.view(), static_cast<i32>(base + i));
        note(StructOp::Insert, id, static_cast<usize>(doc_.index_of(id)));
        pending_.before.push_back(EntitySnapshot{id, -1, {}, {}, {}});
        doc_.find(id)->prefab = src.prefab;
        spawn_mirror(*doc_.find(id));
        remap.from.push_back(src.id);
        remap.to.push_back(id);
    }
    for (usize i = 0; i < clip.size(); ++i) {
        const DocEntity& src = clip.at(i);
        for (const DocComponent& c : src.components) {
            DocComponent* dst = doc_.add_component(remap.to[i], *c.type);
            std::memcpy(dst->data(), c.data(), c.type->size);
            visit_entity_refs(*c.type, dst->data(), [](Entity* f, void* user) {
                *f = doc_entity_ref(static_cast<const Remap*>(user)->map(doc_entity_id(*f)));
            }, &remap);
        }
    }
    selection_.clear();
    for (EntityId id : remap.to) {
        for (const DocComponent& c : doc_.find(id)->components) mirror(id, c);
        selection_.toggle(id);
    }
    if (pasted) {
        pasted->clear();
        for (EntityId id : remap.to) pasted->push_back(id);
    }
    touch();
    return true;
}

bool EditorScene::duplicate_entities(Span<const EntityId> ids, Array<EntityId>* created) {
    i32 last = -1;
    for (EntityId id : ids) last = doc_.index_of(id) > last ? doc_.index_of(id) : last;
    if (last < 0) return false;
    cook::String text;
    copy_entities(ids, text);
    cook::DiagList diags;
    Edit edit(*this, "Duplicate");
    return paste_entities(text.view(), last + 1, diags, created);
}

bool EditorScene::set_component(EntityId id, const refl::TypeRecord& type, const void* value) {
    DocEntity* e = doc_.find(id);
    DocComponent* c = e ? e->find(type) : nullptr;
    if (!c) return false;
    if (std::memcmp(c->data(), value, type.size) == 0) return false;
    // One continuous drag of one component is one entry (coalescing until seal_undo()).
    Edit edit(*this, "Edit component", (type.type_hash ^ (u64(id) * 0x9E3779B97F4A7C15ull)) | 1u);
    record(id);
    std::memcpy(c->data(), value, type.size);
    mirror(id, *c);
    touch();
    return true;
}

// ---------------------------------------------------------------- undo/redo

namespace {

bool same_snapshot(const EntitySnapshot& a, const EntitySnapshot& b) {
    if (a.index != b.index) return false;
    if (a.index < 0) return true;
    if (a.name.view() != b.name.view() || !(a.prefab == b.prefab) || a.components.size() != b.components.size()) return false;
    for (usize i = 0; i < a.components.size(); ++i) {
        const DocComponent& x = a.components[i];
        const DocComponent& y = b.components[i];
        if (x.type != y.type || std::memcmp(x.data(), y.data(), x.type->size) != 0) return false;
    }
    return true;
}

DocComponent copy_component(const DocComponent& c) {
    DocComponent out;
    out.type = c.type;
    out.storage.resize(c.storage.size());
    std::memcpy(out.storage.data(), c.storage.data(), c.storage.size() * sizeof(u64));
    return out;
}

usize snapshot_bytes(const EntitySnapshot& s) {
    usize n = sizeof(EntitySnapshot);
    for (const DocComponent& c : s.components) n += sizeof(DocComponent) + c.storage.size() * sizeof(u64);
    return n;
}

}  // namespace

void EditorScene::snapshot(EntityId id, EntitySnapshot& out) const {
    out = EntitySnapshot{};
    out.id = id;
    const DocEntity* e = doc_.find(id);
    if (!e) return;
    out.index = doc_.index_of(id);
    out.name = e->name;
    out.prefab = e->prefab;
    for (const DocComponent& c : e->components) out.components.push_back(copy_component(c));
}

void EditorScene::record(EntityId id) {
    MCE_ASSERT_MSG(edit_depth_ > 0, "EditorScene mutation outside an edit");
    for (const EntitySnapshot& s : pending_.before)
        if (s.id == id) return;
    EntitySnapshot snap;
    snapshot(id, snap);
    snap.index = index_before_edit(snap.index);
    pending_.before.push_back(std::move(snap));
}

i32 EditorScene::index_before_edit(i32 index) const {
    if (index < 0) return index;
    usize i = static_cast<usize>(index);
    for (usize k = struct_ops_.size(); k-- > 0;) {
        const StructOp& op = struct_ops_[k];
        switch (op.kind) {
            case StructOp::Erase:
                if (i >= op.a) ++i;
                break;
            case StructOp::Insert:
                if (i == op.a) return -1;  // created in this edit: absent before it
                if (i > op.a) --i;
                break;
            case StructOp::Move:
                if (i == op.b) i = op.a;
                else if (op.a < op.b && i >= op.a && i < op.b) ++i;
                else if (op.a > op.b && i > op.b && i <= op.a) --i;
                break;
        }
    }
    return static_cast<i32>(i);
}

void EditorScene::begin_edit(StringView label, u64 coalesce_key) {
    if (edit_depth_++ > 0) return;
    pending_ = UndoEntry{};
    struct_ops_.clear();
    pending_.label.assign(label);
    pending_.coalesce_key = coalesce_key;
}

void EditorScene::end_edit() {
    MCE_ASSERT_MSG(edit_depth_ > 0, "end_edit without begin_edit");
    if (edit_depth_ == 0) return;
    --edit_depth_;
    if (edit_depth_ > 0) return;
    UndoEntry e = std::move(pending_);
    pending_ = UndoEntry{};
    // Keep only the entities that actually changed.
    Array<EntitySnapshot> before;
    for (EntitySnapshot& b : e.before) {
        EntitySnapshot a;
        snapshot(b.id, a);
        // A moved entity stays even at its old index: the others may have shifted around it.
        bool moved = false;
        for (const StructOp& op : struct_ops_) moved = moved || (op.kind == StructOp::Move && op.id == b.id);
        if (!moved && same_snapshot(b, a)) continue;
        e.after.push_back(std::move(a));
        before.push_back(std::move(b));
    }
    e.before = std::move(before);
    if (e.before.empty()) return;

    if (e.coalesce_key != 0 && !sealed_ && !undo_.empty() && undo_.back().coalesce_key == e.coalesce_key) {
        UndoEntry& top = undo_.back();
        for (usize i = 0; i < e.before.size(); ++i) {
            bool merged = false;
            for (usize j = 0; j < top.after.size() && !merged; ++j) {
                if (top.after[j].id != e.after[i].id) continue;
                top.after[j] = std::move(e.after[i]);
                merged = true;
            }
            if (!merged) {
                top.before.push_back(std::move(e.before[i]));
                top.after.push_back(std::move(e.after[i]));
            }
        }
        undo_bytes_ -= top.bytes;
        top.bytes = 0;
        for (usize i = 0; i < top.before.size(); ++i) top.bytes += snapshot_bytes(top.before[i]) + snapshot_bytes(top.after[i]);
        undo_bytes_ += top.bytes;
        redo_.clear();
        return;
    }
    push_entry(std::move(e));
    sealed_ = false;
}

void EditorScene::push_entry(UndoEntry&& e) {
    e.bytes = 0;
    for (usize i = 0; i < e.before.size(); ++i) e.bytes += snapshot_bytes(e.before[i]) + snapshot_bytes(e.after[i]);
    undo_bytes_ += e.bytes;
    undo_.push_back(std::move(e));
    redo_.clear();
    // Bounded history: the oldest entries go first (the newest one always stays).
    while (undo_.size() > 1 && (undo_.size() > kMaxUndoEntries || undo_bytes_ > kMaxUndoBytes)) {
        undo_bytes_ -= undo_[0].bytes;
        undo_.erase(0);
    }
}

void EditorScene::apply(const Array<EntitySnapshot>& side) {
    for (const EntitySnapshot& s : side) doc_.erase(s.id);
    // Restoring in ascending index puts every entity at its recorded place: the others never
    // moved relative to each other.
    Array<const EntitySnapshot*> present;
    for (const EntitySnapshot& s : side)
        if (s.index >= 0) present.push_back(&s);
    for (usize i = 1; i < present.size(); ++i)
        for (usize j = i; j > 0 && present[j - 1]->index > present[j]->index; --j) std::swap(present[j - 1], present[j]);
    for (const EntitySnapshot* s : present) {
        DocEntity e;
        e.id = s->id;
        e.name = s->name;
        e.prefab = s->prefab;
        for (const DocComponent& c : s->components) e.components.push_back(copy_component(c));
        doc_.insert_entity(std::move(e), static_cast<usize>(s->index));
    }
    Array<EntityId> gone;
    for (EntityId id : selection_.items())
        if (!doc_.find(id)) gone.push_back(id);
    for (EntityId id : gone) selection_.remove(id);
    rebuild_world();
    touch();
}

bool EditorScene::undo() {
    if (edit_depth_ > 0 || undo_.empty()) return false;
    UndoEntry e = std::move(undo_.back());
    undo_.pop_back();
    undo_bytes_ -= e.bytes;
    apply(e.before);
    redo_.push_back(std::move(e));
    sealed_ = true;
    return true;
}

bool EditorScene::redo() {
    if (edit_depth_ > 0 || redo_.empty()) return false;
    UndoEntry e = std::move(redo_.back());
    redo_.pop_back();
    apply(e.after);
    undo_bytes_ += e.bytes;
    undo_.push_back(std::move(e));
    sealed_ = true;
    return true;
}

void EditorScene::clear_undo() {
    undo_.clear();
    redo_.clear();
    undo_bytes_ = 0;
    sealed_ = true;
}

bool EditorScene::reload_text(StringView text, StringView file, cook::DiagList& diags) {
    SceneDocument loaded;
    if (!loaded.parse(text, file, diags)) return false;
    if (!doc_.same_header(loaded)) {
        clear_undo();
        doc_ = std::move(loaded);
        selection_.clear();
        rebuild_world();
        touch();
        return true;
    }
    Edit edit(*this, "Reload from disk");
    for (const DocEntity& e : doc_.entities()) record(e.id);
    for (const DocEntity& e : loaded.entities())
        if (!doc_.find(e.id)) pending_.before.push_back(EntitySnapshot{e.id, -1, {}, {}, {}});
    while (doc_.size() > 0) doc_.erase(doc_.at(doc_.size() - 1).id);
    for (DocEntity& e : loaded.entities_mut()) doc_.insert_entity(std::move(e), doc_.size());
    Array<EntityId> gone;
    for (EntityId id : selection_.items())
        if (!doc_.find(id)) gone.push_back(id);
    for (EntityId id : gone) selection_.remove(id);
    rebuild_world();
    touch();
    return true;
}

}  // namespace mce::editor
