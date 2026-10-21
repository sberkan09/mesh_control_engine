"""iOS packaging for scripts/ship.py --target ios (M-MOBILE step 7).

    python3 scripts/ship.py platformer --target ios                       # Simulator: dist/<Name>-<version>-ios-sim.zip
    python3 scripts/ship.py platformer --target ios --smoke               # ... and play 300 frames in a Simulator
    python3 scripts/ship.py diorama --target ios --preset ios-arm64       # device: dist/<Name>-<version>-ios.ipa

The game's app bundle comes from the ios-sim-arm64 / ios-arm64 preset (cmake/MceMobile.cmake: every
engine executable is <target>.app with SDL's UIKit entry point). This module copies it, puts the
pack (assets.mce) and CREDITS.md in the bundle root, where platform::base_path() (SDL_GetBasePath, the
bundle's resource directory) finds them, sets the display name, and signs it:

- Simulator builds: ad-hoc (codesign -s -); the result is a zipped .app for `xcrun simctl install`.
- Device builds: MCE_IOS_SIGN_IDENTITY (e.g. "Apple Development: Name (TEAMID)") and
  MCE_IOS_PROVISIONING_PROFILE (a .mobileprovision whose app id matches the bundle id, default
  org.meshcontrol.<target>, see MCE_IOS_BUNDLE_PREFIX) are required; the profile is embedded, its
  entitlements are signed in, and the bundle is zipped as Payload/<Name>.app into the .ipa. Install it
  with Xcode's Devices window or `xcrun devicectl device install app --device <id> <ipa>`.

Store readiness (step 10): the game's ship/icon.png becomes the AppIcon catalog (app_icon.py),
PrivacyInfo.xcprivacy (cmake/mobile/ios/, plus the game's ship/PrivacyInfo.xcprivacy merged in) goes to
the bundle root, CFBundleVersion is the integer build number (ship.py --build-number) and
CFBundleShortVersionString the release version without its +commit suffix.

Ads (M-MONETIZE step 4, monetize_info.py): for a game whose .mcstore has ad placements,
ios/PrivacyInfo.ads.xcprivacy is merged into the privacy manifest and Info.plist gets the AdMob app id,
the ATT prompt text and Google's SKAdNetwork id. The SDKs themselves are linked by the build
(-DMCE_ADMOB=ON, src/engine/platform/CMakeLists.txt).

Live services vendor (M-LIVEOPS step 1): with MCE_FIREBASE_IOS_CONFIG pointing at the project's
GoogleService-Info.plist (from the Firebase console; kept out of the repository and handed to the build
through the environment), the plist goes into the bundle, Info.plist starts Analytics and Crashlytics
with collection off until the player consents, and ios/PrivacyInfo.firebase.xcprivacy is merged into
the privacy manifest. With MCE_FIREBASE_SDK_DIR (the unzipped Firebase.zip the build linked,
-DMCE_FIREBASE=ON) the dSYM is uploaded with Crashlytics' upload-symbols.
"""

import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

import app_icon
import monetize_info
import app_names

HERE = os.path.dirname(os.path.abspath(__file__))
MIN_OS = "16.0"  # MinimumOSVersion in ios/Info.plist.in


def simulator(args):
    return "sim" in args.preset


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def entitlements_from_profile(profile, out_path):
    decoded = subprocess.run(["security", "cms", "-D", "-i", profile], check=True, capture_output=True).stdout
    plist = plistlib.loads(decoded)
    with open(out_path, "wb") as f:
        plistlib.dump(plist.get("Entitlements", {}), f)


