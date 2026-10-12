#!/usr/bin/env python3
"""mce story compile's checks (tools/story/story_compile.py), one case per rule: a file that falls
through, letter texts outside the flow, the speaker rule, and/or conditions, the stage along
straight-line flow, undefined labels and variables. Run by ctest (story_compile) or directly."""

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


if __name__ == "__main__":
    unittest.main()
