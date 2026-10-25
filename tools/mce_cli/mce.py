#!/usr/bin/env python3
"""mce: the installed engine's command (installed as <sdk>/bin/mce by cmake/MceInstall.cmake).

    mce new <Name> [--game id] [--title "Title"] [--template project|novel|script] [--dest dir]
                                                stamp a project from a template (novel: a visual novel;
                                                script: Luau scripts and data, no C++, no compiler)
    mce example [<Name>] [--dest dir]           copy an example project out of the SDK (no name: list them)
    mce build <project> [--config Debug|Release] [-D<var>=<v>...]  configure (find_package(MCE)) + build
    mce play <project> [player flags...]        run the game (its host) from the build
    mce test <project> [path]                   play the project's .mctest scenarios (no agent needed)
    mce headless <project> [--ticks N] [flags]  run it without a window; prints the state hash
    mce editor <project> [editor flags...]      the editor with the game module loaded
    mce mcp <project> [--allow-cheats]          the MCP server over stdio with the module loaded
    mce mcp-config <project> [--desktop] [--remove]   register that server with Claude: <project>/.mcp.json
                                                (Claude Code, also Claude Desktop's Code tab); --desktop
                                                also adds it to Claude Desktop's own config
    mce cook <project> [cook flags...]          cook assets/ with the module loaded (--pack, --force, --adopt, --only <path>, --skip-errors)
    mce story compile [<project>] [--check] [--werror]   compile story/*.story (story/story.json) into the
                                                game's C++ header (mce/kit/novel.h) and story/REPORT.md
    mce import aseprite <export.json> [--out f.mcsheet] [--werror]   Aseprite's JSON export + PNG into a .mcsheet
                                                (frames, tags as clips, slices; unsupported features warn)
    mce import tiled <map.tmj> [--out-dir d] [--tile-size m] [--werror]   Tiled map into a .mcscene + .mctilemap per layer
    mce import ldtk <project.ldtk> [--level L] [--out-dir d] [--tile-size m] [--werror]   LDtk levels into scenes + tilemaps
    mce import spine|dragonbones <export.json> [--out f.mcskel2d] [--ppm 100] [--atlas a] [--texture png] [--strict]
                                                Spine 3.8/4.x JSON or DragonBones 5.x JSON into a .mcskel2d
    mce ship <project> [--out dir] [--version v]   zip the host, module, engine and pack for distribution
    mce ship <project> --steam <app id> [--steam-library f]   ... and the Steam depot for this OS with the
                                                steamcmd build scripts in <out>/<Name>-steam (uploads nothing)
    mce ship <project> --target web [--smoke] [--smoke-ticks N]   the browser build, zipped for itch.io (runs the engine
                                                checkout's scripts/ship.py: needs the Emscripten SDK)
    mce serve <zip | folder | project> [--port N] [--lan] [--open]   play a web build locally (the right
                                                MIME types and headers; browsers do not run file:// pages)
    mce scene diff <before> <after>             what changed between two .mcscene files (exit 1 when they differ)
    mce perf [<project | file.mcperf>] [--json]   read a perf capture (perf_run's cooked/.perf/last.mcperf, or the player's
                                                -perf_capture file): the frame cost and the draw calls by cause
    mce friction [<project>...] [--out file] [--json]   collect the agents' friction notes (friction.jsonl,
                                                written by the MCP tool report_friction) into one backlog
    mce update [<project>...] [--release [tag]] [--no-pull]  reinstall this SDK from where it came from, then rebuild
                                                the projects named (their modules must match the new engine)
    mce version                                 the SDK's version and engine build stamp
    mce --help

A script project (mce new --template script) has no game module: build cooks assets/ into <project>/assets.mce,
play and headless run it with the stock mce_player, ship zips the player, the engine library and the pack with
a launcher. No C++ toolchain is involved.

A project is a folder with one <Name>.mceproj (mce new makes one, and mce new and mce example
write its .mcp.json). Its build lives in
<project>/build (mce build), where <game> (the host), lib<game>.so (the module) and
project/assets.mce (the cooked pack) land. Every subcommand prints the command it runs (mce mcp on stderr: its stdout is the protocol).
Exit code: the tool's, or 2 for a usage error.
"""

import argparse
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import zipfile

HERE = pathlib.Path(__file__).resolve()
PREFIX = HERE.parents[1]                       # <sdk>/bin/mce -> <sdk>
BIN = PREFIX / "bin"
LIB = PREFIX / "lib"
SHARE = PREFIX / "share" / "mce"
EXE = ".exe" if os.name == "nt" else ""
# Game modules are CMake MODULE libraries (.so on macOS too); the engine is a SHARED one (.dylib there).
MODULE_PREFIX, MODULE_SUFFIX = ("", ".dll") if os.name == "nt" else ("lib", ".so")
ENGINE_SUFFIX = ".dylib" if sys.platform == "darwin" else MODULE_SUFFIX
ENGINE_LIB = f"{MODULE_PREFIX}mce_engine{ENGINE_SUFFIX}"


def tool(name):
    path = BIN / f"{name}{EXE}"
    if not path.is_file():
        sys.exit(f"mce: {path} is missing (is this an SDK install? expected bin/{name} beside bin/mce)")
    return str(path)


def run(cmd, cwd=None, exec_=False):
    # The MCP server owns stdout (JSON-RPC lines), so its command line goes to stderr.
    print("mce:", " ".join(str(c) for c in cmd), file=sys.stderr if exec_ else sys.stdout, flush=True)
    if exec_ and os.name != "nt":
        os.execv(cmd[0], [str(c) for c in cmd])  # stdio passthrough (the MCP server)
    return subprocess.call([str(c) for c in cmd], cwd=cwd)


class Project:
    def __init__(self, path):
        self.dir = pathlib.Path(path).resolve()
        files = sorted(self.dir.glob("*.mceproj"))
        if len(files) != 1:
            sys.exit(f"mce: {self.dir} is not a project directory (expected one <Name>.mceproj)")
        self.meta = json.loads(files[0].read_text(encoding="utf-8"))
        games = self.meta.get("games") or []
        self.name = self.meta.get("name", files[0].stem)
        self.title = self.meta.get("title", self.name)
        # No game module: a script project (Luau scripts and data) plays on the stock player.
        self.script = not games
        self.game = games[0] if games else None
        self.build = self.dir / "build"
        self.start_scene = str(self.meta.get("start_scene") or "").removeprefix("guid:")

    def start_scene_path(self):
        """The path under assets/ of the scene `start_scene` names (the MCP run_headless tool wants a path, not a guid), or None."""
        if not self.start_scene:
            return None
        assets = self.dir / "assets"
        for f in sorted(assets.rglob("*.mcscene")):
            try:
                if self.start_scene in f.read_text(encoding="utf-8", errors="replace"):
                    return f.relative_to(assets).as_posix()
            except OSError:
                pass
        return None

    @property
    def host(self):
        if self.script:
            return pathlib.Path(tool("mce_player"))
        return self.build / f"{self.game}{EXE}"

    @property
    def module(self):
        return self.build / f"{MODULE_PREFIX}{self.game}{MODULE_SUFFIX}"

    @property
    def pack(self):
        if self.script:
            return self.dir / "assets.mce"
        return self.build / "project" / "assets.mce"

    def player_args(self):
        """A script project's command line for the stock player: its pack and the project's start scene."""
        args = ["-pack", self.pack]
        if self.start_scene:
            args += ["-scene", self.start_scene]
        return args

    def built(self, host=True):
        """Exit unless the build has what the command runs: the module always, the host unless host=False
        (cook, mcp and editor load only the module, so they work while a cook error keeps the host unlinked)."""
        if self.script:
            return
        for f in (self.host, self.module) if host else (self.module,):
            if not f.is_file():
                sys.exit(f"mce: {f} is missing: run `mce build {self.dir}` first")


