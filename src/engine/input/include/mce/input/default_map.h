#pragma once
// The starter-kit default input map (keyboard + mouse; gamepad joins at v0.5):
//   "Gameplay": Move (WASD / arrows, Axis2D), Look (mouse delta, Axis2D), Jump (Space),
//               Fire (J / left mouse), Pause (Escape)
//   "UI" (BlocksBelow): Navigate (arrows / WASD, Axis2D), Confirm (Return / Space), Cancel (Escape)

#include "mce/input/input_map.h"

namespace mce::input {

void build_default_map(InputMapBuilder& b);
void default_map_chunk(Array<byte>& out);

}  // namespace mce::input
