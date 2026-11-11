# soak job 2: M-SOAK step 4. Each (backend, scene) runs FRAMES frames windowed at 1280x720 High on the real GPU while
# nvidia-smi samples the card's memory every 2 s. Per run: the live heap allocation count first vs last frame
# (the CPU side), the GPU memory median of the first and the last minute, the frame time of the first and last tenth.
cd /work/repo; source .runpod-jobs/common.sh
FRAMES=${FRAMES:-60000}
run() { # run <be> <scene>
  local be=$1 sc=$2 tag=${1}_${2}
  ( while sleep 2; do echo "$(date +%s),$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits | head -1)"; done ) > /work/out/mem_$tag.csv &
  local mp=$!
  $be $P -pack $PK -scene scenes/$sc.mcscene -window 1280x720 -perf_capture /work/out/$tag.mcperf -sim.fast_forward 1 \
     -sim.max_catchup 1 -r.vsync 0 -perf_warmup 300 -frames $FRAMES -r.quality high -platform.error_box 0 \
     -pause_on_focus_loss 0 > /work/out/$tag.log 2>&1
  local rc=$?; kill $mp 2>/dev/null; echo "## $tag rc=$rc"; tail -2 /work/out/$tag.log
}
T0=$(date +%s)
for sc in tour_caustics tour_river; do for be in gl vk; do run $be $sc; done; done
echo "soak runs in $(( $(date +%s) - T0 )) s"
python3 .runpod-jobs/soak.py /work/out
grep -h -m1 -E 'rhi\.(gl|sdlgpu):' /work/out/*.log
echo "soak job 2 done $(date -u +%H:%M:%S)"
