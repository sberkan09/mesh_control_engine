#!/usr/bin/env python3
"""Summarises the soak runs in a directory: heap allocations, GPU memory and frame time, first vs last."""
import csv, glob, json, os, statistics, sys

def med(v): return statistics.median(v) if v else float("nan")

for f in sorted(glob.glob(os.path.join(sys.argv[1], "*.mcperf"))):
    tag = os.path.basename(f)[:-len(".mcperf")]
    try: c = json.load(open(f))
    except Exception as e: print(f"{tag}: {e}"); continue
    fr = c.get("frames", [])
    if not fr: print(f"{tag}: no frames"); continue
    heap = [x["counters"]["heap_allocations"] for x in fr if "counters" in x and "heap_allocations" in x["counters"]]
    s = c["summary"]; fm = s.get("frame_ms", {})
    print(f"{tag}: {len(fr)} frames, frame_ms avg {fm.get('avg', float('nan')):.3f} p50 {fm.get('p50', float('nan')):.3f}, "
          f"heap first {heap[0] if heap else '-'} last {heap[-1] if heap else '-'} max {max(heap) if heap else '-'}")
    key = next((k for k in ("frame_ms", "cpu_ms") if k in fr[0]), None)
    if key:
        n = len(fr) // 10; a = [x[key] for x in fr[:n]]; b = [x[key] for x in fr[-n:]]
        print(f"   {key} first tenth median {med(a):.3f} last tenth median {med(b):.3f}")
    mf = os.path.join(sys.argv[1], f"mem_{tag}.csv")
    if os.path.exists(mf):
        rows = []
        for r in csv.reader(open(mf)):
            try: rows.append((int(r[0]), int(r[1])))
            except Exception: pass
        if len(rows) > 40:
            t0 = rows[0][0]; first = [m for t, m in rows if t - t0 < 60 and t - t0 >= 10]; end = rows[-1][0]
            last = [m for t, m in rows if end - t < 60]
            print(f"   GPU memory (MiB): min {min(m for _, m in rows)} max {max(m for _, m in rows)}, median first minute {med(first)} last minute {med(last)}, samples {len(rows)}")
        else: print(f"   GPU memory: {len(rows)} samples (too few)")
