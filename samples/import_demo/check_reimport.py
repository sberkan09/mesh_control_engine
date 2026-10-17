#!/usr/bin/env python3
"""ctest import_demo_reimport: converting the sample's exports again gives the committed assets, byte for
byte (M-IMPORT gate: a converter is a pure function of its input and keeps GUIDs).

    check_reimport.py <scratch dir> <mce_import.py>
"""

import filecmp
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    scratch, importer = sys.argv[1], sys.argv[2]
    shutil.rmtree(scratch, ignore_errors=True)
    shutil.copytree(HERE, scratch)
    env = dict(os.environ, MCE_IMPORT_SCRIPT=importer)
    r = subprocess.run([sys.executable, os.path.join(scratch, "gen_art.py")], env=env, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        return 1
    bad = []
    for base, _dirs, names in os.walk(os.path.join(HERE, "assets")):
        for n in names:
            a = os.path.join(base, n)
            b = os.path.join(scratch, os.path.relpath(a, HERE))
            if not os.path.isfile(b) or not filecmp.cmp(a, b, shallow=False):
                bad.append(os.path.relpath(a, HERE))
    extra = [os.path.relpath(os.path.join(b, n), scratch) for b, _d, ns in os.walk(os.path.join(scratch, "assets")) for n in ns
             if not os.path.isfile(os.path.join(HERE, os.path.relpath(os.path.join(b, n), scratch)))]
    if bad or extra:
        print("re-importing changed the sample's assets:", sorted(bad), "new:", sorted(extra))
        return 1
    print("import_demo: re-import gives the same bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
