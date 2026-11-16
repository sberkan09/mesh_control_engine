- `perf_run` progress and short runs, M-CONTENT step 3 (GH-390): with a `progressToken` it sends `notifications/progress` while
  the player runs (launching, warmup N/M, capturing N/M, reading the capture); `frames` now starts at 1 and a run of
  fewer than 30 frames says in a `note` that its percentiles are noisy. M-CONTENT is closed in `docs/milestones/M-CONTENT.md`.
