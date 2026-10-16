"""The web package (M-WEB step 5): a web build's .js/.wasm and its pack staged around the shell page
(scripts/web/shell.html) and zipped with index.html at the root, as itch.io wants an HTML5 upload.

    <stem>.zip
        index.html      the shell: loading bar, the game's icon, "click to play" (it starts the audio),
                        a full-screen button, a WebGPU check; the canvas fills the page
        <Name>.js       the Emscripten loader (renamed; the shell points it at <Name>.wasm)
        <Name>.wasm
        assets.mce      the cooked pack, fetched by the game at start (samples::resolve_shipped_path)
        icon.png        when the game has <game>/ship/icon.png
        VERSION.txt, README.txt, CREDITS.md

scripts/ship.py --target web calls package(); `mce serve <zip or folder>` (tools/mce_cli/mce.py) plays
it locally with the right MIME types.
"""

import datetime
import gzip
import hashlib
import html
import os
import shutil
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def shell(title, script, wasm, icon):
    """index.html for a game: the shell template with its fields filled in."""
    with open(os.path.join(HERE, "shell.html"), encoding="utf-8") as f:
        page = f.read()
    t = html.escape(title)
    fields = {
        "{{TITLE}}": t,
        "{{SCRIPT}}": html.escape(script),
        "{{WASM}}": wasm.replace("\\", "\\\\").replace('"', '\\"'),
        "{{ICON_LINK}}": f'<link rel="icon" href="{icon}"><link rel="apple-touch-icon" href="{icon}">' if icon else "",
        "{{ICON_IMG}}": f'<img src="{icon}" alt="">' if icon else "",
    }
    for key, value in fields.items():
        page = page.replace(key, value)
    return page


def package(art, args, commit, dirty):
    """Stages and zips the web build of `art` (scripts/ship.py's artifact dict); returns the zip path."""
    base = os.path.splitext(art["exe"])[0]
    js, wasm = base + ".js", base + ".wasm"
    for p in (js, wasm, art["pack"]):
        if not os.path.isfile(p):
            raise SystemExit(f"ship: missing web build output {p} (build the '{args.preset}' preset first)")
    stem = f"{art['name']}-{args.version}-web"
    stage = os.path.join(args.out, stem)
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    name = art["name"]
    files = {f"{name}.js": js, f"{name}.wasm": wasm, "assets.mce": art["pack"]}
    if art.get("credits"):
        files["CREDITS.md"] = art["credits"]
    icon = "icon.png" if art.get("icon") and os.path.isfile(art["icon"]) else None
    if icon:
        files[icon] = art["icon"]
    for dst, src in files.items():
        shutil.copy2(src, os.path.join(stage, dst))
    with open(os.path.join(stage, "index.html"), "w", encoding="utf-8", newline="\n") as f:
        f.write(shell(art.get("display_title") or art["title"], f"{name}.js", f"{name}.wasm", icon))
    with open(os.path.join(stage, "README.txt"), "w", newline="\n") as f:
        f.write(f"{art['title']} {args.version}\nBuilt with Mesh Control Engine.\n\n")
        f.write("Play: open index.html from a web server (browsers do not run it from a file:// address):\n"
                "  mce serve <this folder or its zip>\n"
                "or upload the zip to itch.io as an HTML game (\"This file will be played in the browser\").\n"
                "It needs WebGPU: a current Chrome, Edge or Safari, or Firefox 141 and later.\n\n")
        f.write(f"Controls: {art['controls']}\n")
        if art.get("credits"):
            f.write("Content credits: CREDITS.md.\n")
    hashes = {n: _sha256(os.path.join(stage, n)) for n in sorted(os.listdir(stage))}
    with open(os.path.join(stage, "VERSION.txt"), "w", newline="\n") as f:
        f.write(f"version {args.version}\nengine_commit {commit}{' (dirty)' if dirty else ''}\n")
        f.write(f"preset {args.preset}\nplatform web\n"
                f"built {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}\n")
        for n, h in hashes.items():
            f.write(f"sha256 {h}  {n}\n")

    zip_path = os.path.join(args.out, stem + ".zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for n in sorted(os.listdir(stage)):  # flat, index.html at the root (itch.io)
            z.write(os.path.join(stage, n), arcname=n)
    with open(os.path.join(stage, f"{name}.wasm"), "rb") as f:
        wasm_gz = len(gzip.compress(f.read(), 6))
    print(f"ship: {zip_path} ({os.path.getsize(zip_path) / 1024:.0f} KiB; {name}.wasm "
          f"{os.path.getsize(wasm) / 1048576:.1f} MiB, {wasm_gz / 1048576:.1f} MiB gzipped)")
    return zip_path, stage
