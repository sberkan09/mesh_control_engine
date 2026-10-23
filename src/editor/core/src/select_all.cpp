#include "mce/editor/select_all.h"

namespace mce::editor {

Array<EntityId> select_all(const EditorScene& scene) {
    Array<EntityId> out;
    for (const DocEntity& e : scene.doc().entities()) out.push_back(e.id);
    return out;
}

Array<EntityId> invert_selection(const EditorScene& scene) {
    Array<EntityId> out;
    for (const DocEntity& e : scene.doc().entities())
        if (!scene.selection().contains(e.id)) out.push_back(e.id);
    return out;
}

}  // namespace mce::editor
