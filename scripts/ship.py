#!/usr/bin/env python3
"""One-command ship v1 (ROADMAP M4): build, cook, package and zip a shippable artifact.

    python3 scripts/ship.py diorama              # Artifact B -> dist/MCE-Diorama-<version>-<os>.zip
    python3 scripts/ship.py platformer           # Artifact A -> dist/Ember-Step-<version>-<os>.zip
    python3 scripts/ship.py all --version 0.1.0  # both
    python3 scripts/ship.py --project projects/MyGame   # a project from scripts/new_project.py
    python3 scripts/ship.py platformer --target android # dist/Ember-Step-<version>-android.apk (M-MOBILE,
                                                        # cmake/mobile/android_app.py; --aab, --smoke)
    python3 scripts/ship.py platformer --target ios     # dist/Ember-Step-<version>-ios-sim.zip; --preset ios-arm64
                                                        # signs an .ipa (cmake/mobile/ios_app.py; --smoke)
    python3 scripts/ship.py platformer --target android --store [--dry-run]   # ... then the Play internal track,
                                                        # or TestFlight for an .ipa (cmake/mobile/store.py)

Phone builds take the app icon from <game>/ship/icon.png (1024x1024; cmake/mobile/app_icon.py) and, on
iOS, privacy-manifest additions from <game>/ship/PrivacyInfo.xcprivacy. Their store build number
(CFBundleVersion, versionCode) is --build-number, by default the commit count of HEAD.

What a zip holds (one folder, double-click the executable; macOS: <Name>.app, see
cmake/macos/macos_bundle.py):
    <Name>[.exe]        the sample executable, renamed (Windows: icon + VERSIONINFO resource)
    <module>            the game module next to it, when the artifact has one
    assets.mce          the cooked shipping pack (mce_cook --pack; CRC-verified once at mount)
    CREDITS.md          content attribution (CC0 Kenney packs)
    VERSION.txt         version, engine commit, build preset, file hashes, build ids
    README.txt          how to run it

Beside the zip, never inside it: <stem>-symbols.zip, the debug info of every shipped binary
(Linux: objcopy --only-keep-debug files under .build-id/, the shipped copies stripped; Windows:
the PDBs). A crash report from the field (mce/crash.h) resolves against it with
scripts/symbolicate.py <report> --symbols <unzipped symbols folder>.

The build uses the given CMake preset (default: the platform's release preset); the cook runs as
part of the build (mce_sample_pack in samples/CMakeLists.txt). Exit code 0 = zip written.
"""

import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = ".exe" if os.name == "nt" else ""
MODULE_PREFIX, MODULE_SUFFIX = ("", ".dll") if os.name == "nt" else ("lib", ".so")
ENGINE_SUFFIX = ".dylib" if sys.platform == "darwin" else MODULE_SUFFIX  # SHARED, not MODULE
MACOS = sys.platform == "darwin"
sys.path.insert(0, os.path.join(ROOT, "cmake", "mobile"))
if MACOS:
    sys.path.insert(0, os.path.join(ROOT, "cmake", "macos"))
    import macos_bundle  # dSYMs, strip + ad-hoc signing, the .app bundle (Mac-owned)

ARTIFACTS = {
    "diorama": {
        "name": "MCE-Diorama",
        "title": "MCE Diorama (Artifact B)",
        "sample": "04_diorama",
        "target": "sample_04_diorama",
        "module": None,
        "controls": "Drag with the left mouse button (or use the arrow keys) to orbit, mouse wheel or Z/X to zoom,\n"
                    "Space toggles the slow automatic orbit, F toggles fog, Esc quits.",
    },
    "platformer": {
        "name": "Ember-Step",
        "title": "Ember Step (Artifact A)",
        "sample": "03_platformer",
        "target": "sample_03_platformer",
        "module": "platformer",
        "controls": "Arrows or A/D run, Space jumps, Esc pauses. Collect the coins and reach the flag.",
    },
}


def run(cmd):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=ROOT, check=True)


def git(*args):
    try:
        return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def default_preset():
    return "msvc-release" if os.name == "nt" else "macos-release" if MACOS else "linux-gcc-release"