def mcp_server_entry(p):
    """The MCP client entry that starts `mce mcp` for project p (absolute paths: clients start it from anywhere)."""
    mce = str(BIN / "mce")
    if os.name == "nt":  # a Python script, not an executable, there
        return f"mce_{p.game or p.name}", {"command": sys.executable, "args": [mce, "mcp", str(p.dir)]}
    return f"mce_{p.game or p.name}", {"command": mce, "args": ["mcp", str(p.dir)]}


def claude_desktop_config():
    override = os.environ.get("MCE_CLAUDE_DESKTOP_CONFIG")
    if override:
        return pathlib.Path(override)
    if sys.platform == "darwin":
        base = pathlib.Path.home() / "Library" / "Application Support"
    elif os.name == "nt":
        base = pathlib.Path(os.environ.get("APPDATA", pathlib.Path.home() / "AppData" / "Roaming"))
    else:
        base = pathlib.Path(os.environ.get("XDG_CONFIG_HOME", pathlib.Path.home() / ".config"))
    return base / "Claude" / "claude_desktop_config.json"


def merge_mcp_server(path, name, entry, backup=False):
    """Set (or with entry None remove) mcpServers[name] in the JSON file at path, keeping everything else."""
    data = {}
    if path.is_file():
        text = path.read_text(encoding="utf-8")
        try:
            data = json.loads(text) if text.strip() else {}
        except json.JSONDecodeError as e:
            sys.exit(f"mce: {path} is not valid JSON ({e}); fix it or move it away first")
        if not isinstance(data, dict):
            sys.exit(f"mce: {path} does not hold a JSON object")
        if backup:
            shutil.copy2(path, path.with_name(path.name + ".bak"))
    servers = data.setdefault("mcpServers", {})
    if entry is None:
        if servers.pop(name, None) is None:
            print(f"mce: {path} has no {name} server")
            return
    else:
        servers[name] = entry
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, path)
    print(f"mce: {'removed' if entry is None else 'wrote'} {name} in {path}")


def write_project_mcp_json(project_dir):
    p = Project(project_dir)
    name, entry = mcp_server_entry(p)
    merge_mcp_server(p.dir / ".mcp.json", name, entry)


def cmd_mcp_config(args):
    p = Project(args.project)
    name, entry = mcp_server_entry(p)
    entry = None if args.remove else entry
    merge_mcp_server(p.dir / ".mcp.json", name, entry)
    if args.desktop:
        merge_mcp_server(claude_desktop_config(), name, entry, backup=True)
        print("mce: quit and reopen Claude Desktop to load the change")
    if not args.remove and not p.module.is_file():
        print(f"mce: build the project before using the server: mce build {p.dir}")
    return 0


def cmd_new(args):
    gen = SHARE / "scripts" / "new_project.py"
    cmd = [sys.executable, gen, args.name, "--dest", args.dest]
    if args.game:
        cmd += ["--game", args.game]
    if args.title:
        cmd += ["--title", args.title]
    if args.template:
        cmd += ["--template", args.template]
    if args.force:
        cmd += ["--force"]
    rc = run(cmd)
    if rc == 0:
        write_project_mcp_json(pathlib.Path(args.dest).resolve() / args.name)
    return rc


def cmd_example(args):
    examples = SHARE / "examples"
    names = sorted(p.name for p in examples.iterdir() if (p / "CMakeLists.txt").is_file()) if examples.is_dir() else []
    if not args.name:
        print("examples in this SDK (mce example <Name> copies one to ./<Name>):")
        for n in names:
            print(f"  {n}")
        return 0
    if args.name not in names:
        sys.exit(f"mce: no example named {args.name}; the SDK has: {', '.join(names) or 'none'}")
    dst = pathlib.Path(args.dest).resolve() / args.name
    if dst.exists() and any(dst.iterdir()) and not args.force:
        sys.exit(f"mce: {dst} exists (use --force to overwrite)")
    shutil.copytree(examples / args.name, dst, dirs_exist_ok=True)
    print(f"mce: {args.name} -> {dst}")
    write_project_mcp_json(dst)
    print(f"  build: mce build {dst}    play: mce play {dst}    edit: mce editor {dst}")
    return 0


def max_texture_size_flag(value):
    """['--max-texture-size', 'N'] for mce_cook (nothing for None); exits on a value mce_cook would refuse."""
    if value is None:
        return []
    if not 1 <= value <= 16384 or value & (value - 1):
        sys.exit(f"mce: --max-texture-size {value} is not valid: use a power of two from 1 to 16384 (for example 1024), "
                 f"or leave the flag out for no limit")
    return ["--max-texture-size", str(value)]


def textures_flag(value, ship=False):
    """['--textures', V] for mce_cook (nothing for None). `mce ship` offers rgba8|bc (astc is a phone cook:
    scripts/ship.py); an invalid value exits with the fix."""
    if value is None:
        return []
    allowed = ("rgba8", "bc") if ship else ("rgba8", "bc", "astc")
    if value not in allowed:
        sys.exit(f"mce: --textures {value} is not valid: use {' or '.join(allowed)}"
                 + (" (phones cook astc with scripts/ship.py --target android|ios)" if value == "astc" else ""))
    return ["--textures", value]


def cook_script_pack(p, max_texture_size=None, textures=None):
    """A script project has no build: cooking assets/ (which checks the scripts) is it. Incremental."""
    return run([tool("mce_cook"), p.dir, "--pack"] + max_texture_size_flag(max_texture_size) + textures_flag(textures, True))


