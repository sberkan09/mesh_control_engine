#pragma once
// lint_scene / format_lint — mistakes in a scene document that load fine and misbehave later (M-SCENELINT): an
// entity without a name, two siblings with one name (a path like Room/Wall then names both), a scale of zero on an
// axis (the entity has no volume and cannot be picked), a position, rotation or scale that is not a number, a parent
// that does not exist, a reference from another component (a follow target, a trigger link) to an entity the scene
// lacks. Headless, document-level (no ECS), tested in CI.

#include "mce/containers/array.h"
#include "mce/cook/string.h"
#include "mce/editor/scene_document.h"

namespace mce::editor {

enum class LintKind : u8 {
    EmptyName,
    DuplicateSiblingName,  // the second and later of the siblings that share a name
    ZeroScale,
    NonFiniteTransform,
    MissingParent,  // ChildOf names an entity the scene lacks, or the entity itself
    DanglingRef,    // another component refers to an entity the scene lacks (prefab instances are not checked)
};

struct LintFinding {
    EntityId id = kNoEntity;
    LintKind kind = LintKind::EmptyName;
    FixedString<64> name;
};

// The findings in the scene's entity order; a kind appears at most once per entity.
Array<LintFinding> lint_scene(const SceneDocument& doc);

// Repairs what `lint_scene` finds, in place, without losing data a scene could still mean: an empty name becomes
// "Entity <id>"; a second sibling with a taken name gets a numbered suffix (" (2)", ...); a parent that does not exist is
// dropped (the entity becomes a root); a reference to a missing entity is set to null; a position, rotation or scale
// that is not a number is reset to its default; a scale axis of zero becomes 1. Returns the number of findings fixed
// (0: nothing changed); `lint_scene` is clean afterwards. Edits the document directly, so it is for tools that load a
// file, fix it and save it, not for an open EditorScene (which has undo).
usize fix_scene_lint(SceneDocument& doc);

// One line per finding: `Wall (id 3): duplicate name among its siblings`. Empty text for no findings.
cook::String format_lint(Span<const LintFinding> findings);

}  // namespace mce::editor
