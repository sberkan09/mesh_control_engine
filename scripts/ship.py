#!/usr/bin/env python3
"""One-command ship v1 (ROADMAP M4): build, cook, package and zip a shippable artifact.

    python3 scripts/ship.py diorama              # Artifact B -> dist/MCE-Diorama-<version>-<os>.zip
    python3 scripts/ship.py platformer           # Artifact A -> dist/Ember-Step-<version>-<os>.zip
    python3 scripts/ship.py all --version 0.1.0  # both
    python3 scripts/ship.py --project projects/MyGame   # a project from scripts/new_project.py

What a zip holds (one folder, double-click the executable):
    <Name>[.exe]        the sample executable, renamed (Windows: icon + VERSIONINFO resource)
    <module>            the game module next to it, when the artifact has one
    assets.mce          the cooked shipping pack (mce_cook --pack; CRC-verified once at mount)
    CREDITS.md          content attribution (CC0 Kenney packs)
    VERSION.txt         version, engine commit, build preset, file hashes
    README.txt          how to run it

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
    return "msvc-release" if os.name == "nt" else "linux-gcc-release"


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
    art["autoplay"] = bool(art["module"])
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
        "targets": [game] + ([game + "_game"] if os.name != "nt" else []),
        "exe": os.path.join(out_dir, game + EXE),
        "pack": os.path.join(out_dir, "project", "assets.mce"),
        "module": game,
        "module_file": os.path.join(out_dir, MODULE_PREFIX + game + MODULE_SUFFIX),
        "credits": credits if os.path.isfile(credits) else None,
        "controls": "Esc pauses, ` or F1 opens the developer console.",
        "autoplay": False,
    }


def ship(art, args):
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
    engine = os.path.join(build_dir, "src", "module", MODULE_PREFIX + "mce_engine" + MODULE_SUFFIX)
    if module and os.path.isfile(engine):
        files[os.path.basename(engine)] = engine
    for name, src in files.items():
        shutil.copy2(src, os.path.join(stage, name))
    if os.name != "nt":
        os.chmod(os.path.join(stage, art["name"] + EXE), 0o755)
        if not args.keep_symbols:
            strip = shutil.which("strip")
            if strip:
                subprocess.run([strip, "--strip-unneeded", os.path.join(stage, art["name"] + EXE)], check=True)
                for lib in [n for n in files if n.endswith(MODULE_SUFFIX)]:
                    subprocess.run([strip, "--strip-unneeded", os.path.join(stage, lib)], check=True)
    check_self_contained(stage)

    commit = git("rev-parse", "HEAD") or "unknown"
    dirty = bool(git("status", "--porcelain", "--untracked-files=no"))
    with open(os.path.join(stage, "README.txt"), "w", newline="\n") as f:
        f.write(f"{art['title']} {args.version}\nBuilt with Mesh Control Engine.\n\n")
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

    zip_path = os.path.join(args.out, stem + ".zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in sorted(os.listdir(stage)):
            info = zipfile.ZipInfo.from_file(os.path.join(stage, name), arcname=f"{stem}/{name}")
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(os.path.join(stage, name), "rb") as src:
                z.writestr(info, src.read())
    size = os.path.getsize(zip_path)
    print(f"ship: {zip_path} ({size / 1024:.0f} KiB, sha256 {sha256(zip_path)[:16]}...)")
    if args.smoke:
        smoke(stage, art, args)
    return zip_path


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
    exe = os.path.join(stage, art["name"] + EXE)
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
    ap.add_argument("--preset", default=default_preset())
    ap.add_argument("--version", default=None, help="default: <project version>+<short commit>")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--no-build", action="store_true", help="package the existing build outputs")
    ap.add_argument("--keep-symbols", action="store_true", help="do not strip (Linux)")
    ap.add_argument("--smoke", action="store_true", help="run the staged executable once after packaging")
    ap.add_argument("--smoke-headless", action="store_true", help="with --smoke: headless run (no display needed)")
    args = ap.parse_args()
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
