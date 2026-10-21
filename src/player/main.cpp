// mce_player: the player runtime with no game module of its own (editor-authored scenes; -module
// adds one). See src/player/src/player.cpp for the command line.
#include "mce/player/player.h"

int main(int argc, char** argv) {
    mce::player::PlayerDesc desc;
    desc.mods_dir = "mods";  // the generic player takes any game's data mods: try one with -mods_dir <dir>
    return mce::player::run(argc, argv, desc);
}