def cmd_build(args):
    p = Project(args.project)
    if p.script:
        return cook_script_pack(p)
    configure = ["cmake", "-S", p.dir, "-B", p.build, f"-DCMAKE_PREFIX_PATH={PREFIX}",
                 f"-DCMAKE_BUILD_TYPE={args.config}"]
    if shutil.which("ninja"):
        configure += ["-G", "Ninja"]
    configure += args.defines
    rc = run(configure)
    if rc:
        return rc
    rc = run(["cmake", "--build", p.build, "--config", args.config])
    if rc and not p.host.is_file():
        # The pack step failed (an unregistered asset, a scene the module no longer loads) before the host
        # linked. Build the module alone so `mce cook --adopt` / `mce mcp` can repair the content.
        if run(["cmake", "--build", p.build, "--config", args.config, "--target", f"{p.game}_game"]) == 0:
            print(f"mce: the game module built but the pack did not; for new assets without a .mceasset sidecar run\n"
                  f"  mce cook {p.dir} --adopt\nthen mce build {p.dir} again (mce mcp / mce editor also work now)",
                  file=sys.stderr)
    return rc


def newest_source(assets_dir):
    """(mtime, path) of the most recently modified file under assets_dir (sidecars and scenes included), or None."""
    best = None
    for root, dirs, files in os.walk(assets_dir):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for name in files:
            if name.startswith("."):
                continue
            path = os.path.join(root, name)
            try:
                t = os.stat(path).st_mtime
            except OSError:
                continue
            if best is None or t > best[0]:
                best = (t, path)
    return best


def stale_pack_warning(pack, assets_dir, project_dir=None):
    """The warning `mce play` / `mce headless` print when the pack they run is older than the newest source asset or scene
    (only `mce build` refreshes build/project/assets.mce; `cook_run` and `mce cook` write cooked/ only), else None."""
    pack = pathlib.Path(pack)
    if not pack.is_file():
        return None
    newest = newest_source(assets_dir)
    if newest is None or newest[0] <= pack.stat().st_mtime:
        return None
    name = os.path.relpath(newest[1], assets_dir).replace(os.sep, "/")
    return (f"mce: warning: {pack} is older than assets/{name}, so this run plays the stale scene and assets.\n"
            f"  Run `mce build {project_dir if project_dir else '<project>'}` to refresh it "
            f"(`cook_run` and `mce cook` update cooked/, not this pack).")


def warn_stale_pack(p):
    msg = None if p.script else stale_pack_warning(p.pack, p.dir / "assets", p.dir)
    if msg:
        print(msg, file=sys.stderr)


def cmd_play(args):
    p = Project(args.project)
    if p.script:
        return cook_script_pack(p) or run([p.host] + p.player_args() + args.rest)
    p.built()
    warn_stale_pack(p)
    return run([p.host] + args.rest)


def cmd_headless(args):
    p = Project(args.project)
    if p.script:
        return cook_script_pack(p) or run([p.host] + p.player_args() + ["-headless", "1", "-ticks", str(args.ticks)] + args.rest)
    p.built()
    warn_stale_pack(p)
    return run([p.host, "-headless", "1", "-ticks", str(args.ticks)] + args.rest)


def cmd_editor(args):
    p = Project(args.project)
    p.built(host=False)
    module = [] if p.script else ["-module", p.module]
    return run([tool("mce_editor")] + module + ["-project", p.dir] + args.rest)


def cmd_mcp(args):
    p = Project(args.project)
    p.built(host=False)
    module = [] if p.script else ["--module", p.module]
    return run([tool("mce_mcp")] + module + ["--project", p.dir] + args.rest, exec_=True)


def mcp_call(cmd, tool_name, arguments):
    """Call one tool of the project's MCP server without an agent: spawn the server on stdio, call the tool once and
    return its structured result (used by `mce test` and `mce inspect`)."""
    srv = subprocess.Popen([str(c) for c in cmd], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    n = 0

    def request(method, params):
        nonlocal n
        n += 1
        srv.stdin.write(json.dumps({"jsonrpc": "2.0", "id": n, "method": method, "params": params}) + "\n")
        srv.stdin.flush()
        line = srv.stdout.readline()
        if not line:
            sys.exit("mce: the MCP server closed its output before answering")
        return json.loads(line)

    try:
        request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "mce", "version": "1"}})
        r = request("tools/call", {"name": tool_name, "arguments": arguments}).get("result", {})
    finally:
        srv.stdin.close()
        srv.wait(timeout=60)
    if r.get("isError") or r.get("structuredContent") is None:
        sys.exit(f"mce: {tool_name} failed: " + (r.get("content") or [{}])[0].get("text", "")[:400])
    return r["structuredContent"]


def cmd_test(args):
    p = Project(args.project)
    p.built(host=False)
    module = [] if p.script else ["--module", p.module]
    out = mcp_call([tool("mce_mcp")] + module + ["--project", p.dir], "test_run", {"path": args.path} if args.path else {})
    results = out.get("results", [])
    for r in results:
        name = r.get("file") or r.get("name")
        if r.get("ok"):
            print(f"  pass  {name}")
            continue
        bad = [c.get("check", "?") for c in r.get("checks", []) if not c.get("ok")]
        print(f"  FAIL  {name}: {r.get('error') or ', '.join(bad)}")
        for e in r.get("script_errors", []) or []:
            print(f"        script {e.get('script')}: {e.get('error')}")
    print(f"tests: {out.get('passed', 0)} passed, {out.get('failed', 0)} failed")
    if not results:
        print("mce: no .mctest found (scenarios live under the project's tests/ folder)")
        return 1
    return 1 if out.get("failed", 0) else 0


def print_trace(trace):
    """One row per sampled tick, one column per traced field (`run_headless`'s `trace`)."""
    series = trace.get("series") or []
    ticks = trace.get("ticks") or []

    def cell(v):
        if isinstance(v, float):
            return f"{v:.4g}"
        if isinstance(v, list):
            return "(" + ", ".join(cell(x) for x in v) + ")"
        return "-" if v is None else str(v)

    heads = ["tick"] + [f"{s['entity']}:{s['field']}" for s in series]
    rows = [[str(t)] + [cell(s["values"][i]) if i < len(s.get("values", [])) else "-" for s in series] for i, t in enumerate(ticks)]
    widths = [max(len(h), *(len(r[c]) for r in rows)) if rows else len(h) for c, h in enumerate(heads)]
    print("  ".join(h.rjust(w) for h, w in zip(heads, widths)))
    for r in rows:
        print("  ".join(c.rjust(w) for c, w in zip(r, widths)))
    if trace.get("truncated"):
        print(f"(truncated at {len(ticks)} rows)")


