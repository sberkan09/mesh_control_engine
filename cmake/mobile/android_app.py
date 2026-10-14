"""Android packaging for scripts/ship.py --target android (M-MOBILE step 6).

    python3 scripts/ship.py platformer --target android            # dist/Ember-Step-<version>-android.apk
    python3 scripts/ship.py diorama --target android --aab         # ... and the Play Store bundle (.aab)
    python3 scripts/ship.py platformer --target android --smoke    # install on the running emulator/device and play

What it does, given the game's shared library from the android-arm64-* preset (cmake/MceMobile.cmake)
and the pack cooked by the host's mce_cook:

1. stamps cmake/mobile/android into build/<preset>/android/<Name>/ (a Gradle project), with SDL's Java
   sources and Gradle wrapper copied from the SDL checkout of the same build;
2. puts lib<target>.so (stripped; the unstripped copy goes to <stem>-symbols.zip) under jniLibs and
   assets.mce + CREDITS.md + mce_bundle.txt under assets (platform::base_path extracts them on the
   first launch, src/engine/platform/src/platform.cpp);
3. runs the wrapper: assembleRelease (and bundleRelease with --aab), signed with the keystore given
   by MCE_ANDROID_KEYSTORE (passwords: MCE_ANDROID_STORE_PASSWORD / _KEY_ALIAS / _KEY_PASSWORD), else
   with the Android debug keystore, which is fine for testing and refused by the Play Store.

Store readiness (step 10): the game's ship/icon.png becomes the adaptive launcher icon and
<stem>-play-icon.png (app_icon.py); versionCode is the build number (ship.py --build-number);
compileSdk/targetSdk are ANDROID_API below.

Purchases and ads (M-MONETIZE steps 3-4, monetize_info.py): a game whose .mcstore sells products
also gets android/billing/MceBilling.java, the Play Billing library (PLAY_BILLING below) and
MceActivity's mceBilling* methods; one with ad placements gets android/ads/MceAds.java, Google Mobile
Ads and UMP (GMA, UMP), the mceAds* methods and its AdMob app id in the manifest. mce_platform calls
both over JNI. Other games get none of it, and their store and ad backends are "none".

Live services vendor (M-LIVEOPS step 1): with MCE_FIREBASE_ANDROID_CONFIG pointing at the project's
google-services.json (from the Firebase console; it is kept out of the repository and handed to the
build through the environment), the game gets android/firebase/MceFirebase.java, the Firebase BoM with
Analytics and Crashlytics NDK (FIREBASE_* below), the google-services and Crashlytics Gradle plugins,
MceActivity's mceFirebase* methods and manifest flags that start both SDKs with collection off until
the player consents. The release build uploads the unstripped library's symbols to Crashlytics
(uploadCrashlyticsSymbolFileRelease). Without the variable nothing of Firebase is stamped in and
platform::firebase::backend() is "none".

Review prompt (M-MOBILE step 11c): every game gets android/review/MceReview.java, Play In-App Review
(PLAY_REVIEW) and MceActivity's mceReviewRequest, which platform::request_review() calls over JNI.

Haptics (M-MOBILE step 11d): every game gets android/haptics/MceHaptics.java (the vibrator's predefined
effects; the manifest's VIBRATE permission) and MceActivity's mceHaptic, which platform::haptics::play() calls.

Tooling: ANDROID_HOME (default ~/Library/Android/sdk), ANDROID_NDK_HOME (default: the newest NDK in
the SDK), JAVA_HOME (default: /usr/libexec/java_home -v 17, then Homebrew's openjdk@17).
"""

import glob
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
import zipfile

import app_icon
import monetize_info
import app_names

# compileSdk and targetSdk. Google Play raises its minimum target API every August (new apps and updates:
# API 36 since 2026-08-31); check it once a year (docs/milestones/M-MOBILE.md) and bump this with the
# Android Gradle plugin in android/build.gradle when the new level needs it.
ANDROID_API = 36

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(HERE, "android")
PLAY_BILLING = "com.android.billingclient:billing:7.1.1"
GMA = "com.google.android.gms:play-services-ads:23.6.0"
UMP = "com.google.android.ump:user-messaging-platform:3.1.0"
PLAY_REVIEW = "com.google.android.play:review:2.0.2"
FIREBASE_BOM = "com.google.firebase:firebase-bom:33.7.0"
FIREBASE_LIBS = ["com.google.firebase:firebase-analytics", "com.google.firebase:firebase-crashlytics-ndk"]
GOOGLE_SERVICES_PLUGIN = "com.google.gms:google-services:4.4.2"
CRASHLYTICS_PLUGIN = "com.google.firebase:firebase-crashlytics-gradle:3.0.2"