def os_tag():
    return {"Windows": "win64", "Linux": "linux-x64", "Darwin": "macos"}.get(platform.system(), platform.system().lower())


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def sample_outputs(key, args):
    """A sample artifact: its target, module and cooked pack under build/<preset>/samples/<sample>."""
    art = dict(ARTIFACTS[key])
    sample_dir = os.path.join(ROOT, "build", args.preset, "samples", art["sample"])
    art["targets"] = [art["target"]] + ([art["module"]] if art["module"] and os.name != "nt" else [])
    art["exe"] = os.path.join(sample_dir, art["target"] + EXE)
    art["pack"] = os.path.join(sample_dir, "project", "assets.mce")
    art["module_file"] = os.path.join(sample_dir, MODULE_PREFIX + art["module"] + MODULE_SUFFIX) if art["module"] else None
    art["credits"] = os.path.join(ROOT, "samples", art["sample"], "CREDITS.md")
    art["source"] = os.path.join(ROOT, "samples", art["sample"])
    art["android_lib"] = os.path.join(sample_dir, "lib" + art["target"] + ".so")
    art["ios_app"] = os.path.join(sample_dir, art["target"] + ".app")
    art["autoplay"] = bool(art["module"])
    ship_dir = os.path.join(ROOT, "samples", art["sample"], "ship")
    art["icon"] = os.path.join(ship_dir, "icon.png")
    art["privacy"] = os.path.join(ship_dir, "PrivacyInfo.xcprivacy")
    return art


def project_outputs(project, args):
    """A project made from the template (mce_add_project): <game> host, <game>_game module, the pack."""
    proj_dir = os.path.abspath(project)
    files = [f for f in os.listdir(proj_dir) if f.endswith(".mceproj")] if os.path.isdir(proj_dir) else []
    if len(files) != 1:
        sys.exit(f"ship: {project} is not a project directory (expected one <name>.mceproj)")
    with open(os.path.join(proj_dir, files[0]), encoding="utf-8") as f:
        meta = json.load(f)
    game = meta["games"][0]
    rel = os.path.relpath(proj_dir, ROOT)
    if rel.startswith(".."):
        sys.exit("ship: --project must be inside the engine checkout (projects/<name>)")
    out_dir = os.path.join(ROOT, "build", args.preset, rel)
    title = meta.get("title") or meta["name"]
    credits = os.path.join(proj_dir, "CREDITS.md")
    return {
        "name": re.sub(r"[^A-Za-z0-9_.-]+", "-", title).strip("-") or meta["name"],
        "title": title,
        "source": proj_dir,
        "targets": [game] + ([game + "_game"] if os.name != "nt" else []),
        "exe": os.path.join(out_dir, game + EXE),
        "pack": os.path.join(out_dir, "project", "assets.mce"),
        "module": game,
        "module_file": os.path.join(out_dir, MODULE_PREFIX + game + MODULE_SUFFIX),
        "credits": credits if os.path.isfile(credits) else None,
        "controls": "Esc pauses, ` or F1 opens the developer console.",
        "autoplay": False,
        "icon": os.path.join(proj_dir, "ship", "icon.png"),
        "privacy": os.path.join(proj_dir, "ship", "PrivacyInfo.xcprivacy"),
    }


def build_mobile(art, args, env=None):
    """A phone build: the host's mce_cook first (it cooks the packs), then the game on the phone preset."""
    run(["cmake", "--preset", args.host_preset])
    run(["cmake", "--build", "--preset", args.host_preset, "--target", "mce_cook"])
    cook = os.path.join(ROOT, "build", args.host_preset, "tools", "cook", "mce_cook" + EXE)
    print(f"+ cmake --preset {args.preset} -DMCE_HOST_COOK={cook}", flush=True)
    subprocess.run(["cmake", "--preset", args.preset, f"-DMCE_HOST_COOK={cook}"], cwd=ROOT, env=env, check=True)
    run(["cmake", "--build", "--preset", args.preset, "--target", art["target"]])


def ship_android(art, args):
    """The APK: the game's library from the android preset, then Gradle."""
    import android_app
    if not args.no_build:
        build_mobile(art, args, dict(os.environ, ANDROID_NDK_HOME=android_app.ndk_root()))
    apk = android_app.package(art, args, ROOT)
    if args.store:
        import store
        store.android(apk[:-len(".apk")] + ".aab", android_app.app_id(art["name"]), args.build_number, args.dry_run)
    return apk


