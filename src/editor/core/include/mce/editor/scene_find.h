#pragma once
// find_entities / format_hits — which entities of a scene document match a question (M-SCENEFIND): a part of the
// name, a component they carry, whether they are prefab instances. Every condition given must hold. Headless,
// document-level (no ECS), tested in CI.

#include "mce/containers/array.h"
#include "mce/cook/string.h"
#include "mce/editor/scene_document.h"

namespace mce::editor {

struct FindQuery {
    StringView name;                              // part of the entity name, ignoring case; empty: any name
    const refl::TypeRecord* component = nullptr;  // the entity must carry it; null: any
    bool prefab_instances_only = false;           // only entities linked to a prefab
};

struct FindHit {
    EntityId id = kNoEntity;
    FixedString<64> name;
    cook::String path;  // the hierarchy path, `Room/Wall/Torch`
};

// The matches in file order. An empty query matches every entity.
Array<FindHit> find_entities(const SceneDocument& doc, const FindQuery& query);

// One line per hit, `Room/Wall/Torch (id 3)`; `no matches` for none.
cook::String format_hits(Span<const FindHit> hits);

}  // namespace mce::editor
