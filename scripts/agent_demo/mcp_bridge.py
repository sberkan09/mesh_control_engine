#!/usr/bin/env python3
"""A shell front for one MCP server, for the M-AGENT demo (docs/research/m-agent-demo/).

An agent that can run shell commands but has no MCP client of its own talks to the engine through
this bridge: `serve` starts the server (one per agent, as an MCP client would) and keeps it
running behind a Unix socket; `call` sends one request and prints the answer. Images in a tool
result are written as PNG files and their paths printed, so the agent can open them. Every request
and answer goes to the transcript (JSON lines, images by path).

    mcp_bridge.py serve --sock S --log T.jsonl --images DIR [--name agent] -- mce mcp <project> [--agent a]
    mcp_bridge.py call  --sock S <tool> ['{"json": "arguments"}']
    mcp_bridge.py call  --sock S --method resources/read '{"uri": "mce://docs/index"}'
    mcp_bridge.py stop  --sock S
"""

import argparse
import base64
import json
import os
import socket
import subprocess
import sys
import threading
import time


def recv_all(conn):
    chunks = []
    while True:
        b = conn.recv(65536)
        if not b:
            break
        chunks.append(b)
    return b"".join(chunks)


def serve(args):
    proc = subprocess.Popen(args.server, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(args.log + ".stderr", "ab"))
    lock = threading.Lock()
    log = open(args.log, "a", encoding="utf-8")
    os.makedirs(args.images, exist_ok=True)
    state = {"id": 0, "image": 0}

    def rpc(method, params, notify=False):
        msg = {"jsonrpc": "2.0", "method": method, "params": params}
        if not notify:
            state["id"] += 1
            msg["id"] = state["id"]
        proc.stdin.write((json.dumps(msg) + "\n").encode())
        proc.stdin.flush()
        if notify:
            return None
        while True:
            line = proc.stdout.readline()
            if not line:
                return {"error": {"message": "the server exited"}}
            r = json.loads(line)
            if r.get("id") == msg["id"]:
                return r

    rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": args.name, "version": "1"}})
    rpc("notifications/initialized", {}, notify=True)

    if os.path.exists(args.sock):
        os.unlink(args.sock)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(args.sock)
    srv.listen(4)
    print(f"bridge: serving {' '.join(args.server)} on {args.sock}", flush=True)
    while True:
        conn, _ = srv.accept()
        with conn:
            req = json.loads(recv_all(conn).decode())
            if req.get("stop"):
                conn.sendall(b"stopped\n")
                break
            with lock:
                started = time.time()
                r = rpc(req["method"], req["params"])
                images = []
                result = r.get("result", {})
                for item in result.get("content", []) if isinstance(result, dict) else []:
                    if item.get("type") == "image":
                        state["image"] += 1
                        path = os.path.join(args.images, f"{state['image']:04d}.png")
                        with open(path, "wb") as f:
                            f.write(base64.b64decode(item["data"]))
                        item.pop("data")
                        item["path"] = os.path.abspath(path)
                        images.append(item["path"])
                entry = {"time": round(started, 3), "ms": int((time.time() - started) * 1000), "agent": args.name,
                         "method": req["method"], "params": req["params"], "response": r}
                log.write(json.dumps(entry) + "\n")
                log.flush()
            conn.sendall(json.dumps({"response": r, "images": images}).encode())
    proc.stdin.close()
    proc.wait(timeout=10)
    os.unlink(args.sock)
    return 0


def send(sock_path, payload):
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.connect(sock_path)
    c.sendall(json.dumps(payload).encode())
    c.shutdown(socket.SHUT_WR)
    data = recv_all(c)
    c.close()
    return data.decode()


def call(args):
    if args.method == "tools/call":
        payload = {"method": "tools/call", "params": {"name": args.target, "arguments": json.loads(args.json or "{}")}}
    else:  # the one argument is the params object
        payload = {"method": args.method, "params": json.loads(args.target or "{}")}
    out = json.loads(send(args.sock, payload))
    r = out["response"]
    if "error" in r:
        print(f"error: {r['error'].get('message')}")
        return 1
    res = r["result"]
    if args.method == "tools/call":
        text = res["content"][0]["text"] if res.get("content") else ""
        print(("TOOL ERROR: " if res.get("isError") else "") + text)
        for p in out["images"]:
            print(f"image: {p}")
        return 1 if res.get("isError") else 0
    if args.method == "resources/read":
        for c in res.get("contents", []):
            print(c.get("text", ""))
        return 0
    if args.method == "prompts/get":
        for m in res.get("messages", []):
            print(m.get("content", {}).get("text", ""))
        return 0
    print(json.dumps(res, indent=1))
    return 0


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve")
    s.add_argument("--sock", required=True)
    s.add_argument("--log", required=True)
    s.add_argument("--images", required=True)
    s.add_argument("--name", default="agent")
    s.add_argument("server", nargs=argparse.REMAINDER)
    s = sub.add_parser("call")
    s.add_argument("--sock", required=True)
    s.add_argument("--method", default="tools/call")
    s.add_argument("target", nargs="?", default="")
    s.add_argument("json", nargs="?", default="")
    s = sub.add_parser("stop")
    s.add_argument("--sock", required=True)
    args = ap.parse_args(argv)
    if args.cmd == "serve":
        if args.server and args.server[0] == "--":
            args.server = args.server[1:]
        return serve(args)
    if args.cmd == "stop":
        print(send(args.sock, {"stop": True}).strip())
        return 0
    return call(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
