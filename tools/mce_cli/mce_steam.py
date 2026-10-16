"""Steam depots for a shipped desktop build (M-STEAM step 5): used by scripts/ship.py --steam and
`mce ship --steam`. Installed next to `mce` in the SDK's bin folder.

    steam/
      content/<os>/            the shipped folder as players get it, steam_api beside the executable
      scripts/app_build_<app>.vdf    the app build: every depot whose content/<os> exists
      scripts/depot_build_<depot>.vdf  one per platform depot
      UPLOAD.txt               the steamcmd line that uploads it (nothing here uploads)

Platforms accumulate: shipping on Windows, Linux and macOS into the same output fills content/win64,
content/linux-x64 and content/macos, and the app build lists each. Depot ids default to the Steamworks
convention for a new app: app + 1 for Windows, + 2 for Linux, + 3 for macOS.

Nothing here needs the Steamworks SDK except the steam_api library itself: give it with
--steam-library, or set STEAMWORKS_SDK to the SDK's folder (sdk/redistributable_bin/<platform>/).
No steam_appid.txt is written: Steam starts the game with its app id (the player reads SteamAppId),
and a run outside Steam runs without it.
"""

import os
import shutil
import sys

PLATFORMS = ("win64", "linux-x64", "macos")
LIBRARY = {"win64": "steam_api64.dll", "linux-x64": "libsteam_api.so", "macos": "libsteam_api.dylib"}
SDK_DIR = {"win64": "win64", "linux-x64": "linux64", "macos": "osx"}
DEPOT_OFFSET = {"win64": 1, "linux-x64": 2, "macos": 3}
OS_NAME = {"win64": "windows", "linux-x64": "linux", "macos": "macos"}


def default_depot(app_id, os_tag):
    return app_id + DEPOT_OFFSET[os_tag]


def find_library(os_tag, given=None):
    """The steam_api to ship: the one given, else STEAMWORKS_SDK's redistributable for the platform."""
    if given:
        if not os.path.isfile(given):
            sys.exit(f"steam: no such steam_api library {given}")
        return given
    sdk = os.environ.get("STEAMWORKS_SDK")
    if sdk:
        for base in (sdk, os.path.join(sdk, "sdk")):
            path = os.path.join(base, "redistributable_bin", SDK_DIR[os_tag], LIBRARY[os_tag])
            if os.path.isfile(path):
                return path
    sys.exit(f"steam: the Steam depot needs {LIBRARY[os_tag]} from the Steamworks SDK: pass --steam-library <file> "
             f"or set STEAMWORKS_SDK to the SDK folder")


