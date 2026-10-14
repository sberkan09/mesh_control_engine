#!/usr/bin/env python3
"""Launch an installed Simulator app under a StoreKit configuration (mce_sktest.m), printing its console.

    python3 cmake/mobile/ios/storekit_test/run.py org.meshcontrol.sample_shop_demo \\
        samples/shop_demo/ship/ShopDemo.storekit --reset --no-dialogs -- -monetize_test "30:query 60:buy:gems_100"

--device picks the Simulator (default: the booted one; name it when several are booted); --seconds stops the app after that long. The
app must already be installed (scripts/ship.py shop --target ios, then xcrun simctl install).
"""

import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def build_library(out_dir):
    lib = os.path.join(out_dir, "mce_sktest.dylib")
    subprocess.run(["xcrun", "-sdk", "iphonesimulator", "clang", "-target", "arm64-apple-ios16.0-simulator", "-dynamiclib",
                    "-fobjc-arc", "-framework", "Foundation", "-o", lib, os.path.join(HERE, "mce_sktest.m")], check=True)
    subprocess.run(["codesign", "-f", "-s", "-", lib], check=True, capture_output=True)
    return lib


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bundle_id")
    ap.add_argument("config", help="the .storekit file")
    ap.add_argument("--device", default="booted")
    ap.add_argument("--reset", action="store_true", help="forget earlier test transactions (a fresh install)")
    ap.add_argument("--no-dialogs", action="store_true", help="purchases succeed without the purchase sheet")
    ap.add_argument("--ask-to-buy", action="store_true", help="purchases wait for approval")
    ap.add_argument("--interrupt", action="store_true", help="purchases are interrupted")
    ap.add_argument("--seconds", type=float, default=30)
    argv = sys.argv[1:]
    split = argv.index("--") if "--" in argv else len(argv)
    args = ap.parse_args(argv[:split])
    app_args = argv[split + 1:]
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ,
                   SIMCTL_CHILD_DYLD_INSERT_LIBRARIES=build_library(tmp),
                   SIMCTL_CHILD_MCE_SKTEST_CONFIG=os.path.abspath(args.config),
                   SIMCTL_CHILD_MCE_SKTEST_RESET="1" if args.reset else "0",
                   SIMCTL_CHILD_MCE_SKTEST_DIALOGS="0" if args.no_dialogs else "1",
                   SIMCTL_CHILD_MCE_SKTEST_ASK_TO_BUY="1" if args.ask_to_buy else "0",
                   SIMCTL_CHILD_MCE_SKTEST_INTERRUPT="1" if args.interrupt else "0",
                   SIMCTL_CHILD_MTL_DEBUG_LAYER="0")
        cmd = ["xcrun", "simctl", "launch", "--console-pty", "--terminate-running-process", args.device, args.bundle_id,
               *app_args]
        print("+ " + " ".join(cmd), flush=True)
        try:
            subprocess.run(cmd, env=env, timeout=args.seconds)
        except subprocess.TimeoutExpired:
            subprocess.run(["xcrun", "simctl", "terminate", args.device, args.bundle_id])
    return 0


if __name__ == "__main__":
    sys.exit(main())
