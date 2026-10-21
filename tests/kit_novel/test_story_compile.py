#!/usr/bin/env python3
"""mce story compile's checks (tools/story/story_compile.py), one case per rule: a file that falls
through, letter texts outside the flow, the speaker rule, and/or conditions, the stage along
straight-line flow, undefined labels and variables, the translation tables. Run by ctest (story_compile) or directly."""

import csv
import io
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "story"))
import story_compile as sc  # noqa: E402

MANIFEST = """{
  "format": "mce-story", "version": 1, "namespace": "t",
  "characters": ["mira", {"id": "harbour_master", "name": "Yusuf Reis"}],
  "cast": ["mira", "harbour_master"],
  "flags": ["a", "b", "c"], "counters": ["n"], "endings": ["done"], "commands": ["wave"]
}"""


class StoryTest(unittest.TestCase):
    def compile(self, files, manifest=MANIFEST):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            (root / "story").mkdir()
            (root / "story" / "story.json").write_text(manifest, encoding="utf-8")
            for name, text in files.items():
                (root / "story" / name).write_text(text, encoding="utf-8")
            c, _ = sc.compile_project(root, check=True)
            return c

    def localized(self, files, tables, check=False):
        """Compiles with "localize" on and the given CSVs in assets/loc; returns (compiler, en.csv after)."""
        manifest = MANIFEST.replace('"format"', '"localize": {"base": "en"}, "format"')
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            (root / "story").mkdir()
            (root / "assets" / "loc").mkdir(parents=True)
            (root / "story" / "story.json").write_text(manifest, encoding="utf-8")
            for name, text in files.items():
                (root / "story" / name).write_text(text, encoding="utf-8")
            for name, text in tables.items():
                (root / "assets" / "loc" / name).write_text(text, encoding="utf-8")
            c, _ = sc.compile_project(root, check=check)
            base = root / "assets" / "loc" / "en.csv"
            return c, base.read_text(encoding="utf-8") if base.is_file() else ""

    def fails(self, files, needle):
        with self.assertRaises(sc.CompileError) as e:
            self.compile(files)
        self.assertIn(needle, str(e.exception))

    def ops(self, c, kind):
        return [o for o in c.ops if o["kind"] == kind]

    def test_a_file_must_not_fall_through(self):
        self.fails({"a.story": "@label start\nHello.\n@jump next\n", "b.story": "@label next\nThere.\n"}, "falls through")
        self.compile({"a.story": "@label start\nHello.\n@jump next\n", "b.story": "@label next\nThere.\n@end\n"})

    def test_letter_texts_compile_outside_the_flow(self):
        c = self.compile({"a.story": "@label start\n@letter 1\nAfter.\n@end\n"
                                     "@lettertext 1\nDear Mira,\nthe boat.\n@endletter\n"})
        # the letter's page op comes after the flow's @end, so reading on never enters it
        end = next(i for i, o in enumerate(c.ops) if o["kind"] == "End")
        pages = [i for i, o in enumerate(c.ops) if o["kind"] == "LetterPage"]
        self.assertTrue(pages)
        self.assertTrue(all(i > end for i in pages))
        self.fails({"a.story": "@label start\n@letter 2\n@end\n"}, "@letter 2 has no @lettertext")

    def test_a_speaker_is_a_declared_character(self):
        c = self.compile({"a.story": "@label start\nNote: the tide.\nYusuf: Morning.\nMira: Hi.\nMiro: typo.\n@end\n"})
        says = self.ops(c, "Say")
        self.assertEqual(says[0]["a"], sc.NONE)  # Note: narration
        self.assertEqual(says[1]["a"], 1)        # Yusuf, the harbour master's first name
        self.assertEqual(says[2]["a"], 0)
        self.assertEqual(says[3]["a"], sc.NONE)
        self.assertTrue(any("did you mean the speaker 'mira'" in w for w in c.warnings))

    def test_conditions_and_binds_tighter_than_or(self):
        c = self.compile({"a.story": "@label start\n@if a and b or !c -> x\n@if n >= 2 and n != 5 -> x\nNo.\n@end\n"
                                     "@label x\nYes.\n@end\n"})
        ifs = self.ops(c, "If")
        first = c.clauses[ifs[0]["a"]:ifs[0]["a"] + 3]
        self.assertEqual([sc.TESTS[k["test"]] for k in first], ["Set", "Set", "Clear"])
        self.assertEqual([sc.JOINS[k["join"]] for k in first], ["And", "Or", "End"])
        second = c.clauses[ifs[1]["a"]:ifs[1]["a"] + 2]
        self.assertEqual([(sc.TESTS[k["test"]], k["n"]) for k in second], [("Ge", 2), ("Ne", 5)])
        self.fails({"a.story": "@label start\n@if a and -> start\n@end\n"}, "condition")

    def test_the_stage_along_straight_line_flow(self):
        self.fails({"a.story": "@label start\n@show mira left\n@show harbour_master left\nHi.\n@end\n"}, "the slot holds mira")
        self.fails({"a.story": "@label start\n@show mira left\n@show mira right\nHi.\n@end\n"}, "already in the left slot")
        self.compile({"a.story": "@label start\n@show mira left\n@hide mira\n@show harbour_master left\nHi.\n@end\n"})

    def test_names_resolve_or_fail(self):
        self.fails({"a.story": "@label start\n@jump nowher\n@label nowhere\n@end\n"}, "did you mean 'nowhere'")
        self.fails({"a.story": "@label start\n@flag aa\n@end\n"}, "unknown variable 'aa'")
        self.fails({"a.story": "@label start\n@dance\n@end\n"}, "unknown statement @dance")
        self.fails({"a.story": "@label begin\n@end\n"}, "no 'start' label")

    def test_tags_and_commands(self):
        c = self.compile({"a.story": "@label start\nMira: Hello. #greet #portrait=warm\n@wave 3 4\n@end\n"})
        tags = [c.text[o["a"]] for o in self.ops(c, "Tag")]
        self.assertEqual(tags, ["greet", "portrait=warm"])
        cmd = self.ops(c, "Command")[0]
        self.assertEqual((cmd["a"], cmd["b"], cmd["c"]), (0, 3, 4))

    def test_the_text_font_reaches_the_program_as_a_pack_path(self):
        # The stage draws the box with the font the pages were wrapped with, not the pack's first font
        # (an ASCII font there lost the Turkish letters on phones).
        story = {"a.story": "@label start\nMira: Hello.\n@end\n"}
        manifest = MANIFEST.replace('"format"', '"text": {"font": "assets/fonts/Body.ttf"}, "format"')
        self.assertEqual(self.compile(story, manifest).m.text_font, "fonts/Body.ttf")
        self.assertIsNone(self.compile(story).m.text_font)

    def test_the_base_table_is_written_beside_the_game_rows(self):
        story = {"a.story": "@label start\nMira: Hello there.\nMira:\n  > \"Go\" -> x\n  > \"Stay\" -> x\n@label x\n@end\n"}
        c, en = self.localized(story, {"en.csv": "key,text\nmenu.play,Play\nstory.dead,Gone\n"})
        rows = dict(csv.reader(io.StringIO(en)))
        self.assertEqual(rows["menu.play"], "Play")           # the game's own rows stay
        self.assertNotIn("story.dead", rows)                 # a story row the script dropped goes
        self.assertEqual(rows["story.name.mira"], "Mira")
        self.assertEqual(rows["story.ending.done"], "Done")   # the titles the kit reads from the tables (SBA-59)
        self.assertNotIn("story.title", rows)                # no "title": the folder's name is not translated
        self.assertNotIn("story.extras", rows)               # no extras row: no key
        self.assertEqual(rows[sc.loc_key("Hello there.")], "Hello there.")
        self.assertEqual(rows[sc.loc_key("Go")], "Go")
        # --check fails while the table is stale, and passes once it is written
        with self.assertRaises(sc.CompileError) as e:
            self.localized(story, {"en.csv": "key,text\n"}, check=True)
        self.assertIn("out of date", str(e.exception))
        self.localized(story, {"en.csv": en}, check=True)

    def test_a_translation_keeps_the_page_count_or_falls_back(self):
        long = " ".join(["word"] * 60)
        story = {"a.story": f"@label start\nMira: Short.\nMira: {long}\n@end\n"}
        c, en = self.localized(story, {})
        tr = (f"key,text\n{sc.loc_key('Short.')},{long} {long} {long}\n"
              f"{sc.loc_key(long)},{' '.join(['kelime'] * 50)}\n")
        c, _ = self.localized(story, {"en.csv": en, "tr.csv": tr})
        self.assertTrue(any("'Short.' wraps to" in w for w in c.warnings))
        self.assertEqual(c.loc_stats, [("tr", 1, 2, 0, 0)])
        tag, tr_pages, _, names = c.loc_tables[0]
        long_pages = next(p for t, p in c.loc_says if t == long)
        self.assertGreater(len(long_pages), 1)
        self.assertEqual(sorted(tr_pages), sorted(long_pages))  # the same pages, translated
        self.assertEqual(names, [0, 0])                        # no names translated: base shows


if __name__ == "__main__":
    unittest.main()
