#!/usr/bin/env python3
"""Docs checks of the walls stage (M-DOCCHECK).

  links     a relative link in a Markdown file points at a file or directory that exists, and a
            `#anchor` names a heading of the target (GitHub's slug rules)
  includes  an `#include "mce/..."` line in a document names a header under some `src/**/include`
  flags     a `-name` after `mce_player` on a line of a document is a console variable (`MCE_CVAR(id, "name"`
            or `ConVar id("name"`) some source declares: the command line sets convars

A miss is an error unless `scripts/ci/docs.allow` lists it as `kind | file | target | reason`. A listed miss
that no longer happens is an error too, so the list only shrinks. `--list` prints the misses as allow lines.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
ALLOW = ROOT / "scripts/ci/docs.allow"
SKIP_DIRS = {"build", ".git", "node_modules", "_fetch"}


def md_files():
    out = []
    for p in sorted(ROOT.rglob("*.md")):
        rel = p.relative_to(ROOT).parts
        if SKIP_DIRS & set(rel) or rel[0] in ("third_party", "external"):
            continue
        out.append(p)
    return out


def strip_code(text):
    """Fenced and inline code hold example text, not links."""
    text = re.sub(r"^(```|~~~).*?^\1[^\n]*$", "", text, flags=re.S | re.M)
    return re.sub(r"`[^`\n]*`", "", text)


def slug(heading):
    h = re.sub(r"<[^>]+>", "", heading).strip().lower()
    h = h.replace("`", "")
    h = re.sub(r"[^\w\- ]", "", h, flags=re.U)
    return h.replace(" ", "-")


_anchor_cache = {}


def anchors(path):
    if path not in _anchor_cache:
        seen, found = {}, set()
        text = re.sub(r"^(```|~~~).*?^\1[^\n]*$", "", path.read_text(errors="ignore"), flags=re.S | re.M)
        for m in re.finditer(r"^#{1,6}\s+(.*?)\s*#*\s*$", text, flags=re.M):
            s = slug(m.group(1))
            n = seen.get(s, 0)
            seen[s] = n + 1
            found.add(s if n == 0 else f"{s}-{n}")
        for m in re.finditer(r'<a\s+(?:name|id)="([^"]+)"', text):
            found.add(m.group(1))
        _anchor_cache[path] = found
    return _anchor_cache[path]


def include_roots():
    inc = set()
    for d in ROOT.glob("**/include"):
        if SKIP_DIRS & set(d.relative_to(ROOT).parts) or not d.is_dir():
            continue
        for h in d.rglob("*"):
            if h.suffix in (".h", ".hpp"):
                inc.add(h.relative_to(d).as_posix())
    return inc


def convar_names():
    names = set()
    pat = re.compile(r'(?:MCE_CVAR\(\s*\w+,|\bConVar\s+\w+\s*[({])\s*"([^"]+)"')
    for d in ("src", "samples", "projects", "templates", "tests"):
        for p in (ROOT / d).rglob("*"):
            if p.suffix in (".cpp", ".h") and "build" not in p.relative_to(ROOT).parts:
                names.update(pat.findall(p.read_text(errors="ignore")))
    return names


def player_flags(line):
    """The `-name` tokens of a `mce_player` command on `line` (up to a pipe, `&&` or `ctest`)."""
    seg = re.split(r"\||&&|;|\bctest\b", line[line.index("mce_player"):])[0]
    return re.findall(r"(?:^|\s)-{1,2}([A-Za-z_][A-Za-z0-9_.]*)", seg)


def misses():
    out = []
    incs = include_roots()
    cvars = convar_names()
    for p in md_files():
        rel = p.relative_to(ROOT).as_posix()
        raw = p.read_text(errors="ignore")
        for m in re.finditer(r"\]\(\s*<?([^)\s>]*)>?(?:\s+\"[^\"]*\")?\s*\)", strip_code(raw)):
            url = m.group(1)
            if not url or re.match(r"[A-Za-z][A-Za-z0-9+.-]*:", url) or url.startswith("//"):
                continue
            target, _, frag = url.partition("#")
            dest = p if not target else (p.parent / target).resolve()
            if not dest.exists():
                out.append(("link", rel, url, "no such file"))
            elif frag and dest.suffix == ".md" and frag not in anchors(dest):
                out.append(("anchor", rel, url, "no such heading"))
        for line in raw.splitlines():
            if "mce_player" in line:
                for f in player_flags(line):
                    if f not in cvars:
                        out.append(("flag", rel, "-" + f, "no convar of that name"))
        for m in re.finditer(r'#include\s+[<"](mce/[^>"]+)[>"]', raw):
            if m.group(1) not in incs:
                out.append(("include", rel, m.group(1), "no such header"))
    return sorted(set(out))


def allowed():
    allow, problems = {}, []
    if ALLOW.exists():
        for n, line in enumerate(ALLOW.read_text().splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [x.strip() for x in line.split("|")]
            if len(parts) != 4 or not parts[3]:
                problems.append(f"{ALLOW.name}:{n}: expected `kind | file | target | reason`")
                continue
            allow[tuple(parts[:3])] = parts[3]
    return allow, problems


def main():
    found = misses()
    if "--list" in sys.argv:
        for kind, rel, target, why in found:
            print(f"{kind} | {rel} | {target} | {why}")
        return 0
    allow, problems = allowed()
    keys = {(k, f, t) for k, f, t, _ in found}
    for k, f, t, why in found:
        if (k, f, t) not in allow:
            problems.append(f"{f}: {k} `{t}`: {why}")
    for key in sorted(set(allow) - keys):
        problems.append(f"{ALLOW.name}: `{' | '.join(key)}` no longer happens; remove it")
    for p in problems:
        print(f"check_docs: {p}")
    if problems:
        print(f"check_docs: {len(problems)} problem(s); fix the document or list the miss in scripts/ci/docs.allow")
        return 1
    print(f"check_docs: OK ({len(md_files())} documents)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
