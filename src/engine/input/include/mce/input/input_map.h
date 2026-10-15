#pragma once
// Action map data (audio-input-ui §3.1 L2). An input map is a flat POD table — maps → actions →
// bindings — authored as data, cooked to an `.mce` input-map chunk, and loaded with
// InputSystem::load_map. encode_input_map() is the chunk writer the cook step uses;
// InputMapBuilder is the code-side authoring helper (tests, samples, the default map).
//
// Chunk layout v1 (little-endian, every array tightly packed POD):
//   InputMapHeader | ActionMapDef[map_count] | ActionDef[action_count] | BindingDef[binding_count]

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/input/actions.h"
#include "mce/platform/event.h"
#include "mce/result.h"

namespace mce::input {

enum class Device : u8 {
    Keyboard,     // code = platform::Key (USB HID usage)
    MouseButton,  // code = platform::MouseButton
    MouseWheel,   // axis selects x/y; value = wheel delta accumulated over the tick window
    MouseDelta,   // axis selects x/y; value = pointer motion over the tick window (pixels)
    // v0.5 (ROADMAP M6). A gamepad binding reads every connected pad (single-player "any pad"):
    // a button is held if any pad holds it; an axis takes the pad with the largest deflection.
    GamepadButton,  // code = platform::GamepadButton
    GamepadAxis,    // code = platform::GamepadAxis; deadzone + curve apply (sticks -1..1, y down)
    // M-MOBILE (FR-INP-070). code = a screen zone (pack_touch_zone, normalized, y down); a touch
    // belongs to every zone it STARTED in, wherever it moves afterwards.
    TouchZone,   // on-screen button: 1 while a touch that started in the zone is down
    TouchStick,  // virtual stick: the oldest touch that started in the zone, its offset from where it
                 // started over kTouchStickRadius (unit circle), x or y by BindingFlag::SourceY,
                 // y down; deadzone + curve apply
    TouchTap,    // 1 for the tick a short, still touch that started in the zone lifts
    TouchSwipe,  // +/-1 for the tick a fast touch that started in the zone lifts, on the axis it
                 // mostly moved along (BindingFlag::SourceY picks which axis this binding reads)
};

enum class Curve : u8 {
    Linear,
    Smooth,     // smoothstep on |v|, sign preserved
    Quadratic,  // sign(v) * v^2
    Cubic,      // v^3
};

namespace BindingFlag {
enum : u8 {
    Invert = 1u << 0,
    SourceY = 1u << 1,  // TouchStick / TouchSwipe: read the y component (default x)
};
}

struct Deadzone {
    f32 inner = 0.f;  // |v| below inner reads 0
    f32 outer = 1.f;  // |v| at/above outer reads 1; rescaled linearly in between
};

// A touch zone packed into BindingDef::code: x0, y0, x1, y1 as 0..255 over the window (0..1).
struct TouchZoneRect {
    f32 x0 = 0.f, y0 = 0.f, x1 = 1.f, y1 = 1.f;
    bool contains(f32 x, f32 y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};
u32 pack_touch_zone(const TouchZoneRect& r);
TouchZoneRect unpack_touch_zone(u32 code);

struct BindingDef {
    Device device = Device::Keyboard;
    u8 axis = 0;   // which action component this binding drives: 0 = x, 1 = y
    Curve curve = Curve::Linear;
    u8 flags = 0;  // BindingFlag
    u32 code = 0;
    f32 scale = 1.f;  // e.g. -1 for "A" on the Move x axis
    Deadzone dz;
};

struct ActionDef {
    StringId name;
    ActionType type = ActionType::Button;
    u8 reserved[3] = {};
    u16 first_binding = 0;
    u16 binding_count = 0;
};

namespace MapFlag {
enum : u32 {
    BlocksBelow = 1u << 0,  // while on the stack, maps beneath it are not evaluated (menus)
};
}

struct ActionMapDef {
    StringId name;
    u16 first_action = 0;
    u16 action_count = 0;
    u32 flags = 0;  // MapFlag
};

inline constexpr u32 kInputMapMagic = 0x4D49434Du;  // "MCIM"
// v2 (M-ACCESS): an optional label table after the bindings, one u8 length + UTF-8 bytes per action
// (the authored name, for options screens). v1 chunks decode with empty labels.
inline constexpr u16 kInputMapVersion = 2;
inline constexpr u32 kMaxActionLabel = 47;

struct InputMapHeader {
    u32 magic = kInputMapMagic;
    u16 version = kInputMapVersion;
    u16 reserved = 0;
    u32 map_count = 0;
    u32 action_count = 0;
    u32 binding_count = 0;
};

// Parsed, validated map set (owned copy; the chunk can be unmapped after load).
using ActionLabel = FixedString<kMaxActionLabel + 1>;

struct InputMapSet {
    explicit InputMapSet(mem::IAllocator& a = mem::heap()) : maps(a), actions(a), bindings(a), labels(a) {}
    Array<ActionMapDef> maps;
    Array<ActionDef> actions;
    Array<BindingDef> bindings;
    Array<ActionLabel> labels;  // per action, its authored name; empty (or all blank) when unknown

