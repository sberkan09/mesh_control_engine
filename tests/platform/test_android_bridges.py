#!/usr/bin/env python3
"""cmake/mobile/android_app.py (M-MOBILE steps 11c-11d): every stamped Gradle project carries the Java bridges
mce_platform calls over JNI for every game: Play In-App Review (MceReview.java, MceActivity.mceReviewRequest,
src/engine/platform/src/review.cpp) and haptics (MceHaptics.java, MceActivity.mceHaptic, haptics.cpp). A fake SDL
android-project stands in for the real one; no Gradle runs."""

import os
import sys
import tempfile
import types

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "cmake", "mobile"))
import android_app  # noqa: E402


def write(path, text=""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def fake_sdl(build_dir):
    sdl = os.path.join(build_dir, "_deps", "sdl3-src", "android-project")
    write(os.path.join(sdl, "gradlew"), "#!/bin/sh\n")
    write(os.path.join(sdl, "gradlew.bat"))
    write(os.path.join(sdl, "gradle", "wrapper", "gradle-wrapper.properties"))
    write(os.path.join(sdl, "app", "src", "main", "java", "org", "libsdl", "app", "SDLActivity.java"))
    write(os.path.join(sdl, "app", "src", "main", "res", "values", "strings.xml"))
    write(os.path.join(sdl, "app", "src", "main", "res", "mipmap-mdpi", "ic_launcher.png"))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        build_dir = os.path.join(tmp, "build")
        fake_sdl(build_dir)
        game = os.path.join(tmp, "game")
        os.makedirs(game)
        art = {"name": "review_probe", "title": "Review Probe", "source": game, "icon": None, "loc_dir": None}
        args = types.SimpleNamespace(build_number=7, version="1.2.3", out=tmp)
        project = os.path.join(build_dir, "android", "review_probe")
        aid = android_app.stamp(project, art, args, tmp, build_dir, os.path.join(tmp, "libreview_probe.so"),
                                os.path.join(tmp, "debug.keystore"))
        assert aid == "org.meshcontrol.review_probe", aid
        main_dir = os.path.join(project, "app", "src", "main")

        # A game without a store catalog: review only, no billing or ads.
        gradle = read(os.path.join(project, "app", "build.gradle"))
        assert f"implementation '{android_app.PLAY_REVIEW}'" in gradle, gradle
        assert "billingclient" not in gradle and "play-services-ads" not in gradle, gradle
        activity = read(os.path.join(main_dir, "java", "org", "meshcontrol", "review_probe", "MceActivity.java"))
        assert "public void mceReviewRequest()" in activity, activity
        assert "mceBilling" not in activity and "mceAds" not in activity, activity
        review = read(os.path.join(main_dir, "java", "org", "meshcontrol", "review", "MceReview.java"))
        assert "package org.meshcontrol.review;" in review
        assert "requestReviewFlow" in review and "launchReviewFlow" in review
        assert "public void mceHaptic(int preset)" in activity, activity
        assert "public boolean mceScreenReaderOn()" in activity and "public void mceAnnounce(final String text)" in activity
        assert "announceForAccessibility" in activity
        haptics = read(os.path.join(main_dir, "java", "org", "meshcontrol", "haptics", "MceHaptics.java"))
        assert "package org.meshcontrol.haptics;" in haptics
        assert "VibrationEffect.createPredefined" in haptics
        manifest = read(os.path.join(main_dir, "AndroidManifest.xml"))
        assert "android.permission.VIBRATE" in manifest
        # The template folders themselves are not copied as stray top-level directories.
        for folder in ("review", "haptics"):
            assert not os.path.exists(os.path.join(project, folder)), os.listdir(project)

        # The JNI side looks each bridge up by this name and signature.
        src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "engine", "platform", "src")
        assert 'method("mceReviewRequest", "()V")' in read(os.path.join(src, "review.cpp"))
        assert 'method("mceHaptic", "(I)V")' in read(os.path.join(src, "haptics.cpp"))
        reader = read(os.path.join(src, "screen_reader.cpp"))
        assert 'method("mceScreenReaderOn", "()Z")' in reader and 'method("mceAnnounce", "(Ljava/lang/String;)V")' in reader
        # MceHaptics numbers its presets as platform::haptics::Preset does.
        header = read(os.path.join(src, "..", "include", "mce", "platform", "haptics.h"))
        names = ["Selection", "Light", "Medium", "Heavy", "Success", "Warning", "Error"]
        order = [header.index(f"    {n},") for n in names]
        assert order == sorted(order), "haptics.h Preset order changed: update MceHaptics.java and haptics_uikit.swift"
        assert "SELECTION = 0, LIGHT = 1, MEDIUM = 2, HEAVY = 3, SUCCESS = 4, WARNING = 5, ERROR = 6" in haptics
        assert "case selection = 0, light, medium, heavy, success, warning, error" in read(
            os.path.join(src, "haptics_uikit.swift"))

        # A game with products still gets the review library next to Play Billing.
        write(os.path.join(game, "shop.mcstore"), '{"products": [{"id": "coins"}]}')
        android_app.stamp(project, art, args, tmp, build_dir, os.path.join(tmp, "libreview_probe.so"),
                          os.path.join(tmp, "debug.keystore"))
        gradle = read(os.path.join(project, "app", "build.gradle"))
        assert android_app.PLAY_REVIEW in gradle and android_app.PLAY_BILLING in gradle, gradle
        assert "firebase" not in gradle and "mceFirebase" not in read(
            os.path.join(main_dir, "java", "org", "meshcontrol", "review_probe", "MceActivity.java"))

        # MCE_FIREBASE_ANDROID_CONFIG (M-LIVEOPS step 1): Crashlytics NDK and Analytics, collection off by default.
        config = os.path.join(tmp, "google-services.json")
        write(config, '{"project_info": {"project_id": "probe"}}')
        os.environ["MCE_FIREBASE_ANDROID_CONFIG"] = config
        try:
            android_app.stamp(project, art, args, tmp, build_dir, os.path.join(tmp, "libreview_probe.so"),
                              os.path.join(tmp, "debug.keystore"))
        finally:
            del os.environ["MCE_FIREBASE_ANDROID_CONFIG"]
        gradle = read(os.path.join(project, "app", "build.gradle"))
        assert f"implementation platform('{android_app.FIREBASE_BOM}')" in gradle, gradle
        for lib in android_app.FIREBASE_LIBS:
            assert f"implementation '{lib}'" in gradle, gradle
        assert "id 'com.google.gms.google-services'" in gradle and "id 'com.google.firebase.crashlytics'" in gradle
        assert "nativeSymbolUploadEnabled true" in gradle and "crashlytics-symbols" in gradle, gradle
        root_gradle = read(os.path.join(project, "build.gradle"))
        assert android_app.GOOGLE_SERVICES_PLUGIN in root_gradle and android_app.CRASHLYTICS_PLUGIN in root_gradle
        assert read(os.path.join(project, "app", "google-services.json")) == read(config)
        manifest = read(os.path.join(main_dir, "AndroidManifest.xml"))
        assert 'firebase_analytics_collection_enabled" android:value="false"' in manifest, manifest
        assert 'firebase_crashlytics_collection_enabled" android:value="false"' in manifest, manifest
        activity = read(os.path.join(main_dir, "java", "org", "meshcontrol", "review_probe", "MceActivity.java"))
        assert "public void mceFirebaseStart()" in activity and "public void mceFirebaseCollection(int" in activity
        bridge = read(os.path.join(main_dir, "java", "org", "meshcontrol", "firebase", "MceFirebase.java"))
        assert "setCrashlyticsCollectionEnabled" in bridge and "deleteUnsentReports" in bridge
        assert not os.path.exists(os.path.join(project, "firebase")), os.listdir(project)
        firebase_cpp = read(os.path.join(src, "firebase.cpp"))
        assert 'method("mceFirebaseStart", "()V")' in firebase_cpp
        assert 'method("mceFirebaseCollection", "(I)V")' in firebase_cpp
        assert ('"(Ljava/lang/String;[Ljava/lang/String;[I[J[D[Ljava/lang/String;)V"' in firebase_cpp and
                "String name, String[] keys, int[] kinds, long[] ints, double[] floats, String[] strings" in activity)
        # Collection's values, as MceFirebase and firebase_ios.swift number them.
        header = read(os.path.join(src, "..", "include", "mce", "platform", "firebase.h"))
        order = [header.index(f"    {n},") for n in ("Undecided", "Granted", "Denied")]
        assert order == sorted(order), "firebase.h Collection order changed: update MceFirebase.java and firebase_ios.swift"
        assert "case 1:  // Granted" in bridge and "case 2:  // Denied" in bridge
        swift = read(os.path.join(src, "firebase_ios.swift"))
        assert "case 1:  // Granted" in swift and "case 2:  // Denied" in swift

        # Remote Config (M-LIVEOPS step 3): the bridge method, and the natives firebase.cpp exports for its answer.
        assert "com.google.firebase:firebase-config" in android_app.FIREBASE_LIBS
        assert "public void mceFirebaseRemoteConfig()" in activity, activity
        assert 'method("mceFirebaseRemoteConfig", "()V")' in firebase_cpp
        assert "static native void nativeConfigValue(String key, String value);" in bridge
        assert "static native void nativeConfigDone(boolean ok);" in bridge
        assert "fetchAndActivate" in bridge and "VALUE_SOURCE_REMOTE" in bridge
        assert "Java_org_meshcontrol_firebase_MceFirebase_nativeConfigValue" in firebase_cpp
        assert "Java_org_meshcontrol_firebase_MceFirebase_nativeConfigDone" in firebase_cpp
        for name in ("mce_firebase_config_value", "mce_firebase_config_done", "mce_firebase_remote_config_fetch"):
            assert name in swift and name in firebase_cpp, name

        # Game services (M-LIVEOPS step 2): Play Games only for a game with a .mcservices and MCE_PLAY_GAMES_APP_ID.
        android_app.stamp(project, art, args, tmp, build_dir, os.path.join(tmp, "libreview_probe.so"),
                          os.path.join(tmp, "debug.keystore"))
        assert android_app.PLAY_GAMES not in read(os.path.join(project, "app", "build.gradle"))
        write(os.path.join(game, "services.mcservices"), '{"achievements": [{"id": "first_win"}]}')
        os.environ["MCE_PLAY_GAMES_APP_ID"] = "123456789012"
        try:
            android_app.stamp(project, art, args, tmp, build_dir, os.path.join(tmp, "libreview_probe.so"),
                              os.path.join(tmp, "debug.keystore"))
        finally:
            del os.environ["MCE_PLAY_GAMES_APP_ID"]
        gradle = read(os.path.join(project, "app", "build.gradle"))
        assert f"implementation '{android_app.PLAY_GAMES}'" in gradle, gradle
        manifest = read(os.path.join(main_dir, "AndroidManifest.xml"))
        assert 'com.google.android.gms.games.APP_ID" android:value="@string/mce_play_games_app_id"' in manifest
        ids = read(os.path.join(main_dir, "res", "values", "mce_games.xml"))
        assert '<string name="mce_play_games_app_id" translatable="false">123456789012</string>' in ids, ids
        activity = read(os.path.join(main_dir, "java", "org", "meshcontrol", "review_probe", "MceActivity.java"))
        games = read(os.path.join(main_dir, "java", "org", "meshcontrol", "games", "MceGames.java"))
        assert "package org.meshcontrol.games;" in games
        assert "static native void nativeAnswer(int kind, boolean ok, boolean missing, int tag, byte[] bytes);" in games
        assert not os.path.exists(os.path.join(project, "games")), os.listdir(project)
        games_cpp = read(os.path.join(src, "game_services.cpp"))
        assert "Java_org_meshcontrol_games_MceGames_nativeAnswer" in games_cpp
        # Every bridge method firebase-style: the activity declares it with the signature the JNI side looks up.
        for method, sig, decl in [
                ("mceGamesStart", "()V", "public void mceGamesStart()"),
                ("mceGamesSignIn", "(Z)V", "public void mceGamesSignIn(boolean interactive)"),
                ("mceGamesAchievement", "(Ljava/lang/String;III)V",
                 "public void mceGamesAchievement(String id, int steps, int total, int tag)"),
                ("mceGamesScore", "(Ljava/lang/String;JI)V", "public void mceGamesScore(String id, long score, int tag)"),
                ("mceGamesWrite", "(Ljava/lang/String;[BI)V", "public void mceGamesWrite(String name, byte[] bytes, int tag)"),
                ("mceGamesShowAchievements", "()V", "public void mceGamesShowAchievements()"),
                ("mceGamesShowLeaderboard", "(Ljava/lang/String;)V", "public void mceGamesShowLeaderboard(String id)")]:
            assert f'method("{method}", "{sig}")' in games_cpp, method
            assert decl in activity, decl
        assert 'call_tagged("mceGamesRead"' in games_cpp and "public void mceGamesRead(String name, int tag)" in activity
        # MceGames and game_services_gamekit.swift number their answers as AnswerKind does.
        header = read(os.path.join(src, "..", "include", "mce", "platform", "game_services.h"))
        names = ["SignedIn", "Achievement", "Score", "BlobRead", "BlobWritten"]
        order = [header.index(f"    {n},") for n in names]
        assert order == sorted(order), "game_services.h AnswerKind order changed: update MceGames.java and the Swift side"
        assert "SIGNED_IN = 0" in games and "BLOB_WRITTEN = 4" in games
        swift = read(os.path.join(src, "game_services_gamekit.swift"))
        assert "case signedIn = 0, achievement, score, blobRead, blobWritten" in swift
        for name in ("mce_gamekit_start", "mce_gamekit_sign_in", "mce_gamekit_achievement", "mce_gamekit_score",
                     "mce_gamekit_read", "mce_gamekit_write", "mce_gamekit_show_achievements",
                     "mce_gamekit_show_leaderboard", "mce_gamekit_answer"):
            assert name in swift and name in games_cpp, name
    print("android bridges stamping OK")


if __name__ == "__main__":
    main()
