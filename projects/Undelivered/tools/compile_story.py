#!/usr/bin/env python3
"""Compiles Undelivered's script (story/*.story) into games/undelivered/module/story_data.h.

    python3 projects/Undelivered/tools/compile_story.py            # writes story_data.h and story/REPORT.md
    python3 projects/Undelivered/tools/compile_story.py --check    # validate only

The format (docs/design/undelivered.md §7.2), one statement per line:

    # comment
    @scene <label> [bg=<location>] [time=<morning|noon|evening|night>] [lut=<warm|neutral|cold|sepia|night>]
                   [music=<id|none>] [ambience=<id|none>]
    @label <name>
    @bg <location> [time] [lut]           @music <id>|none      @ambience <id>|none      @sfx <id>
    @show <char> <left|center|right> [expr] [pose]      @hide <char>|all     @expr <char> <expr>
    @pose <char> <pose>
    Speaker: text (continuation lines are indented)      a paragraph with no speaker is narration
    Speaker:                                              a choice: the option lines follow
      > "option text" [gate] {tag} -> label               gate: [flag] [!flag] [trust.x >= n] [var == n] ...
      >? "hidden unless the gate passes" [gate] -> label
    @set <var> <+n|-n|=n>      @flag <var>      @unflag <var>
    @if <cond> -> label        @jump label      @call label      @return
    @letter n                  @drawer          @journal         @fade        @wait <seconds>
    @ending <id>               @end
    @lettertext n ... @endletter                the text of letter n (shown by @letter n)

Variables: trust.<char> (-3..6), delivered.<1-7>, read.<1-7>, named flags, and counters (resolve, day,
drawer_letter, drawer_action, afternoon, aux0..aux7). The compiler lays them out as the module's
NovelSave fields; the VM reads them by (kind, index).

Text is wrapped and paginated here with the cooked font's metrics (Pillow measures the TTF at the
baked size), four lines of at most 62 bytes per page, so the runtime never measures text.
"""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
STORY = ROOT / "story"
OUT = ROOT / "games" / "undelivered" / "module" / "story_data.h"
REPORT = STORY / "REPORT.md"
FONT = ROOT / "assets" / "fonts" / "CrimsonPro-Regular.ttf"
LETTER_FONT = ROOT / "assets" / "fonts" / "NothingYouCouldDo-Regular.ttf"

FONT_PX = 32          # the prose font's baked size (the sidecar's "size")
LETTER_PX = 30
LINE_PX = 860         # the dialogue box's inner width in pixels (1280-wide view)
LETTER_LINE_PX = 820
MAX_LINE_BYTES = 62   # TextLabel.text is a FixedString<64>
LINES_PER_PAGE = 4
LETTER_LINES_PER_PAGE = 12

CHARACTERS = ["deniz", "mira", "feride", "selin", "halil", "yusuf", "hasan", "cemil", "nermin", "kemal", "crowd"]
CAST = ["mira", "feride", "selin", "halil", "yusuf", "hasan", "cemil"]  # the ones with sprites
EXPRESSIONS = ["neutral", "warm", "troubled", "sharp", "open"]
POSES = ["stand", "gesture"]
SLOTS = ["left", "center", "right"]
LOCATIONS = ["none", "pier", "cafe", "postoffice", "lighthouse", "radio", "cove", "school", "grocery", "drawer"]
TIMES = ["morning", "noon", "evening", "night"]
LUTS = ["neutral", "warm", "cold", "sepia", "night"]
MUSIC = ["none", "harbour", "cafe", "drawer", "light", "cove", "sunday"]
AMBIENCE = ["none", "sea", "cafe", "wind", "clock", "radio"]
SFX = ["none", "page", "move", "confirm", "envelope", "stamp", "horn", "motor", "static", "bell"]
ENDINGS = ["none", "return_address", "open_water", "lamp_keeper", "drawer_shut", "last_ferry"]
TAGS = ["first", "leave", "stay", "shut", "write", "honest", "hard"]
TRUST = ["mira", "feride", "selin", "halil", "yusuf", "hasan", "cemil"]
FLAGS = ["cove_sold", "kemal_boat", "feride_blessing", "halil_stays_keeper", "wrote_back", "told_mira",
         "letter7_read", "honest_drawer", "cemil_pressed", "radio_visited", "mira_knows_conservatory",
         "selin_knows_transfer", "hasan_read", "yusuf_wrote_back", "school_visited", "boat_fixed",
         "feride_sang", "halil_told_selin", "deniz_decided_stay", "deniz_decided_go", "mira_asked",
         "selin_asked", "cemil_refused", "nermin_defended", "nermin_blamed", "kept_six", "epilogue_seen",
         "aux_flag_a", "aux_flag_b", "aux_flag_c", "aux_flag_d", "aux_flag_e"]
