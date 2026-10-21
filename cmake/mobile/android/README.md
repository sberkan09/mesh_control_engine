# Android app template (M-MOBILE step 6)

`scripts/ship.py --target android` stamps this template into `build/<preset>/android/<Name>/`, a
Gradle project around the game's shared library:

- `settings.gradle`, `build.gradle`, `gradle.properties`: the project (Android Gradle plugin from
  Google's Maven repository; the Gradle wrapper is copied from SDL's `android-project`).
- `app/build.gradle.in`: application id, version, SDK levels (`ANDROID_API` in `android_app.py`); no native build of its own (the
  library comes prebuilt from the `android-arm64-*` CMake preset, under `app/src/main/jniLibs`).
- `app/src/main/AndroidManifest.xml.in`: one fullscreen landscape activity, Vulkan 1.1 required.
- `MceActivity.java.in`: SDL's `SDLActivity` loading `lib<target>.so` (SDL3 is linked into it).
- `billing/MceBilling.java`: Play Billing for `mce/platform/store.h` (M-MONETIZE step 3). Only a game whose
  `.mcstore` sells products gets it, with the billing library in `app/build.gradle` and `mceBilling*` methods on its activity,
  which `src/engine/platform/src/store.cpp` calls over JNI.
- `ads/MceAds.java`: Google Mobile Ads and UMP consent for `mce/platform/ads.h` (step 4), for a game whose
  `.mcstore` has ad placements, with its AdMob app id in the manifest (`cmake/mobile/monetize_info.py`).
- `review/MceReview.java`: Play In-App Review for `mce/platform/review.h` (M-MOBILE step 11c). Every game gets it,
  with the review library in `app/build.gradle` and `mceReviewRequest` on its activity (`src/engine/platform/src/review.cpp`).
- `haptics/MceHaptics.java`: the vibrator's predefined effects for `mce/platform/haptics.h` (step 11d). Every game gets it,
  with `mceHaptic` on its activity (`src/engine/platform/src/haptics.cpp`); the manifest already asks for `VIBRATE`.

SDL's Java sources (`org.libsdl.app`) are copied from the SDL checkout of the same build, so the
Java side always matches the native SDL; SDL's default launcher icon is replaced by the game's
`ship/icon.png` (`app_icon.py`) when it has one. `@NAME@`-style fields are filled by ship.py.
