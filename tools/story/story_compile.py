#!/usr/bin/env python3
"""mce story compile: a project's script (story/*.story) and manifest (story/story.json) into a
C++ header holding an mce::novel::Program (mce/kit/novel.h), plus story/REPORT.md.

    python3 tools/story/story_compile.py <project>            # writes the header and the report
    python3 tools/story/story_compile.py <project> --check    # validate only
    mce story compile <project> [--check]                     # the same, from the SDK

The manifest names everything the script may use, so a typo fails here and not at run time:

    {
      "format": "mce-story", "version": 1,
      "namespace": "my_novel::story",                 the header's C++ namespace
      "output": "games/my_novel/module/story_data.h", relative to the project
      "title": "My Novel", "extras": "Letters",       the title screen's name and third row ("" = none)
      "characters": ["deniz", {"id": "yusuf", "name": "Yusuf Reis"}],   speakers (name: the tag's text)
      "cast": ["yusuf"],                              who has sprites (textures/cast/<id>_<expr>_<pose>.png)
      "expressions": ["neutral", "warm"], "poses": ["stand"], "slots": ["left", "center", "right"],
      "locations": ["pier"],                          textures/bg/<location>_<layer digit>[_<note>].png ("none" is added)
      "times": [{"id": "noon", "paper": [1,1,1,1], "cast": [...], "ambient": [...], "lamp": [...],
                 "lamp_pos": [0, 5.5], "lamp_intensity": 1.0}],     or plain names (a white look)
      "luts": ["neutral"],                            textures/lut_<name>.png
      "music": ["harbour"], "ambience": ["sea"], "sfx": ["page"],     audio/<kind>/<id>.wav ("none" is added)
      "timelines": ["harbour_intro"],                 timelines/<id>.mctimeline, played by @cutscene
      "ui_sfx": {"page": "page", "move": "move", "confirm": "confirm", "letter": "envelope"},
      "endings": [{"id": "open_water", "title": "Open Water"}],
      "tags": ["first", "leave"],                     option tags; an option without one carries the first
      "bot": {"write": ["write", "stay"]},            the bot's preference per policy (default: the tag itself)
      "flags": ["met_mira", "delivered.1"],           0/1 variables (at most 256)
      "counters": ["day", {"name": "trust.mira", "min": -3, "max": 6}],   i16 variables (at most 64)
      "commands": ["drawer"],                         @drawer runs the game's NovelHooks::command
      "title_screen": {"location": "pier", "time": "evening", "lut": "warm", "music": "harbour", "ambience": "sea"},
      "background_normal_map": "textures/paper_normal.png",
      "text": {"font": "assets/fonts/X.ttf", "px": 32, "line_px": 860, "lines_per_page": 4},
      "letters": {"font": "assets/fonts/Y.ttf", "px": 30, "line_px": 820, "lines_per_page": 12},
      "ink": {"speech": [r,g,b,a], "narration": [...], "row": [...], "row_selected": [...],
              "row_disabled": [...], "row_tint": [...]},
      "localize": {"base": "en"}                      translate the story (below; default: off)
    }

The script, one statement per line:

    # comment
    @scene <label> [bg=<location>] [time=<time>] [lut=<lut>] [music=<id|none>] [ambience=<id|none>]
    @label <name>        @jump <label>        @call <label>        @return        @end
    @bg <location> [time] [lut]     @music <id>|none     @ambience <id>|none     @sfx <id>
    @show <char> <slot> [expr] [pose]     @hide <char>|all     @expr <char> <expr>     @pose <char> <pose>
    Speaker: text (continuation lines are indented)       a paragraph with no speaker is narration
    Speaker(expr): text #tag #portrait=x                  trailing #tags reach the page (dialogue)
    Speaker:                                              a choice: the option lines follow
      > "option text" [condition] {tag} -> label
      >? "hidden unless the condition holds" [condition] -> label
    @set <var> <+n|-n|=n>     @flag <var>     @unflag <var>     @if <condition> -> label
    @letter <n>     @lettertext <n> ... @endletter     @checkpoint (or @journal)     @ending <id>
    @fade     @wait <seconds>     @cutscene <timeline>     @<command> [args...]

Conditions: `flag`, `!flag`, `var >= n` (also <=, ==, !=, >, <), joined by `and` / `or` (and
binds tighter). A `Name:` prefix is a speaker only when Name is a declared character; anything
else (`Note: ...`) is narration.

Checks: labels defined once and every target defined, a 'start' label, every @letter has its
text, no file falls through into the next (its last statement is @end, @jump or @return),
letter texts are reached only by @letter (they compile outside the flow), the stage (a slot
named in "slots", a character never in two slots, no @show over a slot that holds someone else
along straight-line flow), every ending reachable from start (a warning), lines and options
that fit (TextLabel holds 63 bytes).

Text is wrapped and paginated here with the cooked font's metrics (Pillow measures the TTF at the
baked size; without Pillow an average glyph width), so the runtime never measures text.

Translation (M-MOBILE step 11e), on with "localize": every text statement, option and speaker name
gets a key in the project's string tables (assets/loc/<locale>.csv, mce/loc/string_table.h):

    story.title, story.extras     the title screen's name (when story.json names one) and third row
    story.ending.<id>             an ending's title (the kit reads these three from the tables at
                                  runtime, novel::kit_text; its own words are novel.* keys)
    story.name.<character>        a speaker's name tag
    story.<8 hex digits>          a statement or an option: FNV-1a of its text, so an edited line gets
                                  a new key and its stale translations stop showing

The compile writes those rows into the base table (assets/loc/<base>.csv, keeping the rows the game
wrote itself; --check fails while it is out of date) and reads them back from every other table,
wrapped and paginated like the base text. A translated statement keeps its page count (the run is
the same in every language; only what the box shows changes): one that cannot fill as many pages,
or overflows them, is a warning and shows in the base language. The header carries a table per
language (Program::locales); the kit picks the one the player's language chain names
(novel::story_locale). Letters are not translated yet.
"""

import argparse
import csv
import difflib
import io
import glob
import json
import pathlib
import re
import sys

MAX_LINE_BYTES = 62  # TextLabel.text is a FixedString<64>

OPS = ["Nop", "Say", "Choice", "Jump", "Call", "Return", "If", "Add", "Set", "Bg", "Music", "Ambience", "Sfx", "Show",
       "Hide", "Expr", "Pose", "Letter", "LetterPage", "Checkpoint", "Ending", "End", "Fade", "Wait", "Command", "Tag", "Cutscene"]
TESTS = ["Always", "Set", "Clear", "Ge", "Le", "Eq", "Ne", "Gt", "Lt"]
JOINS = ["End", "And", "Or"]
NONE = 0xFFFF
COUNTER_BIT = 0x8000
TERMINATORS = ("End", "Jump", "Return")
BUILTIN = {"scene", "label", "bg", "music", "ambience", "sfx", "show", "hide", "expr", "pose", "set", "flag", "unflag",
           "if", "jump", "call", "return", "letter", "lettertext", "endletter", "checkpoint", "journal", "fade",
           "wait", "cutscene", "ending", "end"}


LOCALE_RE = re.compile(r"[A-Za-z]{2,3}(?:[-_][A-Za-z0-9]{2,8})*")


class CompileError(Exception):
    pass


def loc_key(text):
    """story.<FNV-1a 32 of the UTF-8 text, 8 hex digits>: the key of a statement or an option."""
    h = 0x811C9DC5
    for b in text.encode("utf-8"):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return f"story.{h:08x}"


