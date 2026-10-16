#pragma once
// ProcLevel (M-PROCGEN step 3): a level made from a seed. An entity carrying ProcLevel names a generator
// (procgen.h), a size and a seed; kit_proclevel (FixedUpdate_Pre) builds the level as ordinary entities
// beside it and rebuilds it when the seed, size, kind or position changes.
//
//   - Walls are merged into rectangles (rows first, then equal-width neighbours stack), each a Static
//     RigidBody2D + box Collider2D + solid Sprite; one Sprite is the floor. A 128x128 cave is a few hundred
//     rectangles, not 16384 tiles.
//   - Two ProcMarker entities, Start and Exit, sit at the centres of their cells. With `path_markers` a Path
//     marker (index 1, 2, ... n) stands on each cell of a shortest 4-neighbour walk from Start to Exit (n is the
//     Exit cell): a guide for hints, bots and tests.
//   - Everything built carries ProcLevelOwned{owner}. The ProcLevel entity is the one that is saved
//     (give it a SaveKey); the built entities are not: loading rebuilds them from the seed, so a replay and a
//     save/load give the same level. A project with no ProcLevel creates nothing.
//   - `reroll` is read and cleared by the next kit_proclevel run: it adds one to the seed. Scripts do
//     mce.set(e, "ProcLevel", "reroll", true) (or set "seed").
//   - Destroying the ProcLevel entity destroys what it built.
//
// Cell (x, y) of the generator's grid has y = 0 at the top (as the text rows read); the world has y up. The
// entity's LocalTransform position is the bottom-left corner of the level; a cell's centre is
// position + ((x + .5) * tile_size, (height - y - .5) * tile_size). Pathfinding: proclevel_nav_grid gives a
// NavGrid with the same origin and cell size, so mark_static_colliders (pathfinding.h) lines up with the walls.
//
// ProcLevel holds authored fields; the built hash is a plain member (not reflected), so it is neither saved
// nor hashed. kit::install_stock installs it.

#include "mce/core.h"
#include "mce/ecs/entity.h"
#include "mce/kit/pathfinding.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

enum class ProcKind : u8 { Rooms, Caves, Walk, Noise };
MCE_ENUM(ProcKind, Rooms, Caves, Walk, Noise)

struct ProcLevel {
    bool enabled = true;
    bool reroll = false;
    bool path_markers = false;
    ProcKind kind = ProcKind::Rooms;
    u32 width = 48;
    u32 height = 32;
    u32 seed = 1;
    f32 tile_size = 1.0f;
    u32 param = 0;
    math::Color wall_color{0.25f, 0.25f, 0.3f, 1.0f};
    math::Color floor_color{0.1f, 0.1f, 0.12f, 1.0f};
    i16 layer = 0;
    u8 pad2_[2] = {};
    u64 built = 0;  // hash of what is built; 0: nothing yet. Not reflected: rebuilt on load.
};
MCE_COMPONENT(ProcLevel, (enabled, Help{"off: nothing is built (what was built stays)"}),
              (reroll, Help{"set true: the next tick adds one to the seed and clears this"}),
              (path_markers, Help{"also stand a Path marker (index 1..n) on each cell of a shortest walk from Start to Exit"}),
              (kind, Help{"Rooms: BSP rooms and corridors; Caves: cellular automaton; Walk: drunkard's walk; Noise: value-noise blobs"}),
              (width, Range{8, 512}), (height, Range{8, 512}), (seed, Help{"the level is a function of kind, size, param and seed"}),
              (tile_size, Help{"metres per cell"}),
              (param, Help{"0: the generator's default. Caves: wall fill %; Walk: floor coverage %; Noise: floor %; Rooms: unused"}),
              wall_color, floor_color, (layer, Range{-100, 100}))

// Entities ProcLevel built (and will destroy when it rebuilds or goes away).
struct ProcLevelOwned {
    Entity owner;
};
MCE_COMPONENT(ProcLevelOwned, owner)

enum class ProcMarkerKind : u8 { Start, Exit, Path };
MCE_ENUM(ProcMarkerKind, Start, Exit, Path)

// A point the generator picked; scripts find it with mce.each("ProcMarker", ...).
struct ProcMarker {
    ProcMarkerKind kind = ProcMarkerKind::Start;
    u8 pad_[3] = {};
    u32 index = 0;  // Path: 1 is the cell after Start, the last is the Exit cell
};
MCE_COMPONENT(ProcMarker, kind, (index, Help{"Path markers: 1 .. n along the walk from Start to Exit; 0 for Start and Exit"}))

// Registers the components and "kit_proclevel" (FixedUpdate_Pre). Idempotent.
void install_proclevel(World& world);

// An empty NavGrid with the level's cell size and origin (`origin` = the ProcLevel entity's position xy).
NavGrid proclevel_nav_grid(const ProcLevel& level, math::vec2 origin);

// The most wall rectangles (and so entities) a width x height grid can need: used by tests.
u32 proclevel_max_wall_rects(u32 width, u32 height);

}  // namespace mce::kit
