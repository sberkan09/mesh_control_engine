#!/usr/bin/env python3
"""Collects one M-AGENT demo run into the record (docs/research/m-agent-demo/<round>/<run>/).

    record.py <run dir> <out dir> [--cut 1500]

For each agent under <run dir>/agents/<name>/: transcript.jsonl with long tool texts cut to --cut
characters and long lists to their first items (the full ones stay with the run), and contact sheets of the images the agent looked at
(sheet_<n>.png, 24 thumbnails each, numbered as the transcript's image paths). The run's prompts and
summaries are copied, and the project's friction.jsonl as friction.md (through `mce friction`).
Needs Pillow for the sheets.
"""

import argparse
import json
import pathlib
import shutil
import subprocess
import sys

from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parents[2]
THUMB_W, THUMB_H, COLUMNS, PER_SHEET = 320, 180, 4, 24
LIST_KEEP = 40  # items a cut list keeps (input scripts run to thousands of segments)


def cut(value, limit):
    """`value` with every string longer than `limit` and every list longer than LIST_KEEP shortened."""
    if isinstance(value, str):
        return value if len(value) <= limit else value[:limit] + f" [... {len(value) - limit} characters cut]"
    if isinstance(value, list):
        kept = [cut(v, limit) for v in value[:LIST_KEEP]]
        return kept + ([f"[... {len(value) - LIST_KEEP} more items cut]"] if len(value) > LIST_KEEP else [])
    if isinstance(value, dict):
        return {k: cut(v, limit) for k, v in value.items()}
    return value


def transcript(src, dst, limit):
    calls = 0
    with open(src, encoding="utf-8") as f, open(dst, "w", encoding="utf-8") as out:
        for line in f:
            try:
                entry = json.loads(line)
            except json.JSONDecodeError:
                continue
            result = entry.get("response", {}).get("result")
            # structuredContent repeats the text content as JSON: keep it only when small.
            if isinstance(result, dict) and len(json.dumps(result.get("structuredContent", ""))) > limit:
                result.pop("structuredContent", None)
            out.write(json.dumps(cut(entry, limit), ensure_ascii=False) + "\n")
            calls += 1
    return calls


def sheets(images, out_dir):
    files = sorted(images.glob("*.png"))
    for n in range(0, len(files), PER_SHEET):
        batch = files[n:n + PER_SHEET]
        rows = (len(batch) + COLUMNS - 1) // COLUMNS
        sheet = Image.new("RGB", (COLUMNS * THUMB_W, rows * (THUMB_H + 18)), (24, 24, 28))
        draw = ImageDraw.Draw(sheet)
        for i, p in enumerate(batch):
            img = Image.open(p).convert("RGB")
            img.thumbnail((THUMB_W, THUMB_H))
            x, y = (i % COLUMNS) * THUMB_W, (i // COLUMNS) * (THUMB_H + 18)
            sheet.paste(img, (x + (THUMB_W - img.width) // 2, y + 18 + (THUMB_H - img.height) // 2))
            draw.text((x + 4, y + 3), p.name, fill=(220, 220, 220))
        sheet.save(out_dir / f"sheet_{n // PER_SHEET + 1}.png", optimize=True)
    return len(files)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", type=pathlib.Path)
    ap.add_argument("out", type=pathlib.Path)
    ap.add_argument("--cut", type=int, default=1500)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    for f in sorted(a.run.glob("*.md")):
        shutil.copy(f, a.out / f.name)
    for agent in sorted((a.run / "agents").iterdir()):
        if not (agent / "transcript.jsonl").is_file():
            continue
        out = a.out / agent.name
        out.mkdir(exist_ok=True)
        calls = transcript(agent / "transcript.jsonl", out / "transcript.jsonl", a.cut)
        shots = sheets(agent / "images", out) if (agent / "images").is_dir() else 0
        print(f"{agent.name}: {calls} calls, {shots} images")
    for project in sorted(p.parent for p in a.run.glob("*/friction.jsonl")):
        md = subprocess.run([sys.executable, str(ROOT / "tools/mce_cli/mce.py"), "friction", str(project)],
                            capture_output=True, text=True, check=True).stdout
        (a.out / "friction.md").write_text(md, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
