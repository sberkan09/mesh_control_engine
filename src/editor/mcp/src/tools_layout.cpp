// Layout tools (M-LAYOUTTOOLS step 2): align, distribute, snap, flip and match the transforms of several entities of
// the open scene. The same commands as the editor's Edit menu, each one undo step (scene_undo).

#include <cmath>

#include "mce/editor/align_entities.h"
#include "mce/editor/array_duplicate.h"
#include "mce/editor/flip_entities.h"
#include "mce/editor/match_transform.h"
#include "mce/editor/snap_entities.h"
#include "tool_args.h"
#include "tools_agent.h"

namespace mce::mcp {

namespace {

constexpr u32 kMaxLayoutEntities = 4096;

// "entities": an array of entity ids or names, at least `min` of them.
bool arg_entities(ToolCall& c, Array<editor::EntityId>& out, u32 min) {
    const cook::JsonValue v = arg(c, "entities");
    if (!v.valid() || !v.is_array()) return arg_missing(c, "entities", "an array of entity ids or names");
    if (v.size() < min || v.size() > kMaxLayoutEntities) {
        c.error += "argument 'entities' needs ";
        cook::appendf(c.error, "%u to %u entries", min, kMaxLayoutEntities);
        return false;
    }
    for (u32 i = 0; i < v.size(); ++i) {
        editor::EntityId id = editor::kNoEntity;
        if (!resolve_entity(c, "entities", v.at(i), id)) return false;
        out.push_back(id);
    }
    return true;
}

bool arg_axis(ToolCall& c, editor::AlignAxis& axis) {
    StringView s;
    if (!arg_string(c, "axis", s, true)) return false;
    if (s == "x")
        axis = editor::AlignAxis::X;
    else if (s == "y")
        axis = editor::AlignAxis::Y;
    else
        return arg_missing(c, "axis", "\"x\" or \"y\"");
    return true;
}

void changed(ToolCall& c, bool did) { c.out.begin_object().key("changed").boolean(did).end_object(); }

bool entity_align(ToolCall& c) {
    Array<editor::EntityId> ids;
    editor::AlignAxis axis = editor::AlignAxis::X;
    StringView mode_name;
    if (!arg_entities(c, ids, 2) || !arg_axis(c, axis) || !arg_string(c, "mode", mode_name, true)) return false;
    editor::AlignMode mode = editor::AlignMode::Min;
    if (mode_name == "min")
        mode = editor::AlignMode::Min;
    else if (mode_name == "center")
        mode = editor::AlignMode::Center;
    else if (mode_name == "max")
        mode = editor::AlignMode::Max;
    else
        return arg_missing(c, "mode", "\"min\", \"center\" or \"max\"");
    changed(c, editor::align_entities(scene(c), Span<const editor::EntityId>(ids.data(), ids.size()), axis, mode));
    return true;
}

bool entity_distribute(ToolCall& c) {
    Array<editor::EntityId> ids;
    editor::AlignAxis axis = editor::AlignAxis::X;
    if (!arg_entities(c, ids, 3) || !arg_axis(c, axis)) return false;
    changed(c, editor::distribute_entities(scene(c), Span<const editor::EntityId>(ids.data(), ids.size()), axis));
    return true;
}

bool entity_flip(ToolCall& c) {
    Array<editor::EntityId> ids;
    editor::AlignAxis axis = editor::AlignAxis::X;
    if (!arg_entities(c, ids, 1) || !arg_axis(c, axis)) return false;
    changed(c, editor::flip_entities(scene(c), Span<const editor::EntityId>(ids.data(), ids.size()), axis));
    return true;
}

bool entity_snap(ToolCall& c) {
    Array<editor::EntityId> ids;
    if (!arg_entities(c, ids, 1)) return false;
    const cook::JsonValue step = arg(c, "step");
    if (!step.valid() || !step.is_number() || !(step.as_number() > 0.0) || step.as_number() > 1.0e6)
        return arg_missing(c, "step", "a number above 0 (the grid size in world units)");
    const usize n = editor::snap_entities(scene(c), Span<const editor::EntityId>(ids.data(), ids.size()),
                                          static_cast<f32>(step.as_number()));
    c.out.begin_object().key("moved").num_u(n).end_object();
    return true;
}

bool entity_match_transform(ToolCall& c) {
    Array<editor::EntityId> ids;
    editor::EntityId anchor = editor::kNoEntity;
    StringView part_name;
    if (!arg_entities(c, ids, 1) || !arg_entity(c, "anchor", anchor) || !arg_string(c, "part", part_name, true))
        return false;
    editor::MatchPart part = editor::MatchPart::Position;
    if (part_name == "position")
        part = editor::MatchPart::Position;
    else if (part_name == "rotation")
        part = editor::MatchPart::Rotation;
    else if (part_name == "scale")
        part = editor::MatchPart::Scale;
    else
        return arg_missing(c, "part", "\"position\", \"rotation\" or \"scale\"");
    const usize n =
        editor::match_transforms(scene(c), Span<const editor::EntityId>(ids.data(), ids.size()), anchor, part);
    c.out.begin_object().key("changed").num_u(n).end_object();
    return true;
}

bool entity_array_duplicate(ToolCall& c) {
    Array<editor::EntityId> ids;
    u64 columns = 1, rows = 1;
    if (!arg_entities(c, ids, 1) || !arg_uint(c, "columns", columns, 1, editor::kMaxArrayCopies + 1, true) ||
        !arg_uint(c, "rows", rows, 1, editor::kMaxArrayCopies + 1, false))
        return false;
    const cook::JsonValue dx = arg(c, "dx"), dy = arg(c, "dy");
    if ((dx.valid() && !dx.is_number()) || (dy.valid() && !dy.is_number()))
        return arg_missing(c, "dx", "numbers (the step in world units)");
    if (columns * rows < 2) return fail(c, "columns x rows must be at least 2 (cell 1,1 is the originals)");
    Array<editor::EntityId> made;
    const usize n = editor::array_duplicate(scene(c), Span<const editor::EntityId>(ids.data(), ids.size()),
                                            static_cast<u32>(columns), static_cast<u32>(rows),
                                            dx.valid() ? static_cast<f32>(dx.as_number()) : 0.0f,
                                            dy.valid() ? static_cast<f32>(dy.as_number()) : 0.0f, &made);
    if (n == 0) return fail(c, "nothing was copied: check the entities, a finite step, and at most 4096 copies in all");
    c.out.begin_object().key("created").num_u(n).key("ids").begin_array();
    for (editor::EntityId id : made) c.out.num_u(id);
    c.out.end_array().end_object();
    return true;
}

#define MCE_ENTITIES \
    R"j("entities":{"type":"array","items":{"type":["integer","string"]},"description":"Entity ids, or names when unique"})j"
#define MCE_AXIS R"j("axis":{"type":"string","enum":["x","y"]})j"

const ToolDef kLayoutTools[] = {
    {"entity_align",
     "Move several entities onto one world line: axis x or y, to the lowest (min), middle of the extremes (center) or "
     "highest "
     "(max) position of the group. Parents and children may be listed together, in any order. Returns changed (false: "
     "they "
     "already lined up). One undo step.",
     R"j({"type":"object","properties":{)j" MCE_ENTITIES "," MCE_AXIS
     R"j(,"mode":{"type":"string","enum":["min","center","max"]}},"required":["entities","axis","mode"]})j",
     entity_align},
    {"entity_distribute",
     "Space three or more entities evenly on x or y between the two outermost, keeping their order along the axis. "
     "Returns "
     "changed (false: already even). One undo step.",
     R"j({"type":"object","properties":{)j" MCE_ENTITIES "," MCE_AXIS R"j(},"required":["entities","axis"]})j",
     entity_distribute},
    {"entity_snap",
     "Round the world x and y of the entities to a grid of 'step' world units. Returns moved (how many changed). One "
     "undo step.",
     R"j({"type":"object","properties":{)j" MCE_ENTITIES
     R"j(,"step":{"type":"number","exclusiveMinimum":0}},"required":["entities","step"]})j",
     entity_snap},
    {"entity_flip",
     "Mirror the entities across the middle of the group on x or y; one entity flips in place (its scale changes "
     "sign). Returns "
     "changed. One undo step.",
     R"j({"type":"object","properties":{)j" MCE_ENTITIES "," MCE_AXIS R"j(},"required":["entities","axis"]})j",
     entity_flip},
    {"entity_match_transform",
     "Give the entities another entity's world position, rotation or scale (part). Returns changed (how many "
     "differed). One "
     "undo step.",
     R"j({"type":"object","properties":{)j" MCE_ENTITIES
     R"j(,"anchor":{"type":["integer","string"],"description":"The entity to copy from"},"part":{"type":"string","enum":["position","rotation","scale"]}},"required":["entities","anchor","part"]})j",
     entity_match_transform},
    {"entity_array_duplicate",
     "Copy the entities into a grid in one call: 'columns' x 'rows' cells whose first cell is the originals, the copy "
     "in "
     "column i and row j at the original's world position plus (i * dx, j * dy). Copied parents keep their copied "
     "children. At most 4096 copies in all. Returns created and the new ids, cell by cell. One undo step.",
     R"j({"type":"object","properties":{)j" MCE_ENTITIES
     R"j(,"columns":{"type":"integer","minimum":1},"rows":{"type":"integer","minimum":1,"default":1},"dx":{"type":"number","default":0},"dy":{"type":"number","default":0}},"required":["entities","columns"]})j",
     entity_array_duplicate},
};

}  // namespace

Span<const ToolDef> layout_tool_table() {
    return Span<const ToolDef>(kLayoutTools, sizeof(kLayoutTools) / sizeof(kLayoutTools[0]));
}

}  // namespace mce::mcp
