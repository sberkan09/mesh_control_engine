# M-HARDEN — The MCP server's own lifetime and concurrency bugs (milestone record)

**Status:** not started (added 2026-10-16). Plan: ROADMAP §M-HARDEN; Linear SBA-195 … SBA-197.

**Why:** M-WORKFLOW step 5 (SBA-184) found that `ui_preview` kept a `ProjectAsset*` across `render_ticks`; the cook inside
rescans the project and frees those records, so the reply read freed memory. The Opus review of that fix confirmed the
cause and asked for the pattern to be removed for good: `ui_document_arg` should hand out a value (path and GUID), so a
`ProjectAsset*` is never carried past a rescan. The same step also left two things open: the two-server test uses two
`Server` objects, not two processes, and nothing runs the MCP tests under a sanitizer, which is how a use-after-free
survived.

| # | Step | Linear | Model | State |
|---|---|---|---|---|
| 1 | Handles by value: `ui_document_arg` and the other `ProjectAsset*` uses audited | SBA-195 | Sonnet | not started |
| 2 | `run_transcript.py` drives several `mce_mcp` processes; two-process scene test | SBA-196 | Sonnet | not started |
| 3 | MCP and transcript tests in the ASan stage; findings fixed; the close | SBA-197 | Sonnet | not started |

**Gates:** existing replay hashes unchanged; each fix has a regression test; `local_ci.sh` green on every step.
