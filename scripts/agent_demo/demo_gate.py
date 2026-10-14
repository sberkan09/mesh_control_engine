#!/usr/bin/env python3
"""The M-AGENT demo games' gates (docs/research/m-agent-demo/), run against a project's MCP server.

    demo_gate.py --mcp <game>_mcp --project projects/<Name> --tests      every .mctest under tests/ passes
    demo_gate.py --mcp <game>_mcp --project projects/<Name> --goldens    each goldens/goldens.json frame matches

goldens/goldens.json lists the frames the agents kept as goldens, with the render_frame arguments
that make each one: [{"golden": "goldens/title.png", "render": {"scene": "scenes/title.mcscene",
"tick": 30}, "max_score": 0.02}]. A missing golden fails the gate (image_diff would adopt it).
Exit code 0 when every selected gate holds.
"""

import argparse
import json
import pathlib
import subprocess
import sys


class Server:
    def __init__(self, exe, project):
        self.p = subprocess.Popen([exe, "--project", str(project)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True)
        self.n = 0
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "demo_gate", "version": "1"}})

    def request(self, method, params):
        self.n += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params}) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError(f"{method}: the server closed its output")
        return json.loads(line)

    def tool(self, name, args):
        r = self.request("tools/call", {"name": name, "arguments": args}).get("result", {})
        text = r.get("content", [{}])[0].get("text", "")
        return (not r.get("isError", False)), r.get("structuredContent"), text

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=60)


def gate_tests(s, problems):
    ok, out, text = s.tool("test_run", {})
    if not ok or out is None:
        problems.append(f"test_run failed: {text[:400]}")
        return
    for r in out.get("results", []):
        name = r.get("file") or r.get("name")
        if r.get("ok"):
            print(f"  pass  {name}")
            continue
        bad = [c.get("check", "?") for c in r.get("checks", []) if not c.get("ok")]
        problems.append(f"{name}: {r.get('error') or ', '.join(bad)}")
    print(f"tests: {out.get('passed', 0)} passed, {out.get('failed', 0)} failed")
    if not out.get("results"):
        problems.append("no .mctest under tests/")


def gate_goldens(s, project, problems):
    manifest = project / "goldens" / "goldens.json"
    if not manifest.is_file():
        problems.append(f"{manifest} is missing")
        return
    for g in json.loads(manifest.read_text(encoding="utf-8")):
        if not (project / g["golden"]).is_file():
            problems.append(f"{g['golden']}: no such golden")
            continue
        args = {"golden": g["golden"], "render": g["render"], "max_score": g.get("max_score", 0.02)}
        ok, out, text = s.tool("image_diff", args)
        if not ok or out is None:
            problems.append(f"{g['golden']}: image_diff failed: {text[:400]}")
            continue
        print(f"  {'pass' if out.get('pass') else 'FAIL'}  {g['golden']} score {out.get('score')}")
        if not out.get("pass"):
            problems.append(f"{g['golden']}: score {out.get('score')} over {args['max_score']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mcp", required=True)
    ap.add_argument("--project", required=True, type=pathlib.Path)
    ap.add_argument("--tests", action="store_true")
    ap.add_argument("--goldens", action="store_true")
    a = ap.parse_args()
    if not (a.tests or a.goldens):
        ap.error("pick --tests and/or --goldens")
    problems = []
    s = Server(a.mcp, a.project.resolve())
    try:
        if a.tests:
            gate_tests(s, problems)
        if a.goldens:
            gate_goldens(s, a.project.resolve(), problems)
    finally:
        s.close()
    for p in problems:
        print("FAIL " + p)
    print("gate: " + ("FAIL" if problems else "pass"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
