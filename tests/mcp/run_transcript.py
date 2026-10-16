#!/usr/bin/env python3
"""MCP transcript test (editor-mcp.md §6.2): drives the real mce_mcp binary over pipes.

For every transcripts/<name>.jsonl (one client message per line) it copies samples/scene_demo into a
fresh project, runs `mce_mcp --project <copy>` with the transcript on stdin, and compares what the
server wrote on stdout with transcripts/<name>.golden.jsonl. Values that legitimately differ between
builds are normalized first: the state hash (checked for shape here, for equality in test_mcp's gate)
and log tails (absolute paths, timings). A tool result's text content must equal its
structuredContent and is replaced by a marker.

A transcripts/<name>.multi.jsonl drives SEVERAL mce_mcp processes on ONE project copy: each line is
{"server": "a", "msg": {...JSON-RPC...}}. One process per distinct server name. Lines are sent strictly
one at a time: write the message and, if it has an "id", read exactly one response line from that
server before the next line (a notification gets none), so the order across processes is fixed. At the
end every stdin is closed and every exit code checked. <name>.multi.golden.jsonl holds
{"server": ..., "response": ...} per request, normalized like the single-server goldens.

  run_transcript.py --exe build/linux-gcc-debug/src/editor/mcp/mce_mcp [--work DIR] [--update]
"""

import argparse
import json
import os
import pathlib
import re
import select
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEMO_ASSETS = ROOT / "samples" / "scene_demo" / "assets"
HASH = re.compile(r"^[0-9a-f]{16}$")


def normalize(value, key=None):
    if isinstance(value, dict):
        return {k: normalize(v, k) for k, v in value.items()}
    if isinstance(value, list):
        if key == "tail":
            return "<log tail>"
        return [normalize(v) for v in value]
    if key == "state_hash" and isinstance(value, str):
        return "<hash>" if HASH.match(value) else "<BAD HASH " + value + ">"
    return value


def normalize_message(line):
    msg = json.loads(line)
    result = msg.get("result")
    if isinstance(result, dict) and "structuredContent" in result:
        text = result["content"][0]["text"]
        if json.loads(text) != result["structuredContent"]:
            raise SystemExit("text content differs from structuredContent: " + line)
        result["content"][0]["text"] = "<structuredContent>"
    return normalize(msg)


REPLY_TIMEOUT = 60.0


def read_line(proc, buf, deadline_s=REPLY_TIMEOUT):
    """One stdout line from proc (bytes buffer kept in buf), or None on EOF / timeout."""
    fd = proc.stdout.fileno()
    while b"\n" not in buf:
        ready, _, _ = select.select([fd], [], [], deadline_s)
        if not ready:
            return None
        chunk = os.read(fd, 65536)
        if not chunk:
            return None
        buf += chunk
    line, _, rest = bytes(buf).partition(b"\n")
    buf[:] = rest
    return line.decode()


def compare(name, got, want):
    ok = True
    for i in range(max(len(got), len(want))):
        g = got[i] if i < len(got) else None
        w = want[i] if i < len(want) else None
        if g != w:
            ok = False
            print(f"{name}: message {i + 1} differs")
            print("  want: " + json.dumps(w, ensure_ascii=False)[:2000])
            print("  got:  " + json.dumps(g, ensure_ascii=False)[:2000])
    print(f"{name}: {'ok' if ok else 'FAILED'} ({len(got)} messages)")
    return ok


def run_multi(exe, transcript, work, update):
    name = transcript.name[: -len(".jsonl")]  # "<x>.multi"
    project = work / name
    if project.exists():
        shutil.rmtree(project)
    shutil.copytree(DEMO_ASSETS, project / "assets")
    steps = [json.loads(l) for l in transcript.read_text(encoding="utf-8").splitlines() if l.strip()]
    procs, bufs, got, failed = {}, {}, [], False
    try:
        for step in steps:
            if step["server"] not in procs:
                procs[step["server"]] = subprocess.Popen(
                    [str(exe), "--project", str(project), "--docs", str(work / "no_docs")],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
                bufs[step["server"]] = bytearray()
        for n, step in enumerate(steps, 1):
            srv, msg = step["server"], step["msg"]
            proc = procs[srv]
            proc.stdin.write((json.dumps(msg) + "\n").encode())
            proc.stdin.flush()
            if "id" not in msg:
                continue
            line = read_line(proc, bufs[srv])
            if line is None:
                print(f"{name}: step {n}: no response from server {srv!r} (exit {proc.poll()})")
                failed = True
                break
            got.append({"server": srv, "response": normalize_message(line)})
    finally:
        for proc in procs.values():
            try:
                proc.stdin.close()
            except OSError:
                pass
        for srv, proc in procs.items():
            try:
                code = proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill()
                code = "timeout"
            if code != 0:
                print(f"{name}: server {srv!r} exited with {code}")
                failed = True
            proc.stdout.close()
    if failed:
        return False
    golden = transcript.with_name(name + ".golden.jsonl")
    if update:
        golden.write_text("".join(json.dumps(m, ensure_ascii=False) + "\n" for m in got), encoding="utf-8")
        print(f"{name}: wrote {golden.relative_to(ROOT)} ({len(got)} messages)")
        return True
    want = [json.loads(l) for l in golden.read_text(encoding="utf-8").splitlines() if l.strip()]
    return compare(name, got, want)


def run_one(exe, transcript, work, update):
    if transcript.name.endswith(".multi.jsonl"):
        return run_multi(exe, transcript, work, update)
    name = transcript.name[: -len(".jsonl")]
    project = work / name
    if project.exists():
        shutil.rmtree(project)
    shutil.copytree(DEMO_ASSETS, project / "assets")
    # An empty docs folder: resources/list stays the same as the shipped docs grow.
    proc = subprocess.run([str(exe), "--project", str(project), "--docs", str(work / "no_docs")], stdin=transcript.open("rb"), capture_output=True,
                          timeout=300)
    if proc.returncode != 0:
        print(proc.stderr.decode(errors="replace")[-4000:])
        print(f"{name}: mce_mcp exited with {proc.returncode}")
        return False
    got = [normalize_message(l) for l in proc.stdout.decode().splitlines() if l.strip()]
    golden = transcript.with_name(name + ".golden.jsonl")
    if update:
        golden.write_text("".join(json.dumps(m, ensure_ascii=False) + "\n" for m in got), encoding="utf-8")
        print(f"{name}: wrote {golden.relative_to(ROOT)} ({len(got)} messages)")
        return True
    want = [json.loads(l) for l in golden.read_text(encoding="utf-8").splitlines() if l.strip()]
    return compare(name, got, want)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True, type=pathlib.Path)
    ap.add_argument("--work", type=pathlib.Path)
    ap.add_argument("--update", action="store_true", help="rewrite the golden files")
    args = ap.parse_args()
    work = args.work or pathlib.Path(tempfile.mkdtemp(prefix="mce_mcp_transcript_"))
    work.mkdir(parents=True, exist_ok=True)
    transcripts = sorted(p for p in (HERE / "transcripts").glob("*.jsonl") if not p.name.endswith(".golden.jsonl"))
    if not transcripts:
        print("no transcripts found")
        return 1
    results = [run_one(args.exe.resolve(), t, work.resolve(), args.update) for t in transcripts]
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