COUNTERS = ["resolve", "day", "drawer_letter", "drawer_action", "afternoon", "aux0", "aux1", "aux2", "aux3",
            "aux4", "aux5", "aux6", "aux7", "mira_days", "selin_days", "yusuf_days", "alone_days"]
VAR_KIND = {"trust": 0, "delivered": 1, "read": 2, "flag": 3, "counter": 4}

OPS = ["Nop", "Say", "Choice", "Jump", "Call", "Return", "If", "Set", "SetTo", "Bg", "Music", "Ambience", "Sfx",
       "Show", "Hide", "Expr", "Pose", "Letter", "LetterPage", "Drawer", "Journal", "Ending", "End", "Fade", "Wait"]
GATES = ["None", "Set", "Clear", "Ge", "Le", "Eq"]


class CompileError(Exception):
    pass


# ---- text measuring ----------------------------------------------------------------------------

class Measure:
    def __init__(self, path, px):
        self.font = None
        try:
            from PIL import ImageFont
            self.font = ImageFont.truetype(str(path), px)
        except Exception:  # noqa: BLE001 - no Pillow: a measured average per character
            self.font = None
        self.px = px

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


def paginate(text, measure, max_px, max_bytes, per_page):
    lines = wrap(text, measure, max_px, max_bytes)
    return [lines[i:i + per_page] for i in range(0, len(lines), per_page)]


# ---- the compiler --------------------------------------------------------------------------------

