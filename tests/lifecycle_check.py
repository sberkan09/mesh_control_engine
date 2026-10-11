#!/usr/bin/env python3
"""FR-RUN-100 (M-MOBILE): a trip to the background and back pauses and resumes the sim with an
unchanged state hash, and writes the save slot inside the background callback.

    lifecycle_check.py <player executable> <pack> <work dir>

A headless run of 600 app ticks that spends ticks 200..259 in the background simulates 540 ticks;
its World hash must equal a plain 540-tick run's.
"""

import os
import re
import shutil
import subprocess
import sys

HASH = re.compile(r"(\d+) ticks, \d+ entities, state_hash ([0-9a-f]+)")


def run(exe, pack, *args):
    r = subprocess.run([exe, "-headless", "1", "-pack", pack, *args], capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        sys.exit(f"{' '.join(args)}: exit {r.returncode}\n{r.stdout}\n{r.stderr}")
    m = HASH.search(r.stdout)
    if not m:
        sys.exit(f"no state hash in the output of {' '.join(args)}:\n{r.stdout}")
    return int(m.group(1)), m.group(2), r.stdout + r.stderr


def main():
    exe, pack, work = sys.argv[1:4]
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    ticks, plain, _ = run(exe, pack, "-ticks", "540")
    if ticks != 540:
        sys.exit(f"plain run simulated {ticks} ticks, expected 540")
    ticks, trip, out = run(exe, pack, "-ticks", "600", "-lifecycle_test", "200,60", "-save", "trip", "-save_dir", work)
    if ticks != 540:
        sys.exit(f"the background trip simulated {ticks} ticks, expected 540 (60 paused)\n{out}")
    if trip != plain:
        sys.exit(f"state hash after the background trip {trip} != plain run {plain}\n{out}")
    # The save is written twice: in the background callback (tick 199 simulated) and on quit.
    saves = re.findall(r"save 'trip' written \(tick (\d+)\)", out)
    if saves != ["199", "540"]:
        sys.exit(f"expected the save written at tick 199 (background) and 540 (quit), got {saves}\n{out}")
    if "in the background" not in out or "back in the foreground" not in out:
        sys.exit(f"lifecycle log lines missing\n{out}")
    r = subprocess.run([exe, "-headless", "1", "-pack", pack, "-lifecycle_test", "0,5"], capture_output=True, text=True,
                       timeout=120)
    if r.returncode == 0:
        sys.exit("-lifecycle_test 0,5 should be rejected")
    print(f"lifecycle round trip: hash {trip} == plain 540-tick run; saves at ticks {saves}")


if __name__ == "__main__":
    main()
