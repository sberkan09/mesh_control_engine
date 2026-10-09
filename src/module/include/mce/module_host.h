#pragma once
// Host side of the game-module boundary (scripting.md §2.2): used identically by the runtime host
// (game.exe), play-in-editor (v0.5) and the devhost/tests.
//
//   World world;
//   physics::install_physics2d(world);          // engine systems first
//   MCE_TRY_ASSIGN(LoadedModule mod, load_game_module("libplatformer.so", world));
//   ... world.tick(dt, n) ...
//   world.~World() / scope exit                 // World first: it holds module code pointers
//   unload_game_module(mod);
//
// Copy-and-load: the loader maps a copy (`<stem>_loaded_<pid>_<N><ext>` next to the original),
// never the build output itself, so the build can overwrite the module while a copy is mapped
// (Windows file locks), a rebuilt module is never confused with the mapped one, and two processes
// (the editor and the running game) never map the same copy.

#include "mce/fixed_string.h"
#include "mce/module.h"
#include "mce/result.h"

namespace mce {

struct LoadedModule {
    const ModuleInfo* info = nullptr;
    void* handle = nullptr;           // platform library handle (opaque)
    u32 slot = ~0u;                   // copy-and-load slot
    FixedString<512> source_path;     // what the caller asked for
    FixedString<512> loaded_path;     // the copy actually mapped

    bool loaded() const { return handle != nullptr; }
};

// Maps and version-checks a module without installing it: for tools (the editor, the MCP server,
// the cook) that keep one module mapped for the process and install it into every World they make
// (install_game_module), or only need its reflected components, which register with mce_refl as
// the library maps. Errors are load_game_module's up to module.version.
Result<LoadedModule> open_game_module(StringView path);

// Installs an opened module into `world` (one module per World): Errc::AlreadyExists when the
// World has one, Errc::ModuleInstallFailed when install() returned an error (logged; the World may
// be half populated: discard it).
ResultV install_game_module(const LoadedModule& module, World& world);

// open_game_module + install_game_module. Every failure
// is an Error with code + stage + path, never a crash (FR-DX-050):
//   Errc::ModuleNotFound       file missing, or the OS refused to map it (stage module.open)
//   Errc::IoError              the copy could not be written (stage module.copy)
//   Errc::ModuleMissingEntry   no `mce_game_module` export, or it returned null / no install
//   Errc::EngineIdMismatch     module built against another engine build (both ids are logged)
//   Errc::AlreadyExists        `world` already has a module installed
//   Errc::ModuleInstallFailed  install() returned an error (logged). The World may be half
//                              populated: discard it. The library then stays mapped for the
//                              process lifetime, because the World may hold its code pointers.
Result<LoadedModule> load_game_module(StringView path, World& world);

// Unmaps the module and deletes its copy. Every World it was installed into must already be
// destroyed (asserted). Unloading a not-loaded module is a no-op.
void unload_game_module(LoadedModule& module);

// The MCE_ENGINE_ID this engine was built with (hosts print it in their boot log line).
EngineId engine_id();

// Copies currently mapped by this process (diagnostics/tests).
u32 loaded_module_count();

}  // namespace mce
