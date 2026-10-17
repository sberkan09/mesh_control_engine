#!/usr/bin/env python3
"""SBA-182: `mce play` / `mce headless` warn when the pack they run is older than the project's sources.

    test_cli_stale_pack.py <work dir>
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


def touch(path, t, text="x"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.utime(path, (t, t))


def main(work):
    shutil.rmtree(work, ignore_errors=True)
    proj = os.path.join(work, "game")
    assets = os.path.join(proj, "assets")
    pack = os.path.join(proj, "build", "project", "assets.mce")
    touch(os.path.join(assets, "scenes", "main.mcscene"), 1000)
    touch(os.path.join(assets, "textures", "a.png.mceasset"), 1000)
    touch(pack, 2000)

    check(mce.stale_pack_warning(pack, assets, proj) is None, "a pack newer than every source is not stale")

    touch(os.path.join(assets, "scenes", "main.mcscene"), 3000)
    msg = mce.stale_pack_warning(pack, assets, proj)
    check(msg is not None, "a scene newer than the pack warns")
    check(msg and "scenes/main.mcscene" in msg, "the warning names the newest source")
    check(msg and f"mce build {proj}" in msg, "the warning names `mce build <project>`")
    check(msg and "cooked/" in msg, "the warning says cook_run / mce cook leave the pack alone")

    # A hidden folder (.mce autosaves) and hidden files are not sources.
    touch(os.path.join(assets, "scenes", "main.mcscene"), 1000)
    touch(os.path.join(assets, ".hidden", "x.mcscene"), 4000)
    check(mce.stale_pack_warning(pack, assets, proj) is None, "hidden folders do not count")

    os.remove(pack)
    check(mce.stale_pack_warning(pack, assets, proj) is None, "a missing pack is the caller's error, not a stale warning")

    if failures:
        print(f"{len(failures)} failure(s)")
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