def cmd_inspect(args):
    """Run the game headless and print the live component values of entities (the `run_headless` tool's inspect)."""
    if not (args.entity or args.component or args.trace):
        sys.exit("mce: inspect needs --component <Type> (every entity with it), --entity <id or name> and/or --trace <entity>:<Component.path>")
    fields = []
    for t in args.trace or []:
        ent, sep, field = t.partition(":")
        if not sep or not ent or not field:
            sys.exit(f"mce: --trace '{t}' is <entity id or name>:<Component.path>, e.g. Hero:LocalTransform.position.x")
        fields.append({"entity": int(ent) if ent.isdigit() else ent, "field": field})
    p = Project(args.project)
    p.built(host=False)
    module = [] if p.script else ["--module", p.module]
    insp = {}
    if args.entity:
        insp["entities"] = [int(e) if e.isdigit() else e for e in args.entity]
    if args.component:
        insp["component"] = args.component
    if args.show:
        insp["show"] = [t for t in args.show.split(",") if t]
    arguments = {"ticks": args.ticks, "log_lines": 0}
    if insp:
        arguments["inspect"] = insp
    if fields:
        arguments["trace"] = {"every": args.every, "fields": fields}
    scene = args.scene or p.start_scene_path()
    if scene:
        arguments["scene"] = scene
    out = mcp_call([tool("mce_mcp")] + module + ["--project", p.dir], "run_headless", arguments)
    if insp:
        print(json.dumps(out.get("inspect", {}), indent=2))
    if fields:
        print_trace(out.get("trace") or {})
    print(f"{args.ticks} ticks, state_hash {out.get('state_hash', '?')}", file=sys.stderr)
    errs = out.get("script_errors") or []
    for e in errs:
        print(f"script {e.get('script')}: {e.get('error')}", file=sys.stderr)
    return 3 if errs else 0


def cmd_profile(args):
    """Run the game headless and print what each scene script cost (the `run_headless` tool's `scripts`)."""
    p = Project(args.project)
    p.built(host=False)
    module = [] if p.script else ["--module", p.module]
    arguments = {"ticks": args.ticks, "log_lines": 0}
    scene = p.start_scene_path()
    if scene:
        arguments["scene"] = scene
    out = mcp_call([tool("mce_mcp")] + module + ["--project", p.dir], "run_headless", arguments)
    scripts = out.get("scripts") or []
    if not scripts:
        print("mce: the scene runs no scripts")
        return 1
    width = max(len(s["script"]) for s in scripts)
    print(f"{'script'.ljust(width)}  {'updates':>8}  {'avg steps':>10}  {'peak steps':>10}  {'budget':>8}  {'peak/budget':>11}")
    for s in scripts:
        budget = s.get("budget_steps") or 0
        share = f"{100.0 * s['peak_steps'] / budget:.1f}%" if budget else "-"
        print(f"{s['script'].ljust(width)}  {s['updates']:>8}  {s['avg_steps']:>10.1f}  {s['peak_steps']:>10}  {budget:>8}  {share:>11}"
              + ("  FAILED" if s.get("failed") else ""))
    print(f"{args.ticks} ticks, state_hash {out.get('state_hash', '?')}")
    return 3 if any(s.get("failed") for s in scripts) else 0


def cmd_cook(args):
    p = Project(args.project)
    p.built(host=False)
    module = [] if p.script else ["--module", p.module]
    return run([tool("mce_cook")] + module + [p.dir] + max_texture_size_flag(args.max_texture_size)
               + textures_flag(args.textures) + args.rest)


def story_compiler():
    # The SDK's copy, or the engine tree's when mce runs from a checkout (tools/mce_cli/mce.py).
    for path in (SHARE / "scripts" / "story_compile.py", HERE.parents[1] / "story" / "story_compile.py"):
        if path.is_file():
            return path
    sys.exit("mce: story_compile.py is missing (expected share/mce/scripts/story_compile.py)")


def cmd_story(args):
    return run([sys.executable, story_compiler(), args.project] + args.rest)


def cmd_import(args):
    if args.rest and args.rest[0] in ("spine", "dragonbones"):
        import mce_import_skel  # Spine / DragonBones skeletons (SBA-146), installed beside this script
        return mce_import_skel.main(args.rest)
    import mce_import  # installed beside this script
    return mce_import.main(args.rest)


def cmd_content(args):
    import mce_content  # the open-licence content ledger (M-OPENWORLD D2), installed beside this script
    return mce_content.main(args.rest)


def cmd_retarget(args):
    import mce_retarget  # humanoid animation retargeting (M-OPENWORLD D3), installed beside this script
    return mce_retarget.main(args.rest)


def sdk_version():
    version = SHARE / "VERSION"
    lines = version.read_text(encoding="utf-8").splitlines() if version.is_file() else []
    return (lines[0].strip() if lines else "unknown"), (lines[1].strip() if len(lines) > 1 else "")


def engine_checkout():
    """The engine source tree: this script's own (tools/mce_cli/mce.py in a checkout), or the checkout
    install.sh built this SDK from; None for a release SDK."""
    here = HERE.parents[2] if len(HERE.parents) > 2 else None
    if here and (here / "scripts" / "ship.py").is_file():
        return here
    kind, checkout = sdk_origin()
    if kind == "source" and (checkout / "scripts" / "ship.py").is_file():
        return checkout
    return None


def cmd_ship_web(args, p):
    if args.textures == "bc":
        sys.exit("mce: --textures bc is not offered for --target web: a web pack serves every browser, phones "
                 "included, so ship it rgba8 (the default)")
    # A web build cross-compiles the engine with the project (cmake/MceProject.cmake links the game in),
    # which the installed SDK has no libraries for: it runs from the engine checkout.
    checkout = engine_checkout()
    if checkout is None:
        sys.exit("mce: --target web builds from the engine checkout (scripts/ship.py), and this SDK came from a "
                 "release; clone the engine and run python3 scripts/ship.py --project <dir> --target web there")
    try:
        p.dir.relative_to(checkout / "projects")
    except ValueError:
        sys.exit(f"mce: --target web builds projects inside the engine checkout: copy {p.dir} to "
                 f"{checkout / 'projects'} (the engine adds every projects/<name>/), then run this again")
    cmd = [sys.executable, checkout / "scripts" / "ship.py", "--project", p.dir, "--target", "web",
           "--out", pathlib.Path(args.out or (p.dir / "dist")).resolve()]
    if args.version:
        cmd += ["--version", args.version]
    if args.smoke or args.smoke_ticks:
        cmd.append("--smoke")
    if args.smoke_ticks:
        cmd += ["--smoke-ticks", str(args.smoke_ticks)]
    cmd += max_texture_size_flag(args.max_texture_size)
    return run(cmd, cwd=checkout)


