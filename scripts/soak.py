#!/usr/bin/env python3
"""Soak test (ROADMAP M4: "10-minute soaks clean"): runs a shipped artifact for N minutes, samples
its memory, then asks it to quit the way the OS would and checks it leaves cleanly.

    python3 scripts/soak.py dist/MCE-Diorama-<ver>-linux-x64/MCE-Diorama --minutes 10
    python3 scripts/soak.py dist/Ember-Step-<ver>-linux-x64/Ember-Step --minutes 10 -- -autoplay 1

Pass criteria (all must hold):
  * the process is alive for the whole soak (no crash, no early exit);
  * a SIGINT (Linux) / CTRL_BREAK (Windows) quit request ends it with exit code 0 within 20 s;
  * its final report says "0 GL errors";
  * resident memory does not creep: the median RSS of the last minute is within
    --rss-growth-pct (default 5 %) + 8 MiB of the median of the first minute after warm-up.
Linux reads RSS from /proc; on other platforms the memory check is skipped (and says so).
Writes a CSV of the RSS samples next to the executable unless --csv is given. Exit 0 = clean.
"""

import argparse
import os
import signal
import statistics
import subprocess
import sys
import time


def rss_kib(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        return None
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("--minutes", type=float, default=10.0)
    ap.add_argument("--interval", type=float, default=2.0, help="seconds between RSS samples")
    ap.add_argument("--warmup", type=float, default=30.0, help="seconds ignored before the baseline minute")
    ap.add_argument("--rss-growth-pct", type=float, default=5.0)
    ap.add_argument("--csv", default=None)
    ap.add_argument("args", nargs="*", help="extra arguments for the executable (after --)")
    a = ap.parse_args()

    exe = os.path.abspath(a.exe)
    cmd = [exe, "-platform.error_box", "0", *a.args]
    print("soak: " + " ".join(cmd), flush=True)
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    proc = subprocess.Popen(cmd, cwd=os.path.dirname(exe), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, creationflags=flags)
    start = time.monotonic()
    duration = a.minutes * 60.0
    samples = []  # (seconds, rss KiB)
    failures = []
    while time.monotonic() - start < duration:
        if proc.poll() is not None:
            failures.append(f"exited early with code {proc.returncode} after {time.monotonic() - start:.0f} s")
            break
        r = rss_kib(proc.pid)
        if r is not None:
            samples.append((time.monotonic() - start, r))
        time.sleep(a.interval)

    if proc.poll() is None:
        proc.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
            failures.append("did not quit within 20 s of the quit request")
    out = proc.stdout.read() if proc.stdout else ""
    if proc.returncode not in (0, None) and not failures:
        failures.append(f"exit code {proc.returncode}")
    report = [l for l in out.splitlines() if "GL errors" in l]
    if not report:
        failures.append("no final report line (crash or killed?)")
    elif not any(" 0 GL errors" in l for l in report):
        failures.append("GL validation errors: " + report[-1])

    csv = a.csv or os.path.join(os.path.dirname(exe), "soak_rss.csv")
    with open(csv, "w") as f:
        f.write("seconds,rss_kib\n")
        for t, r in samples:
            f.write(f"{t:.1f},{r}\n")
    if samples:
        base = [r for t, r in samples if a.warmup <= t < a.warmup + 60.0]
        tail = [r for t, r in samples if t >= duration - 60.0]
        if base and tail:
            b, e = statistics.median(base), statistics.median(tail)
            limit = b * (1.0 + a.rss_growth_pct / 100.0) + 8 * 1024
            print(f"soak: RSS median {b / 1024:.1f} MiB (first minute) -> {e / 1024:.1f} MiB (last minute), "
                  f"peak {max(r for _, r in samples) / 1024:.1f} MiB, limit {limit / 1024:.1f} MiB")
            if e > limit:
                failures.append(f"RSS grew from {b / 1024:.1f} to {e / 1024:.1f} MiB")
        else:
            print("soak: too short for the RSS trend check (needs warm-up + 2 minutes)")
    else:
        print("soak: RSS not sampled on this platform")

    for l in report:
        print("soak: " + l)
    print(f"soak: {time.monotonic() - start:.0f} s, exit code {proc.returncode}, samples in {csv}")
    if failures:
        for f in failures:
            print("soak: FAIL: " + f)
        tail_lines = out.splitlines()[-30:]
        sys.stdout.write("\n".join(tail_lines) + "\n")
        return 1
    print("soak: CLEAN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
