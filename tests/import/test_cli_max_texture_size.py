#!/usr/bin/env python3
"""M-CONTENT 1a: `mce cook` / `mce ship` take --max-texture-size N and pass it to mce_cook.

    test_cli_max_texture_size.py <work dir>

The mce_cook command line is captured (mce.run is replaced), so no SDK install or build is needed.
"""

import os
import shutil
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

    check(mce.max_texture_size_flag(None) == [], "no flag, no argument")
    check(mce.max_texture_size_flag(1024) == ["--max-texture-size", "1024"], "a valid size is passed on")
    for ok in (1, 2, 16384):
        check(mce.max_texture_size_flag(ok) == ["--max-texture-size", str(ok)], f"{ok} is valid")
    for bad in (0, -2, 100, 3, 32768):
        msg = exits(mce.max_texture_size_flag, bad)
        check(msg is not None and "power of two from 1 to 16384" in msg, f"{bad} is refused with what to change")

    # mce cook: before or after the project, and plain
    calls.clear()
    check(mce.main(["cook", "--max-texture-size", "512", proj]) == 0, "mce cook --max-texture-size N <project>")
    check(calls and calls[-1][-2:] == ["--max-texture-size", "512"] and calls[-1][0].endswith("mce_cook"),
          "mce cook passes --max-texture-size to mce_cook")
    calls.clear()
    mce.main(["cook", proj, "--max-texture-size", "256", "--adopt"])
    check(calls and "--max-texture-size" in calls[-1] and "256" in calls[-1] and "--adopt" in calls[-1],
          "mce cook <project> --max-texture-size N still passes through")
    calls.clear()
    mce.main(["cook", proj])
    check(calls and "--max-texture-size" not in calls[-1], "no flag, no cap: mce_cook gets none")
    check(exits(mce.main, ["cook", "--max-texture-size", "100", proj]) is not None, "mce cook refuses a bad size before cooking")

    # mce ship (script project): the cap reaches the pack cook
    calls.clear()
    exits(mce.main, ["ship", proj, "--max-texture-size", "512", "--out", os.path.join(work, "out")])  # no player to zip: exits after the cook
    check(calls and calls[0][0].endswith("mce_cook") and "--pack" in calls[0] and calls[0][-2:] == ["--max-texture-size", "512"],
          "mce ship --max-texture-size N cooks the pack with it")
    check(exits(mce.main, ["ship", proj, "--max-texture-size", "1000"]) is not None, "mce ship refuses a bad size")
    calls.clear()
    exits(mce.main, ["ship", proj, "--out", os.path.join(work, "out")])
    check(calls and "--max-texture-size" not in calls[0], "mce ship without the flag cooks as before")

    if failures:
        print(f"{len(failures)} failure(s)")
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