def merge_privacy(base, extra):
    """The engine's privacy manifest with a game's additions (its SDKs' data and API reasons) merged in."""
    out = dict(base)
    out["NSPrivacyTracking"] = bool(base.get("NSPrivacyTracking")) or bool(extra.get("NSPrivacyTracking"))
    for key in ("NSPrivacyTrackingDomains", "NSPrivacyCollectedDataTypes"):
        items = list(base.get(key, []))
        items += [i for i in extra.get(key, []) if i not in items]
        out[key] = items
    apis = {a["NSPrivacyAccessedAPIType"]: list(a.get("NSPrivacyAccessedAPITypeReasons", []))
            for a in base.get("NSPrivacyAccessedAPITypes", [])}
    for a in extra.get("NSPrivacyAccessedAPITypes", []):
        reasons = apis.setdefault(a["NSPrivacyAccessedAPIType"], [])
        reasons += [r for r in a.get("NSPrivacyAccessedAPITypeReasons", []) if r not in reasons]
    out["NSPrivacyAccessedAPITypes"] = [{"NSPrivacyAccessedAPIType": k, "NSPrivacyAccessedAPITypeReasons": v}
                                        for k, v in apis.items()]
    return out


# Showing ads (M-MONETIZE step 4): the ATT prompt's text, and Google's own SKAdNetwork id (its ad partners'
# ids belong in the game's ship/PrivacyInfo.xcprivacy-style additions when it mediates other networks).
ATT_TEXT = "Your data will be used to show you ads that are more relevant to you."
GOOGLE_SKADNETWORK = "cstr6suwn9.skadnetwork"


def add_ads(info, art, shop):
    """Info.plist keys Google Mobile Ads and ATT need, for a game with ad placements."""
    info["GADApplicationIdentifier"] = monetize_info.admob_app_id(shop, "ios", art["name"])
    info.setdefault("NSUserTrackingUsageDescription", ATT_TEXT)
    items = info.setdefault("SKAdNetworkItems", [])
    if not any(i.get("SKAdNetworkIdentifier") == GOOGLE_SKADNETWORK for i in items):
        items.append({"SKAdNetworkIdentifier": GOOGLE_SKADNETWORK})
    print(f"ship: {art['name']} shows ads: AdMob app id {info['GADApplicationIdentifier']}, ATT text, SKAdNetwork",
          flush=True)


def firebase_config():
    """The project's GoogleService-Info.plist from MCE_FIREBASE_IOS_CONFIG, or None."""
    path = os.environ.get("MCE_FIREBASE_IOS_CONFIG")
    if not path:
        return None
    if not os.path.isfile(path):
        sys.exit(f"ship: MCE_FIREBASE_IOS_CONFIG={path} does not exist")
    return path


def add_firebase(info, app, art, config):
    """The Firebase config in the bundle and both SDKs' collection off until the player consents."""
    shutil.copy2(config, os.path.join(app, "GoogleService-Info.plist"))
    info["FIREBASE_ANALYTICS_COLLECTION_ENABLED"] = False
    info["FirebaseCrashlyticsCollectionEnabled"] = False
    print(f"ship: {art['name']} reports to Firebase: GoogleService-Info.plist bundled, collection off until consent",
          flush=True)


def upload_firebase_symbols(dsym, config):
    """Crashlytics' upload-symbols from MCE_FIREBASE_SDK_DIR, when it is set."""
    sdk = os.environ.get("MCE_FIREBASE_SDK_DIR")
    tool = os.path.join(sdk, "FirebaseCrashlytics", "upload-symbols") if sdk else ""
    if not tool or not os.path.isfile(tool):
        print("ship: warning: MCE_FIREBASE_SDK_DIR has no FirebaseCrashlytics/upload-symbols; upload the dSYM "
              "from the symbols zip yourself or Crashlytics shows unsymbolicated crashes", flush=True)
        return
    run([tool, "-gsp", config, "-p", "ios", dsym])


