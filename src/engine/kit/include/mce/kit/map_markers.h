#pragma once
// Map markers as data (M-MISSION step 2): a component that puts an icon on the minimap and the full map, and
// the pure functions a host feeds the UI's Minimap widget with (mce/ui/minimap.h; gameplay never includes the
// ui, so the widget reads plain structs the host copies out of this). kit::install_stock installs it. Scenes
// without a MapMarker are unchanged: nothing here runs unless a host asks (no system, no state).
//
//   MapMarker  on an entity: icon, colour, layer, an optional label, `always_visible`, and `follow_objective`
//
// Where a marker is. With no `follow_objective` it is the entity's own world position (x, z), always shown.
// Objective markers (M-MISSION step 1) need no position of their own:
//   - `follow_objective` names an Objective: the marker sits at that objective's target (its `target`, or the
//     entity named `tag`) while the objective is Active, and is hidden when it is Pending or Done.
//   - `follow_objective` names a Mission: the marker follows the mission's current objective the same way, and
//     is hidden while the mission is Idle, Complete or Failed. So one marker on a mission shows its objectives
//     one after another, with nothing added per objective.
//   - null `follow_objective` on an entity that has an Objective (or a Mission) follows that one: add a bare
//     MapMarker to the objective (or the mission) entity and its target gets a marker while it is active.
// A followed target that cannot be found hides the marker.
//
// Coordinates are the UI's: metres (x east, z south) on the ground plane, heading = yaw about +Y in radians (0
// faces -z).

#include "mce/containers/array.h"
#include "mce/ecs/entity.h"
#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct MapMarker {
    u32 icon = 0;                       // a region of the minimap skin's sprite set; 0: a flat square
    math::Color color{1, 1, 1, 1};
    u32 layer = 0;                      // 0..31; a Minimap's layer_mask picks which layers it shows
    FixedString<32> label;              // optional, for the full map's list and scripts
    u8 always_visible = 0;              // 1: beyond the minimap radius it sticks to the rim
    u8 pad_[3] = {};
    Entity follow_objective;            // an Objective or a Mission to follow (see above); null: own position
};
MCE_COMPONENT(MapMarker, (icon, Save, Help{"a region of the minimap skin's sprite set; 0 draws a flat square"}), (color, Save),
              (layer, Save, Range{0, 31}, Help{"0..31; a Minimap's layer_mask picks the layers it shows"}),
              (label, Save, Help{"optional text for the full map and scripts"}),
              (always_visible, Save, Help{"1: beyond the minimap radius the marker sticks to the rim"}),
              (follow_objective, Save, Help{"an Objective or Mission whose active target this marker shows; empty: the entity's own position, or the objective/mission it sits on"}))

// One marker as the host draws it.
struct MapMarkerView {
    Entity source;                      // the entity that holds the MapMarker
    math::vec2 pos;                     // x, z in metres
    math::Color color;
    u32 icon = 0;
    u32 layer = 0;
    bool always_visible = false;
    FixedString<32> label;
};

// Where `marker` (an entity with a MapMarker) is and whether it shows right now; false hides it.
bool map_marker_view(World& world, Entity marker, MapMarkerView& out);
// Every shown marker, in the world's entity order (the same scene gives the same list). Clears `out` first.
void collect_map_markers(World& world, Array<MapMarkerView>& out);

// The tracked entity's place for a Minimap: the entity named `name` (scene::find_entity), its x, z and yaw.
// False when there is no such entity or it has no transform.
bool map_tracked_view(World& world, StringView name, math::vec2& pos, f32& heading);

// Registers MapMarker. Idempotent.
void install_map_markers(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
