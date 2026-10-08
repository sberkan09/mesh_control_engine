#pragma once
// EditorScene — the mutation funnel (editor-tools.md §2.3, §3.1-§3.3): one SceneDocument, its
// derived preview World, the selection, and the EntityId <-> Entity map. Every authoring
// operation (inspector edit, gizmo drag, create/delete, paste) goes through a method here, which
// (1) mutates the document, (2) mirrors the identical write into the preview world, (3) bumps
// the revision and marks the scene dirty. The preview world runs no systems: it exists so edits
// are visible the same frame (two-world model), and verify_world() is the drift killer.

#include "mce/cook/diagnostics.h"
#include "mce/ecs/hierarchy.h"
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

// Undo/redo (editor-tools.md §2.4, FR-EDT-040, ADR-0030 §1–§3). Every mutation records the entities it
// touches as they were before (an absent entity included) and, when the edit ends, as they are
// after: an entry restores either side wholesale, so undoing any mix of commands is exact by
// construction (the fuzz test: N random commands, undo all -> byte-identical document). Snapshots
// are copies of DocComponent bytes, never pointers into the document.
struct EntitySnapshot {
    EntityId id = kNoEntity;
    i32 index = -1;  // position in the document; -1: the entity does not exist on this side
    FixedString<64> name;
    asset::Guid prefab;
    Array<DocComponent> components;
};

struct UndoEntry {
    FixedString<64> label;
    Array<EntitySnapshot> before, after;  // same ids, same order
    u64 coalesce_key = 0;                 // non-zero: a following edit with the same key merges in
    usize bytes = 0;
};

class EditorScene {
public:
    static constexpr usize kMaxUndoEntries = 256;
    static constexpr usize kMaxUndoBytes = 16u << 20;

    EditorScene();
    ~EditorScene();
    EditorScene(const EditorScene&) = delete;
    EditorScene& operator=(const EditorScene&) = delete;

    const SceneDocument& doc() const { return doc_; }
    World& world() { return *world_; }
    SelectionSet& selection() { return selection_; }
    const SelectionSet& selection() const { return selection_; }
    // Shift-click: selects every entity from the anchor to `to` in document order, the anchor
    // staying the anchor (nothing selected: just `to`). `additive` (Ctrl+Shift) keeps the rest.
    void select_range(EntityId to, bool additive = false);

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

    // ---- hierarchy (ChildOf, FR-ENT-050, ADR-0030 §5) ----
    // Deleting an entity deletes its subtree, as the runtime does (orphans-destroy).
    EntityId parent_of(EntityId id) const;  // kNoEntity: a root, or a ChildOf naming nothing
    // Direct children in document order; `id` == kNoEntity lists the roots.
    void children_of(EntityId id, Array<EntityId>& out) const;
    bool is_ancestor(EntityId ancestor, EntityId id) const;
    // Where the entity sits in the scene: its LocalTransform composed up the ChildOf chain.
    WorldTransform world_transform(EntityId id) const;
    // The LocalTransform that places `id` at `world` under its current parent.
    LocalTransform local_for_world(EntityId id, const WorldTransform& world) const;
    // Re-parents `child` (`parent` == kNoEntity: makes it a root) keeping where it sits in the
    // scene, and moves it after its new parent's subtree in the document. One undo entry. False
    // when nothing changed, an entity is missing, or the link would close a cycle.
    bool set_parent(EntityId child, EntityId parent);

    // ---- undo/redo (FR-EDT-040) ----
    // Groups every mutation until the matching end_edit() into one undo entry named `label`
    // (nests: only the outermost pair counts). A mutation outside any group is its own entry.
    // `coalesce_key` != 0 merges this edit into the previous entry when that one has the same key
    // and nothing sealed it (one inspector drag = one entry); 0 never merges.
    void begin_edit(StringView label, u64 coalesce_key = 0);
    void end_edit();
    bool in_edit() const { return edit_depth_ > 0; }
    // Ends coalescing: the next edit starts a new entry (mouse release, focus change).
    void seal_undo() { sealed_ = true; }
    bool undo();
    bool redo();
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    StringView undo_label() const { return undo_.empty() ? StringView() : undo_.back().label.view(); }
    StringView redo_label() const { return redo_.empty() ? StringView() : redo_.back().label.view(); }
    usize undo_count() const { return undo_.size(); }
    usize redo_count() const { return redo_.size(); }
    usize undo_bytes() const { return undo_bytes_; }
    void clear_undo();
    // Replaces the entities with those of `text` (the scene file changed on disk — another tool, an
    // MCP client) as one undoable entry. Falls back to a fresh history when the GUID, name or
    // settings differ. False (nothing changed) when the text does not parse.
    bool reload_text(StringView text, StringView file, cook::DiagList& diags);
    // The document matches its file again (after reload_text of the file's own contents).
    void mark_clean() { dirty_ = false; }

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
    // RAII edit group for one public command.
    struct Edit {
        Edit(EditorScene& s, StringView label, u64 key = 0) : scene(s) { scene.begin_edit(label, key); }
        ~Edit() { scene.end_edit(); }
        EditorScene& scene;
    };
    // Records `id` as it is now (before this edit changes it), once per edit.
    void record(EntityId id);
    void snapshot(EntityId id, EntitySnapshot& out) const;
    void apply(const Array<EntitySnapshot>& side);
    void push_entry(UndoEntry&& e);
    void touch();
    void mirror(EntityId id, const DocComponent& c);
    void spawn_mirror(const DocEntity& e);
    // Writes every entity's composed WorldTransform into the preview world once the document has
    // used ChildOf (the renderer draws WorldTransform when present).
    void sync_world_transforms();
    bool world_transforms_ = false;

    SceneDocument doc_;
    mem::UniquePtr<World> world_;
    SelectionSet selection_;
    Array<Entity> runtime_;  // indexed by EntityId (sparse; null = none)
    FixedString<512> path_;
    u64 revision_ = 0;
    bool dirty_ = false;

    Array<UndoEntry> undo_, redo_;
    usize undo_bytes_ = 0;
    UndoEntry pending_;  // the open edit's before-snapshots
    // Structural changes made so far in the open edit: a snapshot recorded after one of them
    // maps its index back to the document as the edit found it.
    struct StructOp {
        enum Kind : u8 { Erase, Insert, Move } kind;
        usize a, b;  // Erase/Insert: index; Move: from, to
        EntityId id;
    };
    Array<StructOp> struct_ops_;
    void note(StructOp::Kind kind, EntityId id, usize a, usize b = 0) {
        if (edit_depth_ > 0) struct_ops_.push_back(StructOp{kind, a, b, id});
    }
    i32 index_before_edit(i32 index) const;
    u32 edit_depth_ = 0;
    bool sealed_ = true;
};

}  // namespace mce::editor
