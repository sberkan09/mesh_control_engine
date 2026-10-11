# platform — SDL3 window, event pump, clock, gamepads, audio output and OS services

`mce_platform` is an L1 library in `src/engine/platform/`. Its only public dependency is
`mce_core`; `SDL3::SDL3-static` is private (`src/engine/platform/CMakeLists.txt`). It is the SDL
wall: SDL headers and `SDL_Event` live here, and the rest of the engine sees SDL-free types. The
one sanctioned leak is `Window::native_handle()`, an `SDL_Window*` typed as `void*`.

It is linked by `mce_input`, `mce_audio`, the RHI backends and `mce_rhi_default`,
`mce_editor_layer`, `mce_module_host` and the `mce_engine` umbrella, `mce_player_runtime` (directly
where game modules are unsupported), and the samples.

Init order (from `platform.h`): log, then `platform::init`, then the window, then the RHI device.
Shutdown runs in reverse.

## Public surface

Headers live under `include/mce/platform/`.

`platform.h`: process-level services.

- `PlatformDesc`, `init(desc)`, `shutdown()`, `is_initialized()`, `video_driver()`. `init` brings
  up SDL video and events; the `platform.video_driver` convar (or `PlatformDesc::video_driver`)
  picks the SDL video driver, for example `dummy` for tests.
- `pump_events()`: drains the OS queue and delivers each event to its window's handler. Returns
  true on quit (SDL quit, or a close request on any window).
- `list_files(dir, names)`: names of the regular files directly in `dir`, sorted bytewise (SDL's directory
  enumeration); a missing directory gives an empty list. The player lists the mods folder with it (ADR-0034).
- `base_path()`: directory of the running executable, with a trailing separator. Works before
  `init`; shipped builds find their pack this way.
- Clock: `now_seconds()`, `now_us()` (both `std::chrono::steady_clock`, the same source as
  `app::steady_clock()`), `sleep_ms()`.
- Gamepads (v0.5): `gamepad_connected(slot)`, `gamepad_name(slot)`, `gamepad_rumble(slot, low,
  high, ms)`, and virtual pads for tests and bots: `attach_virtual_gamepad`,
  `detach_virtual_gamepad`, `set_virtual_gamepad_button`, `set_virtual_gamepad_axis`.
- Touch (M-MOBILE): `inject_touch(window_id, finger, phase, x, y)` queues a synthetic finger that
  the next pump turns into touch events, for tests and bots.
- `show_error_box(title, message)`: boot-failure report. Always logs at critical level; shows a
  modal box unless the `platform.error_box` convar is 0 or the video driver is `dummy` or
  `offscreen`.

`window.h`: `Window` and `WindowDesc`.

- `Window::create(desc)` requires `platform::init`. `WindowDesc::opengl` asks for a GPU-capable
  surface: on Linux and Windows an OpenGL window with an RGBA8 + D24S8 double-buffered default
  framebuffer (the context itself is made by `mce_rhi_gl`); on macOS an `SDL_WINDOW_METAL` window.
- `native_handle()`, `id()`, `framebuffer_size()` (pixels, DPI-correct), `display_scale()`.
- `set_title`, `set_size`, `set_cursor_visible`, `set_icon(width, height, rgba)`.
- `set_text_input(on)`: `TextInput` events flow only while this is on (an IME may show).
- `request_close()` queues a `Close` event for the next pump; `close_requested()`.
- `set_event_handler(fn, user)`: one handler per window.

`event.h`: the SDL-free event type.

- `Event { type, window_id, code, flags, x, y, timestamp_us }`. `timestamp_us` is rebased from
  SDL ticks onto `now_us()`, so `mce_input` can quantize events against the sim tick grid.
- `EventType`: window events (`Close`, `FocusIn/Out`, `Minimized`, `Restored`, `Resized`,
  `ScaleChanged`), keyboard and mouse, `TextInput` (one event per Unicode code point), `Quit`, and
  gamepad events (`GamepadAdded/Removed`, `GamepadButtonDown/Up`, `GamepadAxis`; slot in `flags`),
  and touch events (`TouchDown/Move/Up`; finger slot in `code`, `x`, `y` normalized 0..1, y down).
- `Key` (USB HID usages, layout-independent; SDL scancodes pass through), `MouseButton`,
  `GamepadButton` and `GamepadAxis` (SDL standard layout, values match SDL), `kMaxGamepads` (4),
  `kMaxTouches` (10).
- `EventFlag`: modifier bits, `Repeat`, `ConsumedByEditor` (set by the editor's UI layer on
  events ImGui captured) and `TouchCanceled` (on a `TouchUp` the OS took away).
- `event_type_name()`.

