#pragma once
// mce_platform (L1): Steam through the Steamworks flat API, loaded at run time (M-STEAM step 1,
// ADR-0042). Neither the repository nor the build carries the Steamworks SDK: the library Valve ships
// with a game (libsteam_api.so, steam_api64.dll, libsteam_api.dylib) is opened when a game asks for
// Steam, and the few flat entry points used here are looked up by name. A game that never calls
// init() never loads it, and a missing library is an ordinary "no Steam" result.
//
// The Steam wall (check_walls.py): Steamworks names live only in mce_platform's steam*.cpp.
//
// Callbacks use Steam's manual dispatch: run_callbacks() once a frame on the main thread hands each
// pending callback (its Steamworks id and payload) to the caller. Nothing here touches the simulation:
// what a game does with Steam goes through the systems that record it for replays (liveops game
// services in step 2).

#include "mce/fixed_string.h"
#include "mce/types.h"

namespace mce::platform::steam {

enum class InitResult : u8 {
    Ok,
    NoLibrary,         // no steam_api library where it was looked for
    MissingEntry,      // the library lacks an entry point this build needs (too old an SDK)
    NoSteamClient,     // Steam is not running, or the user is not logged in
    VersionMismatch,   // the Steam client is older than the library
    Failed,            // any other refusal (the message says why)
    Restarting,        // Steam relaunches the game through itself: quit now
};
const char* init_result_name(InitResult r);  // "ok", "no_library", ...

struct Config {
    // The game's Steam app id. With an id, a run outside Steam sets SteamAppId / SteamGameId for the
    // client (what steam_appid.txt does) unless the environment already names one; 0 leaves it to the
    // client (a game started from Steam).
    u32 app_id = 0;
    // The library to open; empty: steam_api's platform file name next to the executable.
    StringView library_path;
    // Release builds started outside Steam ask Steam to relaunch them through itself.
    bool restart_if_necessary = false;
};

// Opens the library and initialises Steam. Anything but Ok leaves Steam off (running() is false);
// `message` (when given) gets Steam's own reason. Main thread.
InitResult init(const Config& cfg, FixedString<256>* message = nullptr);
bool running();
// Shuts Steam down and closes the library (no-op when off).
void shutdown();

// The platform's file name of steam_api ("libsteam_api.so", "steam_api64.dll", "libsteam_api.dylib").
const char* library_file_name();

// One pending callback: Steamworks' callback id (k_iCallback of its struct) and the struct's bytes,
// valid during the call only.
struct Callback {
    i32 id = 0;
    Span<const byte> data;
};
using CallbackFn = void (*)(void* user, const Callback& cb);
// Pumps Steam and hands every pending callback to `fn` (may be null: drained and dropped). Returns
// how many there were. Main thread, once a frame.
u32 run_callbacks(CallbackFn fn, void* user);

// About the signed-in player and the device (0 / "" / false while off).
u32 app_id();
u64 steam_id();
const char* persona_name();
bool on_steam_deck();
bool big_picture();

// For the later Steam steps inside mce_platform: an entry point of the open library (null while off
// or when absent).
void* entry(const char* name);
// An interface accessor exported under one of several versioned names
// (the user-stats accessor of each SDK version, ...): the first that resolves, called; null when none does.
void* interface(Span<const char* const> versioned_names);

}  // namespace mce::platform::steam