def read_table(path):
    """[(key, text)] of a string table CSV (header 'key,text'), in file order."""
    rows = []
    with open(path, encoding="utf-8-sig", newline="") as f:
        for i, row in enumerate(csv.reader(f)):
            if i == 0 and row[:2] == ["key", "text"]:
                continue
            if len(row) >= 2 and row[0].strip():
                rows.append((row[0].strip(), row[1]))
    return rows


def table_text(rows):
    out = io.StringIO()
    w = csv.writer(out, lineterminator="\n")
    w.writerow(["key", "text"])
    for k, t in rows:
        w.writerow([k, t])
    return out.getvalue()


# ---- text measuring ----------------------------------------------------------------------------

class Measure:
    def __init__(self, path, px):
        self.font = None
        self.px = px
        if path and pathlib.Path(path).is_file():
            try:
                from PIL import ImageFont
                self.font = ImageFont.truetype(str(path), px)
            except Exception:  # noqa: BLE001 - no Pillow: a measured average per character
                self.font = None

    def width(self, text):
        if self.font is not None:
            return float(self.font.getlength(text))
        return len(text) * self.px * 0.42


def wrap(text, measure, max_px, max_bytes):
    words = text.split()
    lines, cur = [], ""
    for w in words:
        cand = (cur + " " + w) if cur else w
        if cur and (measure.width(cand) > max_px or len(cand.encode("utf-8")) > max_bytes):
            lines.append(cur)
            cur = w
        else:
            cur = cand
        while len(cur.encode("utf-8")) > max_bytes or measure.width(cur) > max_px:  # one very long word
            cut = len(cur) - 1
            while cut > 1 and (len(cur[:cut].encode("utf-8")) > max_bytes or measure.width(cur[:cut]) > max_px):
                cut -= 1
            lines.append(cur[:cut])
            cur = cur[cut:]
    if cur:
        lines.append(cur)
    return lines or [""]


# ---- the manifest ------------------------------------------------------------------------------

def color(v, default):
    if v is None:
        return default
    if not (isinstance(v, list) and len(v) in (3, 4) and all(isinstance(x, (int, float)) for x in v)):
        raise CompileError(f"story.json: a colour must be [r, g, b] or [r, g, b, a], got {v!r}")
    return [float(x) for x in v] + ([1.0] if len(v) == 3 else [])


def names(m, key, none=False, default=None):
    v = m.get(key, default if default is not None else [])
    if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
        raise CompileError(f"story.json: '{key}' must be a list of names")
    if none and (not v or v[0] != "none"):
        v = ["none"] + v
    if len(set(v)) != len(v):
        raise CompileError(f"story.json: '{key}' names something twice")
    return v


class Manifest:
    def __init__(self, project):
        self.project = project
        path = project / "story" / "story.json"
        if not path.is_file():
            raise CompileError(f"{path} is missing (the story manifest)")
        try:
            m = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            raise CompileError(f"story.json: {e}") from e
        if m.get("format") != "mce-story":
            raise CompileError("story.json: \"format\" must be \"mce-story\"")
        self.namespace = m.get("namespace", "story")
        if not re.fullmatch(r"[A-Za-z_]\w*(::[A-Za-z_]\w*)*", self.namespace):
            raise CompileError(f"story.json: bad namespace '{self.namespace}'")
        self.output = project / m.get("output", "story_data.h")
        self.report = project / m.get("report", "story/REPORT.md")
        self.sources = m.get("sources", ["story/*.story"])
        self.start = m.get("start", "start")
        self.title = m.get("title", project.name)
        self.title_set = "title" in m  # a title the manifest names (the folder's name is not translated)
        self.extras = m.get("extras", "")
        self.characters, self.display = [], []
        for c in m.get("characters", []):
            cid, name = (c, c.title()) if isinstance(c, str) else (c["id"], c.get("name", c["id"].title()))
            if not re.fullmatch(r"[a-z][a-z0-9_]*", cid):
                raise CompileError(f"story.json: character id '{cid}' must be lower_snake_case")
            self.characters.append(cid)
            self.display.append(name)
        if len(set(self.characters)) != len(self.characters):
            raise CompileError("story.json: a character is declared twice")
        if len(self.characters) > 254:
            raise CompileError("story.json: at most 254 characters")
        self.cast = names(m, "cast")
        for c in self.cast:
            if c not in self.characters:
                raise CompileError(f"story.json: cast member '{c}' is not a character")
        if len(self.cast) > 16:
            raise CompileError("story.json: at most 16 cast members with sprites")
        self.expressions = names(m, "expressions", default=["neutral"])
        self.poses = names(m, "poses", default=["stand"])
        self.slots = names(m, "slots", default=["left", "center", "right"])
        if not 1 <= len(self.slots) <= 4:
            raise CompileError("story.json: 1 to 4 slots")
        self.locations = names(m, "locations", none=True)
        white = [1.0, 1.0, 1.0, 1.0]
        self.times = []
        for t in m.get("times", ["noon"]):
            if isinstance(t, str):
                t = {"id": t}
            self.times.append({"id": t["id"], "paper": color(t.get("paper"), white), "cast": color(t.get("cast"), white),
                               "ambient": color(t.get("ambient"), white), "lamp": color(t.get("lamp"), white),
                               "lamp_pos": t.get("lamp_pos", [0.0, 5.5]), "lamp_intensity": float(t.get("lamp_intensity", 1.0))})
        self.time_names = [t["id"] for t in self.times]
        self.luts = names(m, "luts", default=["neutral"])
        self.music = names(m, "music", none=True)
        self.ambience = names(m, "ambience", none=True)
        self.sfx = names(m, "sfx", none=True)
        self.timelines = names(m, "timelines", none=True)
        self.endings, self.ending_titles = ["none"], [""]
        for e in m.get("endings", []):
            eid, title = (e, e.replace("_", " ").title()) if isinstance(e, str) else (e["id"], e.get("title", e["id"]))
            self.endings.append(eid)
            self.ending_titles.append(title)
        if len(self.endings) > 32:
            raise CompileError("story.json: at most 31 endings")
        self.tags = names(m, "tags", default=["first"])
        self.bot = m.get("bot", {})
        for k, v in self.bot.items():
            for t in [k] + list(v):
                if t not in self.tags:
                    raise CompileError(f"story.json: bot policy names unknown tag '{t}'")
        self.flags = names(m, "flags")
        if len(self.flags) > 256:
            raise CompileError("story.json: at most 256 flags")
        self.counters = []
        for c in m.get("counters", []):
            if isinstance(c, str):
                c = {"name": c}
            self.counters.append((c["name"], int(c.get("min", -32768)), int(c.get("max", 32767))))
        if len(self.counters) > 64:
            raise CompileError("story.json: at most 64 counters")
        all_vars = self.flags + [c[0] for c in self.counters]
        if len(set(all_vars)) != len(all_vars):
            raise CompileError("story.json: a variable is declared twice")
        self.commands = names(m, "commands")
        for c in self.commands:
            if c in BUILTIN:
                raise CompileError(f"story.json: command '{c}' is a built-in statement")
        ui = m.get("ui_sfx", {})
        self.ui_sfx = {k: self.index(self.sfx, ui[k], "sfx") if ui.get(k) else 0 for k in ("page", "move", "confirm", "letter")}
        ts = m.get("title_screen", {})
        self.title_screen = {
            "location": self.index(self.locations, ts.get("location", "none"), "location"),
            "time": self.index(self.time_names, ts.get("time", self.time_names[0]), "time"),
            "lut": self.index(self.luts, ts.get("lut", self.luts[0]), "lut"),
            "music": self.index(self.music, ts.get("music", "none"), "music"),
            "ambience": self.index(self.ambience, ts.get("ambience", "none"), "ambience"),
        }
        self.normal_map = m.get("background_normal_map")
        tx = m.get("text", {})
        lt = m.get("letters", {})
        self.font = project / tx["font"] if tx.get("font") else None
        # The same font as the cooked pack names it (paths under assets/), for the stage to pick it.
        self.text_font = tx["font"][len("assets/"):] if tx.get("font", "").startswith("assets/") else None
        self.px = int(tx.get("px", 32))
        self.line_px = float(tx.get("line_px", 860))
        self.lines_per_page = int(tx.get("lines_per_page", 4))
        self.letter_font = project / lt["font"] if lt.get("font") else self.font
        self.letter_px = int(lt.get("px", self.px))
        self.letter_line_px = float(lt.get("line_px", self.line_px))
        self.letter_lines_per_page = int(lt.get("lines_per_page", 12))
        if not 1 <= self.lines_per_page <= 4:
            raise CompileError("story.json: text.lines_per_page is 1 to 4 (the box has four lines)")
        loc = m.get("localize")
        self.loc_base = None
        if loc:
            loc = loc if isinstance(loc, dict) else {}
            self.loc_base = loc.get("base", "en")
            if not LOCALE_RE.fullmatch(self.loc_base):
                raise CompileError(f"story.json: localize.base '{self.loc_base}' is not a language tag")
            self.loc_dir = project / loc.get("dir", "assets/loc")
        ink = m.get("ink", {})
        self.ink = {
            "speech": color(ink.get("speech"), [0.16, 0.12, 0.1, 1.0]),
            "narration": color(ink.get("narration"), [0.36, 0.29, 0.23, 1.0]),
            "row": color(ink.get("row"), [0.3, 0.255, 0.225, 1.0]),
            "row_selected": color(ink.get("row_selected"), [0.12, 0.102, 0.09, 1.0]),
            "row_disabled": color(ink.get("row_disabled"), [0.55, 0.4675, 0.4125, 1.0]),
            "row_tint": color(ink.get("row_tint"), [1.0, 1.0, 1.0, 1.0]),
        }

    @staticmethod
    def index(table, name, what):
        if name not in table:
            raise CompileError(f"unknown {what} '{name}' (one of {', '.join(table) or 'none declared'})")
        return table.index(name)

    def files(self):
        out = []
        for pattern in self.sources:
            out += sorted(pathlib.Path(p) for p in glob.glob(str(self.project / pattern)))
        return out


