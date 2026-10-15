#pragma once
// Human-readable names for bindings (M-ACCESS step 3): what an options screen shows on a key or
// button ("W", "Space", "Mouse Left", "Pad South", "Left Stick Up"). ASCII only, so the built-in
// UI font draws every one. Names follow the platform enumerators (positional gamepad names: South
// is A on Xbox, Cross on PlayStation).

#include "mce/fixed_string.h"
#include "mce/input/input_map.h"

namespace mce::input {

using BindingName = FixedString<48>;

const char* key_name(platform::Key k);                       // "W", "Space", "Left Shift"; "Key" for an unnamed one
const char* mouse_button_name(platform::MouseButton b);      // "Mouse Left"
const char* gamepad_button_name(platform::GamepadButton b);  // "Pad South", "Left Shoulder"
const char* gamepad_axis_name(platform::GamepadAxis a);      // "Left Stick X", "Right Trigger"

// The binding as a player reads it: the device input and, for a stick or mouse axis, the direction
// the action reads from it ("Left Stick Left" for LeftX scaled -1). Touch bindings read "Touch".
void binding_name(const BindingDef& b, BindingName& out);

}  // namespace mce::input
