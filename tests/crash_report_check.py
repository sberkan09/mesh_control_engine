#!/usr/bin/env python3
"""The crash-report chain end to end (packaging v2): a player that crashes on purpose writes a
report, and scripts/symbolicate.py resolves its faulting frame against the build tree.

    crash_report_check.py <player executable> <build dir> <work dir>
"""

import glob
import os
import shutil
import signal
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    exe, build_dir, work = sys.argv[1:4]
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    run = subprocess.run([exe, "-headless", "1", "-crash_test", "1", "-crash_dir", work],
                         capture_output=True, text=True, timeout=120)
    if run.returncode != -signal.SIGSEGV:
        sys.exit(f"expected death by SIGSEGV, got exit code {run.returncode}\n{run.stderr}")
    reports = glob.glob(os.path.join(work, "*.crash"))
    if len(reports) != 1:
        sys.exit(f"expected one crash report in {work}, found {reports}")
    sym = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "symbolicate.py"), reports[0],
                          "--symbols", build_dir], capture_output=True, text=True, timeout=300)
    print(sym.stdout)
    if sym.returncode != 0:
        sys.exit(f"symbolicate failed ({sym.returncode})\n{sym.stderr}")
    for needle in ("#0 mce::crash::trigger_test_crash()", "crash.cpp:", "mce::player::run(", "player.cpp:"):
        if needle not in sym.stdout:
            sys.exit(f"symbolicated report lacks '{needle}'")
    print("crash report chain OK")


if __name__ == "__main__":
    main()
