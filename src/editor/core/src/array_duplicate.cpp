#include "mce/editor/array_duplicate.h"

#include <cmath>

#include "mce/editor/duplicate_offset.h"

namespace mce::editor {

usize array_duplicate(EditorScene& scene, Span<const EntityId> ids, u32 columns, u32 rows, f32 dx, f32 dy,
                      Array<EntityId>* created) {
    if (columns == 0 || rows == 0 || (columns == 1 && rows == 1)) return 0;
    if (!std::isfinite(dx) || !std::isfinite(dy)) return 0;
    usize live = 0;  // the entities one cell copies: the listed ones that exist
    for (EntityId id : ids) live += scene.doc().find(id) != nullptr ? 1 : 0;
    if (live == 0) return 0;
    const usize cells = static_cast<usize>(columns) * rows - 1;
    if (cells > kMaxArrayCopies || cells * live > kMaxArrayCopies) return 0;

    Array<EntityId> made;
    scene.begin_edit("Array duplicate");
    for (u32 j = 0; j < rows; ++j)
        for (u32 i = 0; i < columns; ++i) {
            if (i == 0 && j == 0) continue;
            duplicate_offset(scene, ids, static_cast<f32>(i) * dx, static_cast<f32>(j) * dy, &made);
        }
    scene.end_edit();
    if (created != nullptr)
        for (EntityId id : made) created->push_back(id);
    return made.size();
}

}  // namespace mce::editor
