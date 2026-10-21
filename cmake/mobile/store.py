"""Store upload after packaging: scripts/ship.py --target ios|android --store [--dry-run] (M-MOBILE step 10, #140).

    python3 scripts/ship.py platformer --target ios --preset ios-arm64 --store     # .ipa -> TestFlight
    python3 scripts/ship.py platformer --target android --store                    # .aab -> Play internal track
    ... --store --dry-run                                                          # validate only, no upload

Build numbers: CFBundleVersion and versionCode are ship.py's --build-number (default: the commit count of
HEAD, which only grows on a linear branch). A store refuses a number it has already seen; pass a larger one.

Credentials, read from the environment only and never printed or written anywhere:
    iOS     MCE_ASC_KEY_ID, MCE_ASC_ISSUER_ID, MCE_ASC_KEY_PATH (the AuthKey_<id>.p8 of an App Store
            Connect API key with the App Manager role); the upload goes through `xcrun altool`.
    Android MCE_PLAY_SERVICE_ACCOUNT (path of the service account JSON with release rights on the app in
            the Play Console), MCE_PLAY_TRACK (default internal), MCE_PLAY_RELEASE_STATUS (default completed;
            draft while the app has never been published); the upload goes through the Play Developer API.
            The AAB must be signed with the upload key (MCE_ANDROID_KEYSTORE), not the debug keystore.

--dry-run checks the package locally (no credentials needed): iOS bundle id, integer CFBundleVersion, app
icon (Assets.car), privacy manifest, signature (codesign --verify) and, for an .ipa with credentials,
`altool --validate-app`; Android: bundle layout, arm64 library, jarsigner signature, not debug-signed, and
`bundletool validate` when bundletool (or MCE_BUNDLETOOL, a jar) is available. Problems are listed and the
exit code is 1 without --dry-run's upload ever starting.
"""

import base64
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

PLAY_API = "https://androidpublisher.googleapis.com/androidpublisher/v3/applications"
PLAY_UPLOAD = "https://androidpublisher.googleapis.com/upload/androidpublisher/v3/applications"


def env_paths(*names):
    """The named environment variables, or None when any is unset (their values are never printed)."""
    vals = [os.environ.get(n) for n in names]
    return vals if all(vals) else None


def report(kind, problems, notes):
    for n in notes:
        print(f"store: {kind}: ok   {n}")
    for p in problems:
        print(f"store: {kind}: FAIL {p}")
    if problems:
        sys.exit(f"store: {kind} package is not ready for the store ({len(problems)} problem(s))")


# ---- iOS: TestFlight ----

def check_ios(path):
    """(problems, notes) for an .ipa, or a Simulator .zip / .app (which can never be uploaded)."""
    problems, notes = [], []
    with tempfile.TemporaryDirectory() as tmp:
        if path.endswith(".app"):
            app = path
        else:
            # ditto keeps the modes, symlinks and extended attributes the signature seals; zipfile does not.
            subprocess.run(["ditto", "-x", "-k", path, tmp], check=True)
            apps = [os.path.join(r, d) for r, ds, _ in os.walk(tmp) for d in ds if d.endswith(".app")]
            if not apps:
                return [f"{path} holds no .app"], notes
            app = apps[0]
        if not path.endswith(".ipa"):
            notes.append("Simulator build: bundle checks only (an upload needs the ios-arm64 preset's .ipa)")
        with open(os.path.join(app, "Info.plist"), "rb") as f:
            info = plistlib.load(f)
        bid = info.get("CFBundleIdentifier", "")
        notes.append(f"bundle id {bid}")
        build = str(info.get("CFBundleVersion", ""))
        if build.isdigit():
            notes.append(f"build number {build}, version {info.get('CFBundleShortVersionString')}")
        else:
            problems.append(f"CFBundleVersion '{build}' is not an integer build number")
        if os.path.isfile(os.path.join(app, "Assets.car")) and "CFBundleIcons" in info:
            notes.append("app icon (Assets.car, CFBundleIcons)")
        else:
            problems.append("no app icon (add ship/icon.png to the game)")
        if os.path.isfile(os.path.join(app, "PrivacyInfo.xcprivacy")):
            notes.append("privacy manifest")
        else:
            problems.append("no PrivacyInfo.xcprivacy in the bundle")
        sig = subprocess.run(["codesign", "--verify", "--strict", app], capture_output=True, text=True)
        if sig.returncode == 0:
            notes.append("signature verifies")
        else:
            problems.append("codesign --verify: " + sig.stderr.strip())
        if path.endswith(".ipa"):
            if os.path.isfile(os.path.join(app, "embedded.mobileprovision")):
                prof = subprocess.run(["security", "cms", "-D", "-i", os.path.join(app, "embedded.mobileprovision")],
                                      capture_output=True).stdout
                ent = plistlib.loads(prof).get("Entitlements", {}) if prof else {}
                if ent.get("get-task-allow"):
                    problems.append("signed with a development profile; TestFlight needs an App Store distribution profile")
                else:
                    notes.append("distribution profile")
            else:
                problems.append("no embedded.mobileprovision")
    return problems, notes


