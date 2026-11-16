#pragma once
// select_related — the entities around a selection in the ChildOf tree (M-SELRELATED): their parents, their
// children, everything below them, or their siblings. The shell turns the result into the new selection
// (Alt+Up, Alt+Down, Alt+Shift+Down, Edit > Select). Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

enum class Relation : u8 {
    Parent,       // the parent of each entity (roots have none)
    Children,     // the direct children of each
    Descendants,  // everything below each, children first
    Siblings,     // each entity and the other children of its parent (roots are siblings of each other)
};

// Each entity once, in the order found; empty when nothing is related (a root's parent, a leaf's children).
Array<EntityId> select_related(const EditorScene& scene, Span<const EntityId> from, Relation relation);

}  // namespace mce::editor