# MceActivity's bridge to MceFirebase, for platform/firebase.h's JNI calls (src/engine/platform/src/firebase.cpp).
FIREBASE_METHODS = '''
    // ---- live services vendor (mce_platform's firebase over JNI, cmake/mobile/android/firebase) ----
    private org.meshcontrol.firebase.MceFirebase firebase;

    public void mceFirebaseStart() {
        if (firebase == null) firebase = new org.meshcontrol.firebase.MceFirebase(this);
    }
    public void mceFirebaseCollection(int collection) { if (firebase != null) firebase.setCollection(collection); }
    public void mceFirebaseEvent(String name, String[] keys, int[] kinds, long[] ints, double[] floats, String[] strings) {
        if (firebase != null) firebase.logEvent(name, keys, kinds, ints, floats, strings);
    }
'''

# Both SDKs start with collection off: nothing leaves the device before the player consents.
FIREBASE_META = ('''
        <meta-data android:name="firebase_analytics_collection_enabled" android:value="false" />
        <meta-data android:name="firebase_crashlytics_collection_enabled" android:value="false" />''')


def firebase_config():
    """The project's google-services.json from MCE_FIREBASE_ANDROID_CONFIG, or None."""
    path = os.environ.get("MCE_FIREBASE_ANDROID_CONFIG")
    if not path:
        return None
    if not os.path.isfile(path):
        sys.exit(f"ship: MCE_FIREBASE_ANDROID_CONFIG={path} does not exist")
    return path

# MceActivity's bridge to MceBilling, for platform/store.h's JNI calls (src/engine/platform/src/store.cpp).
BILLING_METHODS = '''
    // ---- in-app purchases (mce_platform's store over JNI, cmake/mobile/android/billing) ----
    private org.meshcontrol.billing.MceBilling billing;

    public void mceBillingStart() {
        if (billing == null) billing = new org.meshcontrol.billing.MceBilling(this);
        billing.start();
    }
    public void mceBillingStop() { if (billing != null) billing.stop(); }
    public void mceBillingQuery(String[] ids, int request) { billing.query(ids, request); }
    public void mceBillingPurchase(String id, boolean consumable, int request) { billing.purchase(id, consumable, request); }
    public void mceBillingRestore(int request) { billing.restore(request); }
    public void mceBillingFinish(String token, boolean consumable) { billing.finish(token, consumable); }
'''

# MceActivity's bridge to MceAds, for platform/ads.h's JNI calls (src/engine/platform/src/ads.cpp).
ADS_METHODS = '''
    // ---- ads and consent (mce_platform's ads over JNI, cmake/mobile/android/ads) ----
    private org.meshcontrol.ads.MceAds ads;

    public void mceAdsStart(boolean testDevice) {
        if (ads == null) ads = new org.meshcontrol.ads.MceAds(this);
        ads.start(testDevice);
    }
    public void mceAdsStop() { if (ads != null) ads.stop(); }
    public boolean mceAdsPrivacyRequired() { return ads != null && ads.privacyRequired(); }
    public void mceAdsShowPrivacy() { ads.showPrivacy(); }
    public void mceAdsLoad(int format, String unit, int request) { ads.load(format, unit, request); }
    public void mceAdsShow(int format, String unit, int request) { ads.show(format, unit, request); }
    public void mceAdsBanner(String unit, int request, boolean top, int l, int t, int r, int b) { ads.banner(unit, request, top, l, t, r, b); }
    public void mceAdsHideBanner() { if (ads != null) ads.hideBanner(); }
'''

# MceActivity's bridge to MceReview, for platform/review.h's JNI call (src/engine/platform/src/review.cpp).
REVIEW_METHODS = '''
    // ---- the store's review prompt (mce_platform's request_review over JNI, cmake/mobile/android/review) ----
    private org.meshcontrol.review.MceReview review;

    public void mceReviewRequest() {
        if (review == null) review = new org.meshcontrol.review.MceReview(this);
        review.request();
    }
'''

