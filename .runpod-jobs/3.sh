# r8 job 3: variant packs (job-side copies of the sample assets, nothing committed) and GPU cost at 1920x1080.
cd /work/repo; source .runpod-jobs/common.sh
R=.runpod-jobs/r8.py; V=/work/var; mkdir -p $V
TA=samples/kit3d_tour/assets; CA=samples/09_openworld/assets; TS=scenes/tour_reflect.mcscene; CS=scenes/city.mcscene
LB="mat:materials/reflect_r00.mcmat=0.4 mat:materials/reflect_r15.mcmat=0.5 mat:materials/reflect_r30.mcmat=0.6 mat:materials/reflect_r50.mcmat=0.7 mat:materials/reflect_r70.mcmat=0.8"
mk() { local n=$1 src=$2 sc=$3; shift 3; python3 $R variant $src $V/$n $sc "$@" && $C $V/$n --pack --quiet > /work/cook_$n.log 2>&1; echo "cook $n rc=$? $(ls -s $V/$n/assets.mce 2>/dev/null)"; }
T0=$(date +%s)
for mr in 0.5 0.6 0.8; do mk ta$mr $TA $TS ssr_max_roughness=$mr; mk tb$mr $TA $TS ssr_max_roughness=$mr $LB; done
for mr in 0.6 0.8; do mk city$mr $CA $CS ssr_max_roughness=$mr; done
echo "variants in $(( $(date +%s) - T0 )) s"
run() { # run <tag> <pack> <scene> <quality> [extra...]  -> /work/out/<tag>_<i>.mcperf
  local tag=$1 pk=$2 sc=$3 q=$4; shift 4; local i=1; while [[ -e /work/out/${tag}_$i.mcperf ]]; do i=$((i+1)); done
  local t=$(date +%s)
  gl $P -pack $pk -scene $sc -window 1920x1080 -perf_capture /work/out/${tag}_$i.mcperf -sim.fast_forward 1 -sim.max_catchup 1 \
     -r.vsync 0 -perf_warmup 60 -frames 600 -r.quality $q -platform.error_box 0 -pause_on_focus_loss 0 "$@" > /work/out/${tag}_$i.log 2>&1
  local rc=$?; [[ $rc != 0 ]] && { echo "!! $tag rc=$rc"; tail -3 /work/out/${tag}_$i.log; }
  echo "$tag run $i: $(( $(date +%s) - t )) s" >> /work/out/times.txt
}
for rep in 1 2; do
  for q in high medium; do
    run tour_${q}_on $TOUR $TS $q; run tour_${q}_blur0 $TOUR $TS $q -r.ssr.blur 0; run tour_${q}_motion0 $TOUR $TS $q -r.ssr.motion 0; run tour_${q}_ssr0 $TOUR $TS $q -r.ssr 0
  done
  for mr in 0.5 0.6; do run tour_high_mr$mr $V/ta$mr/assets.mce $TS high; done
  for mr in 0.5 0.6 0.8; do run tourB_high_mr$mr $V/tb$mr/assets.mce $TS high; run tourB_high_mr${mr}_blur0 $V/tb$mr/assets.mce $TS high -r.ssr.blur 0; done
  run city_high_on $CITY $CS high; run city_high_blur0 $CITY $CS high -r.ssr.blur 0; run city_high_motion0 $CITY $CS high -r.ssr.motion 0; run city_high_ssr0 $CITY $CS high -r.ssr 0
  run city_medium_on $CITY $CS medium; run city_medium_blur0 $CITY $CS medium -r.ssr.blur 0
  for mr in 0.6 0.8; do run city_high_mr$mr $V/city$mr/assets.mce $CS high; run city_high_mr${mr}_blur0 $V/city$mr/assets.mce $CS high -r.ssr.blur 0; done
done
tail -4 /work/out/times.txt; grep -h -E 'OpenGL|renderer' /work/out/tour_high_on_1.log | head -3; head -c 400 /work/out/tour_high_on_1.mcperf; echo
grep -o '"gpu_stages": \[[^]]*\]' /work/out/tour_high_on_1.mcperf
for f in $(ls /work/out/*_1.mcperf | sort); do t=$(basename $f _1.mcperf); printf '%-28s ' $t; python3 $R perf /work/out/${t}_*.mcperf; done
grep -h -E 'ssr|3D path' /work/out/tour_high_on_1.log /work/out/city_high_on_1.log | head -6