class Compiler:
    def __init__(self):
        self.text = [""]
        self.text_index = {"": 0}
        self.ops = []          # list of dicts {kind, a..f, src}
        self.options = []
        self.letter_lines = []
        self.labels = {}       # name -> op index
        self.fixups = []       # (op index, field, label name, src)
        self.option_fixups = []
        self.letters = {}      # n -> label name
        self.measure = Measure(FONT, FONT_PX)
        self.letter_measure = Measure(LETTER_FONT, LETTER_PX)
        self.words = {}        # file -> words
        self.pages = 0
        self.choices = 0
        self.endings_used = set()
        self.src = ""

    # -- tables
    def sid(self, s):
        if s not in self.text_index:
            self.text_index[s] = len(self.text)
            self.text.append(s)
        return self.text_index[s]

    def emit(self, kind, a=0, b=0, c=0, d=0, e=0, f=0):
        self.ops.append({"kind": kind, "a": a, "b": b, "c": c, "d": d, "e": e, "f": f, "src": self.src})
        return len(self.ops) - 1

    def label(self, name):
        if name in self.labels:
            raise CompileError(f"{self.src}: label '{name}' defined twice")
        self.labels[name] = len(self.ops)

    def ref(self, op, field, name):
        self.fixups.append((op, field, name, self.src))

    @staticmethod
    def index_of(table, name, what):
        if name not in table:
            raise CompileError(f"unknown {what} '{name}' (one of {', '.join(table)})")
        return table.index(name)

    def var(self, name):
        """A variable reference packed as kind << 8 | index."""
        if name.startswith("trust."):
            return (VAR_KIND["trust"] << 8) | self.index_of(TRUST, name[6:], "trust target")
        if name.startswith("delivered."):
            n = int(name[10:])
            if not 1 <= n <= 7:
                raise CompileError(f"{self.src}: letter number out of range in '{name}'")
            return (VAR_KIND["delivered"] << 8) | n
        if name.startswith("read."):
            n = int(name[5:])
            if not 1 <= n <= 7:
                raise CompileError(f"{self.src}: letter number out of range in '{name}'")
            return (VAR_KIND["read"] << 8) | n
        if name in FLAGS:
            return (VAR_KIND["flag"] << 8) | FLAGS.index(name)
        if name in COUNTERS:
            return (VAR_KIND["counter"] << 8) | COUNTERS.index(name)
        raise CompileError(f"{self.src}: unknown variable '{name}'")

    def gate(self, expr):
        """'flag' | '!flag' | 'var >= n' | 'var <= n' | 'var == n' -> (gate kind, var, n)."""
        expr = expr.strip()
        if not expr:
            return 0, 0, 0
        m = re.match(r"^(\S+)\s*(>=|<=|==)\s*(-?\d+)$", expr)
        if m:
            kind = {">=": 3, "<=": 4, "==": 5}[m.group(2)]
            return kind, self.var(m.group(1)), int(m.group(3))
        if expr.startswith("!"):
            return 2, self.var(expr[1:].strip()), 0
        return 1, self.var(expr), 0

    # -- statements
    def say(self, speaker, text, expr):
        text = re.sub(r"\[pause\]", "", text)
        text = re.sub(r"\s+", " ", text).strip()
        if not text:
            return
        speaker_id = self.index_of(CHARACTERS, speaker, "speaker") if speaker else 255
        expr_id = self.index_of(EXPRESSIONS, expr, "expression") if expr else 255
        for page in paginate(text, self.measure, LINE_PX, MAX_LINE_BYTES, LINES_PER_PAGE):
            ids = [self.sid(line) for line in page] + [0] * (LINES_PER_PAGE - len(page))
            self.emit("Say", speaker_id, expr_id, ids[0], ids[1], ids[2], ids[3])
            self.pages += 1
        self.words[self.src.split(":")[0]] = self.words.get(self.src.split(":")[0], 0) + len(text.split())

    def choice(self, options):
        first = len(self.options)
        for text, gate_expr, tag, hidden, target in options:
            gk, gv, gn = self.gate(gate_expr)
            tag_id = self.index_of(TAGS, tag, "tag") if tag else 0
            lines = wrap(text, self.measure, LINE_PX, MAX_LINE_BYTES)
            if len(lines) > 2:
                raise CompileError(f"{self.src}: option text wraps to {len(lines)} lines (at most 2): {text}")
            self.options.append({"text": self.sid(lines[0]), "text2": self.sid(lines[1]) if len(lines) > 1 else 0,
                                 "target": 0, "gate": gk, "tags": tag_id, "gate_var": gv,
                                 "gate_n": gn, "hidden": 1 if hidden else 0})
            self.option_fixups.append((len(self.options) - 1, target, self.src))
            self.words[self.src.split(":")[0]] = self.words.get(self.src.split(":")[0], 0) + len(text.split())
        self.emit("Choice", first, len(options))
        self.choices += 1

    def lettertext(self, n, paragraphs):
        name = f"__letter_{n}"
        self.label(name)
        self.letters[n] = name
        lines = []
        for p in paragraphs:
            if p.strip() == "":
                lines.append("")
            else:
                lines.extend(wrap(re.sub(r"\s+", " ", p.strip()), self.letter_measure, LETTER_LINE_PX, MAX_LINE_BYTES))
            self.words[self.src.split(":")[0]] = self.words.get(self.src.split(":")[0], 0) + len(p.split())
        while lines and lines[-1] == "":
            lines.pop()
        for i in range(0, len(lines), LETTER_LINES_PER_PAGE):
            page = lines[i:i + LETTER_LINES_PER_PAGE]
            first = len(self.letter_lines)
            self.letter_lines.extend(self.sid(line) for line in page)
            self.emit("LetterPage", first, len(page))
        self.emit("Return")

    # -- the parser
    def compile_file(self, path):
        lines = path.read_text(encoding="utf-8").split("\n")
        i = 0
        pending = None  # (speaker, text, expr) being accumulated
        in_letter = None

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
                    self.lettertext(in_letter[0], in_letter[1])
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
                self.directive(stripped, lines, i)
                if stripped.startswith("@lettertext"):
                    in_letter = (int(stripped.split()[1]), [])
                continue
            m = re.match(r"^([A-Z][a-z]+)(?:\(([a-z]+)\))?:\s*(.*)$", stripped)
            if m:
                speaker, expr, text = m.group(1).lower(), m.group(2), m.group(3)
                if speaker not in CHARACTERS:
                    raise CompileError(f"{self.src}: unknown speaker '{m.group(1)}'")
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
            if stripped.startswith(">"):
                raise CompileError(f"{self.src}: an option line outside a choice")
            # narration
            pending = (None, stripped.lstrip("*").strip(), None)
        flush()

    def option(self, text, src):
        self.src = src
        m = re.match(r'^>(\?)?\s*"(.*?)"\s*(?:\[(.*?)\])?\s*(?:\{(\w+)\})?\s*->\s*(\S+)\s*$', text)
        if not m:
            raise CompileError(f'{src}: bad option line (want > "text" [gate] {{tag}} -> label): {text}')
        hidden, body, gate_expr, tag, target = m.group(1), m.group(2), m.group(3) or "", m.group(4), m.group(5)
        return body, gate_expr, tag, bool(hidden), target

    def directive(self, line, lines, i):
        parts = line.split()
        cmd, args = parts[0], parts[1:]
        if cmd == "@scene":
            self.label(args[0])
            kv = dict(a.split("=", 1) for a in args[1:])
            if "bg" in kv:
                self.emit("Bg", self.index_of(LOCATIONS, kv["bg"], "location"),
                          self.index_of(TIMES, kv.get("time", "noon"), "time"),
                          self.index_of(LUTS, kv.get("lut", "neutral"), "lut"))
            if "music" in kv:
                self.emit("Music", self.index_of(MUSIC, kv["music"], "music"))
            if "ambience" in kv:
                self.emit("Ambience", self.index_of(AMBIENCE, kv["ambience"], "ambience"))
        elif cmd == "@label":
            self.label(args[0])
        elif cmd == "@bg":
            self.emit("Bg", self.index_of(LOCATIONS, args[0], "location"),
                      self.index_of(TIMES, args[1] if len(args) > 1 else "noon", "time"),
                      self.index_of(LUTS, args[2] if len(args) > 2 else "neutral", "lut"))
        elif cmd == "@music":
            self.emit("Music", self.index_of(MUSIC, args[0], "music"))
        elif cmd == "@ambience":
            self.emit("Ambience", self.index_of(AMBIENCE, args[0], "ambience"))
        elif cmd == "@sfx":
            self.emit("Sfx", self.index_of(SFX, args[0], "sfx"))
        elif cmd == "@show":
            self.emit("Show", self.index_of(SLOTS, args[1], "slot"), self.index_of(CAST, args[0], "cast member"),
                      self.index_of(EXPRESSIONS, args[2] if len(args) > 2 else "neutral", "expression"),
                      self.index_of(POSES, args[3] if len(args) > 3 else "stand", "pose"))
        elif cmd == "@hide":
            self.emit("Hide", 255 if args[0] == "all" else self.index_of(CAST, args[0], "cast member"))
        elif cmd == "@expr":
            self.emit("Expr", self.index_of(CAST, args[0], "cast member"), self.index_of(EXPRESSIONS, args[1], "expression"))
        elif cmd == "@pose":
            self.emit("Pose", self.index_of(CAST, args[0], "cast member"), self.index_of(POSES, args[1], "pose"))
        elif cmd == "@set":
            var, delta = args[0], args[1]
            if delta.startswith("="):
                self.emit("SetTo", a=self.var(var), c=int(delta[1:]) & 0xFFFF)
            else:
                self.emit("Set", a=self.var(var), c=int(delta) & 0xFFFF)
        elif cmd == "@flag":
            self.emit("SetTo", a=self.var(args[0]), c=1)
        elif cmd == "@unflag":
            self.emit("SetTo", a=self.var(args[0]), c=0)
        elif cmd == "@if":
            m = re.match(r"^@if\s+(.*?)\s*->\s*(\S+)$", line)
            if not m:
                raise CompileError(f"{self.src}: bad @if (want @if <cond> -> label)")
            gk, gv, gn = self.gate(m.group(1))
            op = self.emit("If", gk, gv, gn & 0xFFFF)
            self.ref(op, "d", m.group(2))
        elif cmd in ("@jump", "@call"):
            op = self.emit("Jump" if cmd == "@jump" else "Call")
            self.ref(op, "a", args[0])
        elif cmd == "@return":
            self.emit("Return")
        elif cmd == "@letter":
            op = self.emit("Letter", int(args[0]))
            self.ref(op, "b", f"__letter_{int(args[0])}")
        elif cmd == "@drawer":
            self.emit("Drawer")
        elif cmd == "@journal":
            self.emit("Journal")
        elif cmd == "@fade":
            self.emit("Fade")
        elif cmd == "@wait":
            self.emit("Wait", int(float(args[0]) * 60))
        elif cmd == "@ending":
            self.endings_used.add(args[0])
            self.emit("Ending", self.index_of(ENDINGS, args[0], "ending"))
        elif cmd == "@end":
            self.emit("End")
        elif cmd == "@lettertext":
            pass  # handled by the caller (block)
        else:
            raise CompileError(f"{self.src}: unknown directive {cmd}")

    # -- linking and checks
    def link(self):
        for op, field, name, src in self.fixups:
            if name not in self.labels:
                raise CompileError(f"{src}: jump to undefined label '{name}'")
            self.ops[op][field] = self.labels[name]
        for idx, name, src in self.option_fixups:
            if name not in self.labels:
                raise CompileError(f"{src}: option to undefined label '{name}'")
            self.options[idx]["target"] = self.labels[name]
        if "start" not in self.labels:
            raise CompileError("no 'start' label")
        for n in range(1, 8):
            if n not in self.letters:
                raise CompileError(f"letter {n} has no @lettertext block")

    def reachable(self):
        """Op indices reachable from 'start' following fall-through, jumps, calls and options."""
        seen = set()
        stack = [self.labels["start"]]
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
                    stack.append(op["d"])
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
        endings_reached = {ENDINGS[self.ops[pc]["b"] if False else self.ops[pc]["a"]] for pc in seen if self.ops[pc]["kind"] == "Ending"}
        missing = [e for e in ENDINGS[1:] if e not in endings_reached]
        # a label whose fall-through runs off the end of the program
        if self.ops and self.ops[-1]["kind"] not in ("End", "Return", "Jump", "Ending"):
            raise CompileError("the last statement must be @end, @return, @jump or @ending")
        return unreachable, missing

    # -- output
    @staticmethod
    def cstr(s):
        out = []
        for b in s.encode("utf-8"):
            if b == 0x22:
                out.append('\\"')
            elif b == 0x5C:
                out.append("\\\\")
            elif 0x20 <= b < 0x7F:
                out.append(chr(b))
            else:
                out.append("\\%03o" % b)
        return '"' + "".join(out) + '"'

    def write_header(self, path):
        def enum(name, values):
            def camel(v):
                return "".join(part[:1].upper() + part[1:] for part in v.split("_"))
            return f"enum class {name} : u8 {{ " + ", ".join(camel(v) for v in values) + " };\n"
        lines = []
        lines.append("// Generated by tools/compile_story.py from story/*.story. Do not edit.\n")
        lines.append("#pragma once\n#include \"mce/types.h\"\n\nnamespace undelivered::story {\n\nusing mce::i16;\nusing mce::u16;\nusing mce::u8;\n\n")
        lines.append(enum("OpKind", OPS))
        lines.append(enum("Gate", GATES))
        lines.append(enum("Character", CHARACTERS))
        lines.append(enum("Expression", EXPRESSIONS))
        lines.append(enum("Pose", POSES))
        lines.append(enum("Location", LOCATIONS))
        lines.append(enum("TimeOfDay", TIMES))
        lines.append(enum("Lut", LUTS))
        lines.append(enum("Music", MUSIC))
        lines.append(enum("Ambience", AMBIENCE))
        lines.append(enum("Sfx", SFX))
        lines.append(enum("Ending", ENDINGS))
        lines.append(enum("Tag", TAGS))
        lines.append(enum("Flag", FLAGS))
        lines.append(enum("Counter", COUNTERS))
        lines.append("\ninline constexpr u8 kVarTrust = 0, kVarDelivered = 1, kVarRead = 2, kVarFlag = 3, kVarCounter = 4;\n")
        lines.append(f"inline constexpr u16 kCharacterCount = {len(CHARACTERS)}, kCastCount = {len(CAST)}, kTrustCount = {len(TRUST)}, "
                     f"kFlagCount = {len(FLAGS)}, kCounterCount = {len(COUNTERS)}, kLocationCount = {len(LOCATIONS)};\n")
        lines.append(f"inline constexpr u16 kLinesPerPage = {LINES_PER_PAGE}, kLetterLinesPerPage = {LETTER_LINES_PER_PAGE};\n\n")
        lines.append("// One statement. Say: a speaker (255 = narration), expression (255 = keep), c..f line text ids.\n"
                     "// Choice: a first option, b count. If: a gate, b var, c n (i16), d target. Set: a var, c delta (i16).\n"
                     "// SetTo: a var, c value. Bg: a location, b time, c lut. Show: a slot, b cast, c expr, d pose.\n"
                     "// Letter: a number, b the text label. LetterPage: a first line id, b count. Wait: a ticks.\n")
        lines.append("struct Op {\n    u16 kind, a, b, c, d, e, f;\n};\n")
        lines.append("struct Option {\n    u16 text, text2, target;  // text2: the second line (0 = none)\n    u8 gate, tags;\n    u16 gate_var;\n    i16 gate_n;\n    u8 hidden, pad_;\n};\n\n")
        lines.append("inline constexpr const char* kCharacterNames[] = {" + ", ".join(self.cstr(c.title()) for c in CHARACTERS) + "};\n")
        lines.append("inline constexpr const char* kLabelNames[] = {" + ", ".join(self.cstr(n) for n in sorted(self.labels, key=self.labels.get)) + "};\n")
        lines.append("inline constexpr u16 kLabelOps[] = {" + ", ".join(str(self.labels[n]) for n in sorted(self.labels, key=self.labels.get)) + "};\n")
        lines.append(f"inline constexpr u16 kStart = {self.labels['start']};\n")
        lines.append("inline constexpr u16 kLetterLabels[8] = {0, " + ", ".join(str(self.labels[self.letters[n]]) for n in range(1, 8)) + "};\n\n")
        lines.append(f"inline constexpr u16 kTextCount = {len(self.text)};\ninline constexpr const char* kText[] = {{\n")
        for s in self.text:
            lines.append("    " + self.cstr(s) + ",\n")
        lines.append("};\n\n")
        lines.append(f"inline constexpr u16 kOpCount = {len(self.ops)};\ninline constexpr Op kOps[] = {{\n")
        for op in self.ops:
            lines.append("    {%d, %d, %d, %d, %d, %d, %d},  // %s\n" % (OPS.index(op["kind"]), op["a"], op["b"], op["c"], op["d"], op["e"], op["f"], op["src"]))
        lines.append("};\n\n")
        lines.append(f"inline constexpr u16 kOptionCount = {len(self.options)};\ninline constexpr Option kOptions[] = {{\n")
        for o in self.options:
            lines.append("    {%d, %d, %d, %d, %d, %d, %d, %d, 0},\n" % (o["text"], o["text2"], o["target"], o["gate"], o["tags"], o["gate_var"], o["gate_n"], o["hidden"]))
        if not self.options:
            lines.append("    {0, 0, 0, 0, 0, 0, 0, 0, 0},\n")
        lines.append("};\n\n")
        lines.append(f"inline constexpr u16 kLetterLineCount = {len(self.letter_lines)};\ninline constexpr u16 kLetterLines[] = {{" +
                     ", ".join(str(x) for x in (self.letter_lines or [0])) + "};\n\n")
        lines.append("}  // namespace undelivered::story\n")
        path.parent.mkdir(parents=True, exist_ok=True)
        new = "".join(lines)
        if not path.exists() or path.read_text(encoding="utf-8") != new:
            path.write_text(new, encoding="utf-8")
            return True
        return False

    def write_report(self, path, unreachable, missing):
        total = sum(self.words.values())
        rows = "\n".join(f"| {k} | {v} |" for k, v in sorted(self.words.items()))
        text = (f"# Undelivered script report\n\nGenerated by tools/compile_story.py.\n\n"
                f"| File | Words |\n|---|---|\n{rows}\n| **total** | **{total}** |\n\n"
                f"- statements: {len(self.ops)}, pages: {self.pages}, choices: {self.choices}, options: {len(self.options)}, "
                f"labels: {len(self.labels)}, strings: {len(self.text)}\n"
                f"- endings reached from start: {', '.join(e for e in ENDINGS[1:] if e not in missing) or 'none'}\n"
                f"- endings NOT reachable: {', '.join(missing) or 'none'}\n"
                f"- unreachable labels: {', '.join(n for n, _ in unreachable) or 'none'}\n")
        path.write_text(text, encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="validate without writing")
    ap.add_argument("--out", default=str(OUT))
    args = ap.parse_args()
    c = Compiler()
    files = sorted(STORY.glob("*.story"))
    if not files:
        sys.exit(f"compile_story: no .story files in {STORY}")
    try:
        for f in files:
            c.compile_file(f)
        c.link()
        unreachable, missing = c.check()
    except CompileError as e:
        sys.exit(f"compile_story: {e}")
    if args.check:
        print(f"compile_story: ok, {len(c.ops)} statements, {sum(c.words.values())} words, missing endings: {missing or 'none'}")
        return 0
    changed = c.write_header(pathlib.Path(args.out))
    c.write_report(REPORT, unreachable, missing)
    print(f"compile_story: {len(files)} files, {len(c.ops)} statements, {c.pages} pages, {c.choices} choices, "
          f"{sum(c.words.values())} words -> {args.out}{' (updated)' if changed else ' (unchanged)'}; "
          f"unreachable labels: {len(unreachable)}; endings missing: {missing or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