def ship_ios(art, args):
    """The signed app bundle: zipped for the Simulator, an .ipa for devices."""
    import ios_app
    if not MACOS:
        sys.exit("ship: --target ios builds on a Mac (Xcode)")
    if not args.no_build:
        build_mobile(art, args)
    out = ios_app.package(art, args, ROOT)
    if args.store:
        import store
        store.ios(out, args.dry_run)
    return out


def ship(art, args):
    if args.target == "android":
        return ship_android(art, args)
    if args.target == "ios":
        return ship_ios(art, args)
    build_dir = os.path.join(ROOT, "build", args.preset)
    if not args.no_build:
        run(["cmake", "--preset", args.preset])
        run(["cmake", "--build", "--preset", args.preset, "--target", *art["targets"]])

    exe = art["exe"]
    pack = art["pack"]
    module = art["module_file"]
    if module and not os.path.isfile(module) and os.name == "nt":
        module = None  # no game modules on Windows until the export ledger: the game is linked in
    for p in [exe, pack] + ([module] if module else []):
        if not os.path.isfile(p):
            sys.exit(f"ship: missing build output {p} (build the '{args.preset}' preset first)")

    stem = f"{art['name']}-{args.version}-{os_tag()}"
    stage = os.path.join(args.out, stem)
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    files = {art["name"] + EXE: exe, "assets.mce": pack}
    if art["credits"]:
        files["CREDITS.md"] = art["credits"]
    if module:
        files[os.path.basename(module)] = module
    # The engine umbrella shared library, when the build has one (game-module platforms).
    engine = os.path.join(build_dir, "src", "module", MODULE_PREFIX + "mce_engine" + ENGINE_SUFFIX)
    if module and os.path.isfile(engine):
        files[os.path.basename(engine)] = engine
    for name, src in files.items():
        shutil.copy2(src, os.path.join(stage, name))
    binaries = [art["name"] + EXE] + [n for n in files if n.endswith((MODULE_SUFFIX, ENGINE_SUFFIX))]
    symbols = os.path.join(args.out, stem + "-symbols")
    shutil.rmtree(symbols, ignore_errors=True)
    build_ids = {}
    if MACOS:
        for name in binaries:
            bid = macos_bundle.macho_uuid(os.path.join(stage, name))
            if bid:
                build_ids[name] = bid
        macos_bundle.archive_and_strip(stage, binaries, symbols, args.keep_symbols)
    elif os.name != "nt":
        os.chmod(os.path.join(stage, art["name"] + EXE), 0o755)
        for name in binaries:
            bid = elf_build_id(os.path.join(stage, name))
            if bid:
                build_ids[name] = bid
        if not args.keep_symbols:
            archive_symbols_elf(stage, binaries, build_ids, symbols)
            strip = shutil.which("strip")
            if strip:
                for name in binaries:
                    subprocess.run([strip, "--strip-unneeded", os.path.join(stage, name)], check=True)
    else:
        archive_symbols_pdb(files, binaries, symbols)
    check_self_contained(stage)

    commit = git("rev-parse", "HEAD") or "unknown"
    dirty = bool(git("status", "--porcelain", "--untracked-files=no"))
    with open(os.path.join(stage, "README.txt"), "w", newline="\n") as f:
        f.write(f"{art['title']} {args.version}\nBuilt with Mesh Control Engine.\n\n")
        if MACOS:
            f.write(macos_bundle.readme_run_line(art))
        else:
            f.write(f"Run: double-click {art['name'] + EXE} (it needs an OpenGL 4.5 driver).\n")
        f.write(f"Controls: {art['controls']}\n\n")
        f.write("Keep every file of this folder together: the executable loads assets.mce and its libraries from here.\n")
        if art["credits"]:
            f.write("Content credits: CREDITS.md.\n")
    hashes = {name: sha256(os.path.join(stage, name)) for name in sorted(os.listdir(stage))}
    with open(os.path.join(stage, "VERSION.txt"), "w", newline="\n") as f:
        f.write(f"version {args.version}\nengine_commit {commit}{' (dirty)' if dirty else ''}\n")
        f.write(f"preset {args.preset}\nplatform {os_tag()}\nbuilt {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}\n")
        for name, h in hashes.items():
            f.write(f"sha256 {h}  {name}\n")
        for name, bid in sorted(build_ids.items()):
            f.write(f"build_id {bid}  {name}\n")

    if MACOS:
        art = dict(art, exe_staged=macos_bundle.make_app(stage, art, args.version))

    zip_path = os.path.join(args.out, stem + ".zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for root, dirs, names in os.walk(stage):
            dirs.sort()
            for name in sorted(names + [d for d in dirs if os.path.islink(os.path.join(root, d))]):
                full = os.path.join(root, name)
                arc = f"{stem}/{os.path.relpath(full, stage)}"
                if os.path.islink(full):  # bundle symlinks stay links (unzip / Finder restore them)
                    info = zipfile.ZipInfo(arc)
                    info.external_attr = (0o120777 << 16)
                    z.writestr(info, os.readlink(full))
                    continue
                info = zipfile.ZipInfo.from_file(full, arcname=arc)
                info.compress_type = zipfile.ZIP_DEFLATED
                with open(full, "rb") as src:
                    z.writestr(info, src.read())
    size = os.path.getsize(zip_path)
    print(f"ship: {zip_path} ({size / 1024:.0f} KiB, sha256 {sha256(zip_path)[:16]}...)")
    if os.path.isdir(symbols):
        sym_zip = symbols + ".zip"
        with zipfile.ZipFile(sym_zip, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for root, _, names in os.walk(symbols):
                for name in sorted(names):
                    full = os.path.join(root, name)
                    z.write(full, arcname=os.path.join(stem + "-symbols", os.path.relpath(full, symbols)))
        print(f"ship: {sym_zip} (symbols, keep it; never ship it)")
    if args.smoke:
        smoke(stage, art, args)
    return zip_path


def elf_build_id(path):
    """The GNU build id of an ELF file (readelf -n), or None."""
    readelf = shutil.which("readelf")
    if not readelf:
        return None
    out = subprocess.run([readelf, "-n", path], capture_output=True, text=True).stdout
    m = re.search(r"Build ID: ([0-9a-f]+)", out)
    return m.group(1) if m else None


def archive_symbols_elf(stage, binaries, build_ids, symbols):
    """Linux: the debug info of each shipped binary, as .build-id/ab/cdef....debug (gdb's layout)."""
    objcopy = shutil.which("objcopy")
    if not objcopy:
        print("ship: objcopy not found, no symbol archive", flush=True)
        return
    for name in binaries:
        bid = build_ids.get(name)
        if not bid:
            print(f"ship: {name} has no build id, its symbols are not archived", flush=True)
            continue
        dest = os.path.join(symbols, ".build-id", bid[:2], bid[2:] + ".debug")
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        subprocess.run([objcopy, "--only-keep-debug", os.path.join(stage, name), dest], check=True)
    with open(os.path.join(symbols, "INDEX.txt"), "w", newline="\n") as f:
        for name, bid in sorted(build_ids.items()):
            f.write(f"{bid}  {name}\n")


def archive_symbols_pdb(files, binaries, symbols):
    """Windows: the PDB next to each shipped binary's build output, renamed with it."""
    for name in binaries:
        pdb = os.path.splitext(files.get(name, ""))[0] + ".pdb"
        if os.path.isfile(pdb):
            os.makedirs(symbols, exist_ok=True)
            shutil.copy2(pdb, os.path.join(symbols, os.path.basename(pdb)))


def check_self_contained(stage):
    """Linux: every non-system library a shipped binary needs must resolve inside the folder."""
    if platform.system() != "Linux" or not shutil.which("ldd"):
        return
    system = ("/lib/", "/lib64/", "/usr/lib/", "/usr/lib64/")
    for name in sorted(os.listdir(stage)):
        path = os.path.join(stage, name)
        with open(path, "rb") as f:
            if f.read(4) != b"\x7fELF":
                continue
        out = subprocess.run(["ldd", path], capture_output=True, text=True).stdout
        for line in out.splitlines():
            if "=>" not in line:
                continue
            lib, _, where = line.partition("=>")
            where = where.strip().split(" (")[0]
            if where == "not found":
                sys.exit(f"ship: {name} needs {lib.strip()}, which is not in the folder")
            if not where.startswith(system) and os.path.dirname(os.path.realpath(where)) != os.path.realpath(stage):
                sys.exit(f"ship: {name} loads {lib.strip()} from {where}, outside the shipped folder")


def smoke(stage, art, args):
    """Runs the staged artifact from a copy outside the build tree: proves it finds its own pack."""
    exe = art.get("exe_staged") or os.path.join(stage, art["name"] + EXE)
    cmd = [exe, "-frames", "60", "-r.vsync", "0", "-platform.error_box", "0"]
    if art["autoplay"]:
        cmd += ["-autoplay", "1"]
    if args.smoke_headless:
        cmd = [exe, "-headless", "1", "-ticks", "300"]
    print("+ " + " ".join(cmd), flush=True)
    out = subprocess.run(cmd, cwd=stage, capture_output=True, text=True, timeout=300)
    sys.stdout.write(out.stdout)
    if out.returncode != 0:
        sys.stderr.write(out.stderr)
        sys.exit(f"ship: smoke run failed with exit code {out.returncode}")
    mounted = [l for l in out.stderr.splitlines() + out.stdout.splitlines() if "mounted" in l or "SFX pack" in l]
    if any("project" + os.sep + "assets.mce" in l for l in mounted):
        sys.exit("ship: the staged build mounted the build tree's pack, not its own")
    print("ship: smoke run OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("artifact", nargs="?", choices=[*ARTIFACTS, "all"])
    ap.add_argument("--project", help="ship a project directory made by scripts/new_project.py instead")
    ap.add_argument("--target", choices=["desktop", "android", "ios"], default="desktop",
                    help="android: an APK for arm64 phones (M-MOBILE; needs the Android SDK/NDK and JDK 17); "
                         "ios: an app for the Simulator, or with --preset ios-arm64 a signed .ipa (needs Xcode)")
    ap.add_argument("--preset", default=None,
                    help="default: the platform's release preset (android: android-arm64-release, ios: ios-sim-arm64)")
    ap.add_argument("--host-preset", default=default_preset(), help="--target android/ios: the host build whose mce_cook cooks the pack")
    ap.add_argument("--aab", action="store_true", help="--target android: also build the Play Store bundle (.aab)")
    ap.add_argument("--version", default=None, help="default: <engine version>+<short commit> (the engine's CMake project VERSION)")
    ap.add_argument("--build-number", type=int, default=None,
                    help="android/ios: the store build number (versionCode, CFBundleVersion); default: the commit count of HEAD")
    ap.add_argument("--store", action="store_true",
                    help="android/ios: upload after packaging (Play internal track / TestFlight; credentials from the "
                         "environment, see cmake/mobile/store.py)")
    ap.add_argument("--dry-run", action="store_true", help="with --store: validate the package, upload nothing")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--no-build", action="store_true", help="package the existing build outputs")
    ap.add_argument("--keep-symbols", action="store_true", help="do not strip (Linux)")
    ap.add_argument("--smoke", action="store_true", help="run the staged executable once after packaging")
    ap.add_argument("--smoke-headless", action="store_true", help="with --smoke: headless run (no display needed)")
    args = ap.parse_args()
    if args.preset is None:
        args.preset = {"android": "android-arm64-release", "ios": "ios-sim-arm64"}.get(args.target, default_preset())
    if (args.store or args.dry_run) and args.target == "desktop":
        ap.error("--store and --dry-run apply to --target android/ios")
    if args.dry_run and not args.store:
        ap.error("--dry-run goes with --store")
    if args.store and args.target == "android":
        args.aab = True
    if args.build_number is None:
        count = git("rev-list", "--count", "HEAD")
        args.build_number = max(1, int(count)) if count.isdigit() else 1
    if args.target != "desktop" and args.project:
        ap.error(f"--target {args.target} ships the samples for now (projects: M-MOBILE step 9)")
    if args.version is None:
        base = "0.0.0"
        with open(os.path.join(ROOT, "CMakeLists.txt")) as f:
            for line in f:
                line = line.strip()
                if line.startswith("VERSION "):
                    base = line.split()[1]
                    break
        short = git("rev-parse", "--short=8", "HEAD")
        args.version = f"{base}+{short}" if short else base
    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)
    if args.project:
        ship(project_outputs(args.project, args), args)
        return
    if not args.artifact:
        ap.error("name an artifact or pass --project <dir>")
    for key in (list(ARTIFACTS) if args.artifact == "all" else [args.artifact]):
        ship(sample_outputs(key, args), args)


if __name__ == "__main__":
    main()
