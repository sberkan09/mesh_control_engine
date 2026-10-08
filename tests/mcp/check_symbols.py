#!/usr/bin/env python3
"""Symbol wall for the editor MCP server (editor-mcp.md §6.4): mce_mcp* never references ImGui or a
socket API. stdio is the only transport (N7: no networking in the engine or its tools).

  check_symbols.py --nm nm --no-sockets <libmce_mcp_core.a> --no-imgui <libmce_mcp_core.a> <mce_mcp>

Sockets are checked on the server's own code (mce_mcp_core; main.cpp is covered by the source wall
in scripts/ci/check_walls.py). The linked executable is not socket-checked: the Tracy client that
every development binary carries (MCE_ENABLE_TRACY, compiled out of shipping builds) is the one
socket user there (ADR-0027). Exit code 1 on any hit.
"""

import argparse
import re
import subprocess
import sys

SOCKET = {"socket", "bind", "listen", "accept", "accept4", "connect", "getaddrinfo", "gethostbyname", "recvfrom",
          "sendto", "WSAStartup", "inet_pton", "inet_addr"}
IMGUI = re.compile(r"ImGui|imgui", re.IGNORECASE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--no-sockets", nargs="*", default=[])
    ap.add_argument("--no-imgui", nargs="*", default=[])
    args = ap.parse_args()
    bad = []
    checks = [(f, True, False) for f in args.no_sockets] + [(f, False, True) for f in args.no_imgui]
    for f, sockets, imgui in checks:
        out = subprocess.run([args.nm, "-C", f], capture_output=True, text=True)
        if out.returncode != 0:
            print(out.stderr)
            return 1
        for line in out.stdout.splitlines():
            parts = line.split()
            if not parts:
                continue
            name = parts[-1]
            base = name.split("@")[0]
            if (sockets and base in SOCKET) or (imgui and IMGUI.search(line)):
                bad.append(f"{f}: {line.strip()}")
    if bad:
        print("MCP symbol wall violations:")
        for b in bad[:50]:
            print("  " + b)
        return 1
    print(f"MCP symbol wall holds ({len(checks)} checks).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
