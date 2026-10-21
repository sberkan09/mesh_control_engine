#pragma once
// Persisted bindings (FR-INP-040, ROADMAP M6): the player's rebinds as overrides on top of the
// cooked input map. Each override is a reflected plain struct, so the save file is written and
// read by walking its TypeRecord (audio-input-ui §5.1: saves route through mce_refl), not by a
// hand-written per-field format. A changed cooked map keeps every override whose map/action/index
// still exists; the rest are dropped on load with one warning.

#include "mce/containers/array.h"
#include "mce/input/input_map.h"
#include "mce/refl/macros.h"

namespace mce::input {

MCE_ENUM(Device, Keyboard, MouseButton, MouseWheel, MouseDelta, GamepadButton, GamepadAxis, TouchZone, TouchStick,
         TouchTap, TouchSwipe)
MCE_ENUM(Curve, Linear, Smooth, Quadratic, Cubic)

struct BindingOverride {
    u64 map = 0;     // StringId values: cooked maps carry ids, not names
    u64 action = 0;
    u32 index = 0;   // binding slot inside the action
    Device device = Device::Keyboard;
    u8 axis = 0;
    Curve curve = Curve::Linear;
    u8 flags = 0;
    u32 code = 0;
    f32 scale = 1.f;
    f32 dz_inner = 0.f;
    f32 dz_outer = 1.f;

    BindingDef binding() const;
    static BindingOverride make(StringId map, StringId action, u32 index, const BindingDef& b);
};
MCE_REFL_STRUCT(BindingOverride, map, action, index, device, axis, curve, flags, code, scale, dz_inner, dz_outer)

// Text form: a version line, then one "[BindingOverride]" block per override with one
// `field = value` line per reflected field (enums by name). Unknown fields are ignored and missing
// ones keep their defaults, so the format survives the struct growing.
void write_bindings(Span<const BindingOverride> overrides, Array<char>& out);
Result<void> read_bindings(StringView text, Array<BindingOverride>& out);

}  // namespace mce::input
