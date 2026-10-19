# M-SCRIPTDOC — The Luau API and its documentation cannot drift (milestone record)

**Status:** done 2026-10-19 (one step; added 2026-10-19; owner: the M-SCRIPT thread; device-free; no Linear issues: free limit, this record is the tracker).

**Why.** Scripts get their API from `declare <table>: { ... }` blocks in `src/engine/script/compiler/definitions.cpp` (`mce`, `mission`, `marker`, `clock`, `weather`). Every member was documented
when this was checked (2026-10-19: 71 `mce.*` members and 23 in the other tables, none missing), but nothing kept it so: a thread adding `mce.foo` without a guide line would pass every gate.

## Steps

| # | Step | Model | State |
|---|---|---|---|
| 1 | `check_docs.py` kind `luauapi` (walls stage): every member of a `declare` table appears as `table.member` in `docs/user-guide/27-scripting.md` or `docs/engine/script.md`; misses go in `scripts/ci/docs.allow` | Sonnet | done in PR (this step) |

## Step 1 as built

- `luau_api()` in `scripts/ci/check_docs.py` reads the `declare <table>: {` blocks (top-level members only, four-space indent) and looks for `\btable\.member\b` in the two documents. Checked by adding an
  undocumented member to `definitions.cpp` by hand: the walls stage reports `luauapi mce.zzz_undocumented`. Guide 1 (the walls paragraph) names the new kind.
- Not done: nested members (a table inside a declared table), the `declare` of types (`type X = ...`), and checking that a documented name still exists (a removed function stays in the guide; `docs.allow` style
  "no longer happens" does not apply to prose).
