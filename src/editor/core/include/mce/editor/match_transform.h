#pragma once
// match_transform — copy one part of the anchor's transform onto the other selected entities (M-MATCHXFORM). The
// anchor is the first selected entity. Position is matched in world space (a parent's transform is respected);
// rotation and scale are the local values. One undo entry. Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

enum class MatchPart : u8 { Position, Rotation, Scale };

// Sets that part of every entity of `ids` (except `anchor`) that has a LocalTransform to the anchor's. Returns how
// many entities changed (0: the anchor has no transform, or everything already matched; no undo entry then).
usize match_transforms(EditorScene& scene, Span<const EntityId> ids, EntityId anchor, MatchPart part);

}  // namespace mce::editor
