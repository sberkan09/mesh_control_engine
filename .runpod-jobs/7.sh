# r8 job 7: worst case for the gather, the tour's ground (most of the frame below the horizon) at roughness 0.45, max 0.8.
cd /work/repo; source .runpod-jobs/common.sh
R=.runpod-jobs/r8.py; V=/work/var; TS=scenes/tour_reflect.mcscene
python3 $R variant samples/kit3d_tour/assets $V/tg scenes/tour_reflect.mcscene ssr_max_roughness=0.8 mat:materials/reflect_ground.mcmat=0.45 && $C $V/tg --pack --quiet > /dev/null 2>&1; echo "cook rc=$?"
run() { local tag=$1 q=$2; shift 2; local i=1; while [[ -e /work/out/${tag}_$i.mcperf ]]; do i=$((i+1)); done
  gl $P -pack $V/tg/assets.mce -scene $TS -window 1920x1080 -perf_capture /work/out/${tag}_$i.mcperf -sim.fast_forward 1 -sim.max_catchup 1 \
     -r.vsync 0 -perf_warmup 60 -frames 600 -r.quality $q -platform.error_box 0 -pause_on_focus_loss 0 "$@" > /dev/null 2>&1 || echo "!! $tag"; }
for rep in 1 2; do for q in high medium; do run tg_${q}_on $q; run tg_${q}_blur0 $q -r.ssr.blur 0; run tg_${q}_ssr0 $q -r.ssr 0; done; done
for t in tg_high_on tg_high_blur0 tg_high_ssr0 tg_medium_on tg_medium_blur0 tg_medium_ssr0; do printf '%-18s ' $t; python3 $R perf /work/out/${t}_[0-9].mcperf | sed -e 's/ clusters.*ssao/ ssao/' -e 's/ gi 0.001 volumetrics 0.001//' -e 's/ fxaa 0.001//'; done
I=/work/img; for q in high medium; do for v in on blur0 ssr0; do a=""; [[ $v == blur0 ]] && a="-r.ssr.blur 0"; [[ $v == ssr0 ]] && a="-r.ssr 0"
  gl $P -pack $V/tg/assets.mce -scene $TS -window 1920x1080 -capture_tick 180 -capture $I/tg_${q}_$v.ppm -r.quality $q -platform.error_box 0 -r.vsync 0 \
     -pause_on_focus_loss 0 -sim.fast_forward 1 -sim.max_catchup 32 $a > /dev/null 2>&1; done; done
python3 - <<'PY'
import sys; sys.path.insert(0, '/work/repo/.runpod-jobs'); import r8, numpy as np
for q in ("high", "medium"):
    _, Lo = r8.lum(f"/work/img/tg_{q}_ssr0.ppm"); Ho = r8.hf(Lo); m = np.zeros_like(Lo, bool); m[900:1075, :] = True  # the ground below the near row
    for v in ("on", "blur0"):
        _, L = r8.lum(f"/work/img/tg_{q}_{v}.ppm"); H = r8.hf(L)
        print(f"ground 0.45 {q} {v}: refl {np.abs(L-Lo)[m].mean():.2f} hf {(H-Ho)[m].mean():+.2f} (ssr0 hf {Ho[m].mean():.2f})")
PY
