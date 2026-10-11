// Force-included into every engine executable built for Android (cmake/MceMobile.cmake), where
// an "executable" is the shared library SDL's activity loads: main() becomes the exported
// SDL_main() the activity calls on its own thread. Mirrors what SDL_main.h does, without
// including SDL outside mce_platform.
#pragma once
#ifdef __cplusplus
extern "C"
#endif
    __attribute__((visibility("default"))) int SDL_main(int argc, char** argv);
#define main SDL_main
