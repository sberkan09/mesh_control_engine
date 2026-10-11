# Android app template (M-MOBILE step 6)

`scripts/ship.py --target android` stamps this template into `build/<preset>/android/<Name>/`, a
Gradle project around the game's shared library:

- `settings.gradle`, `build.gradle`, `gradle.properties`: the project (Android Gradle plugin from
  Google's Maven repository; the Gradle wrapper is copied from SDL's `android-project`).
- `app/build.gradle.in`: application id, version, SDK levels; no native build of its own (the
  library comes prebuilt from the `android-arm64-*` CMake preset, under `app/src/main/jniLibs`).
- `app/src/main/AndroidManifest.xml.in`: one fullscreen landscape activity, Vulkan 1.1 required.
- `MceActivity.java.in`: SDL's `SDLActivity` loading `lib<target>.so` (SDL3 is linked into it).

SDL's Java sources (`org.libsdl.app`) are copied from the SDL checkout of the same build, so the
Java side always matches the native SDL. `@NAME@`-style fields are filled by ship.py.
