"""What a game sells and shows, read from its store catalog source (M-MONETIZE steps 3-4).

The packaging scripts (android_app.py, ios_app.py) stamp the store and ad SDKs only into games whose
.mcstore (anywhere under the game's source folder; src/engine/asset/cook/src/store_importer.cpp has
the format) has products or ad placements. The AdMob app ids come from its optional "admob" object,
{"ios": "ca-app-pub-...~...", "android": "ca-app-pub-...~..."}; without one the build gets Google's
test app id, which only ever shows test ads.
"""

import json
import os
import sys

# developers.google.com/admob/{android,ios}/quick-start: the sample app ids that go with the test units.
TEST_APP_ID = {
    "android": "ca-app-pub-3940256099942544~3347511713",
    "ios": "ca-app-pub-3940256099942544~1458002511",
}


def catalogs(art):
    """Paths of the game's .mcstore files."""
    src = art.get("source")
    out = []
    if src and os.path.isdir(src):
        for root, _, files in os.walk(src):
            out += [os.path.join(root, f) for f in sorted(files) if f.endswith(".mcstore")]
    return sorted(out)


def read(art):
    """{"products": bool, "ads": bool, "admob": {"ios": str, "android": str}} for the game."""
    info = {"products": False, "ads": False, "admob": {}}
    for path in catalogs(art):
        try:
            with open(path, encoding="utf-8") as f:
                doc = json.load(f)
        except (OSError, ValueError) as e:
            sys.exit(f"ship: cannot read the store catalog {path}: {e}")
        info["products"] = info["products"] or bool(doc.get("products"))
        info["ads"] = info["ads"] or bool(doc.get("ads"))
        for platform, app_id in (doc.get("admob") or {}).items():
            info["admob"].setdefault(platform, app_id)
    return info


def admob_app_id(info, platform, name):
    """The game's AdMob app id for `platform`, or Google's test one (with a warning)."""
    app_id = info["admob"].get(platform)
    if app_id:
        return app_id
    print(f"ship: warning: {name} has ad placements but no \"admob\": {{\"{platform}\": ...}} app id in its "
          f".mcstore; it gets Google's test app id and shows only test ads", flush=True)
    return TEST_APP_ID[platform]
