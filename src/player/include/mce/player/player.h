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

namespace mce {
struct ModuleInfo;
}

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
};

// Parses the command line into convars, then runs headless (-headless 1) or windowed. Returns the
// process exit code.
int run(int argc, char** argv, const PlayerDesc& desc = {});

}  // namespace mce::player