def cmd_ship_script(args, p):
    """A script project ships the stock player, the engine library, the pack and a launcher."""
    if cook_script_pack(p, args.max_texture_size, args.textures):
        return 1
    version, stamp = sdk_version()
    version = args.version or p.meta.get("version") or version
    os_tag = {"Linux": "linux-x64", "Windows": "win64", "Darwin": "macos"}.get(platform.system(), platform.system().lower())
    out_dir = pathlib.Path(args.out or (p.dir / "dist")).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    zip_path = out_dir / f"{p.name}-{version}-{os_tag}.zip"
    scene = f" -scene {p.start_scene}" if p.start_scene else ""
    if os.name == "nt":
        launcher = (f"{p.name}.bat", f'@echo off\r\n"%~dp0mce_player.exe" -pack "%~dp0assets.mce"{scene} %*\r\n')
    else:
        launcher = (f"{p.name}.sh", f'#!/bin/sh\nd="$(cd "$(dirname "$0")" && pwd)"\nexec "$d/mce_player" -pack "$d/assets.mce"{scene} "$@"\n')
    files = {f"mce_player{EXE}": p.host, ENGINE_LIB: LIB / ENGINE_LIB, "assets.mce": p.pack}
    for name, src in files.items():
        if not src.is_file():
            sys.exit(f"mce: {src} is missing")
    if (p.dir / "CREDITS.md").is_file():
        files["CREDITS.md"] = p.dir / "CREDITS.md"
    version_txt = (f"version {version}\nengine {stamp}\nname {p.title}\nkind script project (stock player)\n" +
                   "".join(f"file {n}\n" for n in [launcher[0], *files]))
    readme = (f"{p.title}\n\nRun {launcher[0]}. The player, the engine library and the cooked pack sit beside it;\n"
              f"keep them together. The launcher passes its arguments to mce_player (-help lists them).\n")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, src in files.items():
            info = zipfile.ZipInfo.from_file(src, f"{p.name}/{name}")
            info.external_attr = (0o644 << 16) if name in ("assets.mce", "CREDITS.md") else (0o755 << 16)
            with open(src, "rb") as f:
                z.writestr(info, f.read(), compress_type=zipfile.ZIP_DEFLATED)
        info = zipfile.ZipInfo(f"{p.name}/{launcher[0]}", date_time=(2020, 1, 1, 0, 0, 0))
        info.external_attr = 0o755 << 16
        z.writestr(info, launcher[1], compress_type=zipfile.ZIP_DEFLATED)
        z.writestr(f"{p.name}/VERSION.txt", version_txt)
        z.writestr(f"{p.name}/README.txt", readme)
    print(f"mce: wrote {zip_path} ({zip_path.stat().st_size // 1024} KiB)")
    return 0


def cmd_ship(args):
    p = Project(args.project)
    if p.script:
        if args.steam:
            sys.exit("mce: --steam is not offered for script projects yet")
        # The browser build (and phones, with the engine checkout's scripts/ship.py --target android|ios) builds
        # the stock player with the project's pack: user guide 27.7.
        return cmd_ship_web(args, p) if args.target == "web" else cmd_ship_script(args, p)
    cap = max_texture_size_flag(args.max_texture_size) + textures_flag(args.textures, True)
    if args.target == "web":
        if args.steam:
            sys.exit("mce: --steam ships the desktop build")
        return cmd_ship_web(args, p)
    p.built()
    if not p.pack.is_file():
        sys.exit(f"mce: {p.pack} is missing: run `mce build {p.dir}` first")
    pack = p.pack
    if cap:
        # `mce build` cooks at full size: re-cook the project with the cap into <project>/assets.mce and ship that pack
        # (it prints the size report; the build tree's pack stays as it was). `cap` holds --max-texture-size and --textures.
        if run([tool("mce_cook"), "--module", p.module, p.dir, "--pack"] + cap):
            sys.exit("mce: the re-cook failed; nothing was shipped")
        pack = p.dir / "assets.mce"
    version, stamp = sdk_version()
    version = args.version or p.meta.get("version") or version
    # The same OS tag as scripts/ship.py, so both tools name a project's zip alike.
    os_tag = {"Linux": "linux-x64", "Windows": "win64", "Darwin": "macos"}.get(platform.system(), platform.system().lower())
    out_dir = pathlib.Path(args.out or (p.dir / "dist")).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    zip_path = out_dir / f"{p.name}-{version}-{os_tag}.zip"
    files = {
        f"{p.name}{EXE}": p.host,
        p.module.name: p.module,
        ENGINE_LIB: LIB / ENGINE_LIB,
        "assets.mce": pack,
    }
    for name, src in files.items():
        if not src.is_file():
            sys.exit(f"mce: {src} is missing")
    if (p.dir / "CREDITS.md").is_file():  # content attribution travels with the game
        files["CREDITS.md"] = p.dir / "CREDITS.md"
    version_txt = (f"version {version}\nengine {stamp}\nname {p.title}\n" +
                   "".join(f"file {n}\n" for n in files))
    readme = (f"{p.title}\n\nRun {p.name}{EXE}. The game module, the engine library and the cooked pack\n"
              f"sit beside it; keep them together. -help lists the command line.\n")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, src in files.items():
            info = zipfile.ZipInfo.from_file(src, f"{p.name}/{name}")
            info.external_attr = (0o644 << 16) if name in ("assets.mce", "CREDITS.md") else (0o755 << 16)
            with open(src, "rb") as f:
                z.writestr(info, f.read(), compress_type=zipfile.ZIP_DEFLATED)
        z.writestr(f"{p.name}/VERSION.txt", version_txt)
        z.writestr(f"{p.name}/README.txt", readme)
    print(f"mce: wrote {zip_path} ({zip_path.stat().st_size // 1024} KiB)")
    if args.steam:
        import tempfile
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
        import mce_steam  # installed beside this script
        library = mce_steam.find_library(os_tag, args.steam_library)
        with tempfile.TemporaryDirectory(prefix="mce-steam-") as tmp:
            stage = pathlib.Path(tmp) / p.name
            stage.mkdir()
            for name, src in files.items():
                shutil.copy2(src, stage / name)
            (stage / "VERSION.txt").write_text(version_txt)
            (stage / "README.txt").write_text(readme)
            mce_steam.make_depot(str(stage), str(out_dir / f"{p.name}-steam"), args.steam, os_tag, library, version,
                                 p.title, args.steam_depot, args.steam_branch)
    return 0


def web_root(where):
    """The folder `mce serve` serves: a web build folder (index.html), its zip (unpacked to a temporary
    folder) or a project, whose newest dist/*-web.zip it picks."""
    path = pathlib.Path(where).resolve()
    if path.is_dir() and not (path / "index.html").is_file():
        zips = sorted((path / "dist").glob("*-web.zip"), key=lambda z: z.stat().st_mtime) if (path / "dist").is_dir() else []
        if not zips:
            sys.exit(f"mce: {path} has no index.html and no dist/*-web.zip (mce ship {path} --target web makes one)")
        path = zips[-1]
    if path.is_file() and path.suffix == ".zip":
        import tempfile
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="mce-serve-"))
        with zipfile.ZipFile(path) as z:
            z.extractall(tmp)
        tops = [d for d in tmp.iterdir()]
        root = tops[0] if len(tops) == 1 and tops[0].is_dir() else tmp
        print(f"mce: {path.name} unpacked to {root}")
        return root
    if not (path / "index.html").is_file():
        sys.exit(f"mce: {path} is not a web build (no index.html)")
    return path


