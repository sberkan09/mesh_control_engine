#!/usr/bin/env python3
"""M-STEAM step 5: the Steam depot layout and steamcmd build scripts (tools/mce_cli/mce_steam.py), and a
shipped player started the way Steam starts it: from its depot folder, with SteamAppId set and no command
line of ours, it finds steam_api beside itself (the stand-in here) and signs in.

    test_steam_depot.py <work dir> [<mce_player> <libmce_engine or -> <steam stub> <pack>]
"""

import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "mce_cli"))
import mce_steam  # noqa: E402

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL: " + what)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def braces_balance(text):
    depth = 0
    for ch in text:
        depth += {"{": 1, "}": -1}.get(ch, 0)
        if depth < 0:
            return False
    return depth == 0


def stage_folder(work, name, files):
    stage = os.path.join(work, name)
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    for rel, text in files.items():
        path = os.path.join(stage, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
    return stage


def layout(work):
    lib = os.path.join(work, "fake_steam_api")
    with open(lib, "w") as f:
        f.write("steam")
    out = os.path.join(work, "Game-steam")
    shutil.rmtree(out, ignore_errors=True)

    linux = stage_folder(work, "linux", {"Game": "elf", "assets.mce": "pack", "steam_appid.txt": "480"})
    app = mce_steam.make_depot(linux, out, 480, "linux-x64", lib, "1.2.0", "The Game")
    content = os.path.join(out, "content", "linux-x64")
    check(os.path.isfile(os.path.join(content, "libsteam_api.so")), "steam_api beside the Linux executable")
    check(os.path.isfile(os.path.join(content, "assets.mce")), "the shipped folder copied")
    check(not os.path.exists(os.path.join(content, "steam_appid.txt")), "no steam_appid.txt in the depot")
    check(os.path.basename(app) == "app_build_480.vdf", "the app build script's name")
    depot = read(os.path.join(out, "scripts", "depot_build_482.vdf"))
    check('"DepotID"\t\t"482"' in depot, "Linux depot id app + 2")
    check('"ContentRoot"\t\t"../content/linux-x64/"' in depot, "the depot's content root")
    check('"FileExclusion"\t\t"*.debug"' in depot, "debug files excluded")
    check(braces_balance(depot), "the depot script parses")
    text = read(app)
    check('"AppID"\t\t"480"' in text and '"Desc"\t\t"The Game 1.2.0"' in text, "app id and description")
    check('"482"\t\t"depot_build_482.vdf"' in text, "the app build names the Linux depot")
    check('"SetLive"\t\t""' in text, "nothing set live by default")
    check(braces_balance(text), "the app build script parses")

    # Windows next, into the same output: both depots, a chosen depot id, a branch.
    win = stage_folder(work, "win", {"Game.exe": "pe", "assets.mce": "pack"})
    app = mce_steam.make_depot(win, out, 480, "win64", lib, "1.2.1", "The Game", depot_id=900, branch="beta")
    check(os.path.isfile(os.path.join(out, "content", "win64", "steam_api64.dll")), "steam_api64.dll beside the .exe")
    text = read(app)
    check('"482"\t\t"depot_build_482.vdf"' in text and '"900"\t\t"depot_build_900.vdf"' in text, "both depots listed")
    check('"SetLive"\t\t"beta"' in text, "the branch")
    upload = read(os.path.join(out, "UPLOAD.txt"))
    check("+run_app_build" in upload and "depot 900  content/win64" in upload, "the steamcmd line and the depots")

    # macOS: inside the bundle, next to the executable.
    mac = stage_folder(work, "mac", {"The Game.app/Contents/MacOS/The Game": "macho", "README.txt": "x"})
    mce_steam.make_depot(mac, out, 480, "macos", lib, "1.2.1", "The Game")
    check(os.path.isfile(os.path.join(out, "content", "macos", "The Game.app", "Contents", "MacOS", "libsteam_api.dylib")),
          "libsteam_api.dylib in the .app's MacOS folder")
    check(os.path.isfile(os.path.join(out, "scripts", "depot_build_483.vdf")), "macOS depot app + 3")

    # Shipping again on one platform keeps the others' depots.
    mce_steam.make_depot(linux, out, 480, "linux-x64", lib, "1.2.2", "The Game")
    text = read(os.path.join(out, "scripts", "app_build_480.vdf"))
    check(text.count("depot_build_") == 3, "the three platforms stay listed")

    # The library: given, from STEAMWORKS_SDK, or a clear error.
    sdk = os.path.join(work, "sdk")
    os.makedirs(os.path.join(sdk, "redistributable_bin", "linux64"), exist_ok=True)
    shutil.copy(lib, os.path.join(sdk, "redistributable_bin", "linux64", "libsteam_api.so"))
    os.environ["STEAMWORKS_SDK"] = sdk
    check(mce_steam.find_library("linux-x64").endswith(os.path.join("linux64", "libsteam_api.so")), "the SDK's library")
    del os.environ["STEAMWORKS_SDK"]
    try:
        mce_steam.find_library("linux-x64")
        check(False, "no library is an error")
    except SystemExit as e:
        check("STEAMWORKS_SDK" in str(e), "the error names STEAMWORKS_SDK")
    try:
        mce_steam.make_depot(linux, out, 0, "linux-x64", lib, "1", "x")
        check(False, "app id 0 is an error")
    except SystemExit:
        pass


def player_from_depot(work, player, engine, stub, pack):
    """The stock player in a depot folder, started like Steam starts it."""
    stage = os.path.join(work, "player_stage")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    shutil.copy2(player, os.path.join(stage, "Game"))
    if engine != "-":
        shutil.copy2(engine, os.path.join(stage, os.path.basename(engine)))
    shutil.copy2(pack, os.path.join(stage, "assets.mce"))
    out = os.path.join(work, "Player-steam")
    shutil.rmtree(out, ignore_errors=True)
    mce_steam.make_depot(stage, out, 480, "linux-x64", stub, "1.0", "Player")
    exe = os.path.join(out, "content", "linux-x64", "Game")
    env = dict(os.environ, SteamAppId="480")
    run = subprocess.run([exe, "-pack", "assets.mce", "-headless", "1", "-ticks", "5", "-platform.error_box", "0",
                          "-save_dir", os.path.join(work, "save")],
                         cwd=os.path.dirname(exe), env=env, capture_output=True, text=True, timeout=120)
    log = run.stdout + run.stderr
    check(run.returncode == 0, f"the depot's player exits 0 ({run.returncode})")
    check(re.search(r"Steam on \(app 480, Stub Player\)", log) is not None, "Steam found beside the executable, app from SteamAppId")
    if failures:
        print(log)
    # Without SteamAppId (a run outside Steam): no Steam, and the game still runs.
    env.pop("SteamAppId")
    run = subprocess.run([exe, "-pack", "assets.mce", "-headless", "1", "-ticks", "5", "-platform.error_box", "0",
                          "-save_dir", os.path.join(work, "save")],
                         cwd=os.path.dirname(exe), env=env, capture_output=True, text=True, timeout=120)
    check(run.returncode == 0 and "Steam on" not in run.stdout + run.stderr, "outside Steam: no Steam, still runs")


def main():
    work = os.path.abspath(sys.argv[1])
    os.makedirs(work, exist_ok=True)
    os.environ.pop("STEAMWORKS_SDK", None)
    layout(work)
    if len(sys.argv) == 6 and sys.platform.startswith("linux"):
        player_from_depot(work, *sys.argv[2:6])
    if failures:
        sys.exit(f"{len(failures)} check(s) failed")
    print("steam depot: layout, build scripts and a player started like Steam starts it")


if __name__ == "__main__":
    main()