# MceActivity's bridge to MceHaptics, for platform/haptics.h's JNI call (src/engine/platform/src/haptics.cpp).
HAPTICS_METHODS = '''
    // ---- haptics (mce_platform's haptics::play over JNI, cmake/mobile/android/haptics) ----
    private org.meshcontrol.haptics.MceHaptics haptics;

    public void mceHaptic(int preset) {
        if (haptics == null) haptics = new org.meshcontrol.haptics.MceHaptics(this);
        haptics.play(preset);
    }
'''


def sdk_root():
    root = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT") or os.path.expanduser("~/Library/Android/sdk")
    if not os.path.isdir(root):
        sys.exit(f"ship: no Android SDK at {root} (set ANDROID_HOME)")
    return root


def ndk_root():
    ndk = os.environ.get("ANDROID_NDK_HOME")
    if ndk and os.path.isdir(ndk):
        return ndk
    found = sorted(glob.glob(os.path.join(sdk_root(), "ndk", "*")), key=lambda p: [int(x) for x in re.findall(r"\d+", os.path.basename(p))])
    if not found:
        sys.exit("ship: no Android NDK (install one with sdkmanager 'ndk;28.2.13676358' or set ANDROID_NDK_HOME)")
    return found[-1]


def java_home():
    home = os.environ.get("JAVA_HOME")
    if home and os.path.isfile(os.path.join(home, "bin", "java")):
        return home
    if os.path.isfile("/usr/libexec/java_home"):
        out = subprocess.run(["/usr/libexec/java_home", "-v", "17"], capture_output=True, text=True)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    for cand in ("/opt/homebrew/opt/openjdk@17", "/usr/local/opt/openjdk@17", "/usr/lib/jvm/java-17-openjdk-amd64"):
        if os.path.isfile(os.path.join(cand, "bin", "java")):
            return cand
    sys.exit("ship: no JDK 17 found (set JAVA_HOME)")


def tool_env():
    env = dict(os.environ)
    env["ANDROID_HOME"] = sdk_root()
    env["ANDROID_NDK_HOME"] = ndk_root()
    env["JAVA_HOME"] = java_home()
    env["PATH"] = os.path.join(env["JAVA_HOME"], "bin") + os.pathsep + env.get("PATH", "")
    return env


def adb():
    path = os.path.join(sdk_root(), "platform-tools", "adb")
    if not os.path.isfile(path):
        sys.exit("ship: adb not found (sdkmanager platform-tools)")
    return path


def llvm_tool(name):
    hits = glob.glob(os.path.join(ndk_root(), "toolchains", "llvm", "prebuilt", "*", "bin", name))
    return hits[0] if hits else None


def app_id(name):
    return "org.meshcontrol." + (re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "game")


def fill(text, fields):
    for k, v in fields.items():
        text = text.replace(f"@{k}@", str(v))
    left = re.findall(r"@[A-Z_]+@", text)
    if left:
        sys.exit(f"ship: unfilled template fields {left}")
    return text


def copy_java(package, name, main):
    """Copies android/<package>/<name> into the stamped sources as org.meshcontrol.<package>."""
    dest = os.path.join(main, "java", "org", "meshcontrol", package)
    os.makedirs(dest, exist_ok=True)
    shutil.copy2(os.path.join(TEMPLATE, package, name), dest)


