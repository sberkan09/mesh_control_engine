#pragma once
// mce_platform (L1): process-level platform services (core.md §2.5).
//
// Init order (core.md §4.4): log -> platform::init -> window -> rhi device. Shutdown in reverse.
// Nothing in here exposes SDL types; SDL_Event never leaves this module (FR-INP-010).

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/platform/event.h"
#include "mce/result.h"

namespace mce::platform {

struct PlatformDesc {
    // SDL video driver override ("dummy", "offscreen", "x11", "wayland", "windows"); null = default.
    // The `platform.video_driver` convar overrides this when set.
    const char* video_driver = nullptr;
};

// Initializes SDL video + events. Safe to call once per process; pairs with shutdown().
ResultV init(const PlatformDesc& desc = {});
void shutdown();
bool is_initialized();

// Name of the active video driver ("x11", "windows", "dummy", ...); "" before init.
const char* video_driver();

// Drains the OS event queue, dispatching each event to its window's handler.
// Returns true when the user asked to quit (last window closed, SIGINT, OS logout).
bool pump_events();

// Directory holding the running executable, with a trailing separator ("" when unknown). Shipped
// builds find their pack next to the exe this way (FR-SHP-010); works before init().
const char* base_path();

// Names (not paths) of the regular files directly inside `dir`, sorted bytewise. A missing
// directory is an empty list, not an error; names longer than 255 bytes are left out. Works
// before init(). Used for folders a player fills by hand (the mods folder, ADR-0034).
void list_files(StringView dir, Array<FixedString<256>>& out);

// The device's languages, best first, as "language" or "language-COUNTRY" tags ("tr-TR", "en"),
// for mce/loc's language choice. Empty when the OS does not say. Works before init().
void preferred_locales(Array<FixedString<16>>& out);

// The engine wall clock (std::chrono::steady_clock; same source as app::steady_clock()).
f64 now_seconds();
u64 now_us();
void sleep_ms(u32 ms);

// ---- gamepads (v0.5) ------------------------------------------------------------------------
// Pads are opened on hot-plug and given the lowest free player slot; GamepadAdded/Removed and the
// button/axis events are delivered to the focused window (else the first one). Main thread only.
bool gamepad_connected(u32 slot);
const char* gamepad_name(u32 slot);  // "" when the slot is empty
// FR-INP-050 (Could): rumble for `ms` milliseconds, intensities 0..1. False if unsupported.
bool gamepad_rumble(u32 slot, f32 low, f32 high, u32 ms);

// A software pad with the full standard layout (SDL virtual joystick): tests, bots, demos.
// Returns an id for the setters below, or 0 on failure. It hot-plugs like real hardware.
u32 attach_virtual_gamepad(const char* name = "MCE Virtual Gamepad");
void detach_virtual_gamepad(u32 id);
void set_virtual_gamepad_button(u32 id, GamepadButton b, bool down);
void set_virtual_gamepad_axis(u32 id, GamepadAxis a, f32 value);  // same ranges as GamepadAxis events
// What rumble did to a virtual pad: its motors now (0..1; SDL zeroes them when the duration ends)
// and how many rumbles started on it.
struct VirtualRumble {
    f32 low = 0.f;
    f32 high = 0.f;
    u32 count = 0;
};
VirtualRumble virtual_gamepad_rumble(u32 id);

// A synthetic finger (M-MOBILE): queues an OS-level touch event for `window_id` that the next
// pump_events() turns into Touch{Down,Move,Up} with slot assignment, as for a real screen. Any
// u64 names the finger; x, y are normalized (0..1, y down). Tests, bots, desktop touch demos.
enum class TouchPhase : u8 { Down, Move, Up, Cancel };
void inject_touch(u32 window_id, u64 finger, TouchPhase phase, f32 x, f32 y);
// Delivers an app lifecycle event (AppBackground, AppForeground, AppLowMemory, AppTerminating) to
// every window exactly as the OS callback would: synchronously, now. Tests and desktop rehearsal
// of the mobile lifecycle; other event types are ignored.
void simulate_app_event(EventType type);

// Boot-failure report (core.md §2.1, FR-SHP-010): a modal message box, when allowed. Always also
// logs. Suppressed when the `platform.error_box` convar is 0 (CI, tests) or no display exists.
void show_error_box(const char* title, const char* message);

}  // namespace mce::platform
