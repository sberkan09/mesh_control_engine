#!/usr/bin/env python3
"""cmake/mobile/android_app.py (M-MOBILE step 11c): every stamped Gradle project carries Play In-App Review,
MceReview.java and MceActivity.mceReviewRequest, the method platform::request_review() calls over JNI
(src/engine/platform/src/review.cpp). A fake SDL android-project stands in for the real one; no Gradle runs."""

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
        # The template folder itself is not copied as a stray top-level directory.
        assert not os.path.exists(os.path.join(project, "review")), os.listdir(project)

        # The JNI side looks the bridge up by this name and signature.
        native = read(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "engine", "platform",
                                   "src", "review.cpp"))
        assert 'method("mceReviewRequest", "()V")' in native

        # A game with products still gets the review library next to Play Billing.
        write(os.path.join(game, "shop.mcstore"), '{"products": [{"id": "coins"}]}')
        android_app.stamp(project, art, args, tmp, build_dir, os.path.join(tmp, "libreview_probe.so"),
                          os.path.join(tmp, "debug.keystore"))
        gradle = read(os.path.join(project, "app", "build.gradle"))
        assert android_app.PLAY_REVIEW in gradle and android_app.PLAY_BILLING in gradle, gradle
    print("android review stamping OK")


if __name__ == "__main__":
    main()
