# Contributing: reporting bugs and requests

Bugs, improvement ideas and feature requests go in **GitHub Issues** on
`sberkan09/mesh_control_engine`. This works the same for people and for other
Claude sessions (for example a session that built a game with the MCP tools and
hit friction). Issues are not tied to any session id, so nothing is lost when a
session ends.

## Filing an issue

```sh
gh issue create -R sberkan09/mesh_control_engine \
  --title "mce scene.add_entity ignores parent when name has spaces" \
  --label bug --label from-claude-session \
  --body-file issue.md
```

Labels to pick from: `bug`, `enhancement`, `documentation`, `question`.
Claude sessions also add `from-claude-session`.

A good body has:

- **What happened** and **what you expected**.
- **How to reproduce**: commands, MCP calls, a minimal project or scene.
- **Where**: engine commit (`git rev-parse --short HEAD`) or release zip name,
  platform, and the file/module if you know it.
- **Why it matters**: what it blocked or how often it bites.

One problem per issue. Search open issues first (`gh issue list -R
sberkan09/mesh_control_engine --search "<words>"`) and comment on an existing
one instead of opening a duplicate.

## What happens next

A Claude triage routine reads new issues twice a day (around 09:00 and 21:00
Europe/Istanbul) and labels each one `claude-triaged` plus one of:

| Label | Meaning |
|-------|---------|
| `claude-accepted` | Will be (or was) implemented. The PR targets `dev_claude`, says `Fixes #N`, and closes the issue on merge. |
| `claude-declined` | Not doing it; the comment explains why. The issue is closed. |
| `needs-human` | Needs the maintainer's decision (scope, design, cost). |

Disagree with a decision? Comment on the issue with new information and remove
`claude-triaged`; the next run looks at it again.
