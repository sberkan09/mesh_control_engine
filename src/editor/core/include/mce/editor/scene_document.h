#pragma once
// SceneDocument — the canonical, serialized truth of one scene (editor-tools.md §2.3, §3.1).
//
// It is not the ECS: entities carry a persistent EntityId and a per-entity list of reflected
// component values (raw bytes of the C++ type, sorted by type name). EntityRef fields hold
// Entity{index = EntityId, generation = 1} (null stays null); the preview world maps them to its
// own handles. Text I/O is the .mcscene format through the cook's codec (mce/cook/scene_text.h),
// so what the editor saves is exactly what the cook accepts.
//
// The document is mutated only through EditorScene (the mutation funnel), which mirrors every
// change into the preview world. Headless: no ImGui, no GPU.

#include "mce/asset/guid.h"
#include "mce/containers/array.h"
#include "mce/cook/diagnostics.h"
#include "mce/ecs/entity.h"
#include "mce/fixed_string.h"
#include "mce/refl/refl.h"

namespace mce::editor {

using EntityId = u32;  // persistent within a document; never reused while it is open
inline constexpr EntityId kNoEntity = 0;

struct DocComponent {
    const refl::TypeRecord* type = nullptr;
    Array<u64> storage;  // type->size bytes, 8-byte aligned
    // On a prefab instance: the fields this instance overrides (bit i = type->fields[i]); the rest
    // come from the prefab root. Saved as exactly these fields, read back from the keys present.
    // Unused on other entities (they save their non-default fields).
    u64 overrides = 0;

    void* data() { return storage.data(); }
    const void* data() const { return storage.data(); }
};

struct DocEntity {
    EntityId id = kNoEntity;
    FixedString<64> name;
    // Prefab instance (null: none): the cook expands the prefab here and the overridden fields of
    // `components` (DocComponent::overrides) are written over its root (FR-EDT-080). The editor
    // shows the prefab's entities as preview ghosts (EditorScene) and keeps only the link and the
    // overrides in the document.
    asset::Guid prefab;
    Array<DocComponent> components;  // sorted by type name

    const DocComponent* find(const refl::TypeRecord& t) const;
    DocComponent* find(const refl::TypeRecord& t);
};

// Document-side spelling of an entity reference.
inline Entity doc_entity_ref(EntityId id) { return id == kNoEntity ? Entity::null : Entity{id, 1}; }
inline EntityId doc_entity_id(Entity e) { return e.is_null() ? kNoEntity : e.index; }

// Field masks for DocComponent::overrides (top-level fields; a type has at most 64 of them).
u64 all_fields_mask(const refl::TypeRecord& type);
// The top-level fields whose bytes differ between two values of `type` (Transient ones excluded).
u64 changed_fields_mask(const refl::TypeRecord& type, const void* a, const void* b);
// Copies the fields in `mask` from `src` to `dst` (both values of `type`).
void copy_fields(const refl::TypeRecord& type, u64 mask, const void* src, void* dst);
// The fields that hold an EntityRef anywhere inside (they do not carry across documents).
u64 entity_ref_fields_mask(const refl::TypeRecord& type);

// What the document is: a scene (.mcscene) or a prefab asset (.mcprefab, entity 0 = the root;
// the same entity model, no "settings").
enum class DocKind : u8 { Scene, Prefab };

// Calls fn(Entity* field) for every EntityRef field of a `type` value (nested structs and fixed
// arrays included).
void visit_entity_refs(const refl::TypeRecord& type, void* value, void (*fn)(Entity* field, void* user), void* user);

class SceneDocument {
public:
    static constexpr u32 kFormatVersion = 1;

    SceneDocument();

    // Empty document with a fresh GUID.
    void reset(StringView scene_name);

    asset::Guid guid() const { return guid_; }
    StringView name() const { return name_.view(); }
    void set_name(StringView n) { name_.assign(n); }
    void set_guid(asset::Guid g) { guid_ = g; }
    DocKind kind() const { return kind_; }
    void set_kind(DocKind k) { kind_ = k; }

    usize size() const { return entities_.size(); }
    const DocEntity& at(usize i) const { return entities_[i]; }
    Span<const DocEntity> entities() const { return entities_.span(); }
    Span<DocEntity> entities_mut() { return entities_.span(); }
    const DocEntity* find(EntityId id) const;
    DocEntity* find(EntityId id);
    i32 index_of(EntityId id) const;  // -1 when absent

    // ---- structural edits (EditorScene calls these; nothing else should) ----
    // Inserts an entity without components at `at` (-1 = end). `id` = kNoEntity assigns a fresh one.
    EntityId insert(StringView name, i32 at = -1, EntityId id = kNoEntity);
    bool erase(EntityId id);
    // Adds a default-constructed `type` value (or returns the existing one).
    DocComponent* add_component(EntityId id, const refl::TypeRecord& type);
    bool remove_component(EntityId id, const refl::TypeRecord& type);
    // Moves the entity to `index` (hierarchy order = file order).
    bool move(EntityId id, usize index);
    // Inserts a whole entity (its id must be free) at `at` (clamped): how undo restores one.
    void insert_entity(DocEntity&& e, usize at);
    // Same GUID, name and settings: the documents differ in their entities at most.
    bool same_header(const SceneDocument& other) const;

    // ---- text (.mcscene / .mcprefab) ----
    // Replaces the document with `text` ("mce-scene" or "mce-prefab"; kind() follows). On error the document is left empty and `diags` holds
    // file:line:col rows. A "version" newer than kFormatVersion is refused (never guessed at).
    bool parse(StringView text, StringView file, cook::DiagList& diags);
    // Deterministic, diff-friendly text: one component object per line, non-default fields only
    // (a prefab instance: its overridden fields).
    void serialize(cook::String& out) const;

private:
    asset::Guid guid_;
    FixedString<64> name_;
    Array<DocEntity> entities_;
    cook::String settings_;  // the "settings" object, carried verbatim (not edited at MVP)
    EntityId next_id_ = 1;
    DocKind kind_ = DocKind::Scene;
};

}  // namespace mce::editor
