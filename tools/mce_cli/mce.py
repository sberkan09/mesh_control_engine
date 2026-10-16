#!/usr/bin/env python3
"""mce: the installed engine's command (installed as <sdk>/bin/mce by cmake/MceInstall.cmake).

    mce new <Name> [--game id] [--title "Title"] [--template project|novel|script] [--dest dir]
                                                stamp a project from a template (novel: a visual novel;
                                                script: Luau scripts and data, no C++, no compiler)
    mce example [<Name>] [--dest dir]           copy an example project out of the SDK (no name: list them)
    mce build <project> [--config Debug|Release] [-D<var>=<v>...]  configure (find_package(MCE)) + build
    mce play <project> [player flags...]        run the game (its host) from the build
    mce headless <project> [--ticks N] [flags]  run it without a window; prints the state hash
    mce editor <project> [editor flags...]      the editor with the game module loaded
    mce mcp <project> [--allow-cheats]          the MCP server over stdio with the module loaded
    mce mcp-config <project> [--desktop] [--remove]   register that server with Claude: <project>/.mcp.json
                                                (Claude Code, also Claude Desktop's Code tab); --desktop
                                                also adds it to Claude Desktop's own config
    mce cook <project> [cook flags...]          cook assets/ with the module loaded (--pack, --force, --adopt)
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


def cook_script_pack(p):
    """A script project has no build: cooking assets/ (which checks the scripts) is it. Incremental."""
    return run([tool("mce_cook"), p.dir, "--pack"])


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


def cmd_play(args):
    p = Project(args.project)
    if p.script:
        return cook_script_pack(p) or run([p.host] + p.player_args() + args.rest)
    p.built()
    return run([p.host] + args.rest)


def cmd_headless(args):
    p = Project(args.project)
    if p.script:
        return cook_script_pack(p) or run([p.host] + p.player_args() + ["-headless", "1", "-ticks", str(args.ticks)] + args.rest)
    p.built()
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


def cmd_cook(args):
    p = Project(args.project)
    p.built(host=False)
    module = [] if p.script else ["--module", p.module]
    return run([tool("mce_cook")] + module + [p.dir] + args.rest)


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
    return run(cmd, cwd=checkout)


def cmd_ship_script(args, p):
    """A script project ships the stock player, the engine library, the pack and a launcher."""
    if cook_script_pack(p):
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
    if args.target == "web":
        if args.steam:
            sys.exit("mce: --steam ships the desktop build")
        return cmd_ship_web(args, p)
    p.built()
    if not p.pack.is_file():
        sys.exit(f"mce: {p.pack} is missing: run `mce build {p.dir}` first")
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
        "assets.mce": p.pack,
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

    s = sub.add_parser("mcp-config", help="register the project's MCP server with Claude Code (.mcp.json) "
                                          "and, with --desktop, Claude Desktop")
    s.add_argument("project")
    s.add_argument("--desktop", action="store_true", help="also add it to Claude Desktop's claude_desktop_config.json")
    s.add_argument("--remove", action="store_true", help="remove the entry instead")
    s.set_defaults(fn=cmd_mcp_config)

    s = sub.add_parser("headless", help="run the game without a window and print the state hash")
    s.add_argument("project")
    s.add_argument("--ticks", type=int, default=600)
    s.add_argument("rest", nargs=argparse.REMAINDER)
    s.set_defaults(fn=cmd_headless)

    s = sub.add_parser("ship", help="zip the host, module, engine library and pack (--target web: the browser build)")
    s.add_argument("project")
    s.add_argument("--target", choices=("desktop", "web"), default="desktop")
    s.add_argument("--out")
    s.add_argument("--version")
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
