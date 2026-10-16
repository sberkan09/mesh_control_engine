#pragma once
// mce_platform (L1): Steam Input (M-STEAM step 4). With it on, Steam reads every controller (Xbox,
// PlayStation, Switch, the Steam Deck's own) as the game's actions, configured per player in
// Steam, and the engine feeds those values into input::InputSystem by action name
// (set_action_feeds) next to the map's own bindings. The one action set and its actions are the
// input map's, as the generated action manifest (mce/input/steam_config.h) lists them.
//
// Requires steam::running(). Main thread. Behind the Steam wall like the rest of Steam.

#include "mce/fixed_string.h"
#include "mce/types.h"

namespace mce::platform::steam::input {

enum class Kind : u8 { Digital, Analog };

struct Action {
    FixedString<64> name;
    Kind kind = Kind::Digital;
};

// One action's value across the connected controllers: a digital one 0 or 1 in x (held on any);
// an analog one the largest deflection, x and y as Steam reports them (y up).
struct Value {
    f32 x = 0.f;
    f32 y = 0.f;
    bool active = false;  // some controller has the action bound and its set active
};

// Starts Steam Input. False while Steam is off or when its library lacks Steam Input.
bool start();
bool running();
void stop();

// Once a frame: pumps Steam Input, activates `set` (an action set by name) on every controller and
// reads `actions` into `out` (same order; out.size() >= actions.size()). Returns the controllers
// connected.
u32 frame(const char* set, Span<const Action> actions, Span<Value> out);
u32 controllers();  // as of the last frame

// The first controller's input for an action of `set`: Steam's name for it ("A Button") and the
// path of its glyph image. False with no controller, or when the action is not bound.
bool origin(const char* set, const Action& action, FixedString<64>& name, FixedString<256>& glyph);

}  // namespace mce::platform::steam::input
