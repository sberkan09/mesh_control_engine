# changelog.d

One fragment per PR that changes what a user of the engine sees: `changelog.d/<ticket>.md`
(`SBA-127.md`; `<ticket>-<slug>.md` when one ticket ships several PRs). The file holds the same
entry you would have added under `## Unreleased` in `CHANGELOG.md`: a `- ` bullet, continuation lines
indented by two spaces.

```
- Windowed arena sample, M-NET3 step 5 (SBA-127): `samples/net_arena` is a 2-4 player top-down arena ...
  second line of the same entry.
```

Do not edit `CHANGELOG.md` in a PR: two PRs never touch the same line this way. `scripts/changelog.py`
(run by `bump_version.py` when a version is cut, or by hand at a milestone's end) appends every fragment to
`## Unreleased`, in ticket order, and deletes the fragment files. The walls stage runs
`scripts/changelog.py --check`.