def cmd_serve(args):
    import functools
    import http.server
    root = web_root(args.where)

    class Handler(http.server.SimpleHTTPRequestHandler):
        extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                          ".wasm": "application/wasm", ".js": "text/javascript", ".mce": "application/octet-stream"}

        def end_headers(self):
            # Cross-origin isolation (what a threaded build needs for SharedArrayBuffer; itch.io offers the
            # same switch), and no stale copy after a rebuild.
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            self.send_header("Cross-Origin-Resource-Policy", "same-origin")
            self.send_header("Cache-Control", "no-cache")
            super().end_headers()

        def log_message(self, fmt, *a):
            if not args.quiet:
                super().log_message(fmt, *a)

    host = "0.0.0.0" if args.lan else "127.0.0.1"
    server = http.server.ThreadingHTTPServer((host, args.port), functools.partial(Handler, directory=str(root)))
    port = server.server_address[1]
    url = f"http://127.0.0.1:{port}/"
    print(f"mce: serving {root} at {url}" + (" (and on this machine's address, for a phone on the same network)"
                                            if args.lan else "") + "; Ctrl+C stops", flush=True)
    if args.open:
        import webbrowser
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


SEVERITIES = ("blocking", "major", "minor")


def read_friction(project_dir):
    """The notes in <project>/friction.jsonl; lines that do not parse are skipped (and counted)."""
    path = pathlib.Path(project_dir) / "friction.jsonl"
    notes, bad = [], 0
    if not path.is_file():
        return notes, bad
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        try:
            n = json.loads(line)
        except ValueError:
            bad += 1
            continue
        if isinstance(n, dict) and n.get("summary") and n.get("area"):
            notes.append(n)
        else:
            bad += 1
    return notes, bad


def friction_backlog(groups, version):
    """Markdown: blocking first, then by how many notes an area has."""
    out = [f"# Friction backlog\n", f"Collected by mce {version} from {len(groups)} project(s).\n"]
    notes = [(proj, n) for proj, ns in groups for n in ns]
    if not notes:
        out.append("No friction notes.\n")
        return "\n".join(out)
    for sev in SEVERITIES:
        of_sev = [(proj, n) for proj, n in notes if n.get("severity", "major") == sev]
        if not of_sev:
            continue
        out.append(f"## {sev} ({len(of_sev)})\n")
        areas = {}
        for proj, n in of_sev:
            areas.setdefault(n["area"], []).append((proj, n))
        for area, items in sorted(areas.items(), key=lambda kv: (-len(kv[1]), kv[0])):
            out.append(f"### {area} ({len(items)})\n")
            for proj, n in items:
                line = f"- {n['summary']} ({proj}, {n.get('agent', '?')}"
                line += f", {n['tool']})" if n.get("tool") else ")"
                out.append(line)
                for key in ("expected", "actual", "workaround", "details"):
                    if n.get(key):
                        out.append(f"  - {key}: {n[key]}")
            out.append("")
    return "\n".join(out)


def cmd_friction(args):
    groups, bad_total = [], 0
    for d in args.projects or ["."]:
        if not pathlib.Path(d).is_dir():
            sys.exit(f"mce: {d} is not a folder")
        notes, bad = read_friction(d)
        bad_total += bad
        groups.append((pathlib.Path(d).resolve().name, notes))
    if bad_total:
        print(f"mce: skipped {bad_total} line(s) that are not friction notes", file=sys.stderr)
    if args.json:
        text = json.dumps([dict(n, project=proj) for proj, ns in groups for n in ns], indent=2) + "\n"
    else:
        text = friction_backlog(groups, sdk_version()[0])
    if args.out:
        pathlib.Path(args.out).write_text(text, encoding="utf-8")
        count = sum(len(ns) for _, ns in groups)
        print(f"mce: wrote {args.out} ({count} note(s))")
    else:
        sys.stdout.write(text)
    return 0


def sdk_origin():
    """How install.sh made this SDK: ("source", <checkout>), ("release", None) or (None, None)."""
    origin = SHARE / "ORIGIN"
    text = origin.read_text(encoding="utf-8").strip() if origin.is_file() else ""
    kind, _, where = text.partition(" ")
    if kind == "source" and where:
        return "source", pathlib.Path(where)
    if kind == "release":
        return "release", None
    return None, None


def cmd_update(args):
    if os.name == "nt":
        sys.exit("mce: update needs install.sh (macOS and Linux)")
    kind, checkout = sdk_origin()
    if args.release is not None:
        kind = "release"
    projects = [Project(p) for p in args.projects]  # check them before replacing the SDK
    before = sdk_version()[1].split(" ")[0] or "unknown"
    if kind == "source":
        if not (checkout / "install.sh").is_file():
            sys.exit(f"mce: the checkout this SDK was built from ({checkout}) is gone; run install.sh from a checkout")
        if not args.no_pull:
            rc = run(["git", "-C", checkout, "pull", "--ff-only"])
            if rc:
                sys.exit(f"mce: git pull failed in {checkout} (git's message is above). If git could not sign in "
                         "(\"could not read Username\", \"Authentication failed\"), use an SSH remote "
                         "(git remote set-url origin git@github.com:<owner>/<repo>.git) or `gh auth login` + "
                         "`gh auth setup-git`; otherwise fix the local changes or diverged branch. "
                         "Or pull yourself and run `mce update --no-pull`.")
        rc = run(["sh", checkout / "install.sh", "--prefix", PREFIX, "--no-path"])
    elif kind == "release":
        # install.sh replaces this prefix, itself included, so run a copy from outside it.
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            script = pathlib.Path(tmp) / "install.sh"
            shutil.copy2(SHARE / "install.sh", script)
            rc = run(["sh", script, "--release"] + ([args.release] if args.release else []) +
                     ["--prefix", PREFIX, "--no-path"])
    else:
        sys.exit("mce: this SDK was not installed by install.sh (or from an archive); reinstall it with install.sh")
    if rc:
        return rc
    for p in projects:
        print(f"mce: rebuilding {p.dir} against the new engine")
        rc = run([sys.executable, PREFIX / "bin" / "mce", "build", p.dir])
        if rc:
            return rc
    version, stamp = sdk_version()
    after = stamp.split(" ")[0] or "unknown"
    print(f"mce: engine {before} -> {after}" + (" (already current)" if before == after else "") + f": mce {version} ({stamp})")
    if not projects:
        print("mce: rebuild each project once (mce build <project>): a module only loads into the engine it was built for")
    return 0


def cmd_version(args):
    version, stamp = sdk_version()
    print(f"mce {version} ({stamp or 'engine stamp unknown'}) at {PREFIX}")
    return 0


def perf_capture_path(where):
    """A .mcperf file, or a project folder's last perf_run capture."""
    path = pathlib.Path(where)
    if path.is_dir():
        path = path / "cooked" / ".perf" / "last.mcperf"
    if not path.is_file():
        sys.exit(f"mce: {path} does not exist (perf_run, an MCP tool, writes <project>/cooked/.perf/last.mcperf; "
                 f"the player writes one with -perf_capture <file>)")
    return path