def stamp(project, art, args, root, build_dir, lib, keystore):
    """The Gradle project for one game under `project` (recreated each run)."""
    sdl_project = os.path.join(build_dir, "_deps", "sdl3-src", "android-project")
    if not os.path.isdir(sdl_project):
        sys.exit(f"ship: SDL's android-project not found under {build_dir}/_deps (configure the preset first)")
    shutil.rmtree(project, ignore_errors=True)
    shutil.copytree(TEMPLATE, project, ignore=shutil.ignore_patterns("*.in", "README.md", "MceActivity.java*", "billing", "ads", "review", "haptics",
                                                                   "firebase"))
    for name in ("gradlew", "gradlew.bat", "gradle"):
        src = os.path.join(sdl_project, name)
        dst = os.path.join(project, name)
        if os.path.isdir(src):
            shutil.copytree(src, dst)
        else:
            shutil.copy2(src, dst)
    main = os.path.join(project, "app", "src", "main")
    shutil.copytree(os.path.join(sdl_project, "app", "src", "main", "java"), os.path.join(main, "java"))
    shutil.copytree(os.path.join(sdl_project, "app", "src", "main", "res"), os.path.join(main, "res"),
                    ignore=shutil.ignore_patterns("strings.xml", "styles.xml", "colors.xml"))
    aid = app_id(art["name"])
    fields = {
        "APP_ID": aid,
        "LIBRARY": os.path.basename(lib)[3:-3],  # lib<name>.so -> <name>
        "VERSION_CODE": args.build_number,
        "ANDROID_API": ANDROID_API,
        "VERSION_NAME": args.version,
        "ORIENTATION": "sensorLandscape",
        "KEYSTORE": keystore.replace("\\", "/"),
        "BILLING": "",
        "ADS": "",
        "ADS_META": "",
        "FIREBASE": "",
        "FIREBASE_META": "",
        "PLUGINS": "",
        "CRASHLYTICS": "",
        "REVIEW": REVIEW_METHODS,
        "HAPTICS": HAPTICS_METHODS,
        "DEPENDENCIES": "",
    }
    shop = monetize_info.read(art)
    deps = [PLAY_REVIEW]
    copy_java("review", "MceReview.java", main)
    copy_java("haptics", "MceHaptics.java", main)
    if shop["products"]:
        fields["BILLING"] = BILLING_METHODS
        deps.append(PLAY_BILLING)
        copy_java("billing", "MceBilling.java", main)
        print(f"ship: {art['name']} sells products: Play Billing ({PLAY_BILLING}) stamped in", flush=True)
    if shop["ads"]:
        admob_id = monetize_info.admob_app_id(shop, "android", art["name"])
        fields["ADS"] = ADS_METHODS
        fields["ADS_META"] = ('\n        <meta-data android:name="com.google.android.gms.ads.APPLICATION_ID"\n'
                              f'            android:value="{admob_id}" />')
        deps += [GMA, UMP]
        copy_java("ads", "MceAds.java", main)
        print(f"ship: {art['name']} shows ads: Google Mobile Ads ({GMA}) and UMP stamped in, app id {admob_id}", flush=True)
    firebase = firebase_config()
    if firebase:
        fields["FIREBASE"] = FIREBASE_METHODS
        fields["FIREBASE_META"] = FIREBASE_META
        fields["PLUGINS"] = "\n    id 'com.google.gms.google-services'\n    id 'com.google.firebase.crashlytics'"
        # The library is stripped before Gradle runs; Crashlytics reads the unstripped copy from here.
        fields["CRASHLYTICS"] = ("\n            firebaseCrashlytics {\n                nativeSymbolUploadEnabled true\n"
                                 "                unstrippedNativeLibsDir file('crashlytics-symbols')\n            }")
        deps.append(f"platform('{FIREBASE_BOM}')")
        deps += FIREBASE_LIBS
        shutil.copy2(firebase, os.path.join(project, "app", "google-services.json"))
        with open(os.path.join(project, "build.gradle")) as f:
            root_gradle = f.read()
        classpath = "        classpath 'com.android.tools.build:"
        if classpath not in root_gradle:
            sys.exit("ship: android/build.gradle no longer has the Android Gradle plugin classpath line")
        root_gradle = root_gradle.replace(classpath, f"        classpath '{GOOGLE_SERVICES_PLUGIN}'\n"
                                          f"        classpath '{CRASHLYTICS_PLUGIN}'\n" + classpath, 1)
        with open(os.path.join(project, "build.gradle"), "w") as f:
            f.write(root_gradle)
        copy_java("firebase", "MceFirebase.java", main)
        print(f"ship: {art['name']} reports to Firebase: Crashlytics NDK and Analytics ({FIREBASE_BOM}) stamped in",
              flush=True)
    if deps:
        lines = "".join(f"    implementation {d}\n" if d.startswith("platform(") else f"    implementation '{d}'\n"
                        for d in deps)
        fields["DEPENDENCIES"] = f"\ndependencies {{\n{lines}}}\n"
    with open(os.path.join(TEMPLATE, "app", "build.gradle.in")) as f:
        text = fill(f.read(), fields)
    with open(os.path.join(project, "app", "build.gradle"), "w") as f:
        f.write(text)
    with open(os.path.join(TEMPLATE, "app", "src", "main", "AndroidManifest.xml.in")) as f:
        text = fill(f.read(), fields)
    with open(os.path.join(main, "AndroidManifest.xml"), "w") as f:
        f.write(text)
    java_dir = os.path.join(main, "java", *aid.split("."))
    os.makedirs(java_dir, exist_ok=True)
    with open(os.path.join(TEMPLATE, "MceActivity.java.in")) as f:
        text = fill(f.read(), fields)
    with open(os.path.join(java_dir, "MceActivity.java"), "w") as f:
        f.write(text)
    values = os.path.join(main, "res", "values")
    os.makedirs(values, exist_ok=True)
    title = art["title"].replace("&", "&amp;").replace("<", "&lt;")
    with open(os.path.join(values, "strings.xml"), "w") as f:
        f.write(f'<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <string name="app_name">{title}</string>\n</resources>\n')
    app_names.android(app_names.read(art.get("loc_dir")), os.path.join(main, "res"))  # app.name per locale
    icon = app_icon.source(art)
    if icon:
        app_icon.android(icon, os.path.join(main, "res"), os.path.join(args.out, f"{art['name']}-{args.version}-android-play-icon.png"))
    return aid