def altool(action, ipa, creds):
    key_id, issuer, key_path = creds
    cmd = ["xcrun", "altool", action, "-f", ipa, "-t", "ios", "--api-key", key_id, "--api-issuer", issuer,
           "--p8-file-path", key_path, "--output-format", "normal"]
    print(f"+ xcrun altool {action} -f {os.path.basename(ipa)} -t ios --api-key *** --api-issuer *** --p8-file-path ***",
          flush=True)
    out = subprocess.run(cmd, capture_output=True, text=True)
    text = (out.stdout + out.stderr).replace(issuer, "***").replace(key_id, "***")
    sys.stdout.write(text[-4000:])
    if out.returncode != 0:
        sys.exit(f"store: altool {action} failed")


def ios(path, dry_run):
    problems, notes = check_ios(path)
    creds = env_paths("MCE_ASC_KEY_ID", "MCE_ASC_ISSUER_ID", "MCE_ASC_KEY_PATH")
    if dry_run:
        report("ios", problems, notes)
        if creds and path.endswith(".ipa"):
            altool("--validate-app", path, creds)
        else:
            print("store: ios: dry run without App Store Connect validation (needs an .ipa and MCE_ASC_KEY_ID/"
                  "MCE_ASC_ISSUER_ID/MCE_ASC_KEY_PATH)")
        print("store: ios dry run OK (nothing uploaded)")
        return
    if not path.endswith(".ipa"):
        problems.append("a Simulator build cannot be uploaded: build the ios-arm64 preset (an .ipa)")
    if not creds:
        problems.append("set MCE_ASC_KEY_ID, MCE_ASC_ISSUER_ID and MCE_ASC_KEY_PATH (App Store Connect API key)")
    report("ios", problems, notes)
    altool("--upload-app", path, creds)
    print("store: uploaded to App Store Connect; the build appears in TestFlight after processing")


# ---- Android: Play internal track ----

def check_android(aab):
    problems, notes = [], []
    if not aab or not os.path.isfile(aab):
        return ["no .aab (ship.py builds it with --aab; --store does that itself)"], notes
    with zipfile.ZipFile(aab) as z:
        names = set(z.namelist())
    for need in ("BundleConfig.pb", "base/manifest/AndroidManifest.xml"):
        (notes.append(need) if need in names else problems.append(f"missing {need}"))
    libs = [n for n in names if n.startswith("base/lib/arm64-v8a/") and n.endswith(".so")]
    (notes.append(f"arm64 library {os.path.basename(libs[0])}") if libs else problems.append("no base/lib/arm64-v8a/*.so"))
    import android_app  # the JDK 17 that signed it (macOS's /usr/bin/jarsigner is a stub without a JDK)
    jarsigner = os.path.join(android_app.java_home(), "bin", "jarsigner")
    if os.path.isfile(jarsigner):
        out = subprocess.run([jarsigner, "-verify", "-verbose", "-certs", aab], capture_output=True, text=True)
        if "jar verified" not in out.stdout:
            problems.append("jarsigner: the bundle is not signed")
        elif "CN=Android Debug" in out.stdout:
            problems.append("signed with the Android debug keystore; Play needs the upload key (MCE_ANDROID_KEYSTORE)")
        else:
            notes.append("signed with an upload key")
    else:
        notes.append("jarsigner not found, signature not checked")
    bundletool = shutil.which("bundletool")
    jar = os.environ.get("MCE_BUNDLETOOL")
    java = os.path.join(android_app.java_home(), "bin", "java")
    cmd = [bundletool] if bundletool else [java, "-jar", jar] if jar and os.path.isfile(jar) else None
    if cmd:
        out = subprocess.run(cmd + ["validate", "--bundle", aab], capture_output=True, text=True)
        (notes.append("bundletool validate") if out.returncode == 0
         else problems.append("bundletool validate: " + (out.stderr or out.stdout).strip()[-500:]))
    else:
        notes.append("bundletool not found (set MCE_BUNDLETOOL to its jar for the full validation)")
    return problems, notes