def perf_draw_causes(cap):
    """[(cause, mean per frame, max, hint)] largest mean first, from the per-frame counters; the file names its
    causes, so a capture from another engine version still reads. Counts are machine independent."""
    names = cap.get("draw_causes") or []
    hints = cap.get("draw_cause_hints") or []
    frames = [f.get("counters", {}) for f in cap.get("frames", [])]
    rows = []
    for i, name in enumerate(names):
        col = [(c.get("draw_causes") or [])[i] if i < len(c.get("draw_causes") or []) else 0 for c in frames]
        if col and max(col) > 0:
            rows.append((name, sum(col) // len(col), max(col), hints[i] if i < len(hints) else ""))
    rows.sort(key=lambda r: (-r[1], names.index(r[0])))
    instanced = [c.get("draws_instanced", 0) for c in frames]
    runs = [c.get("runs_instanced", 0) for c in frames]
    n = len(frames) or 1
    return rows, sum(instanced) // n, sum(runs) // n


def perf_report(cap, path):
    """The text `mce perf` prints. Only the counters are deterministic; the times are the machine's."""
    out = []
    summary = cap.get("summary", {})
    frames = summary.get("frames", len(cap.get("frames", [])))
    out.append(f"{path}: {cap.get('app', '?')} on {cap.get('backend', '?')}, scene {cap.get('scene', '?')}, {frames} frames")
    for key, label in (("frame_ms", "frame"), ("sim_ms", "sim"), ("render_ms", "render"), ("gpu_ms", "gpu")):
        st = summary.get(key) or {}
        if st.get("count"):
            out.append(f"  {label + ' ms':<10} p50 {st['p50']:.2f}  p95 {st['p95']:.2f}  max {st['max']:.2f}")
    k = summary.get("counters_avg", {})
    out.append(f"  per frame (mean): {k.get('draws', 0)} draw calls, {k.get('triangles', 0)} triangles, "
               f"{k.get('meshes', 0)} meshes, {k.get('sprites', 0)} sprites")
    rows, inst_draws, inst_runs = perf_draw_causes(cap)
    out.append("")
    out.append("draw calls by cause (3D scene draws per frame, mean over the capture)")
    if not rows and not inst_draws:
        out.append("  none: the capture drew no 3D (or it predates the report)")
        return "\n".join(out)
    total = inst_draws + sum(r[1] for r in rows)
    out.append(f"  {'cause':<16}{'draws':>7}{'max':>7}  what to change")
    out.append(f"  {'instanced':<16}{inst_draws:>7}{'':>7}  folded into {inst_runs} instanced draw(s): nothing to do")
    for name, mean, mx, hint in rows:
        out.append(f"  {name:<16}{mean:>7}{mx:>7}  {hint}")
    out.append(f"  {'total':<16}{total:>7}")
    return "\n".join(out)


def cmd_perf(args):
    path = perf_capture_path(args.where)
    try:
        cap = json.loads(path.read_text(encoding="utf-8"))
    except ValueError as e:
        sys.exit(f"mce: {path} is not JSON: {e}")
    if cap.get("format") != "mce-perf":
        sys.exit(f'mce: {path} is not a .mcperf capture ("format" must be "mce-perf")')
    if args.json:
        rows, inst_draws, inst_runs = perf_draw_causes(cap)
        print(json.dumps({"file": str(path), "instanced_draws": inst_draws, "instanced_runs": inst_runs,
                          "draw_causes": [{"cause": n, "avg": a, "max": m, "hint": h} for n, a, m, h in rows]}, indent=2))
    else:
        print(perf_report(cap, path))
    return 0


def cmd_scene(args):
    # mce_scene_diff owns stdout and the exit code (0 identical, 1 differ, 2 unreadable), so no "mce:" command line.
    return subprocess.call([tool("mce_scene_diff"), args.before, args.after])


def main(argv):
    ap = argparse.ArgumentParser(prog="mce", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", metavar="<command>")

    s = sub.add_parser("new", help="stamp a project from the template")
    s.add_argument("name")
    s.add_argument("--game")
    s.add_argument("--title")
    s.add_argument("--template", choices=("project", "novel", "script"),
                   help="project: the 2D starter (default); novel: a visual novel on the novel kit; "
                        "script: Luau scripts and data, no C++ and no compiler")
    s.add_argument("--dest", default=os.getcwd())
    s.add_argument("--force", action="store_true")
    s.set_defaults(fn=cmd_new)

    s = sub.add_parser("example", help="copy an example project out of the SDK (no name: list them)")
    s.add_argument("name", nargs="?")
    s.add_argument("--dest", default=os.getcwd())
    s.add_argument("--force", action="store_true")
    s.set_defaults(fn=cmd_example)

    s = sub.add_parser("build", help="configure and build a project against this SDK")
    s.add_argument("project")
    s.add_argument("--config", default="Debug", help="CMAKE_BUILD_TYPE (default Debug)")
    s.set_defaults(fn=cmd_build)

    for name, fn, help_ in (("play", cmd_play, "run the game"),
                            ("editor", cmd_editor, "open the project in the editor (module loaded)"),
                            ("mcp", cmd_mcp, "the MCP server for the project (module loaded), over stdio"),
                            ("cook", cmd_cook, "cook the project's assets (module loaded)")):
        s = sub.add_parser(name, help=help_)
        s.add_argument("project")
        if name == "cook":
            s.add_argument("--max-texture-size", type=int, metavar="N",
                           help="cap PNG/JPG textures at N pixels on the longest side (a power of two, 1..16384); "
                                "default: no limit")
            s.add_argument("--textures", metavar="MODE",
                           help="rgba8 (default), bc (BC7/BC5/BC4/BC1 blocks for desktop packs) or astc (phones)")
        s.add_argument("rest", nargs=argparse.REMAINDER, help="flags passed through")
        s.set_defaults(fn=fn)

    s = sub.add_parser("story", help="the visual-novel story compiler (mce story compile <project>)")
    story = s.add_subparsers(dest="story_command", metavar="compile", required=True)
    s = story.add_parser("compile", help="compile story/*.story into the game's header (--check: validate only)")
    s.add_argument("project", nargs="?", default=".")
    s.add_argument("rest", nargs=argparse.REMAINDER, help="--check, --werror, --out <header>, --report <md>")
    s.set_defaults(fn=cmd_story)

    s = sub.add_parser("import", help="2D authoring-tool exports into native assets (mce import aseprite <export.json>)")
    s.add_argument("rest", nargs=argparse.REMAINDER, help="aseprite|spine|dragonbones <export.json> [--out f] [--name n] [--werror|--strict]")
    s.set_defaults(fn=cmd_import)

    s = sub.add_parser("content", help="the open-licence content ledger: list, fetch and cook CC0 / CC-BY packs into a project")
    s.add_argument("rest", nargs=argparse.REMAINDER, help="list|info|check|fetch|add|verify|credits ... (mce content --help)")
    s.set_defaults(fn=cmd_content)

    s = sub.add_parser("retarget", help="retarget humanoid animation clips from other glTF files onto a character's skeleton")
    s.add_argument("rest", nargs=argparse.REMAINDER, help="<source.glb>... --onto <character.glb> [--out f.glb] (mce retarget --help)")
    s.set_defaults(fn=cmd_retarget)

    s = sub.add_parser("mcp-config", help="register the project's MCP server with Claude Code (.mcp.json) "
                                          "and, with --desktop, Claude Desktop")
    s.add_argument("project")
    s.add_argument("--desktop", action="store_true", help="also add it to Claude Desktop's claude_desktop_config.json")
    s.add_argument("--remove", action="store_true", help="remove the entry instead")
    s.set_defaults(fn=cmd_mcp_config)

    s = sub.add_parser("scene", help="scene tools (mce scene diff <before> <after>)")
    scene = s.add_subparsers(dest="scene_command", metavar="diff", required=True)
    s = scene.add_parser("diff", help="what changed between two .mcscene files: entities added, removed or changed; exit 1 when they differ")
    s.add_argument("before")
    s.add_argument("after")
    s.set_defaults(fn=cmd_scene)

    s = sub.add_parser("headless", help="run the game without a window and print the state hash")
    s.add_argument("project")
    s.add_argument("--ticks", type=int, default=600)
    s.add_argument("rest", nargs=argparse.REMAINDER)
    s.set_defaults(fn=cmd_headless)

    s = sub.add_parser("test", help="play a project's .mctest scenarios (tests/) headless and print pass or fail; exit 1 on a failure")
    s.add_argument("project")
    s.add_argument("path", nargs="?", help="one .mctest file or a folder under the project (default: tests/)")
    s.set_defaults(fn=cmd_test)

    s = sub.add_parser("inspect", help="run the game headless and print the live component values of entities as JSON")
    s.add_argument("project")
    s.add_argument("--ticks", type=int, default=300)
    s.add_argument("--component", help="every live entity with this component (at most 64)")
    s.add_argument("--entity", action="append", help="a scene entity id or name (repeatable)")
    s.add_argument("--show", help="comma-separated components to print (default: the entity's own, or the --component plus LocalTransform)")
    s.add_argument("--scene", help="play this saved scene (path under assets/)")
    s.add_argument("--trace", action="append", metavar="ENTITY:FIELD",
                   help="sample a field while it runs and print the series, e.g. Hero:LocalTransform.position.x (repeatable, at most 16)")
    s.add_argument("--every", type=int, default=10, help="with --trace: sample every N ticks (default 10)")
    s.set_defaults(fn=cmd_inspect)

    s = sub.add_parser("profile", help="run the game headless and print each scene script's step cost against its budget")
    s.add_argument("project")
    s.add_argument("--ticks", type=int, default=300)
    s.set_defaults(fn=cmd_profile)

    s = sub.add_parser("ship", help="zip the host, module, engine library and pack (--target web: the browser build)")
    s.add_argument("project")
    s.add_argument("--target", choices=("desktop", "web"), default="desktop")
    s.add_argument("--out")
    s.add_argument("--version")
    s.add_argument("--max-texture-size", type=int, metavar="N",
                   help="re-cook the pack with PNG/JPG textures capped at N pixels on the longest side (a power of two, "
                        "1..16384) so the zip is smaller; a sidecar's smaller max_size wins. Default: no limit")
    s.add_argument("--textures", metavar="MODE",
                   help="rgba8 (default) or bc: re-cook the pack with BC7/BC5/BC4/BC1 block textures (about a quarter of "
                        "the size; desktop only, a GPU without BC cannot draw them). Not for --target web")
    s.add_argument("--smoke", action="store_true", help="--target web: play it once in headless Chromium")
    s.add_argument("--smoke-ticks", type=int, default=0,
                   help="--target web: play N ticks in headless Chromium, screenshot, and check the state hash against "
                        "a headless browser run")
    s.add_argument("--steam", type=int, default=0, metavar="APP_ID",
                   help="also lay out the Steam depot for this OS and the steamcmd build scripts in <out>/<Name>-steam")
    s.add_argument("--steam-depot", type=int, default=0, help="with --steam: this OS's depot id (default app + 1/2/3)")
    s.add_argument("--steam-library", help="with --steam: the steam_api to ship (default: from STEAMWORKS_SDK)")
    s.add_argument("--steam-branch", default="", help="with --steam: set the build live on this branch")
    s.set_defaults(fn=cmd_ship)

    s = sub.add_parser("serve", help="play a web build locally: its zip, its folder, or a project's newest one")
    s.add_argument("where", nargs="?", default=".")
    s.add_argument("--port", type=int, default=8000, help="0 picks a free port")
    s.add_argument("--lan", action="store_true", help="listen on every address, not only this machine")
    s.add_argument("--open", action="store_true", help="open it in the default browser")
    s.add_argument("--quiet", action="store_true", help="no request log")
    s.set_defaults(fn=cmd_serve)

    s = sub.add_parser("perf", help="read a perf capture: frame cost and the draw calls by cause")
    s.add_argument("where", nargs="?", default=".", help="a .mcperf file, or a project folder (its last perf_run capture)")
    s.add_argument("--json", action="store_true", help="the draw-call causes as JSON")
    s.set_defaults(fn=cmd_perf)

    s = sub.add_parser("friction", help="collect the friction notes agents wrote (report_friction) into a backlog")
    s.add_argument("projects", nargs="*", help="project folders (default: the current folder)")
    s.add_argument("--out", help="write the backlog to this file instead of stdout")
    s.add_argument("--json", action="store_true", help="the notes as one JSON array, each with its project")
    s.set_defaults(fn=cmd_friction)

    s = sub.add_parser("update", help="reinstall this SDK from its checkout (git pull) or the latest release")
    s.add_argument("projects", nargs="*", help="projects to rebuild afterwards")
    s.add_argument("--release", nargs="?", const="", default=None, metavar="TAG",
                   help="install from GitHub releases (default: the latest) instead of the checkout")
    s.add_argument("--no-pull", action="store_true", help="source installs: rebuild the checkout as it is, without git pull")
    s.set_defaults(fn=cmd_update)

    s = sub.add_parser("version", help="the SDK version and engine build stamp")
    s.set_defaults(fn=cmd_version)

    args, extra = ap.parse_known_args(argv)
    if not args.command:
        ap.print_help()
        return 2
    if args.command == "build":
        bad = [e for e in extra if not e.startswith("-D")]
        if bad:
            ap.error(f"unrecognized arguments: {' '.join(bad)} (only -D<var>=<value> passes through to cmake)")
        args.defines = extra
    elif extra:
        ap.error(f"unrecognized arguments: {' '.join(extra)}")
    rest = getattr(args, "rest", None)
    if rest and rest[0] == "--":
        args.rest = rest[1:]
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