def _quote(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def _kv(lines, depth, key, value=None):
    pad = "\t" * depth
    if value is None:
        lines.append(f"{pad}{_quote(key)}")
    else:
        lines.append(f"{pad}{_quote(key)}\t\t{_quote(value)}")


def depot_vdf(depot_id, os_tag):
    """One depot: everything under content/<os>, without debug files."""
    lines = []
    _kv(lines, 0, "DepotBuild")
    lines.append("{")
    _kv(lines, 1, "DepotID", depot_id)
    _kv(lines, 1, "ContentRoot", f"../content/{os_tag}/")
    _kv(lines, 1, "FileMapping")
    lines.append("\t{")
    _kv(lines, 2, "LocalPath", "*")
    _kv(lines, 2, "DepotPath", ".")
    _kv(lines, 2, "Recursive", "1")
    lines.append("\t}")
    _kv(lines, 1, "FileExclusion", "*.pdb")
    _kv(lines, 1, "FileExclusion", "*.debug")
    lines.append("}")
    return "\n".join(lines) + "\n"


def app_vdf(app_id, description, depots, branch=""):
    """The app build: each depot id with its depot build script (in the same folder)."""
    lines = []
    _kv(lines, 0, "AppBuild")
    lines.append("{")
    _kv(lines, 1, "AppID", app_id)
    _kv(lines, 1, "Desc", description)
    _kv(lines, 1, "BuildOutput", "../output/")
    _kv(lines, 1, "ContentRoot", "../content/")
    _kv(lines, 1, "SetLive", branch)  # empty: upload only, set live by hand on the Steamworks page
    _kv(lines, 1, "Depots")
    lines.append("\t{")
    for depot_id in sorted(depots):
        _kv(lines, 2, str(depot_id), f"depot_build_{depot_id}.vdf")
    lines.append("\t}")
    lines.append("}")
    return "\n".join(lines) + "\n"


def _executable_dir(content):
    """Where the executable sits: a macOS .app's Contents/MacOS, else the folder itself."""
    for name in sorted(os.listdir(content)):
        macos = os.path.join(content, name, "Contents", "MacOS")
        if name.endswith(".app") and os.path.isdir(macos):
            return macos
    return content


def make_depot(stage, out, app_id, os_tag, library, version, title, depot_id=None, branch=""):
    """Copies the shipped folder `stage` into out/content/<os_tag> with steam_api beside the executable and
    (re)writes the build scripts for every platform present. Returns the app build script's path."""
    if os_tag not in PLATFORMS:
        sys.exit(f"steam: no Steam depot for the platform {os_tag}")
    if app_id <= 0:
        sys.exit("steam: --steam wants the app id from the Steamworks page (a positive number)")
    depot_id = depot_id or default_depot(app_id, os_tag)
    content = os.path.join(out, "content", os_tag)
    shutil.rmtree(content, ignore_errors=True)
    shutil.copytree(stage, content, symlinks=True)
    for stray in ("steam_appid.txt",):
        path = os.path.join(content, stray)
        if os.path.exists(path):
            os.remove(path)
    dest = os.path.join(_executable_dir(content), LIBRARY[os_tag])
    shutil.copy2(library, dest)
    if os_tag != "win64":
        os.chmod(dest, 0o755)

    scripts = os.path.join(out, "scripts")
    os.makedirs(scripts, exist_ok=True)
    depots_file = os.path.join(scripts, "depots.txt")  # os -> depot id, kept across runs on other platforms
    known = {}
    if os.path.isfile(depots_file):
        with open(depots_file, encoding="utf-8") as f:
            for line in f:
                parts = line.split()
                if len(parts) == 2 and parts[0] in PLATFORMS:
                    known[parts[0]] = int(parts[1])
    known[os_tag] = depot_id
    known = {tag: d for tag, d in known.items() if os.path.isdir(os.path.join(out, "content", tag))}
    with open(depots_file, "w", newline="\n", encoding="utf-8") as f:
        for tag in PLATFORMS:
            if tag in known:
                f.write(f"{tag} {known[tag]}\n")
    for name in os.listdir(scripts):
        if name.startswith(("depot_build_", "app_build_")) and name.endswith(".vdf"):
            os.remove(os.path.join(scripts, name))
    for tag, d in known.items():
        with open(os.path.join(scripts, f"depot_build_{d}.vdf"), "w", newline="\n", encoding="utf-8") as f:
            f.write(depot_vdf(d, tag))
    app_path = os.path.join(scripts, f"app_build_{app_id}.vdf")
    with open(app_path, "w", newline="\n", encoding="utf-8") as f:
        f.write(app_vdf(app_id, f"{title} {version}", list(known.values()), branch))
    with open(os.path.join(out, "UPLOAD.txt"), "w", newline="\n", encoding="utf-8") as f:
        f.write(f"{title} {version} for Steam (app {app_id})\n\n")
        for tag in PLATFORMS:
            if tag in known:
                f.write(f"  depot {known[tag]}  content/{tag}  ({OS_NAME[tag]})\n")
        f.write("\nUpload with steamcmd (Steamworks SDK tools/ContentBuilder), signed in as a builder account:\n\n"
                f"  steamcmd +login <account> +run_app_build {os.path.abspath(app_path)} +quit\n\n"
                "On the Steamworks page: the depots above exist and are in a package, the launch option of each\n"
                "platform runs the executable in its depot, and the build is set live on a branch after it uploads.\n")
    print(f"steam: {content} (depot {depot_id}), {app_path}")
    return app_path
