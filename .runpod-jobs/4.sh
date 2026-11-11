# r12 job 4: GPU cost (the opt-in case ran in job 2). The pass on
# tour_caustics at 1920x1080 and 1280x720, Low / Medium / High, GL (sdlgpu at High 1080p): on, off (-r.caustics 0), on again;
# 1 tick a frame, 600 frames after 60 warm-up.
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r12.py
run() { # run <be> <tag> <WxH> <quality> [extra...] -> /work/out/<be>_<tag>_<i>.mcperf
  local be=$1 tag=$2 wh=$3 q=$4; shift 4; local i=1; while [[ -e /work/out/${be}_${tag}_$i.mcperf ]]; do i=$((i+1)); done
  $be $P -pack $PK -scene $SC -window $wh -perf_capture /work/out/${be}_${tag}_$i.mcperf -sim.fast_forward 1 -sim.max_catchup 1 \
     -r.vsync 0 -perf_warmup 60 -frames 600 -r.quality $q -platform.error_box 0 -pause_on_focus_loss 0 "$@" > /work/out/${be}_${tag}_$i.log 2>&1
  local rc=$?; [[ $rc != 0 ]] && { echo "!! $be $tag rc=$rc"; tail -3 /work/out/${be}_${tag}_$i.log; }
}
T0=$(date +%s)
for wh in 1920x1080 1280x720; do for q in low medium high; do
  run gl ${q}_${wh}_on $wh $q -r.caustics 1; run gl ${q}_${wh}_off $wh $q -r.caustics 0; run gl ${q}_${wh}_on $wh $q -r.caustics 1
done; done
run vk high_1920x1080_on 1920x1080 high -r.caustics 1; run vk high_1920x1080_off 1920x1080 high -r.caustics 0
run vk high_1920x1080_on 1920x1080 high -r.caustics 1; run vk high_1920x1080_off 1920x1080 high -r.caustics 0
echo "perf runs in $(( $(date +%s) - T0 )) s"
head -c 400 /work/out/gl_high_1920x1080_on_1.mcperf; echo
for be in gl vk; do for wh in 1920x1080 1280x720; do for q in low medium high; do
  [[ -e /work/out/${be}_${q}_${wh}_on_1.mcperf ]] || continue
  for s in on off; do printf '%-3s %-6s %-9s %-3s ' $be $q $wh $s; python3 $R perf /work/out/${be}_${q}_${wh}_${s}_*.mcperf; done
  for f in /work/out/${be}_${q}_${wh}_on_*.mcperf; do printf '      run %s: ' $(basename $f); python3 $R perf $f; done
done; done; done
grep -h -m1 -E 'rhi\.(gl|sdlgpu):' /work/out/gl_high_1920x1080_on_1.log /work/out/vk_high_1920x1080_on_1.log
echo "r12 jobs 1-4 done $(date -u +%H:%M:%S)"
