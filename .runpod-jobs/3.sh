# r10 job 3: GPU cost of the city's volume (A28). Arms: A = -r.gi 0 (the city as before 50i, no volume), B = scene copy with
# GiVisibility3D enabled 0 (a plain volume, fixed box, 4 texels a probe), C = as committed, D = C with -r.gi.visibility 0.
# 1 tick a frame (-sim.max_catchup 1): the fill is counted in frames.
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r10.py
run() { # run <be> <tag> <pack> <WxH> <quality> <warmup> <frames> [extra...] -> /work/out/<be>_<tag>_<i>.mcperf
  local be=$1 tag=$2 pk=$3 wh=$4 q=$5 wu=$6 fr=$7; shift 7; local i=1; while [[ -e /work/out/${be}_${tag}_$i.mcperf ]]; do i=$((i+1)); done
  local t=$(date +%s)
  $be $P -pack $pk -scene $SC -window $wh -perf_capture /work/out/${be}_${tag}_$i.mcperf -sim.fast_forward 1 -sim.max_catchup 1 \
     -r.vsync 0 -perf_warmup $wu -frames $fr -r.quality $q -platform.error_box 0 -pause_on_focus_loss 0 "$@" > /work/out/${be}_${tag}_$i.log 2>&1
  local rc=$?; [[ $rc != 0 ]] && { echo "!! $be $tag rc=$rc"; tail -3 /work/out/${be}_${tag}_$i.log; }
  echo "$be $tag run $i: $(( $(date +%s) - t )) s" >> /work/out/times.txt
}
echo "=== fill runs (trace, warmup 0, 1700 frames, 1920x1080 High)"
export MCE_GI_TRACE=1
run gl fill_C $CITY 1920x1080 high 0 1700
run gl fill_B $NOVIS 1920x1080 high 0 1700
run vk fill_C $CITY 1920x1080 high 0 1700
unset MCE_GI_TRACE
head -c 300 /work/out/gl_fill_C_1.mcperf; echo
for f in gl_fill_C_1 gl_fill_B_1 vk_fill_C_1; do echo "--- $f"; python3 $R trace /work/out/$f.log; for w in "0 100" "100 700" "700 780" "900 1700"; do python3 $R perfwin /work/out/$f.mcperf $w; done; done
echo "=== steady state (warmup 1000, 1600 frames)"
T0=$(date +%s)
for rep in 1 2; do for q in high medium; do
  run gl A_${q}_1080 $CITY 1920x1080 $q 1000 1600 -r.gi 0
  run gl B_${q}_1080 $NOVIS 1920x1080 $q 1000 1600
  run gl C_${q}_1080 $CITY 1920x1080 $q 1000 1600
  run gl D_${q}_1080 $CITY 1920x1080 $q 1000 1600 -r.gi.visibility 0
done; done
for a in A B C; do run gl ${a}_high_720 $([[ $a == B ]] && echo $NOVIS || echo $CITY) 1280x720 high 1000 1600 $([[ $a == A ]] && echo "-r.gi 0"); done
run vk A_high_1080 $CITY 1920x1080 high 1000 1600 -r.gi 0; run vk B_high_1080 $NOVIS 1920x1080 high 1000 1600
run vk C_high_1080 $CITY 1920x1080 high 1000 1600; run vk D_high_1080 $CITY 1920x1080 high 1000 1600 -r.gi.visibility 0
run vk B_medium_1080 $NOVIS 1920x1080 medium 1000 1600; run vk C_medium_1080 $CITY 1920x1080 medium 1000 1600
echo "steady runs in $(( $(date +%s) - T0 )) s"; tail -4 /work/out/times.txt
for f in $(ls /work/out/*_1080_1.mcperf /work/out/*_720_1.mcperf | sort); do t=$(basename $f _1.mcperf); printf '%-22s ' $t; python3 $R perf /work/out/${t}_*.mcperf; done
