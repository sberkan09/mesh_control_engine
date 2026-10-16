#!/usr/bin/env python3
# python3 gen_art.py assets/textures   (needs Pillow)
# Generates the skel2d_demo atlas (repository content, no outside art): the parts of a cut-out
# character, laid out so every slot of assets/skeletons/courier.mcskel2d is one rect of one texture
# (slots on one texture keep exact draw order). 128 x 128 px, v = 0 at the top:
#   torso  (0,0)-(32,48)     face      (32,0)-(64,32)    limb  (64,0)-(96,16)
#   foot   (96,0)-(120,12)   face_wow  (32,32)-(64,64)   cape  (0,64)-(64,128)
import sys
from PIL import Image, ImageDraw

out = sys.argv[1]
img = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

def box(x0, y0, x1, y1, fill, edge):
    d.rectangle([x0, y0, x1 - 1, y1 - 1], fill=edge)
    d.rectangle([x0 + 1, y0 + 1, x1 - 2, y1 - 2], fill=fill)

# Torso: a coat with a belt and a pocket.
box(0, 0, 32, 48, (196, 86, 70, 255), (96, 40, 40, 255))
d.rectangle([1, 30, 30, 33], fill=(70, 44, 38, 255))
d.rectangle([12, 29, 19, 34], fill=(230, 200, 90, 255))
d.rectangle([5, 12, 12, 19], fill=(168, 70, 58, 255))
# Faces: calm, then surprised.
for oy, mouth in ((0, "calm"), (32, "wow")):
    box(32, oy, 64, oy + 32, (240, 205, 170, 255), (110, 70, 50, 255))
    d.rectangle([32, oy, 63, oy + 9], fill=(60, 40, 38, 255))  # hair
    for ex in (46, 55):
        d.rectangle([ex, oy + 14, ex + 2, oy + 19 if mouth == "calm" else oy + 21], fill=(30, 30, 40, 255))
    if mouth == "calm":
        d.rectangle([47, oy + 25, 55, oy + 26], fill=(150, 70, 70, 255))
    else:
        d.rectangle([49, oy + 24, 53, oy + 29], fill=(120, 40, 50, 255))
# Limb: a sleeve with a lighter stripe, tinted per slot.
box(64, 0, 96, 16, (200, 205, 215, 255), (90, 95, 110, 255))
d.rectangle([66, 6, 93, 9], fill=(235, 238, 245, 255))
# Foot: a boot.
box(96, 0, 120, 12, (120, 84, 60, 255), (50, 36, 30, 255))
d.rectangle([97, 8, 118, 10], fill=(80, 56, 42, 255))
# Cape: a red gradient with a hem.
for y in range(64, 128):
    t = (y - 64) / 63.0
    c = (int(170 - 60 * t), int(40 + 10 * t), int(60 - 10 * t), 255)
    d.line([(0, y), (63, y)], fill=c)
d.rectangle([0, 122, 63, 127], fill=(236, 190, 70, 255))
d.rectangle([0, 64, 1, 127], fill=(90, 24, 40, 255))
d.rectangle([62, 64, 63, 127], fill=(90, 24, 40, 255))
img.save(f"{out}/courier.png")
