#!/usr/bin/env python3
"""Resolves an MCE crash report (mce/crash.h) to function names and source lines.

    python3 scripts/symbolicate.py <report.crash> [--symbols DIR ...]

Each backtrace frame names its module by load base; the report's module list gives that module's
GNU build id, and the debug file is looked up by build id in each DIR, in order:

    DIR/.build-id/ab/cdef....debug   the symbol archive scripts/ship.py writes beside the zip
    any ELF file under DIR           a build tree (unstripped binaries), matched by build id

The module's own path from the report wins over a directory scan when that file still has the
same build id (a crash on the machine that built it). Frames resolve with addr2line (-f -C -i);
a return address is looked up at offset - 1 so the call site's line is reported. Windows reports carry a .dmp next to
them: open that in Visual Studio or WinDbg with the archived PDBs instead.

Exit code: 0 when the faulting frame resolved, 1 otherwise.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

FRAME_RE = re.compile(r"^#(\d+) (0x[0-9a-f]+)(?: base (0x[0-9a-f]+) \+(0x[0-9a-f]+))?(?: (\S+))?$")
MODULE_RE = re.compile(r"^module (0x[0-9a-f]+)(?: build_id ([0-9a-f]+))? (.+)$")


def elf_build_id(path):
    """The NT_GNU_BUILD_ID of a 64-bit little-endian ELF file, or None."""
    try:
        with open(path, "rb") as f:
            ident = f.read(16)
            if len(ident) < 16 or ident[:4] != b"\x7fELF" or ident[4] != 2 or ident[5] != 1:
                return None
            hdr = f.read(48)
            (_, _, _, _, _, shoff, _, _, _, _, shentsize, shnum, _) = struct.unpack("<HHIQQQIHHHHHH", hdr)
            for i in range(shnum):
                f.seek(shoff + i * shentsize)
                sh = f.read(64)
                if len(sh) < 64:
                    return None
                (_, sh_type, _, _, off, size, _, _, _, _) = struct.unpack("<IIQQQQIIQQ", sh)
                if sh_type != 7:  # SHT_NOTE
                    continue
                f.seek(off)
                data = f.read(size)
                p = 0
                while p + 12 <= len(data):
                    namesz, descsz, ntype = struct.unpack_from("<III", data, p)
                    name = data[p + 12:p + 12 + namesz]
                    desc_at = p + 12 + ((namesz + 3) & ~3)
                    if ntype == 3 and name == b"GNU\0":
                        return data[desc_at:desc_at + descsz].hex()
                    p = desc_at + ((descsz + 3) & ~3)
    except OSError:
        return None
    return None


class SymbolIndex:
    def __init__(self, dirs):
        self.dirs = dirs
        self.scanned = {}  # build id -> path, filled lazily from the plain directories

    def _scan(self):
        if self.scanned or not self.dirs:
            return
        for d in self.dirs:
            for root, subdirs, files in os.walk(d):
                subdirs[:] = [s for s in subdirs if s not in ("_deps", "CMakeFiles", ".git")]
                for name in files:
                    path = os.path.join(root, name)
                    if os.path.islink(path) or os.path.getsize(path) < 64:
                        continue
                    bid = elf_build_id(path)
                    if bid and bid not in self.scanned:
                        self.scanned[bid] = path

    def find(self, build_id, module_path):
        if build_id:
            for d in self.dirs:
                cand = os.path.join(d, ".build-id", build_id[:2], build_id[2:] + ".debug")
                if os.path.isfile(cand):
                    return cand
        if module_path and os.path.isfile(module_path) and (not build_id or elf_build_id(module_path) == build_id):
            return module_path
        if build_id:
            self._scan()
            return self.scanned.get(build_id)
        return None


def addr2line(debug_file, offset):
    out = subprocess.run(["addr2line", "-f", "-C", "-i", "-e", debug_file, hex(offset)],
                         capture_output=True, text=True).stdout.splitlines()
    pairs = []
    for i in range(0, len(out) - 1, 2):
        func, loc = out[i].strip(), out[i + 1].strip()
        if func == "??" and loc.startswith("??"):
            continue
        pairs.append((func, loc))
    return pairs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("report")
    ap.add_argument("--symbols", action="append", default=[], help="symbol archive or build tree (repeatable)")
    args = ap.parse_args()
    if not shutil.which("addr2line"):
        sys.exit("symbolicate: addr2line not found (binutils)")

    with open(args.report, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    modules = {}
    for line in lines:
        m = MODULE_RE.match(line)
        if m:
            modules[int(m.group(1), 16)] = (m.group(2), m.group(3))
    index = SymbolIndex([d for d in args.symbols if os.path.isdir(d)])

    first_resolved = None
    for line in lines:
        m = FRAME_RE.match(line)
        if not m or m.group(3) is None:
            print(line)
            continue
        frame = int(m.group(1))
        base, off = int(m.group(3), 16), int(m.group(4), 16)
        build_id, path = modules.get(base, (None, None))
        debug = index.find(build_id, path)
        where = os.path.basename(path) if path else "?"
        resolved = addr2line(debug, off - 1 if frame > 0 else off) if debug else []
        if not resolved:
            print(f"{line}   [{where}{'' if debug else ': no symbols'}]")
        else:
            func, loc = resolved[-1]
            print(f"#{frame} {func} at {loc}   [{where} +{hex(off)}]")
            for func, loc in reversed(resolved[:-1]):
                print(f"     inlined {func} at {loc}")
        if first_resolved is None:
            first_resolved = bool(resolved)
    return 0 if first_resolved else 1


if __name__ == "__main__":
    sys.exit(main())
