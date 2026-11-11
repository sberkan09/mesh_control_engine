# soak job 13: is the sdlgpu frame-time rise a backend drift or the card warming up? vk river first, gl river after, and again.
rm -f /work/out/DONE /work/out/summary.txt
cd /work/repo; source .runpod-jobs/common.sh
nvidia-smi --query-gpu=temperature.gpu,clocks.sm,clocks.max.sm,power.draw --format=csv
run() { # run <be> <scene> <suffix>
  local be=$1 sc=$2 tag=${1}_${2}_$3
  ( while sleep 2; do echo "$(date +%s),$(nvidia-smi --query-gpu=memory.used,temperature.gpu,clocks.sm --format=csv,noheader,nounits | head -1)"; done ) > /work/out/mem_$tag.csv &
  local mp=$!
  $be $P -pack $PK -scene scenes/$sc.mcscene -window 1280x720 -perf_capture /work/out/$tag.mcperf -sim.fast_forward 1 \
     -sim.max_catchup 1 -r.vsync 0 -perf_warmup 300 -frames 60000 -r.quality high -platform.error_box 0 -pause_on_focus_loss 0 > /work/out/$tag.log 2>&1
  local rc=$?; kill $mp 2>/dev/null; echo "## $tag rc=$rc"
}
run vk tour_river b; run gl tour_river b; run vk tour_river c; run gl tour_river c
python3 - <<'PY' > /work/out/summary.txt 2>&1
import json, glob, os, statistics
for f in sorted(glob.glob("/work/out/*_tour_river_[bc].mcperf")):
    c = json.load(open(f)); fr = c["frames"]; key = next(k for k in ("frame_ms", "cpu_ms") if k in fr[0])
    n = len(fr) // 10
    d = [statistics.median(x[key] for x in fr[i*n:(i+1)*n]) for i in range(10)]
    heap = [x["counters"]["heap_allocations"] for x in fr]
    print(os.path.basename(f), "deciles", " ".join(f"{v:.3f}" for v in d), "| heap", heap[0], heap[-1])
for f in sorted(glob.glob("/work/out/mem_*_[bc].csv")):
    rows = [l.strip().split(",") for l in open(f) if l.count(",") == 3]
    if rows: print(os.path.basename(f), "mem MiB", rows[len(rows)//10][1], "->", rows[-1][1], "temp", rows[len(rows)//10][2], "->", rows[-1][2], "sm clock", rows[len(rows)//10][3], "->", rows[-1][3], "samples", len(rows))
PY
touch /work/out/DONE
echo "soak job 13 done $(date -u +%H:%M:%S)"
