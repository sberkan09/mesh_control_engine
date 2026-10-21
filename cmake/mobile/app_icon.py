"""Per-game app icon for the phone builds (M-MOBILE step 10, #137).

One source image per game, <game>/ship/icon.png (samples/<sample>/ship/icon.png, or
projects/<name>/ship/icon.png): square, ideally 1024x1024. From it:

- iOS (ios_app.py): an AppIcon asset catalog with the single 1024x1024 image (alpha flattened, App
  Store Connect rejects a transparent marketing icon), compiled by `actool` into the bundle's
  Assets.car, and the CFBundleIcons keys actool reports merged into Info.plist.
- Android (android_app.py): an adaptive icon (mipmap-anydpi-v26/ic_launcher.xml) whose foreground
  layer is the image at 72 dp inside the 108 dp canvas in every mipmap density and whose background
  layer is the image's edge colour; plus <stem>-play-icon.png (512x512) for the Play listing.

A game without one still ships (iOS: no icon, Android: SDL's default), with a warning: both stores
reject such an upload. Resizing uses `sips` on a Mac, else Pillow, else a box filter in Python.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

STORE_SIZE = 1024
# Android density buckets: scale of the 108 dp adaptive-icon canvas.
DENSITIES = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}


def warn(msg):
    print(f"ship: warning: {msg}", flush=True)


def source(art):
    """The game's icon.png, or None (with the warning) when it has none."""
    path = art.get("icon")
    if path and os.path.isfile(path):
        return path
    warn(f"{art['name']} has no {path or 'ship/icon.png'}; it ships without an app icon and the stores will refuse "
         "the upload (add a 1024x1024 PNG there)")
    return None


# ---- PNG (8-bit, non-interlaced; enough for icons) ----

def read_png(path):
    """(width, height, RGBA bytes) of a PNG."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"ship: {path} is not a PNG")
    pos, idat, palette, trns = 8, [], None, None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or interlace or channels is None:
        sys.exit(f"ship: {path}: only 8-bit non-interlaced PNGs are supported (re-export it)")
    raw = zlib.decompress(b"".join(idat))
    stride = w * channels
    rows, prev = [], bytearray(stride)
    for y in range(h):
        ftype = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        bpp = channels
        if ftype == 1:
            for i in range(bpp, stride):
                row[i] = (row[i] + row[i - bpp]) & 255
        elif ftype == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 255
        elif ftype == 3:
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                row[i] = (row[i] + ((left + prev[i]) >> 1)) & 255
        elif ftype == 4:
            for i in range(stride):
                a = row[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                row[i] = (row[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(row)
        prev = row
    out = bytearray(w * h * 4)
    o = 0
    for row in rows:
        for x in range(w):
            if ctype == 6:
                out[o:o + 4] = row[x * 4:x * 4 + 4]
            elif ctype == 2:
                out[o:o + 4] = row[x * 3:x * 3 + 3] + b"\xff"
            elif ctype == 0:
                out[o:o + 4] = bytes((row[x],) * 3) + b"\xff"
            elif ctype == 4:
                out[o:o + 4] = bytes((row[x * 2],) * 3) + row[x * 2 + 1:x * 2 + 2]
            else:
                i = row[x]
                alpha = trns[i] if trns and i < len(trns) else 255
                out[o:o + 4] = palette[i * 3:i * 3 + 3] + bytes((alpha,))
            o += 4
    return w, h, bytes(out)


def write_png(path, w, h, rgba, alpha=True):
    channels = 4 if alpha else 3
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = rgba[y * w * 4:(y + 1) * w * 4]
        if alpha:
            raw += row
        else:
            raw += bytes(b for i, b in enumerate(row) if i % 4 != 3)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6 if alpha else 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def edge_colour(w, h, rgba):
    """The most common opaque colour on a ring just inside the border (rounded corners skipped)."""
    counts = {}
    inset = max(1, w // 16)
    for i in range(inset, w - inset):
        for x, y in ((i, inset), (i, h - 1 - inset), (inset, i), (w - 1 - inset, i)):
            px = rgba[(y * w + x) * 4:(y * w + x) * 4 + 4]
            if px[3] == 255:
                counts[px[:3]] = counts.get(px[:3], 0) + 1
    return max(counts, key=counts.get) if counts else b"\x00\x00\x00"


def flatten(w, h, rgba, bg):
    """Composites the image over `bg` (RGB): an opaque image."""
    if all(rgba[i] == 255 for i in range(3, len(rgba), 4)):
        return rgba
    out = bytearray(rgba)
    for i in range(0, len(out), 4):
        a = out[i + 3]
        if a != 255:
            for c in range(3):
                out[i + c] = (out[i + c] * a + bg[c] * (255 - a) + 127) // 255
            out[i + 3] = 255
    return bytes(out)


def resize_box(w, h, rgba, size):
    """Area-average (downscale) or nearest (upscale) resampling to size x size, in Python."""
    out = bytearray(size * size * 4)
    for oy in range(size):
        y0, y1 = oy * h // size, max(oy * h // size + 1, (oy + 1) * h // size)
        for ox in range(size):
            x0, x1 = ox * w // size, max(ox * w // size + 1, (ox + 1) * w // size)
            acc = [0, 0, 0, 0]
            for y in range(y0, y1):
                base = (y * w) * 4
                for x in range(x0, x1):
                    p = base + x * 4
                    a = rgba[p + 3]
                    acc[0] += rgba[p] * a
                    acc[1] += rgba[p + 1] * a
                    acc[2] += rgba[p + 2] * a
                    acc[3] += a
            n = (y1 - y0) * (x1 - x0)
            o = (oy * size + ox) * 4
            if acc[3]:
                out[o:o + 4] = bytes((acc[0] // acc[3], acc[1] // acc[3], acc[2] // acc[3], acc[3] // n))
    return bytes(out)


def resized(path, size, tmp):
    """(size, size, RGBA) of the image at `path` scaled to size x size."""
    w, h, rgba = read_png(path)
    if (w, h) == (size, size):
        return w, h, rgba
    out = os.path.join(tmp, f"resized_{size}.png")
    if shutil.which("sips"):
        subprocess.run(["sips", "-s", "format", "png", "-z", str(size), str(size), path, "--out", out],
                       check=True, capture_output=True)
        return read_png(out)
    try:
        from PIL import Image  # optional
        Image.open(path).convert("RGBA").resize((size, size), Image.LANCZOS).save(out)
        return read_png(out)
    except ImportError:
        return size, size, resize_box(w, h, rgba, size)


def checked(path):
    w, h, _ = read_png(path)
    if w != h:
        sys.exit(f"ship: {path} is {w}x{h}; an app icon must be square")
    if w < STORE_SIZE:
        warn(f"{path} is {w}x{h}; the stores want {STORE_SIZE}x{STORE_SIZE} (it is scaled up)")
    return path


# ---- iOS ----

def ios(path, app, sim, min_os, info):
    """Compiles the AppIcon catalog into `app` and merges actool's Info.plist keys into `info`."""
    import plistlib
    checked(path)
    with tempfile.TemporaryDirectory() as tmp:
        w, h, rgba = resized(path, STORE_SIZE, tmp)
        iconset = os.path.join(tmp, "Assets.xcassets", "AppIcon.appiconset")
        os.makedirs(iconset)
        with open(os.path.join(tmp, "Assets.xcassets", "Contents.json"), "w") as f:
            f.write('{"info": {"author": "mce", "version": 1}}\n')
        with open(os.path.join(iconset, "Contents.json"), "w") as f:
            f.write('{"images": [{"filename": "icon.png", "idiom": "universal", "platform": "ios", '
                    '"size": "1024x1024"}], "info": {"author": "mce", "version": 1}}\n')
        write_png(os.path.join(iconset, "icon.png"), w, h, flatten(w, h, rgba, edge_colour(w, h, rgba)), alpha=False)
        partial = os.path.join(tmp, "partial.plist")
        cmd = ["xcrun", "actool", os.path.join(tmp, "Assets.xcassets"), "--compile", app,
               "--platform", "iphonesimulator" if sim else "iphoneos", "--minimum-deployment-target", min_os,
               "--app-icon", "AppIcon", "--output-partial-info-plist", partial,
               "--target-device", "iphone", "--target-device", "ipad", "--output-format", "human-readable-text",
               "--errors", "--warnings"]
        print("+ xcrun actool ... --app-icon AppIcon", flush=True)
        out = subprocess.run(cmd, capture_output=True, text=True)
        if out.returncode != 0 or "error:" in out.stdout:
            sys.exit(f"ship: actool failed:\n{out.stdout}{out.stderr}")
        with open(partial, "rb") as f:
            info.update(plistlib.load(f))
    if not os.path.isfile(os.path.join(app, "Assets.car")):
        sys.exit("ship: actool did not write Assets.car")


