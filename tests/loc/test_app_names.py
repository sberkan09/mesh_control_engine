#!/usr/bin/env python3
"""cmake/mobile/app_names.py (M-MOBILE step 11a): the app.name of each assets/loc/<locale>.csv becomes the
name under the phone icon in that language (Android values-<qualifier>, iOS <locale>.lproj)."""

import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "cmake", "mobile"))
import app_names  # noqa: E402


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def main():
    assert app_names.normalize("tr_TR.UTF-8") == "tr-TR"
    assert app_names.normalize("zh-hant-tw") == "zh-Hant-TW"
    assert app_names.normalize("strings") is None
    assert app_names.android_qualifier("tr") == "tr"
    assert app_names.android_qualifier("pt-BR") == "pt-rBR"
    assert app_names.android_qualifier("zh-Hant-TW") == "b+zh+Hant+TW"

    with tempfile.TemporaryDirectory() as tmp:
        loc = os.path.join(tmp, "assets", "loc")
        write(os.path.join(loc, "en.csv"), "key,text\nmenu.play,Play\napp.name,Ember Step\n")
        write(os.path.join(loc, "tr.csv"), "﻿key,text\r\napp.name,\"Kor Adım\"\r\n")
        write(os.path.join(loc, "pt-BR.csv"), "key,text\napp.name,\"Passo \"\"Brasa\"\" & Cia\"\n")
        write(os.path.join(loc, "de.csv"), "key,text\nmenu.play,Spielen\n")  # no app.name: keeps the title
        write(os.path.join(loc, "notes.csv"), "key,text\napp.name,ignored\n")  # not a locale
        names = app_names.read(loc)
        assert names == {"en": "Ember Step", "pt-BR": 'Passo "Brasa" & Cia', "tr": "Kor Adım"}, names
        assert app_names.read(os.path.join(tmp, "missing")) == {}

        res = os.path.join(tmp, "res")
        app_names.android(names, res)
        tr_xml = read(os.path.join(res, "values-tr", "strings.xml"))
        assert '<string name="app_name">Kor Adım</string>' in tr_xml, tr_xml
        pt_xml = read(os.path.join(res, "values-pt-rBR", "strings.xml"))
        assert '<string name="app_name">Passo \\"Brasa\\" &amp; Cia</string>' in pt_xml, pt_xml
        assert not os.path.exists(os.path.join(res, "values-de"))

        app = os.path.join(tmp, "Game.app")
        info = {"CFBundleLocalizations": ["en"]}
        app_names.ios(names, app, info)
        tr_strings = read(os.path.join(app, "tr.lproj", "InfoPlist.strings"))
        assert '"CFBundleDisplayName" = "Kor Adım";' in tr_strings, tr_strings
        pt_strings = read(os.path.join(app, "pt-BR.lproj", "InfoPlist.strings"))
        assert '"CFBundleDisplayName" = "Passo \\"Brasa\\" & Cia";' in pt_strings, pt_strings
        assert info["CFBundleLocalizations"] == ["en", "pt-BR", "tr"], info

    print("app_names: ok")


if __name__ == "__main__":
    main()
