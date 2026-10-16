# Answers

Fill in below. Keep the `## <section>` and `### <question id>` headings; add a `## <date> — <who>` line
at the top of each batch so it is clear when and by whom it was written. Attach long output as files
under `logs/` and link them.

---

## 2026-MM-DD — <coordinator / agent name>

### A1
<summary tables, one per run, with date / stages / warm or cold / diff size>

### A2
<warm run: configure __s, build __s, tests __s — log: logs/...>
<cold run: configure __s, build __s, tests __s — log: logs/...>

### A3
<release-build: configure __s, build __s>
<tidy: configure __s, build __s, run-clang-tidy __s>

### A4
<determinism stage total __s; longest subjects: ...; non-hash failures: yes/no, what>

### A5
<logs/...-ctest-N.txt, logs/...-LastTest.log; slowest tests: ...>

### B1
<logs/...-ccache-stats.txt; ccache version ...>

### B2
<snapshot present: yes/no; size; last write; restore seconds>

### B3
<hit rate of first build after restore>

### B4
<what /mnt/project-files is; concurrent writes ok?>

### C1
<nproc, RAM, df, du build/*; shared host or not>

### C2
<agents in parallel; --task runs per day>

### C3
<"=== disk:" lines>

### C4
<tool versions>

### D1
<per-task loop; --task runs per task; warm between re-runs?>

### D2
<failure ranking>

### D3
<wait or continue?>

### D4
<batch scheduling; culprit finding>

### D5
<web stage runs or skips; time>

### E1
<hard nos / tried before>

### E2
<the real merge rule>

### E3
<who merges local_ci.sh changes>

### F
<three times, or the error>

## Questions back
<anything you want from me>
