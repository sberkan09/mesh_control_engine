#pragma once
// parents_first — the order in which a command that places entities in world space must apply its results (M-ALIGN2).
// Moving a parent carries its children, so a child set before its parent ends up displaced; a child set after it
// lands where the command put it. Header-only and small: the commands that move a selection include it.

#include <utility>

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

// Stable-sorts `items` so that an entity comes after every other entity of `items` that is its ancestor.
// `id_of(item)` names the entity. The depth is the number of listed ancestors, so unrelated entities keep their order.
template <class T, class IdOf>
void sort_parents_first(const EditorScene& scene, Array<T>& items, IdOf id_of) {
    // Depths first, from the untouched list: the sort below reorders `items` while it runs.
    Array<usize> depth;
    for (const T& item : items) {
        usize d = 0;
        for (const T& other : items) d += scene.is_ancestor(id_of(other), id_of(item)) ? 1 : 0;
        depth.push_back(d);
    }
    // Stable insertion sort of both lists by depth (selections are small).
    for (usize i = 1; i < items.size(); ++i) {
        for (usize j = i; j > 0 && depth[j - 1] > depth[j]; --j) {
            std::swap(depth[j - 1], depth[j]);
            std::swap(items[j - 1], items[j]);
        }
    }
}

}  // namespace mce::editor
