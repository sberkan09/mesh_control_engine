// iOS entry point (cmake/MceMobile.cmake): SDL_RunApp runs UIApplicationMain and calls the game's
// SDL_main() (its main(), renamed by the force-included mce_sdl_main.h) once the app has launched.
// When the game's main returns the process ends with its exit code, as on every other platform;
// UIKit would otherwise keep a frozen window on screen.
#undef main  // this file defines the real one
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <stdlib.h>

static int run_game(int argc, char** argv) { exit(SDL_main(argc, argv)); }

int main(int argc, char** argv) { return SDL_RunApp(argc, argv, run_game, NULL); }
