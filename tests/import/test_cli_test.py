#!/usr/bin/env python3
"""M-SCRIPT3 9 and 12: `mce test <project> [path]` plays a project's .mctest scenarios without an agent; `mce inspect` prints live components; M-SCRIPTPROF 1: `mce profile`.

    test_cli_test.py <work dir>

A fake `mce_mcp` (a script that answers initialize and test_run from a canned result, chosen by the MODE
environment variable) stands in for the real server, so no SDK install or build is needed; what is tested is
the stdio client, the report and the exit code. The real server was run by hand against projects/Pursuit.
"""

import contextlib
import io
import json
import os
import pathlib
import shutil
import stat
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "mce_cli"))
import mce  # noqa: E402

failures = []

FAKE = r'''#!/usr/bin/env python3
import json, os, sys
mode = os.environ.get("MODE", "pass")
asked = []
for line in sys.stdin:
    req = json.loads(line)
    if req["method"] == "initialize":
        res = {"protocolVersion": "2025-06-18", "capabilities": {}, "serverInfo": {"name": "fake", "version": "1"}}
    elif req["params"]["name"] == "run_headless":
        a = req["params"]["arguments"]
        if a.get("scene") != "scenes/main.mcscene":  # the real tool refuses a run with no saved scene
            res = {"content": [{"type": "text", "text": "the scene was never saved"}], "isError": True}
            sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": req["id"], "result": res}) + "\n")
            sys.stdout.flush()
            continue
        sc = {"state_hash": "00000000deadbeef", "asked": a, "inspect": {"count": 1, "truncated": False, "entities": [
              {"id": 7, "name": "Hero", "alive": True, "components": {"ScriptVars": {"v": [1, 2]}}}]}}
        if a.get("trace"):
            t = a["trace"]
            sc["trace"] = {"every": t["every"], "ticks": [t["every"], 2 * t["every"]], "series": [
                {"entity": f["entity"], "field": f["field"], "values": [0.5, [1.0, 2.0]]} for f in t["fields"]]}
        if a.get("inspect") is None:
            sc["scripts"] = [{"script": "scripts/a.luau", "failed": False, "updates": 300, "peak_steps": 50, "budget_steps": 1000, "avg_steps": 12.5},
                             {"script": "scripts/b.luau", "failed": mode == "scripterr", "updates": 3, "peak_steps": 1000, "budget_steps": 1000, "avg_steps": 900.0}]
        if mode == "scripterr":
            sc["script_errors"] = [{"script": "scripts/x.luau", "error": "x.luau:3: boom"}]
        res = {"content": [{"type": "text", "text": "ok"}], "structuredContent": sc, "isError": False}
    else:
        asked.append(req["params"]["arguments"])
        results = {
            "pass": [{"file": "tests/a.mctest", "ok": True, "checks": []}],
            "fail": [{"file": "tests/a.mctest", "ok": True, "checks": []},
                     {"file": "tests/b.mctest", "ok": False, "checks": [{"check": "coins == 3", "ok": False}],
                      "script_errors": [{"script": "scripts/x.luau", "error": "x.luau:3: boom"}]}],
            "none": [],
        }[mode]
        sc = {"results": results, "passed": sum(1 for r in results if r["ok"]), "failed": sum(1 for r in results if not r["ok"]),
              "asked": asked[-1]}
        res = {"content": [{"type": "text", "text": "ok"}], "structuredContent": sc, "isError": mode == "error"}
    sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": req["id"], "result": res}) + "\n")
    sys.stdout.flush()
'''


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL: " + what)


def run(argv):
    out = io.StringIO()
    code = None
    with contextlib.redirect_stdout(out):
        try:
            code = mce.main(argv)
        except SystemExit as e:
            code = e.code
    return code, out.getvalue()


def main(work):
    shutil.rmtree(work, ignore_errors=True)
    proj = pathlib.Path(work) / "Game"
    (proj / "tests").mkdir(parents=True)
    (proj / "Game.mceproj").write_text(json.dumps({"format": "mce-project", "version": 1, "name": "Game", "games": [], "start_scene": "4a0d"}))
    (proj / "assets" / "scenes").mkdir(parents=True)
    (proj / "assets" / "scenes" / "main.mcscene").write_text('{"guid": "4a0d"}')
    bin_dir = pathlib.Path(work) / "bin"
    bin_dir.mkdir()
    fake = bin_dir / "mce_mcp"
    fake.write_text(FAKE)
    fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
    mce.BIN = bin_dir

    os.environ["MODE"] = "pass"
    code, text = run(["test", str(proj)])
    check(code == 0, "all scenarios pass: exit 0")
    check("pass  tests/a.mctest" in text and "1 passed, 0 failed" in text, "the passing scenario is listed")

    os.environ["MODE"] = "fail"
    code, text = run(["test", str(proj)])
    check(code == 1, "a failing scenario: exit 1")
    check("FAIL  tests/b.mctest: coins == 3" in text, "the failed check is named")
    check("script scripts/x.luau: x.luau:3: boom" in text, "a script error is shown")
    check("1 passed, 1 failed" in text, "the totals are printed")

    os.environ["MODE"] = "none"
    code, text = run(["test", str(proj)])
    check(code == 1 and "no .mctest found" in text, "no scenarios is a failure, not a pass")

    os.environ["MODE"] = "error"
    code, _ = run(["test", str(proj)])
    check(code not in (0, 1) and code is not None, "a tool error exits with a message")

    # M-SCRIPT3 12: `mce inspect` is run_headless's inspect, printed as JSON.
    os.environ["MODE"] = "pass"
    code, text = run(["inspect", str(proj)])
    check(code not in (0, None) and "needs --component" in str(code), "inspect without a filter explains itself")
    code, text = run(["inspect", str(proj), "--component", "ScriptVars", "--entity", "7", "--entity", "Hero", "--show", "ScriptVars,LocalTransform", "--ticks", "50"])
    check(code == 0, "inspect: exit 0")
    doc = json.loads(text)
    check(doc["entities"][0]["components"]["ScriptVars"]["v"] == [1, 2], "inspect prints the live component values as JSON")
    os.environ["MODE"] = "scripterr"
    code, _ = run(["inspect", str(proj), "--component", "ScriptVars"])
    check(code == 3, "a failed script makes inspect exit 3, as mce_player -headless does")

    # M-SCRIPTPROF 4: `--trace` samples fields during the run and prints one row per sampled tick.
    os.environ["MODE"] = "pass"
    code, text = run(["inspect", str(proj), "--trace", "Hero:LocalTransform.position.x", "--trace", "7:ScriptVars.v", "--every", "25", "--ticks", "50"])
    check(code == 0 and "Hero:LocalTransform.position.x" in text and "25" in text and "(1, 2)" in text and "0.5" in text,
          "inspect --trace prints a row per sampled tick")
    check('"entities"' not in text, "inspect --trace alone prints no entity dump")
    code, _ = run(["inspect", str(proj), "--trace", "Hero"])
    check(code not in (0, None), "a --trace without a field explains itself")

    # M-SCRIPTPROF 1: `mce profile` prints the per-script step table.
    os.environ["MODE"] = "pass"
    code, text = run(["profile", str(proj), "--ticks", "300"])
    check(code == 0 and "scripts/a.luau" in text and "5.0%" in text and "12.5" in text, "profile prints each script's steps against its budget")
    os.environ["MODE"] = "scripterr"
    code, text = run(["profile", str(proj)])
    check(code == 3 and "FAILED" in text and "100.0%" in text, "a failed script shows FAILED and exits 3")

    if failures:
        print(f"{len(failures)} check(s) failed")
        return 1
    print("mce test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