    const ActionMapDef* find_map(StringId name) const;
    // The action's authored name, or "" (a v1 chunk, a map built from ids only).
    StringView label(usize action) const { return action < labels.size() ? labels[action].view() : StringView(); }
};

// Validates and copies a chunk. Errors: BadMagic, VersionTooNew, Truncated, InvalidArgument
// (index out of range, too many actions, duplicate map name).
Result<void> decode_input_map(Span<const byte> chunk, InputMapSet& out);
void encode_input_map(const InputMapSet& set, Array<byte>& out);

// Code-side authoring:
//   InputMapBuilder b;
//   b.map("Gameplay");
//   b.action("Move", ActionType::Axis2D).key(Key::D, 0, +1).key(Key::A, 0, -1).key(Key::W, 1, +1)...
//   b.action("Jump").key(Key::Space);
//   b.encode(bytes);
class InputMapBuilder {
public:
    explicit InputMapBuilder(mem::IAllocator& a = mem::heap()) : set_(a) {}

    InputMapBuilder& map(StringId name, u32 flags = 0);
    InputMapBuilder& action(StringId name, ActionType type = ActionType::Button);
    // By name: also kept as the action's label (InputMapSet::label).
    InputMapBuilder& action(const char* name, ActionType type = ActionType::Button) { return action(StringView(name), type); }
    InputMapBuilder& action(StringView name, ActionType type = ActionType::Button);
    InputMapBuilder& bind(const BindingDef& b);
    InputMapBuilder& key(platform::Key k, u8 axis = 0, f32 scale = 1.f);
    InputMapBuilder& mouse_button(platform::MouseButton b);
    InputMapBuilder& mouse_delta(u8 axis, f32 scale = 1.f);
    InputMapBuilder& mouse_wheel(u8 axis, f32 scale = 1.f);
    InputMapBuilder& gamepad_button(platform::GamepadButton b, u8 axis = 0, f32 scale = 1.f);
    InputMapBuilder& gamepad_axis(platform::GamepadAxis a, u8 axis = 0, f32 scale = 1.f, Deadzone dz = {0.2f, 1.f},
                                  Curve curve = Curve::Linear);
    // Touch (M-MOBILE). touch_stick adds two bindings on an axis2d action (x, and y flipped to
    // +y up like the gamepad sticks); on an axis1d action it adds the x binding only.
    InputMapBuilder& touch_zone(const TouchZoneRect& zone, u8 axis = 0, f32 scale = 1.f);
    InputMapBuilder& touch_tap(const TouchZoneRect& zone, u8 axis = 0, f32 scale = 1.f);
    InputMapBuilder& touch_stick(const TouchZoneRect& zone, Deadzone dz = {0.15f, 1.f}, Curve curve = Curve::Linear);
    InputMapBuilder& touch_swipe(const TouchZoneRect& zone, bool source_y, u8 axis = 0, f32 scale = 1.f);

    const InputMapSet& set() const { return set_; }
    void encode(Array<byte>& out) const { encode_input_map(set_, out); }

private:
    InputMapSet set_;
};

}  // namespace mce::input

// The chunk is POD-copied: layouts are frozen (no padding, so cooked bytes are deterministic).
static_assert(sizeof(mce::input::BindingDef) == 20);
static_assert(sizeof(mce::input::ActionDef) == 16);
static_assert(sizeof(mce::input::ActionMapDef) == 16);
static_assert(sizeof(mce::input::InputMapHeader) == 20);
