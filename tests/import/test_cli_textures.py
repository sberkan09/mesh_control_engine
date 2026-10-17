#!/usr/bin/env python3
"""M-CONTENT 1b-iv: `mce cook` / `mce ship` take --textures and pass it to mce_cook; web + bc exits; scripts/ship.py
refuses bc for android, ios and web.

    test_cli_textures.py <work dir>

The mce_cook command line is captured (mce.run is replaced), so no SDK install or build is needed.
"""

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "mce_cli"))
import mce  # noqa: E402

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL: " + what)


def exits(fn, *args):
    try:
        fn(*args)
    except SystemExit as e:
        return str(e.code)
    return None


def main(work):
    shutil.rmtree(work, ignore_errors=True)
    proj = os.path.join(work, "game")
    os.makedirs(os.path.join(proj, "assets"))
    with open(os.path.join(proj, "game.mceproj"), "w") as f:
        f.write('{ "name": "Game" }')  # no "games": a script project, which needs no build tree

    calls = []
    mce.tool = lambda name: "/sdk/bin/" + name
    mce.run = lambda cmd, cwd=None, exec_=False: (calls.append([str(c) for c in cmd]) or 0)
    out = os.path.join(work, "out")

    check(mce.textures_flag(None) == [], "no flag, no argument")
    check(mce.textures_flag("bc") == ["--textures", "bc"], "bc is passed on")
    check(mce.textures_flag("rgba8", True) == ["--textures", "rgba8"], "rgba8 is passed on")
    check(exits(mce.textures_flag, "dxt", True) is not None, "an unknown mode exits")
    msg = exits(mce.textures_flag, "astc", True)
    check(msg is not None and "scripts/ship.py" in msg, "mce ship refuses astc and names the phone tool")
    check(mce.textures_flag("astc") == ["--textures", "astc"], "mce cook still passes astc (a phone cook run by hand)")

    # mce cook
    calls.clear()
    check(mce.main(["cook", "--textures", "bc", proj]) == 0, "mce cook --textures bc <project>")
    check(calls and calls[-1][-2:] == ["--textures", "bc"] and calls[-1][0].endswith("mce_cook"),
          "mce cook passes --textures bc to mce_cook")
    calls.clear()
    mce.main(["cook", proj, "--textures", "bc", "--adopt"])
    check(calls and calls[-1].count("--textures") == 1 and "bc" in calls[-1] and "--adopt" in calls[-1],
          "mce cook <project> --textures bc still passes through")
    calls.clear()
    mce.main(["cook", "--max-texture-size", "512", "--textures", "bc", proj])
    check(calls and "--max-texture-size" in calls[-1] and "--textures" in calls[-1], "both options combine")
    calls.clear()
    mce.main(["cook", proj])
    check(calls and "--textures" not in calls[-1], "no flag: mce_cook gets none")

    # mce ship (script project): the mode reaches the pack cook, with the cap when both are given
    calls.clear()
    exits(mce.main, ["ship", proj, "--textures", "bc", "--out", out])  # no player to zip: exits after the cook
    check(calls and calls[0][0].endswith("mce_cook") and "--pack" in calls[0] and calls[0][-2:] == ["--textures", "bc"],
          "mce ship --textures bc cooks the pack with it")
    calls.clear()
    exits(mce.main, ["ship", proj, "--textures", "bc", "--max-texture-size", "512", "--out", out])
    check(calls and "--textures" in calls[0] and "--max-texture-size" in calls[0], "mce ship combines --textures and the cap")
    calls.clear()
    exits(mce.main, ["ship", proj, "--out", out])
    check(calls and "--textures" not in calls[0], "mce ship without the flag cooks as before")
    check(exits(mce.main, ["ship", proj, "--textures", "dxt"]) is not None, "mce ship refuses an unknown mode")
    check(exits(mce.main, ["ship", proj, "--textures", "astc"]) is not None, "mce ship refuses astc")

    # web + bc exits with the reason, before anything is built (script and desktop projects alike)
    calls.clear()
    msg = exits(mce.main, ["ship", proj, "--target", "web", "--textures", "bc"])
    check(msg is not None and "web" in msg and "rgba8" in msg and "every browser" in msg,
          "mce ship --target web --textures bc exits and says to ship rgba8")
    check(not calls, "nothing ran for web + bc")

    # scripts/ship.py: android and ios always cook astc and the web stays rgba8, so all three refuse bc
    for target in ("android", "ios", "web"):
        r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "ship.py"), "--target", target, "--textures", "bc"],
                           capture_output=True, text=True)
        check(r.returncode == 2 and "--textures bc is for desktop packs" in r.stderr, f"ship.py --target {target} refuses bc")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "ship.py"), "--target", "android", "--textures", "astc"],
                       capture_output=True, text=True)
    check(r.returncode == 2 and "invalid choice" in r.stderr, "ship.py takes rgba8|bc only")

    if failures:
        print(f"{len(failures)} failure(s)")
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
