#!/usr/bin/env python3
"""mce: the installed engine's command (installed as <sdk>/bin/mce by cmake/MceInstall.cmake).

    mce new <Name> [--game id] [--title "Title"] [--dest dir]   stamp a project from the template
    mce build <project> [--config Debug|Release] [-D<var>=<v>...]  configure (find_package(MCE)) + build
    mce play <project> [player flags...]        run the game (its host) from the build
    mce headless <project> [--ticks N] [flags]  run it without a window; prints the state hash
    mce editor <project> [editor flags...]      the editor with the game module loaded
    mce mcp <project> [--allow-cheats]          the MCP server over stdio with the module loaded
    mce cook <project> [cook flags...]          cook assets/ with the module loaded (--pack, --force, --adopt)
    mce ship <project> [--out dir] [--version v]   zip the host, module, engine and pack for distribution
    mce version                                 the SDK's version and engine build stamp
    mce --help

A project is a folder with one <Name>.mceproj (mce new makes one). Its build lives in
<project>/build (mce build), where <game> (the host), lib<game>.so (the module) and
project/assets.mce (the cooked pack) land. Every subcommand prints the command it runs.
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
MODULE_PREFIX, MODULE_SUFFIX = ("", ".dll") if os.name == "nt" else ("lib", ".dylib" if sys.platform == "darwin" else ".so")
ENGINE_LIB = f"{MODULE_PREFIX}mce_engine{MODULE_SUFFIX}"


def tool(name):
    path = BIN / f"{name}{EXE}"
    if not path.is_file():
        sys.exit(f"mce: {path} is missing (is this an SDK install? expected bin/{name} beside bin/mce)")
    return str(path)


def run(cmd, cwd=None, exec_=False):
    print("mce:", " ".join(str(c) for c in cmd), flush=True)
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
        if not games:
            sys.exit(f"mce: {files[0].name} lists no game")
        self.name = self.meta.get("name", files[0].stem)
        self.title = self.meta.get("title", self.name)
        self.game = games[0]
        self.build = self.dir / "build"

    @property
    def host(self):
        return self.build / f"{self.game}{EXE}"

    @property
    def module(self):
        return self.build / f"{MODULE_PREFIX}{self.game}{MODULE_SUFFIX}"

    @property
    def pack(self):
        return self.build / "project" / "assets.mce"

    def built(self):
        for f in (self.host, self.module):
            if not f.is_file():
                sys.exit(f"mce: {f} is missing: run `mce build {self.dir}` first")


def cmd_new(args):
    gen = SHARE / "scripts" / "new_project.py"
    cmd = [sys.executable, gen, args.name, "--dest", args.dest]
    if args.game:
        cmd += ["--game", args.game]
    if args.title:
        cmd += ["--title", args.title]
    if args.force:
        cmd += ["--force"]
    return run(cmd)


def cmd_build(args):
    p = Project(args.project)
    configure = ["cmake", "-S", p.dir, "-B", p.build, f"-DCMAKE_PREFIX_PATH={PREFIX}",
                 f"-DCMAKE_BUILD_TYPE={args.config}"]
    if shutil.which("ninja"):
        configure += ["-G", "Ninja"]
    configure += args.defines
    rc = run(configure)
    if rc:
        return rc
    return run(["cmake", "--build", p.build, "--config", args.config])


def cmd_play(args):
    p = Project(args.project)
    p.built()
    return run([p.host] + args.rest)


def cmd_headless(args):
    p = Project(args.project)
    p.built()
    return run([p.host, "-headless", "1", "-ticks", str(args.ticks)] + args.rest)


def cmd_editor(args):
    p = Project(args.project)
    p.built()
    return run([tool("mce_editor"), "-module", p.module, "-project", p.dir] + args.rest)


def cmd_mcp(args):
    p = Project(args.project)
    p.built()
    return run([tool("mce_mcp"), "--module", p.module, "--project", p.dir] + args.rest, exec_=True)


def cmd_cook(args):
    p = Project(args.project)
    p.built()
    return run([tool("mce_cook"), "--module", p.module, p.dir] + args.rest)


def sdk_version():
    version = SHARE / "VERSION"
    lines = version.read_text(encoding="utf-8").splitlines() if version.is_file() else []
    return (lines[0].strip() if lines else "unknown"), (lines[1].strip() if len(lines) > 1 else "")


def cmd_ship(args):
    p = Project(args.project)
    p.built()
    if not p.pack.is_file():
        sys.exit(f"mce: {p.pack} is missing: run `mce build {p.dir}` first")
    version, stamp = sdk_version()
    version = args.version or p.meta.get("version") or version
    os_tag = {"Linux": "linux", "Windows": "windows", "Darwin": "macos"}.get(platform.system(), platform.system().lower())
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
    version_txt = (f"version {version}\nengine {stamp}\nname {p.title}\n" +
                   "".join(f"file {n}\n" for n in files))
    readme = (f"{p.title}\n\nRun {p.name}{EXE}. The game module, the engine library and the cooked pack\n"
              f"sit beside it; keep them together. -help lists the command line.\n")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, src in files.items():
            info = zipfile.ZipInfo.from_file(src, f"{p.name}/{name}")
            info.external_attr = (0o755 << 16) if name != "assets.mce" else (0o644 << 16)
            with open(src, "rb") as f:
                z.writestr(info, f.read(), compress_type=zipfile.ZIP_DEFLATED)
        z.writestr(f"{p.name}/VERSION.txt", version_txt)
        z.writestr(f"{p.name}/README.txt", readme)
    print(f"mce: wrote {zip_path} ({zip_path.stat().st_size // 1024} KiB)")
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
    s.add_argument("--dest", default=os.getcwd())
    s.add_argument("--force", action="store_true")
    s.set_defaults(fn=cmd_new)

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

    s = sub.add_parser("headless", help="run the game without a window and print the state hash")
    s.add_argument("project")
    s.add_argument("--ticks", type=int, default=600)
    s.add_argument("rest", nargs=argparse.REMAINDER)
    s.set_defaults(fn=cmd_headless)

    s = sub.add_parser("ship", help="zip the host, module, engine library and pack")
    s.add_argument("project")
    s.add_argument("--out")
    s.add_argument("--version")
    s.set_defaults(fn=cmd_ship)

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
