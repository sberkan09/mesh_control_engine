#include "mce/editor/editor_scene.h"

#include <cstdio>
#include <cstring>

#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/log.h"
#include "mce/scene/scene.h"

namespace mce::editor {

namespace {

constexpr const char* kStage = "editor.scene";

bool read_text_file(StringView path, cook::String& out) {
    FixedString<1024> p(path);
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    usize n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(StringView(buf, n));
    std::fclose(f);
    return true;
}

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
    doc_.reset(name);
    path_.clear();
    selection_.clear();
    rebuild_world();
    dirty_ = false;
    ++revision_;
}

Result<void> EditorScene::open(StringView path, cook::DiagList& diags) {
    cook::String text;
    if (!read_text_file(path, text)) {
        diags.error(Errc::FileNotFound, path, 0, 0, kStage, "cannot read the scene file");
        return Error(Errc::FileNotFound, kStage, path);
    }
    SceneDocument loaded;
    if (!loaded.parse(text.view(), path, diags)) return Error(Errc::ParseError, kStage, path);
    doc_ = std::move(loaded);
    path_.assign(path);
    selection_.clear();
    rebuild_world();
    dirty_ = false;
    ++revision_;
    return ok();
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
        for (usize id = 0; id < world_->component_count(); ++id)
            if (world_->has(r, static_cast<ecs::ComponentId>(id))) ++live_comps;
        if (live_comps != comps) return false;
    }
    return world_->entity_count() == expected;
}

// ---------------------------------------------------------------- commands

EntityId EditorScene::create_entity(StringView name, bool with_transform, i32 at) {
    const EntityId id = doc_.insert(name, at);
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

bool EditorScene::destroy_entity(EntityId id) {
    if (!doc_.erase(id)) return false;
    world_->destroy(runtime_entity(id));
    runtime_[id] = Entity::null;
    selection_.remove(id);
    // References to it become null in the document and the mirror alike (FR-DX-050: a missing
    // target is an empty field, never a dangling handle).
    struct Ctx {
        Entity dead;
        bool hit;
    };
    for (DocEntity& e : doc_.entities_mut()) {
        for (DocComponent& c : e.components) {
            Ctx ctx{doc_entity_ref(id), false};
            visit_entity_refs(*c.type, c.data(), [](Entity* f, void* user) {
                Ctx& x = *static_cast<Ctx*>(user);
                if (*f == x.dead) {
                    *f = Entity::null;
                    x.hit = true;
                }
            }, &ctx);
            if (ctx.hit) mirror(e.id, c);
        }
    }
    touch();
    return true;
}

bool EditorScene::rename_entity(EntityId id, StringView name) {
    DocEntity* e = doc_.find(id);
    if (!e || e->name.view() == name) return false;
    e->name.assign(name);
    touch();
    return true;
}

bool EditorScene::move_entity(EntityId id, usize index) {
    const i32 from = doc_.index_of(id);
    if (from < 0 || static_cast<usize>(from) == index) return false;
    doc_.move(id, index);
    touch();
    return true;
}

bool EditorScene::add_component(EntityId id, const refl::TypeRecord& type) {
    DocEntity* e = doc_.find(id);
    if (!e || !type.is_component() || e->find(type)) return false;
    const DocComponent* c = doc_.add_component(id, type);
    mirror(id, *c);
    touch();
    return true;
}

bool EditorScene::remove_component(EntityId id, const refl::TypeRecord& type) {
    if (!doc_.remove_component(id, type)) return false;
    const ecs::ComponentId cid = world_->find_component(type.type_hash);
    if (cid != ecs::kInvalidComponent) world_->remove_raw(runtime_entity(id), cid);
    touch();
    return true;
}

bool EditorScene::set_component(EntityId id, const refl::TypeRecord& type, const void* value) {
    DocEntity* e = doc_.find(id);
    DocComponent* c = e ? e->find(type) : nullptr;
    if (!c) return false;
    if (std::memcmp(c->data(), value, type.size) == 0) return false;
    std::memcpy(c->data(), value, type.size);
    mirror(id, *c);
    touch();
    return true;
}

}  // namespace mce::editor