def write_privacy(app, art, ads=False, firebase=False):
    with open(os.path.join(HERE, "ios", "PrivacyInfo.xcprivacy"), "rb") as f:
        manifest = plistlib.load(f)
    if ads:
        with open(os.path.join(HERE, "ios", "PrivacyInfo.ads.xcprivacy"), "rb") as f:
            manifest = merge_privacy(manifest, plistlib.load(f))
        print("ship: merged the ads declarations into the privacy manifest", flush=True)
    if firebase:
        with open(os.path.join(HERE, "ios", "PrivacyInfo.firebase.xcprivacy"), "rb") as f:
            manifest = merge_privacy(manifest, plistlib.load(f))
        print("ship: merged the Firebase declarations into the privacy manifest", flush=True)
    extra = art.get("privacy")
    if extra and os.path.isfile(extra):
        with open(extra, "rb") as f:
            manifest = merge_privacy(manifest, plistlib.load(f))
        print(f"ship: merged {extra} into the privacy manifest", flush=True)
    with open(os.path.join(app, "PrivacyInfo.xcprivacy"), "wb") as f:
        plistlib.dump(manifest, f, fmt=plistlib.FMT_XML)


def package(art, args, root):
    """Stages, signs and archives the app; returns the .zip (Simulator) or .ipa (device) path."""
    build_app = art["ios_app"]
    pack = art["pack"]
    for p in (build_app, pack):
        if not os.path.exists(p):
            sys.exit(f"ship: missing build output {p}")
    sim = simulator(args)
    stem = f"{art['name']}-{args.version}-{'ios-sim' if sim else 'ios'}"
    stage = os.path.join(args.out, stem)
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    app = os.path.join(stage, art["name"] + ".app")
    shutil.copytree(build_app, app, symlinks=True)
    shutil.copy2(pack, os.path.join(app, "assets.mce"))
    if art.get("credits"):
        shutil.copy2(art["credits"], os.path.join(app, "CREDITS.md"))
    info_path = os.path.join(app, "Info.plist")
    with open(info_path, "rb") as f:
        info = plistlib.load(f)
    info["CFBundleDisplayName"] = art["title"]
    app_names.ios(app_names.read(art.get("loc_dir")), app, info)  # app.name per locale (<locale>.lproj)
    info["CFBundleShortVersionString"] = args.version.split("+")[0]
    info["CFBundleVersion"] = str(args.build_number)
    icon = app_icon.source(art)
    if icon:
        app_icon.ios(icon, app, sim, info.get("MinimumOSVersion", MIN_OS), info)
    shop = monetize_info.read(art)
    if shop["ads"]:
        add_ads(info, art, shop)
    firebase = firebase_config()
    if firebase:
        add_firebase(info, app, art, firebase)
    write_privacy(app, art, ads=shop["ads"], firebase=bool(firebase))
    with open(info_path, "wb") as f:
        plistlib.dump(info, f)
    bundle_id = info["CFBundleIdentifier"]

    # The debug info, archived beside the result (never shipped), then the binary stripped.
    exe = os.path.join(app, info["CFBundleExecutable"])
    symbols = os.path.join(args.out, stem + "-symbols")
    shutil.rmtree(symbols, ignore_errors=True)
    if not args.keep_symbols:
        os.makedirs(symbols)
        dsym = os.path.join(symbols, info["CFBundleExecutable"] + ".dSYM")
        run(["dsymutil", exe, "-o", dsym], capture_output=True)
        run(["strip", "-x", exe])
        if firebase:
            upload_firebase_symbols(dsym, firebase)

    if sim:
        run(["codesign", "--force", "--sign", "-", "--timestamp=none", app])
        out = os.path.join(args.out, stem + ".zip")
        shutil.rmtree(out, ignore_errors=True)
        run(["ditto", "-c", "-k", "--keepParent", app, out])
    else:
        identity = os.environ.get("MCE_IOS_SIGN_IDENTITY")
        profile = os.environ.get("MCE_IOS_PROVISIONING_PROFILE")
        if not identity or not profile or not os.path.isfile(profile):
            sys.exit("ship: a device build needs MCE_IOS_SIGN_IDENTITY and MCE_IOS_PROVISIONING_PROFILE "
                     f"(the profile's app id must match {bundle_id}); the unsigned app is at {app}")
        shutil.copy2(profile, os.path.join(app, "embedded.mobileprovision"))
        with tempfile.TemporaryDirectory() as tmp:
            ent = os.path.join(tmp, "entitlements.plist")
            entitlements_from_profile(profile, ent)
            run(["codesign", "--force", "--sign", identity, "--entitlements", ent, "--timestamp=none", app])
        run(["codesign", "--verify", "--strict", app])
        out = os.path.join(args.out, stem + ".ipa")
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for base, dirs, names in os.walk(app):
                dirs.sort()
                for name in sorted(names):
                    full = os.path.join(base, name)
                    z.write(full, arcname=os.path.join("Payload", os.path.relpath(full, stage)))
    print(f"ship: {out} ({os.path.getsize(out) / 1024:.0f} KiB, {bundle_id})")
    if os.path.isdir(symbols):
        run(["ditto", "-c", "-k", "--keepParent", symbols, symbols + ".zip"])
        shutil.rmtree(symbols)
        print(f"ship: {symbols}.zip (symbols, keep it; never ship it)")
    if args.smoke:
        if not sim:
            sys.exit("ship: --smoke runs in the Simulator: use the ios-sim-arm64 preset")
        smoke(app, bundle_id, art)
    return out


