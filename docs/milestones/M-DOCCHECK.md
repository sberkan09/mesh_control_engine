# M-DOCCHECK — Docs that cannot rot (milestone record)

**Status:** active (added 2026-10-16; steps 1–2 done). Plan: ROADMAP §M-DOCCHECK; Linear SBA-176 … SBA-179.

**Why:** `capabilities.py --check` fails when a component, asset type or tool is named by no document, and
the sample gate (M-COVER) fails when a component is shown in no sample. Nothing checks what the documents say
about the rest: a link, a heading anchor, an `#include` line, a command-line flag, a sample that no test plays.
These rot quietly when a chapter is renumbered or a flag is renamed.

It needs no device and no engine code: the work is scripts in the walls stage and document fixes.

**Not in scope:**
- Compiling the code blocks of the guide.
- Checking external (`http`) links.
- Prose quality.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | `scripts/ci/check_docs.py` in the walls stage: relative links and `#anchors` in every Markdown file resolve, `#include` lines of `mce/` headers name a real header; misses are fixed or on `scripts/ci/docs.allow` with a reason, and the list only shrinks | SBA-176 | done |
| 2 | A command-line flag shown after `mce_player` in a document is a console variable some source declares (the command line sets convars) | SBA-177 | done |
| 3 | Every `samples/` and `projects/` directory is named by a test, or listed with a reason | SBA-178 | not started |
| 4 | The guide and contributor docs describe the checks; gates and the close | SBA-179 | not started |

## Step 1 — Links, anchors and includes (done, SBA-176)

- **Check.** 237 documents. A link is any `](target)` outside code; a `#fragment` must match a heading of the
  target file by GitHub's slug rule (lower case, punctuation removed, spaces to `-`, repeated headings numbered).
  An `#include` line of an `mce/` header must name a header under some `include/` directory.
- **Found.** One real miss: the guide index linked `06-game-code.md#platform-notes`, but the heading is
  "6.14 Platform notes" (slug `614-platform-notes`); fixed. Four include lines in `docs/architecture/scripting.md`
  and `docs/engine/asset.md` name sketched or elided headers; they are on `scripts/ci/docs.allow` with reasons.
- **Tested.** A seeded broken link and anchor fail the stage; removed, it passes.

## Step 2 — Command-line flags (done, SBA-177)

- **Check.** The command line of every host is `-name [value]` for a console variable
  (`parse_command_line`, `src/engine/core/src/convar.cpp`), so a flag shown after the player command is valid when some
  source declares a variable of that name: `MCE_CVAR(id, "name", …)` (233 of them) or `ConVar id("name", …)`
  (`samples/common/ship_paths.h` declares `crash_dir` and `crash_test` this way). Flags are read from a line up to a
  pipe, `&&`, `;` or `ctest`, so `ctest -R` on the same line is not a flag.
- **Found.** No misses: every documented `mce_player` flag exists (the first pass that read only `MCE_CVAR`
  flagged `-crash_test`; it is declared as a plain `ConVar`).
- **Tested.** A seeded unknown flag after the player command fails the stage; removed, it passes.
- **Not covered.** Flags of `mce`, `mce_cook`, `mce_mcp` and the scripts use their own parsers; the sample
  programs' own flags (`-sim`, `-clients`) are not console variables either.