def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def play_token(account_path):
    """An OAuth access token for the service account (JWT signed with openssl; no Google libraries)."""
    with open(account_path) as f:
        account = json.load(f)
    now = int(time.time())
    header = b64url(json.dumps({"alg": "RS256", "typ": "JWT"}).encode())
    claims = b64url(json.dumps({"iss": account["client_email"], "scope": "https://www.googleapis.com/auth/androidpublisher",
                                "aud": account["token_uri"], "iat": now, "exp": now + 3600}).encode())
    with tempfile.TemporaryDirectory() as tmp:
        key = os.path.join(tmp, "key.pem")
        fd = os.open(key, os.O_WRONLY | os.O_CREAT, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(account["private_key"])
        sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", key], input=f"{header}.{claims}".encode(),
                             capture_output=True, check=True).stdout
    body = urllib.parse.urlencode({"grant_type": "urn:ietf:params:oauth:grant-type:jwt-bearer",
                                   "assertion": f"{header}.{claims}.{b64url(sig)}"}).encode()
    return json.loads(http("POST", account["token_uri"], body, {"Content-Type": "application/x-www-form-urlencoded"}))["access_token"]


def http(method, url, body=None, headers=None):
    req = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=600) as r:
            return r.read().decode() or "{}"
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")[:2000]
        sys.exit(f"store: {method} {url.split('?')[0]} -> HTTP {e.code}: {detail}")


def android(aab, package_name, version_code, dry_run):
    problems, notes = check_android(aab)
    account = os.environ.get("MCE_PLAY_SERVICE_ACCOUNT")
    track = os.environ.get("MCE_PLAY_TRACK", "internal")
    status = os.environ.get("MCE_PLAY_RELEASE_STATUS", "completed")
    notes.append(f"package {package_name}, versionCode {version_code}, track {track}")
    if dry_run:
        report("android", problems, notes)
        print("store: android dry run OK (nothing uploaded)")
        return
    if not account or not os.path.isfile(account):
        problems.append("set MCE_PLAY_SERVICE_ACCOUNT to the service account JSON")
    report("android", problems, notes)
    token = play_token(account)
    auth = {"Authorization": f"Bearer {token}"}
    app = f"{PLAY_API}/{package_name}"
    edit = json.loads(http("POST", f"{app}/edits", b"{}", dict(auth, **{"Content-Type": "application/json"})))["id"]
    print(f"store: Play edit opened, uploading {os.path.basename(aab)}", flush=True)
    with open(aab, "rb") as f:
        bundle = json.loads(http("POST", f"{PLAY_UPLOAD}/{package_name}/edits/{edit}/bundles?uploadType=media", f.read(),
                                 dict(auth, **{"Content-Type": "application/octet-stream"})))
    if int(bundle["versionCode"]) != int(version_code):
        sys.exit(f"store: Play read versionCode {bundle['versionCode']}, expected {version_code}")
    release = {"track": track, "releases": [{"versionCodes": [str(version_code)], "status": status}]}
    http("PUT", f"{app}/edits/{edit}/tracks/{track}", json.dumps(release).encode(),
         dict(auth, **{"Content-Type": "application/json"}))
    http("POST", f"{app}/edits/{edit}:commit", b"", auth)
    print(f"store: versionCode {version_code} released to the Play '{track}' track ({status})")
