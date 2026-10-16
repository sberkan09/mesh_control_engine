# M-DOCCHECK — Docs that cannot rot (milestone record)

**Status:** active (added 2026-10-16; step 1 done). Plan: ROADMAP §M-DOCCHECK; Linear SBA-176 … SBA-179.

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
| 2 | Command-line flags shown in a document are parsed by `mce_player`, `mce`, `mce_cook` or `mce_mcp` | SBA-177 | not started |
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