# ---- Android ----

ADAPTIVE_XML = """<?xml version="1.0" encoding="utf-8"?>
<!-- Generated by cmake/mobile/app_icon.py from the game's ship/icon.png. -->
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@color/ic_launcher_background" />
    <foreground android:drawable="@mipmap/ic_launcher_foreground" />
</adaptive-icon>
"""


def android(path, res, store_png):
    """Writes the adaptive icon into the Gradle project's res/ (SDL's default ic_launcher removed)."""
    checked(path)
    for d in os.listdir(res):
        if d.startswith("mipmap"):
            shutil.rmtree(os.path.join(res, d))
    with tempfile.TemporaryDirectory() as tmp:
        w, h, rgba = read_png(path)
        bg = edge_colour(w, h, rgba)
        for name, scale in DENSITIES.items():
            canvas, inner = round(108 * scale), round(72 * scale)
            iw, ih, icon = resized(path, inner, tmp)
            out = bytearray(canvas * canvas * 4)
            off = (canvas - inner) // 2
            for y in range(inner):
                o = ((y + off) * canvas + off) * 4
                out[o:o + inner * 4] = icon[y * inner * 4:(y + 1) * inner * 4]
            d = os.path.join(res, "mipmap-" + name)
            os.makedirs(d)
            write_png(os.path.join(d, "ic_launcher_foreground.png"), canvas, canvas, bytes(out))
        d = os.path.join(res, "mipmap-anydpi-v26")
        os.makedirs(d)
        with open(os.path.join(d, "ic_launcher.xml"), "w") as f:
            f.write(ADAPTIVE_XML)
        values = os.path.join(res, "values")
        os.makedirs(values, exist_ok=True)
        with open(os.path.join(values, "ic_launcher_background.xml"), "w") as f:
            f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
                    f'    <color name="ic_launcher_background">#{bg.hex()}</color>\n</resources>\n')
        sw, sh, srgba = resized(path, 512, tmp)
        write_png(store_png, sw, sh, flatten(sw, sh, srgba, bg))
    print(f"ship: {store_png} (Play listing icon)")
