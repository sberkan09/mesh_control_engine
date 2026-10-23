#include "mce/editor/select_related.h"

namespace mce::editor {

namespace {

bool has(const Array<EntityId>& a, EntityId id) {
    for (const EntityId x : a)
        if (x == id) return true;
    return false;
}

void add(Array<EntityId>& out, EntityId id) {
    if (id != kNoEntity && !has(out, id)) out.push_back(id);
}

}  // namespace

Array<EntityId> select_related(const EditorScene& scene, Span<const EntityId> from, Relation relation) {
    Array<EntityId> out;
    Array<EntityId> kids;
    for (const EntityId id : from) {
        switch (relation) {
            case Relation::Parent:
                add(out, scene.parent_of(id));
                break;
            case Relation::Children:
                scene.children_of(id, kids);
                for (const EntityId k : kids) add(out, k);
                break;
            case Relation::Descendants: {
                // Breadth first over what `out` already holds; a ChildOf cycle ends because every entity is added once.
                const usize start = out.size();
                scene.children_of(id, kids);
                for (const EntityId k : kids) add(out, k);
                for (usize i = start; i < out.size(); ++i) {
                    scene.children_of(out[i], kids);
                    for (const EntityId k : kids) add(out, k);
                }
                break;
            }
            case Relation::Siblings:
                scene.children_of(scene.parent_of(id), kids);
                for (const EntityId k : kids) add(out, k);
                break;
        }
    }
    return out;
}

}  // namespace mce::editor