def package(art, args, root):
    """Builds the APK (and AAB) for `art` from the android preset's outputs; returns the APK path."""
    build_dir = os.path.join(root, "build", args.preset)
    lib = art["android_lib"]
    pack = art["pack"]
    for p in (lib, pack):
        if not os.path.isfile(p):
            sys.exit(f"ship: missing build output {p}")
    stem = f"{art['name']}-{args.version}-android"
    project = os.path.join(build_dir, "android", art["name"])
    keystore = os.environ.get("MCE_ANDROID_KEYSTORE") or os.path.expanduser("~/.android/debug.keystore")
    env = tool_env()
    if not os.path.isfile(keystore):
        if os.environ.get("MCE_ANDROID_KEYSTORE"):
            sys.exit(f"ship: MCE_ANDROID_KEYSTORE={keystore} does not exist")
        os.makedirs(os.path.dirname(keystore), exist_ok=True)
        subprocess.run([os.path.join(env["JAVA_HOME"], "bin", "keytool"), "-genkeypair", "-keystore", keystore,
                        "-storepass", "android", "-keypass", "android", "-alias", "androiddebugkey",
                        "-dname", "CN=Android Debug,O=Android,C=US", "-keyalg", "RSA", "-keysize", "2048",
                        "-validity", "10000"], check=True, capture_output=True)
    if not os.environ.get("MCE_ANDROID_KEYSTORE"):
        print("ship: signing with the Android debug keystore (set MCE_ANDROID_KEYSTORE for a store upload)", flush=True)
    aid = stamp(project, art, args, root, build_dir, lib, keystore)
    firebase = firebase_config()

    main = os.path.join(project, "app", "src", "main")
    jni = os.path.join(main, "jniLibs", "arm64-v8a")
    os.makedirs(jni)
    shipped_lib = os.path.join(jni, os.path.basename(lib))
    shutil.copy2(lib, shipped_lib)
    symbols = os.path.join(args.out, stem + "-symbols")
    shutil.rmtree(symbols, ignore_errors=True)
    if not args.keep_symbols:
        strip = llvm_tool("llvm-strip")
        if strip:
            os.makedirs(symbols)
            shutil.copy2(lib, os.path.join(symbols, os.path.basename(lib)))
            subprocess.run([strip, "--strip-unneeded", shipped_lib], check=True)
        else:
            print("ship: llvm-strip not found in the NDK, the library ships unstripped", flush=True)
    assets = os.path.join(main, "assets")
    os.makedirs(assets)
    bundled = {"assets.mce": pack}
    if art.get("credits"):
        bundled["CREDITS.md"] = art["credits"]
    lines = []
    for name, src in sorted(bundled.items()):
        shutil.copy2(src, os.path.join(assets, name))
        lines.append(f"{name} {os.path.getsize(src)}")
    # The list is also the extraction stamp: a new pack (other size or version) re-extracts.
    lines.append(f"# version {args.version}")
    with open(os.path.join(assets, "mce_bundle.txt"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

    if firebase:
        # Crashlytics' symbol upload reads <dir>/<abi>/lib*.so, the unstripped library.
        unstripped = os.path.join(project, "app", "crashlytics-symbols", "arm64-v8a")
        os.makedirs(unstripped)
        shutil.copy2(lib, unstripped)
    tasks = ["assembleRelease"] + (["bundleRelease"] if args.aab else [])
    if firebase:
        tasks.append("uploadCrashlyticsSymbolFileRelease")
    gradlew = os.path.join(project, "gradlew")
    os.chmod(gradlew, 0o755)
    print(f"+ gradlew {' '.join(tasks)}  ({project})", flush=True)
    subprocess.run([gradlew, "--no-daemon", "-q", *tasks], cwd=project, env=env, check=True)

    apk = os.path.join(project, "app", "build", "outputs", "apk", "release", "app-release.apk")
    if not os.path.isfile(apk):
        sys.exit(f"ship: gradle did not produce {apk}")
    out_apk = os.path.join(args.out, stem + ".apk")
    shutil.copy2(apk, out_apk)
    print(f"ship: {out_apk} ({os.path.getsize(out_apk) / 1024:.0f} KiB, {aid})")
    if args.aab:
        aab = os.path.join(project, "app", "build", "outputs", "bundle", "release", "app-release.aab")
        out_aab = os.path.join(args.out, stem + ".aab")
        shutil.copy2(aab, out_aab)
        print(f"ship: {out_aab} ({os.path.getsize(out_aab) / 1024:.0f} KiB)")
    if os.path.isdir(symbols):
        with zipfile.ZipFile(symbols + ".zip", "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for name in sorted(os.listdir(symbols)):
                z.write(os.path.join(symbols, name), arcname=os.path.join(stem + "-symbols", name))
        shutil.rmtree(symbols)
        print(f"ship: {symbols}.zip (symbols, keep it; never ship it)")
    if args.smoke:
        smoke(out_apk, aid, art)
    return out_apk


def smoke(apk, aid, art):
    """Installs the APK on the one connected emulator/device, plays a short run, checks the log."""
    a = adb()
    devices = [l.split()[0] for l in subprocess.run([a, "devices"], capture_output=True, text=True).stdout.splitlines()[1:]
               if l.strip().endswith("device")]
    if not devices:
        sys.exit("ship: --smoke needs a running emulator or a connected device (adb devices is empty)")
    subprocess.run([a, "install", "-r", apk], check=True)
    subprocess.run([a, "logcat", "-c"], check=True)
    extra = "-frames 300" + (" -autoplay 1" if art.get("autoplay") else "")
    # adb shell joins its arguments into one device shell line: the extra must be quoted there.
    subprocess.run([a, "shell", f"am start -W -n {aid}/.MceActivity --es mce_args {shlex.quote(extra)}"], check=True)
    # The game's main() returning finishes the activity (SDL logs onDestroy); the process may stay cached.
    deadline = time.time() + 120
    alive = True
    while time.time() < deadline:
        time.sleep(2)
        log = subprocess.run([a, "logcat", "-d", "-v", "brief"], capture_output=True, text=True).stdout
        pid = subprocess.run([a, "shell", "pidof", aid], capture_output=True, text=True).stdout.strip()
        if not pid or "onDestroy()" in log or "FATAL EXCEPTION" in log or "Fatal signal" in log:
            alive = False
            break
    mine = [l for l in log.splitlines() if "SDL/APP" in l or "MCE" in l or "AndroidRuntime" in l or "DEBUG" in l
            or "libc" in l and "Fatal" in l]
    sys.stdout.write("\n".join(mine[-60:]) + "\n")
    if alive:
        sys.exit("ship: smoke run still going after 120 s (SwiftShader emulators hang in present: start the "
                 "emulator with -gpu host)")
    if any("Fatal signal" in l or "FATAL EXCEPTION" in l for l in log.splitlines()):
        sys.exit("ship: smoke run crashed (see the log above)")
    if not any(("mounted" in l or "pack" in l) and "/bundle/" in l for l in log.splitlines()):
        sys.exit("ship: the smoke run did not mount the bundled pack")
    print("ship: android smoke run OK")
