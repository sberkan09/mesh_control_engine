#!/usr/bin/env python3
"""Game-module hot-reload stopwatch (ROADMAP M5b "game.dll <= 10 s", ADR-0029 §2).

Measures the code half of the hot-reload matrix the way a developer lives it: a source file of the
game module is saved, the module is rebuilt, and the host loads the fresh image and runs a tick.
The editor cannot host modules itself (it links the engine statically, ADR-0029 §1.7), so the
stopwatch runs on the headless module host, mce_devhost, which reloads at the play boundary through
the same copy-and-load loader the player uses.

    python3 scripts/module_reload_stopwatch.py build/linux-gcc-debug
    python3 scripts/module_reload_stopwatch.py build/linux-gcc-release --target platformer \\
        --source samples/03_platformer/game/module.cpp --rounds 5

Each round: bump the source's mtime, `cmake --build <dir> --target <module>` with ccache disabled
(a real recompile, not a cache hit), then `mce_devhost -module <lib> -ticks 1` until it prints the
module's state hash. Prints every round and the median; exit 0 when the slowest round is within
--budget seconds (default 10).
"""

import argparse
import glob
import os
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_one(build_dir, pattern):
    hits = sorted(glob.glob(os.path.join(build_dir, "**", pattern), recursive=True))
    hits = [h for h in hits if "_loaded_" not in h and os.path.isfile(h)]
    return hits[0] if hits else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build_dir")
    ap.add_argument("--target", default="platformer", help="game module target (default: platformer)")
    ap.add_argument("--source", default="samples/03_platformer/game/module.cpp",
                    help="source file to 'save' each round, relative to the repo root")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--budget", type=float, default=10.0, help="seconds allowed per round")
    args = ap.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    source = os.path.join(ROOT, args.source)
    env = dict(os.environ, CCACHE_DISABLE="1")

    # Warm build: everything the module and the host need, so the rounds time only the edit.
    subprocess.run(["cmake", "--build", build_dir, "--target", args.target, "mce_devhost"], check=True, env=env)
    lib = find_one(build_dir, f"*{args.target}.so") or find_one(build_dir, f"*{args.target}.dylib") \
        or find_one(build_dir, f"{args.target}.dll")
    host = find_one(build_dir, "mce_devhost") or find_one(build_dir, "mce_devhost.exe")
    if not lib or not host:
        print(f"module_reload_stopwatch: could not find the module ({lib}) or mce_devhost ({host})")
        return 2

    times = []
    for i in range(1, args.rounds + 1):
        now = time.time()
        os.utime(source, (now, now))
        t0 = time.perf_counter()
        subprocess.run(["cmake", "--build", build_dir, "--target", args.target], check=True, env=env,
                       stdout=subprocess.DEVNULL)
        t_build = time.perf_counter() - t0
        out = subprocess.run([host, "-module", lib, "-ticks", "1"], capture_output=True, text=True, env=env)
        total = time.perf_counter() - t0
        if out.returncode != 0 or "state_hash" not in out.stdout:
            print(out.stdout + out.stderr)
            print(f"module_reload_stopwatch: round {i}: the host did not load the module")
            return 1
        times.append(total)
        print(f"round {i}: rebuild {t_build:.2f} s, load + tick {total - t_build:.2f} s, total {total:.2f} s")

    worst = max(times)
    print(f"{args.target}: median {statistics.median(times):.2f} s, worst {worst:.2f} s "
          f"(budget {args.budget:.0f} s) -> {'PASS' if worst <= args.budget else 'FAIL'}")
    return 0 if worst <= args.budget else 1


if __name__ == "__main__":
    sys.exit(main())
