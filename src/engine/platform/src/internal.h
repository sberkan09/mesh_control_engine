#pragma once
// mce_platform internals shared by platform.cpp and window.cpp.

#include "mce/platform/window.h"

namespace mce::platform::detail {

void register_window(Window* w);
void unregister_window(Window* w);

// Gamepad slots (gamepad.cpp). Ids are SDL joystick instance ids.
inline constexpr u32 kNoSlot = ~0u;
u32 open_gamepad(u32 instance_id);   // slot, or kNoSlot (already open / full / failed)
u32 close_gamepad(u32 instance_id);  // the freed slot, or kNoSlot
u32 find_gamepad(u32 instance_id);
void close_gamepads();
f32 gamepad_axis_value(u8 axis, i16 raw);

}  // namespace mce::platform::detail