# ---- the compiler --------------------------------------------------------------------------------

class Compiler:
    def __init__(self, manifest):
        self.m = manifest
        self.text = [""]
        self.text_index = {"": 0}
        self.ops = []          # dicts {kind, a..f, src}
        self.options = []
        self.clauses = [{"var": 0, "n": 0, "test": 0, "join": 0}]  # 0: always
        self.letter_lines = []
        self.labels = {}       # name -> op index
        self.fixups = []       # (op list, op index, field, label name, src)
        self.option_fixups = []
        self.letters = {}      # n -> label name
        self.letter_ops = []   # the letter texts, linked after the flow
        self.letter_srcs = {}
        self.measure = Measure(manifest.font, manifest.px)
        self.letter_measure = Measure(manifest.letter_font, manifest.letter_px)
        self.words = {}        # file -> words
        self.pages = 0
        self.choices = 0
        self.warnings = []
        self.src = ""
        self.stage = None      # slot -> character along straight-line flow (None: unknown)
        self.pending_tags = []
        self.loc_says = []     # (text, [page id tuples]) of every statement, for translation
        self.loc_options = []  # (text, text id, text2 id) of every option
        self.loc_tables = []   # (locale, pages {base tuple: tuple}, options {(t, t2): (t, t2)}, names [text id])
        self.loc_stats = []    # (locale, statements, of, options, of)

    # -- tables
    def sid(self, s):
        if s not in self.text_index:
            if len(self.text) >= 0xFFFF:
                raise CompileError("more than 65534 distinct strings")
            self.text_index[s] = len(self.text)
            self.text.append(s)
        return self.text_index[s]

    def emit(self, kind, a=0, b=0, c=0, d=0, e=0, f=0, into=None):
        ops = self.ops if into is None else into
        ops.append({"kind": kind, "a": a, "b": b, "c": c, "d": d, "e": e, "f": f, "src": self.src})
        return len(ops) - 1

    def label(self, name):
        if name in self.labels:
            raise CompileError(f"{self.src}: label '{name}' defined twice")
        self.labels[name] = len(self.ops)
        self.stage = None  # flow arrives here from elsewhere

    def ref(self, op, field, name):
        self.fixups.append((self.ops, op, field, name, self.src))

    def idx(self, table, name, what):
        try:
            return Manifest.index(table, name, what)
        except CompileError as e:
            raise CompileError(f"{self.src}: {e}") from None

    def character(self, name):
        return self.idx(self.m.characters, name, "character")

    def var(self, name):
        """A variable reference: a flag index, or COUNTER_BIT | a counter index."""
        if name in self.m.flags:
            return self.m.flags.index(name)
        for i, (n, _, _) in enumerate(self.m.counters):
            if n == name:
                return COUNTER_BIT | i
        close = difflib.get_close_matches(name, self.m.flags + [c[0] for c in self.m.counters], 1)
        raise CompileError(f"{self.src}: unknown variable '{name}'" + (f" (did you mean '{close[0]}'?)" if close else "") +
                           " (declare it in story.json \"flags\" or \"counters\")")

    def condition(self, expr):
        """'a and b or !c' -> a clause index (0 = always)."""
        expr = expr.strip()
        if not expr:
            return 0
        groups = re.split(r"\s+or\s+", expr)
        clauses = []
        for gi, group in enumerate(groups):
            parts = re.split(r"\s+and\s+", group.strip())
            for pi, part in enumerate(parts):
                part = part.strip()
                if not part:
                    raise CompileError(f"{self.src}: empty term in condition '{expr}'")
                mm = re.match(r"^(\S+?)\s*(>=|<=|==|!=|>|<)\s*(-?\d+)$", part)
                if mm:
                    test = {">=": "Ge", "<=": "Le", "==": "Eq", "!=": "Ne", ">": "Gt", "<": "Lt"}[mm.group(2)]
                    var, n = self.var(mm.group(1)), int(mm.group(3))
                elif part.startswith("!") or part.startswith("not "):
                    test, var, n = "Clear", self.var(part[1:].strip() if part.startswith("!") else part[4:].strip()), 0
                else:
                    if not re.fullmatch(r"[\w.]+", part):
                        raise CompileError(f"{self.src}: cannot read condition term '{part}'")
                    test, var, n = "Set", self.var(part), 0
                last_in_group = pi == len(parts) - 1
                join = "And" if not last_in_group else ("Or" if gi < len(groups) - 1 else "End")
                clauses.append({"var": var, "n": n, "test": TESTS.index(test), "join": JOINS.index(join)})
        first = len(self.clauses)
        self.clauses.extend(clauses)
        return first

    def count_words(self, text):
        f = self.src.split(":")[0]
        self.words[f] = self.words.get(f, 0) + len(text.split())

    # -- statements
    def say(self, speaker, text, expr):
        text = re.sub(r"\[pause\]", "", text)
        text = re.sub(r"\s+", " ", text).strip()
        tags = []
        mm = re.search(r"((?:\s+#[\w.=:-]+)+)$", " " + text)
        if mm:
            tags = [t[1:] for t in mm.group(1).split()]
            text = (" " + text)[:mm.start()].strip()
        if not text:
            if tags:
                raise CompileError(f"{self.src}: #tags with no text")
            return
        speaker_id = self.character(speaker) if speaker else NONE
        expr_id = self.idx(self.m.expressions, expr, "expression") if expr else NONE
        for tag in tags:
            self.emit("Tag", self.sid(tag))
        lines = wrap(text, self.measure, self.m.line_px, MAX_LINE_BYTES)
        pages = []
        for i in range(0, len(lines), self.m.lines_per_page):
            page = lines[i:i + self.m.lines_per_page]
            ids = [self.sid(line) for line in page] + [0] * (4 - len(page))
            self.emit("Say", speaker_id, expr_id, ids[0], ids[1], ids[2], ids[3])
            pages.append(tuple(ids))
            self.pages += 1
        self.loc_says.append((text, pages))
        self.count_words(text)

    def choice(self, options):
        if len(options) > 4:
            raise CompileError(f"{self.src}: a choice has at most 4 options")
        first = len(self.options)
        for text, cond_expr, tag, hidden, target, src in options:
            self.src = src
            cond = self.condition(cond_expr)
            tag_id = self.idx(self.m.tags, tag, "tag") if tag else 0
            lines = wrap(text, self.measure, self.m.line_px, MAX_LINE_BYTES)
            if len(lines) > 2:
                raise CompileError(f"{src}: option text wraps to {len(lines)} lines (at most 2): {text}")
            self.options.append({"text": self.sid(lines[0]), "text2": self.sid(lines[1]) if len(lines) > 1 else 0,
                                 "target": 0, "cond": cond, "tag": tag_id, "hidden": 1 if hidden else 0})
            self.loc_options.append((re.sub(r"\s+", " ", text).strip(), self.options[-1]["text"], self.options[-1]["text2"]))
            self.option_fixups.append((len(self.options) - 1, target, src))
            self.count_words(text)
        self.emit("Choice", first, len(options))
        self.choices += 1

    def lettertext(self, n, paragraphs, src):
        if n in self.letters:
            raise CompileError(f"{src}: letter {n} has two @lettertext blocks (the first at {self.letter_srcs[n]})")
        self.letter_srcs[n] = src
        lines = []
        for p in paragraphs:
            if p.strip() == "":
                lines.append("")
            else:
                lines.extend(wrap(re.sub(r"\s+", " ", p.strip()), self.letter_measure, self.m.letter_line_px, MAX_LINE_BYTES))
            self.count_words(p)
        while lines and lines[-1] == "":
            lines.pop()
        # Outside the flow: nothing falls into a letter's text, only @letter calls it.
        self.letters[n] = len(self.letter_ops)
        per = self.m.letter_lines_per_page
        for i in range(0, max(len(lines), 1), per):
            page = lines[i:i + per]
            first = len(self.letter_lines)
            self.letter_lines.extend(self.sid(line) for line in page)
            self.emit("LetterPage", first, len(page), into=self.letter_ops)
        self.emit("Return", into=self.letter_ops)

    # -- the stage along straight-line flow
    def stage_show(self, who, slot):
        if self.stage is None:
            self.stage = {}
            known = False
        else:
            known = True
        holder = self.stage.get(slot)
        if known and holder is not None and holder != who:
            raise CompileError(f"{self.src}: @show {self.m.characters[who]} {self.m.slots[slot]}: the slot holds "
                               f"{self.m.characters[holder]} (@hide {self.m.characters[holder]} first)")
        for s, c in list(self.stage.items()):
            if c == who and s != slot:
                if known:
                    raise CompileError(f"{self.src}: @show {self.m.characters[who]} {self.m.slots[slot]}: "
                                       f"{self.m.characters[who]} is already in the {self.m.slots[s]} slot "
                                       f"(one character, one slot: @hide first to move)")
                del self.stage[s]
        self.stage[slot] = who

    def stage_hide(self, who):
        if who is None:
            self.stage = {}
        elif self.stage is not None:
            for s, c in list(self.stage.items()):
                if c == who:
                    del self.stage[s]

    # -- the parser
    def compile_file(self, path):
        lines = path.read_text(encoding="utf-8").split("\n")
        i = 0
        pending = None  # (speaker, text, expr) being accumulated
        in_letter = None
        self.stage = None
        file_start = len(self.ops)

        def flush():
            nonlocal pending
            if pending:
                self.say(*pending)
                pending = None

        while i < len(lines):
            raw = lines[i]
            self.src = f"{path.name}:{i + 1}"
            line = raw.rstrip()
            i += 1
            if in_letter is not None:
                if line.strip() == "@endletter":
                    self.lettertext(in_letter[0], in_letter[1], in_letter[2])
                    in_letter = None
                else:
                    in_letter[1].append(line)
                continue
            if not line.strip() or line.lstrip().startswith("#"):
                flush()
                continue
            if line.startswith("  ") and pending is not None and not line.lstrip().startswith(">"):
                pending = (pending[0], pending[1] + " " + line.strip(), pending[2])
                continue
            flush()
            stripped = line.strip()
            if stripped.startswith("@"):
                if stripped.startswith("@lettertext"):
                    parts = stripped.split()
                    if len(parts) != 2 or not parts[1].isdigit():
                        raise CompileError(f"{self.src}: want @lettertext <n>")
                    in_letter = (int(parts[1]), [], self.src)
                    continue
                self.directive(stripped)
                continue
            m = re.match(r"^([^\W\d_][\w]*)(?:\(([a-z_]+)\))?:\s*(.*)$", stripped)
            speaker = None
            if m:
                name = m.group(1).lower()
                if name in self.m.characters:
                    speaker = name
                else:
                    by_display = [c for c, d in zip(self.m.characters, self.m.display) if d.split()[0].lower() == name]
                    if by_display:
                        speaker = by_display[0]
            if m and (speaker or (not m.group(3) and i < len(lines) and lines[i].lstrip().startswith(">"))):
                expr, text = m.group(2), m.group(3)
                if text:
                    pending = (speaker, text, expr)
                else:
                    # a choice: the option lines follow
                    options = []
                    while i < len(lines) and lines[i].lstrip().startswith(">"):
                        options.append(self.option(lines[i].strip(), f"{path.name}:{i + 1}"))
                        i += 1
                    if not options:
                        raise CompileError(f"{self.src}: '{m.group(1)}:' with no text and no options")
                    self.choice(options)
                continue
            if m and m.group(2):
                raise CompileError(f"{self.src}: unknown speaker '{m.group(1)}'")
            if m:
                close = difflib.get_close_matches(m.group(1).lower(), self.m.characters, 1, 0.75)
                if close:
                    self.warnings.append(f"{self.src}: '{m.group(1)}:' reads as narration; did you mean the speaker '{close[0]}'?")
            if stripped.startswith(">"):
                raise CompileError(f"{self.src}: an option line outside a choice")
            # narration
            pending = (None, stripped.lstrip("*").strip(), None)
        flush()
        if in_letter is not None:
            raise CompileError(f"{in_letter[2]}: @lettertext {in_letter[0]} has no @endletter")
        if len(self.ops) > file_start and self.ops[-1]["kind"] not in TERMINATORS:
            raise CompileError(f"{path.name}: the file's last statement ({self.ops[-1]['src']}) falls through into the next "
                               f"file; end it with @end, @jump or @return")

    def option(self, text, src):
        m = re.match(r'^>(\?)?\s*"(.*?)"\s*(?:\[(.*?)\])?\s*(?:\{(\w+)\})?\s*->\s*(\S+)\s*$', text)
        if not m:
            raise CompileError(f'{src}: bad option line (want > "text" [condition] {{tag}} -> label): {text}')
        hidden, body, cond, tag, target = m.group(1), m.group(2), m.group(3) or "", m.group(4), m.group(5)
        if hidden and not cond:
            raise CompileError(f"{src}: a hidden option (>?) needs a [condition]")
        return body, cond, tag, bool(hidden), target, src

    def need(self, args, n, usage):
        if len(args) < n:
            raise CompileError(f"{self.src}: want {usage}")

    def directive(self, line):
        parts = line.split()
        cmd, args = parts[0][1:], parts[1:]
        m = self.m
        if cmd == "scene":
            self.need(args, 1, "@scene <label> [bg=..] [time=..] [lut=..] [music=..] [ambience=..]")
            self.label(args[0])
            try:
                kv = dict(a.split("=", 1) for a in args[1:])
            except ValueError:
                raise CompileError(f"{self.src}: @scene options are key=value") from None
            for k in kv:
                if k not in ("bg", "time", "lut", "music", "ambience"):
                    raise CompileError(f"{self.src}: unknown @scene option '{k}'")
            if "bg" in kv:
                self.emit("Bg", self.idx(m.locations, kv["bg"], "location"),
                          self.idx(m.time_names, kv.get("time", m.time_names[min(1, len(m.time_names) - 1)] if "noon" not in m.time_names else "noon"), "time"),
                          self.idx(m.luts, kv.get("lut", m.luts[0]), "lut"))
            if "music" in kv:
                self.emit("Music", self.idx(m.music, kv["music"], "music"))
            if "ambience" in kv:
                self.emit("Ambience", self.idx(m.ambience, kv["ambience"], "ambience"))
        elif cmd == "label":
            self.need(args, 1, "@label <name>")
            self.label(args[0])
        elif cmd == "bg":
            self.need(args, 1, "@bg <location> [time] [lut]")
            default_time = "noon" if "noon" in m.time_names else m.time_names[0]
            self.emit("Bg", self.idx(m.locations, args[0], "location"),
                      self.idx(m.time_names, args[1] if len(args) > 1 else default_time, "time"),
                      self.idx(m.luts, args[2] if len(args) > 2 else m.luts[0], "lut"))
        elif cmd in ("music", "ambience", "sfx"):
            self.need(args, 1, f"@{cmd} <id>")
            table = {"music": m.music, "ambience": m.ambience, "sfx": m.sfx}[cmd]
            self.emit(cmd.title(), self.idx(table, args[0], cmd))
        elif cmd == "cutscene":
            self.need(args, 1, "@cutscene <timeline>")
            if args[0] == "none":
                raise CompileError(f"{self.src}: @cutscene names a timeline from story.json \"timelines\"")
            self.emit("Cutscene", self.idx(m.timelines, args[0], "timeline"))
        elif cmd == "show":
            self.need(args, 2, "@show <character> <slot> [expression] [pose]")
            who = self.character(args[0])
            if args[0] not in m.cast:
                raise CompileError(f"{self.src}: '{args[0]}' has no sprites (add it to story.json \"cast\")")
            slot = self.idx(m.slots, args[1], "slot")
            self.stage_show(who, slot)
            self.emit("Show", slot, who, self.idx(m.expressions, args[2] if len(args) > 2 else m.expressions[0], "expression"),
                      self.idx(m.poses, args[3] if len(args) > 3 else m.poses[0], "pose"))
        elif cmd == "hide":
            self.need(args, 1, "@hide <character>|all")
            who = None if args[0] == "all" else self.character(args[0])
            self.stage_hide(who)
            self.emit("Hide", NONE if who is None else who)
        elif cmd == "expr":
            self.need(args, 2, "@expr <character> <expression>")
            self.emit("Expr", self.character(args[0]), self.idx(m.expressions, args[1], "expression"))
        elif cmd == "pose":
            self.need(args, 2, "@pose <character> <pose>")
            self.emit("Pose", self.character(args[0]), self.idx(m.poses, args[1], "pose"))
        elif cmd == "set":
            self.need(args, 2, "@set <var> <+n|-n|=n>")
            var, delta = args[0], args[1]
            try:
                if delta.startswith("="):
                    self.emit("Set", self.var(var), int(delta[1:]) & 0xFFFF)
                else:
                    self.emit("Add", self.var(var), int(delta) & 0xFFFF)
            except ValueError:
                raise CompileError(f"{self.src}: @set wants +n, -n or =n, got '{delta}'") from None
        elif cmd in ("flag", "unflag"):
            self.need(args, 1, f"@{cmd} <var>")
            self.emit("Set", self.var(args[0]), 1 if cmd == "flag" else 0)
        elif cmd == "if":
            mm = re.match(r"^@if\s+(.*?)\s*->\s*(\S+)$", line)
            if not mm:
                raise CompileError(f"{self.src}: bad @if (want @if <condition> -> label)")
            op = self.emit("If", self.condition(mm.group(1)))
            self.ref(op, "b", mm.group(2))
        elif cmd in ("jump", "call"):
            self.need(args, 1, f"@{cmd} <label>")
            op = self.emit("Jump" if cmd == "jump" else "Call")
            self.ref(op, "a", args[0])
            if cmd == "jump":
                self.stage = None
        elif cmd == "return":
            self.emit("Return")
            self.stage = None
        elif cmd == "letter":
            self.need(args, 1, "@letter <n>")
            if not args[0].isdigit() or int(args[0]) > 254:
                raise CompileError(f"{self.src}: @letter takes a number (1..254)")
            op = self.emit("Letter", int(args[0]))
            self.fixups.append((self.ops, op, "b", f"__letter_{int(args[0])}", self.src))
        elif cmd == "endletter":
            raise CompileError(f"{self.src}: @endletter without @lettertext")
        elif cmd in ("checkpoint", "journal"):
            self.emit("Checkpoint")
        elif cmd == "fade":
            self.emit("Fade")
        elif cmd == "wait":
            self.need(args, 1, "@wait <seconds>")
            self.emit("Wait", int(float(args[0]) * 60))
        elif cmd == "ending":
            self.need(args, 1, "@ending <id>")
            self.emit("Ending", self.idx(m.endings[1:], args[0], "ending") + 1)
        elif cmd == "end":
            self.emit("End")
            self.stage = None
        elif cmd in m.commands:
            vals = []
            for a in args[:5]:
                vals.append(int(a) & 0xFFFF if re.fullmatch(r"-?\d+", a) else self.sid(a))
            if len(args) > 5:
                raise CompileError(f"{self.src}: @{cmd} takes at most 5 arguments")
            vals += [0] * (5 - len(vals))
            self.emit("Command", m.commands.index(cmd), *vals)
        else:
            raise CompileError(f"{self.src}: unknown statement @{cmd}" +
                               (" (a game command must be declared in story.json \"commands\")" if cmd not in BUILTIN else ""))

    # -- linking and checks
    def link(self):
        base = len(self.ops)
        for n, at in self.letters.items():
            self.labels[f"__letter_{n}"] = base + at
        self.ops.extend(self.letter_ops)
        for ops, op, field, name, src in self.fixups:
            if name.startswith("__letter_") and name not in self.labels:
                raise CompileError(f"{src}: @letter {name[9:]} has no @lettertext {name[9:]} block")
            if name not in self.labels:
                close = difflib.get_close_matches(name, list(self.labels), 1)
                raise CompileError(f"{src}: jump to undefined label '{name}'" + (f" (did you mean '{close[0]}'?)" if close else ""))
            ops[op][field] = self.labels[name]
        for idx, name, src in self.option_fixups:
            if name not in self.labels:
                close = difflib.get_close_matches(name, list(self.labels), 1)
                raise CompileError(f"{src}: option to undefined label '{name}'" + (f" (did you mean '{close[0]}'?)" if close else ""))
            self.options[idx]["target"] = self.labels[name]
        if self.m.start not in self.labels:
            raise CompileError(f"no '{self.m.start}' label (the run starts there)")
        if len(self.ops) >= 0xFFFF:
            raise CompileError("more than 65534 statements")

    def reachable(self):
        """Op indices reachable from start following fall-through, jumps, calls, letters and options."""
        seen = set()
        stack = [self.labels[self.m.start]]
        while stack:
            pc = stack.pop()
            while 0 <= pc < len(self.ops) and pc not in seen:
                seen.add(pc)
                op = self.ops[pc]
                k = op["kind"]
                if k == "Jump":
                    pc = op["a"]
                    continue
                if k == "Call":
                    stack.append(op["a"])
                elif k == "If":
                    stack.append(op["b"])
                elif k == "Letter":
                    stack.append(op["b"])
                elif k == "Choice":
                    for o in self.options[op["a"]:op["a"] + op["b"]]:
                        stack.append(o["target"])
                    break
                elif k in ("Return", "End"):
                    break
                pc += 1
        return seen

    def check(self):
        seen = self.reachable()
        unreachable = sorted((name, pc) for name, pc in self.labels.items() if pc not in seen and not name.startswith("__"))
        reached = {self.m.endings[self.ops[pc]["a"]] for pc in seen if self.ops[pc]["kind"] == "Ending"}
        missing = [e for e in self.m.endings[1:] if e not in reached]
        for e in missing:
            self.warnings.append(f"ending '{e}' is not reachable from '{self.m.start}'")
        return unreachable, missing

    # -- translation
    def base_rows(self):
        """The story's rows of the base string table, in script order (names first)."""
        rows, seen = [], set()

        def add(key, text):
            if key in seen:
                return
            seen.add(key)
            rows.append((key, text))
        if self.m.title_set:
            add("story.title", self.m.title)
        if self.m.extras:
            add("story.extras", self.m.extras)
        for eid, title in zip(self.m.endings[1:], self.m.ending_titles[1:]):
            add(f"story.ending.{eid}", title)
        for cid, name in zip(self.m.characters, self.m.display):
            add(f"story.name.{cid}", name)
        keys = {}
        for text, _ in self.loc_says:
            add(loc_key(text), text)
            keys.setdefault(loc_key(text), text)
        for text, _, _ in self.loc_options:
            add(loc_key(text), text)
            keys.setdefault(loc_key(text), text)
        for text in [t for t, _ in self.loc_says] + [t for t, _, _ in self.loc_options]:
            if keys[loc_key(text)] != text:
                raise CompileError(f"two lines share the key {loc_key(text)}: '{keys[loc_key(text)]}' and '{text}' (reword one)")
        return rows

    def base_table_path(self):
        return self.m.loc_dir / f"{self.m.loc_base}.csv"

    def base_table(self):
        """The base CSV with its story rows replaced by this script's ((text, changed))."""
        path = self.base_table_path()
        old = path.read_text(encoding="utf-8-sig") if path.is_file() else ""
        game = [(k, t) for k, t in (read_table(path) if path.is_file() else []) if not k.startswith("story.")]
        new = table_text(game + self.base_rows())
        return new, new != old.replace("\r\n", "\n")

    def localize(self):
        """Reads every other table's story rows into per-language page and option tables."""
        if not self.m.loc_base:
            return
        lpp = self.m.lines_per_page
        tables = sorted(self.m.loc_dir.glob("*.csv")) if self.m.loc_dir.is_dir() else []
        for path in tables:
            tag = path.stem.replace("_", "-")
            if not LOCALE_RE.fullmatch(tag) or tag.lower() == self.m.loc_base.lower():
                continue
            rows = {k: t for k, t in read_table(path) if k.startswith("story.")}
            if not rows:
                continue
            pages, options = {}, {}
            done_says = done_opts = 0
            for text, base_pages in self.loc_says:
                tr = rows.get(loc_key(text))
                if not tr or not tr.strip():
                    continue
                tr = re.sub(r"\s+", " ", tr).strip()
                lines = wrap(tr, self.measure, self.m.line_px, MAX_LINE_BYTES)
                n = len(base_pages)
                if len(lines) > n * lpp or len(lines) < n:
                    self.warnings.append(f"{path.name}: '{text[:40]}' wraps to {len(lines)} lines in {tag}, its "
                                         f"{n} page(s) hold {n}..{n * lpp}; it shows in {self.m.loc_base}")
                    continue
                q, r = divmod(len(lines), n)
                at = 0
                for k, base in enumerate(base_pages):
                    take = q + (1 if k < r else 0)
                    ids = [self.sid(line) for line in lines[at:at + take]] + [0] * (4 - take)
                    at += take
                    pages.setdefault(base, tuple(ids))
                done_says += 1
            for text, t1, t2 in self.loc_options:
                tr = rows.get(loc_key(text))
                if not tr or not tr.strip():
                    continue
                lines = wrap(re.sub(r"\s+", " ", tr).strip(), self.measure, self.m.line_px, MAX_LINE_BYTES)
                if len(lines) > 2:
                    self.warnings.append(f"{path.name}: option '{text[:40]}' wraps to {len(lines)} lines in {tag} "
                                         f"(at most 2); it shows in {self.m.loc_base}")
                    continue
                options.setdefault((t1, t2), (self.sid(lines[0]), self.sid(lines[1]) if len(lines) > 1 else 0))
                done_opts += 1
            names = []
            for cid in self.m.characters:
                tr = rows.get(f"story.name.{cid}", "").strip()
                names.append(self.sid(tr) if tr else 0)
            self.loc_tables.append((tag, pages, options, names))
            self.loc_stats.append((tag, done_says, len(self.loc_says), done_opts, len(self.loc_options)))

    # -- output
    @staticmethod
    def cstr(s):
        out = []
        for b in (s or "").encode("utf-8"):
            if b == 0x22:
                out.append('\\"')
            elif b == 0x5C:
                out.append("\\\\")
            elif 0x20 <= b < 0x7F:
                out.append(chr(b))
            else:
                out.append("\\%03o" % b)
        return '"' + "".join(out) + '"'

    @staticmethod
    def camel(v):
        s = "".join(part[:1].upper() + part[1:] for part in re.split(r"[_.]", v))
        return s if re.match(r"[A-Za-z_]", s) else "_" + s

    @staticmethod
    def ident(v):
        s = re.sub(r"\W", "_", v)
        return s if re.match(r"[A-Za-z_]", s) else "_" + s

    def strings(self, name, values):
        return f"inline constexpr const char* {name}[] = {{" + ", ".join(self.cstr(v) for v in values) + "};\n"

    def write_header(self, path):
        m = self.m
        L = []

        def enum(name, values):
            L.append(f"enum class {name} : u8 {{ " + ", ".join(self.camel(v) for v in values) + " };\n")

        def fl(x):
            t = f"{float(x):g}"
            return t + ("f" if "." in t or "e" in t else ".0f")

        def col(c):
            return "{" + ", ".join(fl(x) for x in c) + "}"

        L.append("// Generated by mce story compile (tools/story/story_compile.py) from story/story.json and the\n"
                 "// .story files. Do not edit; edit the script and compile again.\n")
        L.append("#pragma once\n#include \"mce/kit/novel.h\"\n\n")
        L.append(f"namespace {m.namespace} {{\n\nusing mce::i16;\nusing mce::u16;\nusing mce::u8;\n"
                 "using mce::novel::Clause;\nusing mce::novel::Join;\nusing mce::novel::Op;\nusing mce::novel::OpKind;\n"
                 "using mce::novel::Option;\nusing mce::novel::Test;\n\n")
        enum("Character", m.characters)
        enum("Expression", m.expressions)
        enum("Pose", m.poses)
        enum("Slot", m.slots)
        enum("Location", m.locations)
        enum("TimeOfDay", m.time_names)
        enum("Lut", m.luts)
        enum("Music", m.music)
        enum("Ambience", m.ambience)
        enum("Sfx", m.sfx)
        enum("Ending", m.endings)
        enum("Tag", m.tags)
        if m.commands:
            enum("Command", m.commands)
        L.append("\n// Variable references for novel::get_var / set_var.\nnamespace var {\n")
        for i, f in enumerate(m.flags):
            L.append(f"inline constexpr u16 {self.ident(f)} = {i};\n")
        for i, (n, _, _) in enumerate(m.counters):
            L.append(f"inline constexpr u16 {self.ident(n)} = 0x{COUNTER_BIT | i:04X};\n")
        L.append("}  // namespace var\n\n")
        L.append(f"inline constexpr u16 kCharacterCount = {len(m.characters)}, kCastCount = {len(m.cast)}, "
                 f"kLocationCount = {len(m.locations)}, kFlagCount = {len(m.flags)}, kCounterCount = {len(m.counters)};\n")
        L.append(f"inline constexpr u16 kLinesPerPage = {m.lines_per_page}, kLetterLinesPerPage = {m.letter_lines_per_page};\n\n")

        labels = sorted(self.labels, key=lambda k: (self.labels[k], k))
        L.append("inline constexpr mce::novel::LabelInfo kLabels[] = {\n")
        for n in labels:
            L.append(f"    {{{self.cstr(n)}, {self.labels[n]}}},\n")
        L.append("};\n")
        L.append(self.strings("kLabelNames", labels))
        L.append("inline constexpr u16 kLabelOps[] = {" + ", ".join(str(self.labels[n]) for n in labels) + "};\n")
        L.append(f"inline constexpr u16 kStart = {self.labels[m.start]};\n")
        top = max(self.letters) if self.letters else 0
        L.append(f"inline constexpr u16 kLetterLabels[{top + 1}] = {{" +
                 ", ".join(str(self.labels[f'__letter_{n}']) if n in self.letters else "0" for n in range(top + 1)) + "};\n\n")

        L.append(f"inline constexpr u16 kTextCount = {len(self.text)};\ninline constexpr const char* kText[] = {{\n")
        for s in self.text:
            L.append("    " + self.cstr(s) + ",\n")
        L.append("};\n\n")
        L.append(f"inline constexpr u16 kOpCount = {len(self.ops)};\ninline constexpr Op kOps[] = {{\n")
        for op in self.ops:
            L.append("    {u16(OpKind::%s), %d, %d, %d, %d, %d, %d},  // %s\n" % (op["kind"], op["a"], op["b"], op["c"], op["d"],
                                                                         op["e"], op["f"], op["src"]))
        L.append("};\n\n")
        L.append(f"inline constexpr u16 kOptionCount = {len(self.options)};\ninline constexpr Option kOptions[] = {{\n")
        for o in self.options or [{"text": 0, "text2": 0, "target": 0, "cond": 0, "tag": 0, "hidden": 0}]:
            L.append("    {%d, %d, %d, %d, %d, %d},\n" % (o["text"], o["text2"], o["target"], o["cond"], o["tag"], o["hidden"]))
        L.append("};\n\n")
        L.append("inline constexpr Clause kClauses[] = {\n")
        for c in self.clauses:
            L.append("    {%d, %d, Test::%s, Join::%s},\n" % (c["var"], c["n"], TESTS[c["test"]], JOINS[c["join"]]))
        L.append("};\n\n")
        L.append(f"inline constexpr u16 kLetterLineCount = {len(self.letter_lines)};\ninline constexpr u16 kLetterLines[] = {{" +
                 ", ".join(str(x) for x in (self.letter_lines or [0])) + "};\n\n")

        cast_row = {c: i for i, c in enumerate(m.cast)}
        L.append("inline constexpr mce::novel::CharacterInfo kCharacters[] = {\n")
        for c, d in zip(m.characters, m.display):
            L.append(f"    {{{self.cstr(c)}, {self.cstr(d)}, {cast_row.get(c, 255)}}},\n")
        if not m.characters:
            L.append('    {"", "", 255},\n')
        L.append("};\n")
        L.append("inline constexpr const char* kCharacterNames[] = {" + ", ".join(self.cstr(d) for d in m.display or [""]) + "};\n")
        L.append(self.strings("kExpressionNames", m.expressions))
        L.append(self.strings("kPoseNames", m.poses))
        L.append(self.strings("kLocationNames", m.locations))
        L.append(self.strings("kLutNames", m.luts))
        L.append(self.strings("kMusicNames", m.music))
        if len(m.timelines) > 1:
            L.append(self.strings("kTimelineNames", m.timelines))
        L.append(self.strings("kAmbienceNames", m.ambience))
        L.append(self.strings("kSfxNames", m.sfx))
        L.append(self.strings("kTagNames", m.tags))
        L.append(self.strings("kCommandNames", m.commands or [""]))
        L.append("inline constexpr mce::novel::TimeLook kTimes[] = {\n")
        for t in m.times:
            L.append(f"    {{{self.cstr(t['id'])}, {col(t['paper'])}, {col(t['cast'])}, {col(t['ambient'])}, {col(t['lamp'])}, "
                     f"{fl(t['lamp_pos'][0])}, {fl(t['lamp_pos'][1])}, {fl(t['lamp_intensity'])}}},\n")
        L.append("};\n")
        L.append("inline constexpr mce::novel::EndingInfo kEndings[] = {" +
                 ", ".join(f"{{{self.cstr(e)}, {self.cstr(t)}}}" for e, t in zip(m.endings, m.ending_titles)) + "};\n")
        prefer = []
        for t in m.tags:
            row = [m.tags.index(x) for x in m.bot.get(t, [t])][:4]
            prefer += row + [NONE] * (4 - len(row))
        L.append("inline constexpr u16 kBotPrefer[] = {" + ", ".join(str(x) for x in prefer) + "};\n")
        L.append("inline constexpr mce::novel::VarInfo kVars[] = {\n")
        for i, f in enumerate(m.flags):
            L.append(f"    {{{self.cstr(f)}, {i}, 0, 1}},\n")
        for i, (n, lo, hi) in enumerate(m.counters):
            L.append(f"    {{{self.cstr(n)}, 0x{COUNTER_BIT | i:04X}, {lo}, {hi}}},\n")
        if not m.flags and not m.counters:
            L.append('    {"", 0xFFFF, 0, 0},\n')
        L.append("};\n\n")

        if m.loc_base:
            L.append("// Translations (story.json \"localize\"): per language, the lines a page or an option shows instead.\n")
            for i, (tag, pages, options, names) in enumerate(self.loc_tables):
                L.append(f"inline constexpr mce::novel::PageText kPages{i}[] = {{\n")
                for base in sorted(pages) or [(0, 0, 0, 0)]:
                    tr = pages.get(base, (0, 0, 0, 0))
                    L.append("    {{%s}, {%s}},\n" % (", ".join(map(str, base)), ", ".join(map(str, tr))))
                L.append("};\n")
                L.append(f"inline constexpr mce::novel::OptionText kOptionTexts{i}[] = {{\n")
                for base in sorted(options) or [(0, 0)]:
                    tr = options.get(base, (0, 0))
                    L.append("    {%d, %d, %d, %d},\n" % (base[0], base[1], tr[0], tr[1]))
                L.append("};\n")
                L.append(f"inline constexpr u16 kNames{i}[] = {{" + ", ".join(str(x) for x in names or [0]) + "};\n")
            L.append("inline constexpr mce::novel::LocaleText kLocales[] = {\n")
            for i, (tag, pages, options, _) in enumerate(self.loc_tables):
                L.append(f"    {{{self.cstr(tag)}, kPages{i}, {len(pages)}, kOptionTexts{i}, {len(options)}, kNames{i}}},\n")
            if not self.loc_tables:
                L.append('    {"", nullptr, 0, nullptr, 0, nullptr},\n')
            L.append("};\n\n")

        ts = m.title_screen
        nvars = len(m.flags) + len(m.counters)
        fields = [
            ("ops", "kOps"), ("op_count", "kOpCount"), ("options", "kOptions"), ("option_count", "kOptionCount"),
            ("clauses", "kClauses"), ("clause_count", str(len(self.clauses))), ("text", "kText"), ("text_count", "kTextCount"),
            ("letter_lines", "kLetterLines"), ("letter_line_count", "kLetterLineCount"), ("letters", "kLetterLabels"),
            ("letter_count", str(top + 1)), ("labels", "kLabels"), ("label_count", str(len(labels))),
            ("characters", "kCharacters"), ("character_count", str(len(m.characters))),
            ("expressions", "kExpressionNames"), ("expression_count", str(len(m.expressions))),
            ("poses", "kPoseNames"), ("pose_count", str(len(m.poses))), ("slot_count", str(len(m.slots))),
            ("locations", "kLocationNames"), ("location_count", str(len(m.locations))),
            ("times", "kTimes"), ("time_count", str(len(m.times))), ("luts", "kLutNames"), ("lut_count", str(len(m.luts))),
            ("music", "kMusicNames"), ("music_count", str(len(m.music))), ("ambience", "kAmbienceNames"),
            ("ambience_count", str(len(m.ambience))), ("sfx", "kSfxNames"), ("sfx_count", str(len(m.sfx))),
            ("endings", "kEndings"), ("ending_count", str(len(m.endings))), ("tags", "kTagNames"), ("tag_count", str(len(m.tags))),
            ("bot_prefer", "kBotPrefer"), ("vars", "kVars"), ("var_count", str(nvars)),
            ("commands", "kCommandNames"), ("command_count", str(len(m.commands))), ("start", "kStart"),
            ("lines_per_page", "kLinesPerPage"), ("letter_lines_per_page", "kLetterLinesPerPage"),
            ("title", self.cstr(m.title)), ("extras", self.cstr(m.extras)),
            ("sfx_page", str(m.ui_sfx["page"])), ("sfx_move", str(m.ui_sfx["move"])), ("sfx_confirm", str(m.ui_sfx["confirm"])),
            ("sfx_letter", str(m.ui_sfx["letter"])),
            ("title_location", str(ts["location"])), ("title_time", str(ts["time"])), ("title_lut", str(ts["lut"])),
            ("title_music", str(ts["music"])), ("title_ambience", str(ts["ambience"])),
            ("background_normal_map", self.cstr(m.normal_map) if m.normal_map else "nullptr"),
            ("text_font", self.cstr(m.text_font) if m.text_font else "nullptr"),
            ("ink_speech", col(m.ink["speech"])), ("ink_narration", col(m.ink["narration"])), ("ink_row", col(m.ink["row"])),
            ("ink_row_selected", col(m.ink["row_selected"])), ("ink_row_disabled", col(m.ink["row_disabled"])),
            ("row_tint", col(m.ink["row_tint"])),
        ]
        if m.loc_base:
            fields += [("locales", "kLocales"), ("locale_count", str(len(self.loc_tables))),
                       ("base_locale", self.cstr(m.loc_base))]
        if len(m.timelines) > 1:
            fields += [("timelines", "kTimelineNames"), ("timeline_count", str(len(m.timelines)))]
        L.append("// The program the kit runs: novel::install(world, kProgram, hooks) / install_dialogue(world, kProgram).\n")
        L.append("inline constexpr mce::novel::Program kProgram = {\n")
        for k, v in fields:
            L.append(f"    .{k} = {v},\n")
        L.append("};\n\n")
        L.append(f"}}  // namespace {m.namespace}\n")
        path.parent.mkdir(parents=True, exist_ok=True)
        new = "".join(L)
        if not path.exists() or path.read_text(encoding="utf-8") != new:
            path.write_text(new, encoding="utf-8", newline="\n")
            return True
        return False

    def write_report(self, path, unreachable, missing):
        total = sum(self.words.values())
        rows = "\n".join(f"| {k} | {v} |" for k, v in sorted(self.words.items()))
        text = (f"# {self.m.title}: script report\n\nGenerated by mce story compile.\n\n"
                f"| File | Words |\n|---|---|\n{rows}\n| **total** | **{total}** |\n\n"
                f"- statements: {len(self.ops)}, pages: {self.pages}, choices: {self.choices}, options: {len(self.options)}, "
                f"labels: {len([n for n in self.labels if not n.startswith('__')])}, strings: {len(self.text)}\n"
                f"- endings reached from start: {', '.join(e for e in self.m.endings[1:] if e not in missing) or 'none'}\n"
                f"- endings NOT reachable: {', '.join(missing) or 'none'}\n"
                f"- unreachable labels: {', '.join(n for n, _ in unreachable) or 'none'}\n")
        if self.m.loc_base:
            text += f"- translations (base {self.m.loc_base}): " + (", ".join(
                f"{tag} {s}/{ns} statements, {o}/{no} options" for tag, s, ns, o, no in self.loc_stats) or "none") + "\n"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")


