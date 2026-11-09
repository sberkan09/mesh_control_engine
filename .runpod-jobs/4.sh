# r9 job 4: GPU cost at 1920x1080 (r8's perf command): tour_river and 13_water (stock, and with SSAO so the water reads the
# depth), High and Medium, GL x2 and sdlgpu x2: both on, refract 0, reflect 0, both 0.
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r9.py
run() { # run <be> <tag> <pack> <scene> <quality> [extra...] -> /work/out/<be>_<tag>_<i>.mcperf
  local be=$1 tag=$2 pk=$3 sc=$4 q=$5; shift 5; local i=1; while [[ -e /work/out/${be}_${tag}_$i.mcperf ]]; do i=$((i+1)); done
  local t=$(date +%s)
  $be $P -pack $pk -scene $sc -window 1920x1080 -perf_capture /work/out/${be}_${tag}_$i.mcperf -sim.fast_forward 1 -sim.max_catchup 1 \
     -r.vsync 0 -perf_warmup 60 -frames 600 -r.quality $q -platform.error_box 0 -pause_on_focus_loss 0 "$@" > /work/out/${be}_${tag}_$i.log 2>&1
  local rc=$?; [[ $rc != 0 ]] && { echo "!! $be $tag rc=$rc"; tail -3 /work/out/${be}_${tag}_$i.log; }
  echo "$be $tag run $i: $(( $(date +%s) - t )) s" >> /work/out/times.txt
}
set4() { # set4 <be> <name> <pack> <scene> <q>
  run $1 $2_$5_on $3 $4 $5; run $1 $2_$5_refract0 $3 $4 $5 -r.water.refract 0
  run $1 $2_$5_reflect0 $3 $4 $5 -r.water.reflect 0; run $1 $2_$5_both0 $3 $4 $5 -r.water.refract 0 -r.water.reflect 0
}
T0=$(date +%s)
for rep in 1 2; do for be in gl vk; do for q in high medium; do
  set4 $be river $TOUR $RS $q; set4 $be wssao $WSSAO $WS $q
  run $be water_${q}_on $WATER $WS $q; run $be water_${q}_both0 $WATER $WS $q -r.water.refract 0 -r.water.reflect 0
done; done; done
echo "perf runs in $(( $(date +%s) - T0 )) s"; tail -3 /work/out/times.txt
head -c 300 /work/out/gl_river_high_on_1.mcperf; echo
for f in $(ls /work/out/*_1.mcperf | sort); do t=$(basename $f _1.mcperf); printf '%-26s ' $t; python3 $R perf /work/out/${t}_*.mcperf; done
