"""macOS packaging for scripts/ship.py (M-MAC): dSYM symbol archive, strip + ad-hoc re-sign, .app bundle.

ship.py stages a flat folder (executable, game module, engine dylib, assets.mce, CREDITS.md,
README.txt, VERSION.txt); on macOS it calls into this module instead of its ELF/PDB steps:

    archive_and_strip(stage, binaries, symbols, keep_symbols)  # before VERSION.txt is written
    make_app(stage, art, version)                               # after: the folder becomes <Name>.app

Bundle layout (SDL_GetBasePath(), i.e. platform::base_path(), is Contents/Resources/ in a bundle):

    <Name>.app/Contents/Info.plist
    <Name>.app/Contents/MacOS/<Name>                the executable
    <Name>.app/Contents/MacOS/lib*.so, *.dylib      the game module and the engine umbrella library
                                                    (the binaries' @loader_path rpath finds them)
    <Name>.app/Contents/Resources/                  assets.mce, VERSION.txt, CREDITS.md, and a
                                                    relative symlink per game module, so the
                                                    base_path() lookup of the module still works

The bundle is signed ad hoc (codesign -s -): Apple silicon refuses unsigned code and
install_name_tool / strip invalidate the linker's signature. Developer ID signing and
notarization are out of scope until FR-SHP-030 says otherwise.
"""

import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys

CODE_SUFFIXES = (".so", ".dylib")


def _run(cmd):
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"ship: {' '.join(cmd)} failed:\n{out.stdout}{out.stderr}")
    return out.stdout


def _tool(name):
    path = shutil.which(name)
    if not path:
        sys.exit(f"ship: {name} not found (install the Xcode command line tools)")
    return path


def macho_uuid(path):
    """The LC_UUID of a thin 64-bit Mach-O file (VERSION.txt's build_id lines), or None."""
    try:
        with open(path, "rb") as f:
            hdr = f.read(32)
            if len(hdr) < 32 or struct.unpack_from("<I", hdr)[0] != 0xFEEDFACF:
                return None
            ncmds, sizeofcmds = struct.unpack_from("<II", hdr, 16)
            cmds = f.read(sizeofcmds)
    except OSError:
        return None
    p = 0
    for _ in range(ncmds):
        if p + 8 > len(cmds):
            return None
        cmd, size = struct.unpack_from("<II", cmds, p)
        if cmd == 0x1B and size >= 24:  # LC_UUID
            return cmds[p + 8:p + 24].hex()
        if size < 8:
            return None
        p += size
    return None


def archive_and_strip(stage, binaries, symbols, keep_symbols):
    """dsymutil every shipped binary into <symbols>/<name>.dSYM (scripts/symbolicate.py finds the
    DWARF file by its LC_UUID), then strip local symbols and re-sign ad hoc."""
    if not keep_symbols:
        dsymutil = _tool("dsymutil")
        os.makedirs(symbols, exist_ok=True)
        for name in binaries:
            _run([dsymutil, os.path.join(stage, name), "-o", os.path.join(symbols, name + ".dSYM")])
        strip = _tool("strip")
        for name in binaries:
            path = os.path.join(stage, name)
            # -x: drop local symbols; executables additionally drop debug symbols (-S). The game
            # module and the engine library keep their exported symbols for dlsym and linking.
            _run([strip, "-x", "-S", path] if not name.endswith(CODE_SUFFIXES) else [strip, "-x", path])
    for name in binaries:
        sign(os.path.join(stage, name))


def sign(path):
    _run([_tool("codesign"), "--force", "--sign", "-", "--timestamp=none", path])


def _bundle_version(version):
    """CFBundleShortVersionString wants up to three dot-separated integers: 0.1.0+abcd1234 -> 0.1.0."""
    m = re.match(r"(\d+)(?:\.(\d+))?(?:\.(\d+))?", version)
    if not m:
        return "0.0.0"
    return ".".join(g or "0" for g in m.groups())


def _bundle_id(name):
    return "dev.meshcontrolengine." + (re.sub(r"[^A-Za-z0-9.-]+", "-", name).strip("-").lower() or "game")


def make_app(stage, art, version, min_macos="26.0"):
    """Turns the flat staged folder into stage/<Name>.app (README.txt stays beside it). Returns the
    path of the bundle's executable."""
    name = art["name"]
    app = os.path.join(stage, name + ".app")
    contents = os.path.join(app, "Contents")
    macos_dir = os.path.join(contents, "MacOS")
    res_dir = os.path.join(contents, "Resources")
    shutil.rmtree(app, ignore_errors=True)
    os.makedirs(macos_dir)
    os.makedirs(res_dir)
    modules = []
    for entry in sorted(os.listdir(stage)):
        src = os.path.join(stage, entry)
        if entry == name + ".app" or entry == "README.txt":
            continue
        if entry == name or entry.endswith(CODE_SUFFIXES):
            shutil.move(src, os.path.join(macos_dir, entry))
            if entry.endswith(".so"):
                modules.append(entry)
        else:
            shutil.move(src, os.path.join(res_dir, entry))
    for m in modules:
        os.symlink(os.path.join("..", "MacOS", m), os.path.join(res_dir, m))

    short = _bundle_version(version)
    info = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": art.get("title", name),
        "CFBundleExecutable": name,
        "CFBundleIdentifier": _bundle_id(name),
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": name[:15],
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": short,
        "CFBundleVersion": version,
        "LSMinimumSystemVersion": min_macos,
        "LSApplicationCategoryType": "public.app-category.games",
        "NSHighResolutionCapable": True,
        "NSSupportsAutomaticGraphicsSwitching": True,
    }
    with open(os.path.join(contents, "Info.plist"), "wb") as f:
        plistlib.dump(info, f)
    with open(os.path.join(contents, "PkgInfo"), "w", newline="") as f:
        f.write("APPL????")

    # Nested code first, then the bundle (codesign seals Resources/ and Info.plist into it).
    for entry in sorted(os.listdir(macos_dir)):
        if entry != name:
            sign(os.path.join(macos_dir, entry))
    sign(app)
    _run([_tool("codesign"), "--verify", "--strict", "--verbose=2", app])
    return os.path.join(macos_dir, name)


def readme_run_line(art):
    return (f"Run: double-click {art['name']}.app (Apple silicon, macOS 26 or newer; it renders with Metal).\n"
            "It is signed ad hoc, not notarized: if macOS refuses to open it, right-click it and choose Open.\n")