def compile_project(project, check=False, out=None, report=None, write_loc=True):
    """Compiles a project's story; returns (compiler, changed). Raises CompileError."""
    m = Manifest(project)
    c = Compiler(m)
    files = m.files()
    if not files:
        raise CompileError(f"no .story files match {m.sources} in {project}")
    for f in files:
        c.compile_file(f)
    c.link()
    unreachable, missing = c.check()
    c.localize()
    if m.loc_base:
        table, stale = c.base_table()
        if check and stale:
            raise CompileError(f"{c.base_table_path()} is out of date with the script (run mce story compile)")
        if stale and not check and write_loc:
            c.base_table_path().parent.mkdir(parents=True, exist_ok=True)
            c.base_table_path().write_text(table, encoding="utf-8", newline="\n")
    changed = False
    if not check:
        changed = c.write_header(pathlib.Path(out) if out else m.output)
        c.write_report(pathlib.Path(report) if report else m.report, unreachable, missing)
    c.unreachable, c.missing, c.files = unreachable, missing, files
    return c, changed


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mce story compile", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("project", nargs="?", default=".", help="the project directory (holds story/story.json)")
    ap.add_argument("--check", action="store_true", help="validate without writing")
    ap.add_argument("--out", help="write the header here instead of the manifest's output")
    ap.add_argument("--report", help="write the report here instead of the manifest's report")
    ap.add_argument("--werror", action="store_true", help="warnings fail the compile")
    ap.add_argument("--no-write-loc", action="store_true", help="do not update the base string table (\"localize\")")
    args = ap.parse_args(argv)
    project = pathlib.Path(args.project).resolve()
    try:
        c, changed = compile_project(project, args.check, args.out, args.report, not args.no_write_loc)
    except CompileError as e:
        print(f"story compile: error: {e}", file=sys.stderr)
        return 1
    for w in c.warnings:
        print(f"story compile: warning: {w}", file=sys.stderr)
    words = sum(c.words.values())
    if args.check:
        print(f"story compile: ok, {len(c.files)} files, {len(c.ops)} statements, {c.pages} pages, {c.choices} choices, {words} words")
    else:
        out = pathlib.Path(args.out) if args.out else c.m.output
        print(f"story compile: {len(c.files)} files, {len(c.ops)} statements, {c.pages} pages, {c.choices} choices, {words} words "
              f"-> {out}{' (updated)' if changed else ' (unchanged)'}; unreachable labels: {len(c.unreachable)}; "
              f"endings missing: {c.missing or 'none'}")
    return 1 if args.werror and c.warnings else 0


if __name__ == "__main__":
    sys.exit(main())
