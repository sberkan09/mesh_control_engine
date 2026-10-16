#!/usr/bin/env python3
"""The Steam demo end to end (M-STEAM step 6, SBA-111) against the stand-in steam_api.

1. The stock player plays the demo's pack on Steam: Steam Input with one controller holding Jump,
   the overlay opened and closed by the stand-in (the game holds meanwhile), the scene's rich presence
   sent once, and game services on Steam: an achievement under its Steam name, progress, a score on the
   leaderboard, a Steam Cloud save.
2. Without the overlay, the same ticks with and without Steam end on the same state hash: Steam Input,
   presence and game services never change the simulation.
3. The build laid out as a Steam depot (tools/mce_cli/mce_steam.py, what ship.py --steam does), with the
   stand-in as steam_api, started the way Steam starts it (SteamAppId, no Steam options) signs in.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "mce_cli"))
import mce_steam  # noqa: E402

TICKS = "120"
failures = []


def run(cmd, env=None, cwd=None):
    out = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True, timeout=300)
    log = out.stdout + out.stderr
    if out.returncode != 0:
        print(log)
        sys.exit(f"exit {out.returncode}: {' '.join(cmd)}")
    return log


def expect(log, pattern, what):
    if not re.search(pattern, log, re.S):
        failures.append(what)
        print(f"FAIL: {what} (no match for {pattern!r})")


def state_hash(log):
    m = re.search(r"state_hash ([0-9a-f]+)", log)
    return m.group(1) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--player", required=True)
    ap.add_argument("--engine", required=True)
    ap.add_argument("--stub", required=True)
    ap.add_argument("--pack", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(a.work)
    base = [a.player, "-pack", a.pack, "-headless", "1", "-ticks", TICKS, "-platform.error_box", "0"]
    env = {k: v for k, v in os.environ.items() if not k.startswith(("MCE_STEAM_STUB", "SteamAppId", "STEAMWORKS"))}

    # 1. Everything on Steam in one run.
    steam_env = dict(env, MCE_STEAM_STUB_BOARDS="Best Time", MCE_STEAM_STUB_PADS="1", MCE_STEAM_STUB_INPUT="Jump")
    log = run(base + ["-save_dir", os.path.join(a.work, "save_steam"), "-liveops", "1",
                      "-steam.app_id", "480", "-steam.library", a.stub,
                      "-game_services_test", "2:unlock:first_win 2:progress:win_5:3 3:score:best_time:4200 "
                                             "3:write:progress:level2 3:cloud_save:progress"], env=steam_env)
    expect(log, r"Steam on \(app 480, Stub Player\)", "Steam signs in")
    expect(log, r"Steam Input on \(\d+ actions", "Steam Input starts with the input map's actions")
    expect(log, r"steam stub: input Jump held", "the controller's Jump is read")
    expect(log, r"rich presence status=Bouncing in the Steam demo", "the scene's rich presence")
    expect(log, r"rich presence level=Steam Demo", "the presence substitution")
    expect(log, r"game services on steam", "Steam is the game services backend")
    expect(log, r"AchievementSent first_win value 1", "the achievement (as ACH_WIN_ONE_GAME)")
    expect(log, r"AchievementSent win_5 value 3", "achievement progress")
    expect(log, r"ScoreSent best_time value 4200", "the score on 'Best Time'")
    expect(log, r"CloudSaved progress value 1", "the Steam Cloud save")
    if len(re.findall(r"rich presence status=", log)) != 1:
        failures.append("the presence is sent once")
    with_steam = state_hash(log)

    # The overlay: open on frame 20, closed on 40; the game holds at the tick it was on, paused.
    log = run(base + ["-save_dir", os.path.join(a.work, "save_overlay"), "-steam.app_id", "480", "-steam.library", a.stub],
              env=dict(env, MCE_STEAM_STUB_OVERLAY="20,40"))
    expect(log, r"Steam overlay open: the game holds.*Steam overlay closed", "the overlay holds the game")
    expect(log, r"mce_player: (1\d|20) ticks", "the sim stopped when the overlay opened")

    # 2. The simulation does not know about Steam.
    log = run(base + ["-save_dir", os.path.join(a.work, "save_plain")], env=env)
    plain = state_hash(log)
    if not with_steam or with_steam != plain:
        failures.append(f"state hash with Steam {with_steam} != without {plain}")

    # 3. The depot, started like Steam starts it.
    stage = os.path.join(a.work, "stage")
    os.makedirs(stage)
    shutil.copy2(a.player, os.path.join(stage, "SteamDemo"))
    if a.engine != "-":
        shutil.copy2(a.engine, stage)
    shutil.copy2(a.pack, os.path.join(stage, "assets.mce"))
    out = os.path.join(a.work, "SteamDemo-steam")
    mce_steam.make_depot(stage, out, 480, "linux-x64", a.stub, "1.0.0", "Steam Demo")
    exe = os.path.join(out, "content", "linux-x64", "SteamDemo")
    log = run([exe, "-pack", "assets.mce", "-headless", "1", "-ticks", TICKS, "-platform.error_box", "0",
               "-save_dir", os.path.join(a.work, "save_depot")], env=dict(env, SteamAppId="480"), cwd=os.path.dirname(exe))
    expect(log, r"Steam on \(app 480, Stub Player\)", "the depot's game signs in from SteamAppId")
    if state_hash(log) != plain:
        failures.append("the depot's run ends on the same state hash")

    if failures:
        sys.exit(f"{len(failures)} check(s) failed: " + "; ".join(failures))
    print(f"steam demo: Steam Input, overlay, presence, achievements, leaderboard, Steam Cloud and the depot; "
          f"state_hash {plain} with and without Steam")


if __name__ == "__main__":
    main()
