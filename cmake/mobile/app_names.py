#!/usr/bin/env python3
"""Localized app names for the phone builds (M-MOBILE step 11a, issue #146).

The name under the icon comes from the game's string tables: the `app.name` key of each
assets/loc/<locale>.csv (the same files the cook turns into CHNK_STR). A table without the key, or a
game without tables, keeps the one title the artifact already has.

    android(names, res_dir)       res/values-<qualifier>/strings.xml per locale ("pt-BR" -> values-pt-rBR)
    ios(names, app_dir, info)     <locale>.lproj/InfoPlist.strings per locale + CFBundleLocalizations

`python3 cmake/mobile/app_names.py <assets/loc dir>` prints what a build would stamp.
"""

import csv
import io
import os
import re
import sys

KEY = "app.name"


def normalize(tag):
    """'tr_TR.UTF-8' -> 'tr-TR', the way mce/loc/loc.h normalizes; None when it is not a language tag."""
    m = re.match(r"^([A-Za-z]{2,3})((?:[-_][A-Za-z0-9]{1,8})*)", tag)
    if not m:
        return None
    rest = m.group(2)
    end = m.end()
    if end < len(tag) and tag[end] not in ".@":
        return None
    out = [m.group(1).lower()]
    for sub in re.split(r"[-_]", rest)[1:]:
        if len(sub) == 2:
            out.append(sub.upper())
        elif len(sub) == 4:
            out.append(sub[0].upper() + sub[1:].lower())
        else:
            out.append(sub.lower())
    return "-".join(out)


def read(loc_dir):
    """{locale: app name} from the `app.name` rows of <loc_dir>/<locale>.csv, sorted by locale."""
    names = {}
    if not loc_dir or not os.path.isdir(loc_dir):
        return names
    for fname in sorted(os.listdir(loc_dir)):
        stem, ext = os.path.splitext(fname)
        tag = normalize(stem) if ext.lower() == ".csv" else None
        if not tag or len(tag) != len(stem):
            continue
        with open(os.path.join(loc_dir, fname), encoding="utf-8-sig", newline="") as f:
            rows = list(csv.reader(io.StringIO(f.read())))
        if not rows or [c.strip() for c in rows[0][:2]] != ["key", "text"]:
            continue
        for row in rows[1:]:
            if len(row) >= 2 and row[0].strip() == KEY and row[1].strip():
                names[tag] = row[1].strip()
                break
    return dict(sorted(names.items()))


def android_qualifier(tag):
    """The res/values-* qualifier: 'tr' -> 'tr', 'pt-BR' -> 'pt-rBR', 'zh-Hant-TW' -> 'b+zh+Hant+TW'."""
    parts = tag.split("-")
    if len(parts) == 1:
        return parts[0]
    if len(parts) == 2 and len(parts[1]) == 2:
        return f"{parts[0]}-r{parts[1]}"
    return "b+" + "+".join(parts)


def xml_escape(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("'", "\\'")
            .replace('"', '\\"'))


def android(names, res_dir):
    for tag, name in names.items():
        values = os.path.join(res_dir, "values-" + android_qualifier(tag))
        os.makedirs(values, exist_ok=True)
        with open(os.path.join(values, "strings.xml"), "w", encoding="utf-8") as f:
            f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
                    f'    <string name="app_name">{xml_escape(name)}</string>\n</resources>\n')


def strings_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def ios(names, app_dir, info):
    for tag, name in names.items():
        lproj = os.path.join(app_dir, tag + ".lproj")
        os.makedirs(lproj, exist_ok=True)
        with open(os.path.join(lproj, "InfoPlist.strings"), "w", encoding="utf-8") as f:
            f.write(f'"CFBundleDisplayName" = "{strings_escape(name)}";\n"CFBundleName" = "{strings_escape(name)}";\n')
    if names:
        info["CFBundleLocalizations"] = sorted(set(info.get("CFBundleLocalizations", [])) | set(names))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: app_names.py <assets/loc dir>")
    for tag, name in read(sys.argv[1]).items():
        print(f"{tag}\t{android_qualifier(tag)}\t{name}")