def booted_or_boot_simulator():
    """A booted iPhone Simulator's UDID, booting the newest available iPhone when none is."""
    devices = json.loads(subprocess.run(["xcrun", "simctl", "list", "devices", "available", "-j"],
                                        check=True, capture_output=True, text=True).stdout)["devices"]
    iphones = []
    for runtime, devs in devices.items():
        if "iOS" not in runtime:
            continue
        for d in devs:
            if d["name"].startswith("iPhone"):
                if d["state"] == "Booted":
                    return d["udid"]
                iphones.append((runtime, d["udid"]))
    if not iphones:
        sys.exit("ship: no iPhone Simulator available (Xcode > Settings > Platforms)")
    udid = sorted(iphones)[-1][1]
    run(["xcrun", "simctl", "boot", udid])
    run(["xcrun", "simctl", "bootstatus", udid, "-b"], capture_output=True)
    return udid


def smoke(app, bundle_id, art):
    """Installs the app in a Simulator, plays 300 frames, checks it exits cleanly with its own pack."""
    udid = booted_or_boot_simulator()
    run(["xcrun", "simctl", "install", udid, app])
    cmd = ["xcrun", "simctl", "launch", "--console-pty", "--terminate-running-process", udid, bundle_id, "-frames", "300"]
    if art.get("autoplay"):
        cmd += ["-autoplay", "1"]
    # The Simulator's Metal validation layer rejects features its virtual GPU does not advertise
    # (multi-draw-indirect) even though the host GPU runs them; devices are not affected.
    env = dict(os.environ, SIMCTL_CHILD_MTL_DEBUG_LAYER="0")
    print("+ " + " ".join(cmd), flush=True)
    start = time.time()
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=180, env=env)
    except subprocess.TimeoutExpired:
        subprocess.run(["xcrun", "simctl", "terminate", udid, bundle_id])
        sys.exit("ship: Simulator smoke run still going after 180 s")
    log = out.stdout + out.stderr
    lines = [l for l in log.splitlines() if not l.startswith("module ")]
    sys.stdout.write("\n".join(lines[-30:]) + "\n")
    if "MCE crash report" in log or "failed assertion" in log:
        sys.exit("ship: the Simulator smoke run crashed (see the log above)")
    if not any(("mounted" in l or "pack" in l) and ".app/assets.mce" in l for l in lines):
        sys.exit("ship: the Simulator smoke run did not mount the bundled pack")
    print(f"ship: ios simulator smoke run OK ({time.time() - start:.0f} s)")