`audio_output.h`: `AudioOutput::open(desc, fn, user)`, the one SDL audio call site. The device
pulls interleaved float32 frames from an `AudioRenderFn` on SDL's audio thread. Works without
`platform::init`. When the destructor returns, the callback is not running and never will again.
`mce_audio` is its consumer.

`library.h`: `load_library`, `library_symbol`, `unload_library` (used by the game-module loader),
plus `file_exists`, `copy_file` and `create_directories(path)`. None needs `platform::init`.

`process.h`: `Process` for child processes (`start`, `running`, `wait` with optional captured
stdout, `kill`, `exit_code`). The editor's run-in-player button uses it.

## Rules and invariants

- **Threading.** Window, event pump and gamepad calls are main thread only. The `AudioRenderFn`
  runs on SDL's real-time audio thread and must not allocate, lock or block (FR-AUD-010).
- **Event routing.** Window, keyboard, mouse and text events go to the window they name. Gamepad
  events go to the focused window, else the first one. `Quit` goes to every window.
- **Touch.** A finger gets the lowest free slot on down and frees it on up or cancel; an eleventh
  finger is dropped until one lifts. SDL's touch-to-mouse and mouse-to-touch emulation is off, so
  a finger never moves the mouse (mce_ui takes its pointer from either, `ui::apply_touch_pointer`).
  Touch events go to the window they name, else the focused one.
- **Gamepads.** Pads open on hot-plug into the lowest free player slot (up to 4). `SDL_Gamepad`
  never leaves `src/gamepad.cpp`. A failed gamepad subsystem init only warns (ADR-0026 §2.1).
- **Clock.** `now_seconds()` is the engine's wall clock. Simulation code uses the app clock and
  tick context instead.
- **Walls** (`scripts/ci/check_walls.py`):
  - SDL wall: `SDL_Event` only under `src/engine/platform/`.
  - SDL include wall: `<SDL3/...>` only under `src/engine/platform/`, `src/engine/rhi_gl/`,
    `src/engine/rhi_metal/` and `src/editor/layer/` (the `native_handle()` consumers).
  - Metal wall: `MTL*` and `<Metal/...>` stay out of platform; the little macOS-specific code sits
    under `__APPLE__` in the shared platform files.
  - The engine container ban applies.

## How it is tested

- `test_platform` (`tests/platform/test_platform.cpp`). It runs on SDL's `dummy` video driver, so
  it needs no display and is a plain unit test on every CI leg.
- Cases: "platform: init, driver, shutdown", "platform: window create requires init",
  "platform: window lifecycle and close request", "platform: clock is monotonic",
  "platform: event names", "platform: a virtual gamepad hot-plugs into slot 0 and streams
  standard-layout events", "platform: fingers get small reusable slots and normalized positions".
- `AudioOutput` is exercised by `test_audio` ("Device backend pulls from the mixer on the platform
  audio thread", SDL `dummy` audio driver). Event quantization is tested in `test_input`.
- GL window creation is covered by the GPU tests (`test_rhi_gl`, `test_render_gl`).

## Design records

- [core.md](../architecture/core.md): §1.3 module dependency rule and walls, §2.1 boot-failure
  path, §2.5 platform seam, §4.4 application lifecycle order.
- [audio-input-ui.md](../architecture/audio-input-ui.md) §4.3 (audio device seam) and §4.6 (the
  editor input seam behind `ConsumedByEditor`).
- [macos-metal.md](../architecture/macos-metal.md) §2: the Metal-capable window seam.
- ADR-0026 (`docs/adr/0026-m6-subsystems.md`) §2: gamepads through the platform layer.
- ADR-0028 (`docs/adr/0028-metal-backend.md`) §7: `WindowDesc::opengl` on macOS.
- ADR-0021 (`docs/adr/0021-game-module-loader-m3.md`): the game-module loader that uses
  `load_library`.
- Milestones: M6-subsystems.md (`docs/milestones/M6-subsystems.md`),
  M-MAC.md (`docs/milestones/M-MAC.md`), M7-ashen-court.md (`docs/milestones/M7-ashen-court.md`).

## Known gaps

- M7 records that `mce_platform` has no relative-mouse or pointer-lock mode yet; Ashen Court's
  mouse look reads raw pointer deltas.
- core.md §2.5 sketches more than is built. The headers today have no window modes or display
  list (`set_mode`, `display_modes`, `set_display`), no `set_cursor_relative` or clip rect, no
  `read_file` / `map_file_readonly` / virtual mounts, and no `spawn_thread`. Whole-file reads live
  in `mce_asset` (`include/mce/asset/file_io.h`).
- ADR-0026 §1 lists device re-open on a default audio device change as not built.
- ADR-0026 §2: rumble is a platform passthrough with no input-map surface; per-player pad
  ownership is not built.
