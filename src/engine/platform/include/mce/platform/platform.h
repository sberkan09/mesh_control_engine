#pragma once
// mce_platform (L1): process-level platform services (core.md §2.5).
//
// Init order (core.md §4.4): log -> platform::init -> window -> rhi device. Shutdown in reverse.
// Nothing in here exposes SDL types; SDL_Event never leaves this module (FR-INP-010).

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

// The engine wall clock (std::chrono::steady_clock; same source as app::steady_clock()).
f64 now_seconds();
u64 now_us();
void sleep_ms(u32 ms);

// Boot-failure report (core.md §2.1, FR-SHP-010): a modal message box, when allowed. Always also
// logs. Suppressed when the `platform.error_box` convar is 0 (CI, tests) or no display exists.
void show_error_box(const char* title, const char* message);

}  // namespace mce::platform
