#pragma once
// The player runtime: everything mce_player does, as a function a game's thin host calls
// (assets.md §4.4 games/<game>/host; the project template's host is these few lines).
//
//   int main(int argc, char** argv) {
//       mce::player::PlayerDesc d;
//       d.app_name = "mygame";
//       d.title = "My Game";
//       d.pack = "assets.mce";            // beside the executable when shipped ...
//       d.pack_fallback = MYGAME_PACK;    // ... else the build tree's cooked pack
//       d.module = "libmygame.so";
//       d.module_fallback = MYGAME_MODULE;
//       return mce::player::run(argc, argv, d);
//   }
//
// The command line still wins (-pack, -module, -scene, -headless, -frames, ... see player.cpp).
//
// A game that takes data mods adds `d.mods_dir = "mods";` (and usually `d.mods_game_id`): see
// docs/user-guide/19-mods.md.

#include "mce/types.h"

namespace mce {
struct ModuleInfo;
}
namespace mce::ui {
struct Layer;
}
namespace mce::monetize {
class IStoreProvider;
class IAdProvider;
}
namespace mce::liveops {
class ICrashUploader;
namespace analytics {
class IBackend;
}
}  // namespace mce::liveops

namespace mce::player {

struct PlayerDesc {
    const char* app_name = "mce_player";  // log prefix and app name
    const char* title = "MCE - Player";   // window title
    // Shipped layout: file names looked up next to the executable; the fallbacks are full paths
    // (the build tree) used when the file is not there. Null pack: -pack is required. Null
    // module: the engine systems only (an editor-authored scene with no game code).
    const char* pack = nullptr;
    const char* pack_fallback = nullptr;
    const char* module = nullptr;
    const char* module_fallback = nullptr;
    // Platforms without game modules link the game into the host: its mce_game_module() entry,
    // whose install() runs where a loaded module's would. Ignored when -module names a file.
    const ModuleInfo* (*static_module)() = nullptr;
    // The scene played when -scene is not given: a GUID (a project's .mceproj start_scene). Null
    // or empty: the first scene in the pack by path.
    const char* start_scene = nullptr;
    // Data mods (FR-RUN-090, ADR-0034), off unless the game turns them on: the folder of mod packs
    // (<id>.mce, plus an optional load_order.txt) mounted over the pack, relative to the executable
    // unless absolute. Null: this game takes no mods, and no command line changes that. Players
    // turn a modding game's mods off with -mods 0 and point elsewhere with -mods_dir.
    const char* mods_dir = nullptr;
    // The game id a mod's mod.json names in "game" to load here; mods for other games are skipped.
    // Null or empty: no check.
    const char* mods_game_id = nullptr;
    // Game UI layers (mce/ui/ui.h ui::Layer), drawn in order over the world and under the pause
    // overlay and the console: views of the world the host adds, such as the NPC dialogue box
    // (mce/kit/novel_ui.h). They must outlive run().
    const ui::Layer* const* ui_layers = nullptr;
    u32 ui_layer_count = 0;
    // In-app purchases and ads (M-MONETIZE, mce/monetize/monetize.h), off unless the game turns
    // them on: the player then reads the pack's store catalog (a cooked .mcstore; -store_catalog
    // picks one), installs the Store and Ads resources before the game module, keeps the
    // entitlements in the save folder and pauses the sim and audio while a fullscreen ad shows.
    // Null providers: no store and no ads on this platform (purchases fail, ads never fill).
    bool monetize = false;
    monetize::IStoreProvider* store_provider = nullptr;
    monetize::IAdProvider* ad_provider = nullptr;
    // Live services (M-LIVEOPS, mce/liveops/liveops.h), off unless the game turns them on: the
    // player then installs the analytics backend and sends the crash reports earlier runs left, as
    // far as the player's consent allows (the ad network's consent form, the game's own
    // liveops::set_consent or -liveops.consent). Null: events are dropped, reports stay on disk.
    bool liveops = false;
    liveops::analytics::IBackend* analytics_backend = nullptr;
    liveops::ICrashUploader* crash_uploader = nullptr;
};

// Parses the command line into convars, then runs headless (-headless 1) or windowed. Returns the
// process exit code.
int run(int argc, char** argv, const PlayerDesc& desc = {});

}  // namespace mce::player
