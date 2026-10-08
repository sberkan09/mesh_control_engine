#pragma once
// EditorScene — the mutation funnel (editor-tools.md §2.3, §3.1-§3.3): one SceneDocument, its
// derived preview World, the selection, and the EntityId <-> Entity map. Every authoring
// operation (inspector edit, gizmo drag, create/delete, paste) goes through a method here, which
// (1) mutates the document, (2) mirrors the identical write into the preview world, (3) bumps
// the revision and marks the scene dirty. The preview world runs no systems: it exists so edits
// are visible the same frame (two-world model), and verify_world() is the drift killer.

#include "mce/cook/diagnostics.h"
#include "mce/ecs/world.h"
#include "mce/editor/scene_document.h"
#include "mce/memory/allocator.h"

namespace mce::editor {

class SelectionSet {
public:
    void set(EntityId e);
    void toggle(EntityId e);
    void clear() { items_.clear(); }
    bool contains(EntityId e) const;
    void remove(EntityId e);
    Span<const EntityId> items() const { return items_.span(); }
    // The inspector subject and gizmo pivot; kNoEntity when nothing is selected.
    EntityId anchor() const { return items_.empty() ? kNoEntity : items_[0]; }
    bool empty() const { return items_.empty(); }

private:
    Array<EntityId> items_;
};

class EditorScene {
public:
    EditorScene();
    ~EditorScene();
    EditorScene(const EditorScene&) = delete;
    EditorScene& operator=(const EditorScene&) = delete;

    const SceneDocument& doc() const { return doc_; }
    World& world() { return *world_; }
    SelectionSet& selection() { return selection_; }
    const SelectionSet& selection() const { return selection_; }

    // ---- document lifecycle ----
    void new_scene(StringView name);
    // Loads a .mcscene file; on failure the current scene is kept and `diags` says why.
    Result<void> open(StringView path, cook::DiagList& diags);
    // Replaces the document with `text` (an autosave) but keeps path(); the scene is dirty after.
    bool recover(StringView text, cook::DiagList& diags);
    // Atomic save (temp file + rename): a crash mid-write leaves the previous file intact.
    Result<void> save(StringView path);
    // Serialization only (also used for autosave and copy/paste).
    void serialize(cook::String& out) const { doc_.serialize(out); }
    // Fresh GUID (Save As to a new file: two files must never share one).
    void assign_new_guid();

    StringView path() const { return path_.view(); }  // empty until saved/opened
    bool dirty() const { return dirty_; }
    u64 revision() const { return revision_; }        // bumps on every mutation

    // ---- commands (the only writers) ----
    EntityId create_entity(StringView name, bool with_transform = true, i32 at = -1);
    bool destroy_entity(EntityId id);
    bool rename_entity(EntityId id, StringView name);
    bool move_entity(EntityId id, usize index);
    bool add_component(EntityId id, const refl::TypeRecord& type);
    bool remove_component(EntityId id, const refl::TypeRecord& type);
    // Replaces the whole component value (inspector / gizmo commit). No-op when unchanged.
    bool set_component(EntityId id, const refl::TypeRecord& type, const void* value);
    // ---- clipboard (FR-EDT-030 copy/paste) ----
    // Writes `ids` (in document order) as a .mcscene text: the system clipboard format.
    // References between copied entities survive; references to anything else become null.
    void copy_entities(Span<const EntityId> ids, cook::String& out) const;
    // Inserts the entities of a copy_entities() text at `at` (-1: the end) with fresh ids and
    // selects them. False (nothing changed) when the text is not a scene fragment.
    bool paste_entities(StringView text, i32 at, cook::DiagList& diags, Array<EntityId>* pasted = nullptr);
    // Copy + paste right after the last of `ids`.
    bool duplicate_entities(Span<const EntityId> ids, Array<EntityId>* created = nullptr);

    // Typed convenience for reflected components.
    template <class T>
    bool set(EntityId id, const T& value) {
        return set_component(id, refl::type_of<T>(), &value);
    }
    template <class T>
    const T* get(EntityId id) const {
        const DocEntity* e = doc_.find(id);
        const DocComponent* c = e ? e->find(refl::type_of<T>()) : nullptr;
        return c ? static_cast<const T*>(c->data()) : nullptr;
    }

    // ---- preview world ----
    Entity runtime_entity(EntityId id) const;
    EntityId entity_for(Entity e) const;
    // Rebuilds the preview world from the document (load, or the user's "fix drift" command).
    void rebuild_world();
    // True when every document component equals its preview-world mirror (after ref mapping) and
    // the world holds nothing else. CI's two-world consistency check.
    bool verify_world() const;

private:
    void touch();
    void mirror(EntityId id, const DocComponent& c);
    void spawn_mirror(const DocEntity& e);

    SceneDocument doc_;
    mem::UniquePtr<World> world_;
    SelectionSet selection_;
    Array<Entity> runtime_;  // indexed by EntityId (sparse; null = none)
    FixedString<512> path_;
    u64 revision_ = 0;
    bool dirty_ = false;
};

}  // namespace mce::editor
