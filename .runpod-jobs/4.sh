# r8 job 4: roughness ladder images at 1920x1080, tick 180 (the sphere is clear of the far row), GL on the real GPU.
cd /work/repo; source .runpod-jobs/common.sh
R=.runpod-jobs/r8.py; V=/work/var; I=/work/img; mkdir -p $I; TS=scenes/tour_reflect.mcscene
cap() { # cap <name> <pack> <quality> [extra...]
  local n=$1 pk=$2 q=$3; shift 3
  gl $P -pack $pk -scene $TS -window 1920x1080 -capture_tick 180 -capture $I/$n.ppm -r.quality $q -platform.error_box 0 -r.vsync 0 \
     -pause_on_focus_loss 0 -sim.fast_forward 1 -sim.max_catchup 32 "$@" > $I/$n.log 2>&1 || { echo "!! $n"; tail -3 $I/$n.log; }
}
for L in ta tb; do for q in high medium; do
  cap ${L}_${q}_off $V/${L}0.8/assets.mce $q -r.ssr 0
  for mr in 0.5 0.6 0.8; do cap ${L}${mr}_${q} $V/$L$mr/assets.mce $q; cap ${L}${mr}_${q}_blur0 $V/$L$mr/assets.mce $q -r.ssr.blur 0; done
done; done
# repeat one capture: is the frame deterministic on this GPU?
cap tb0.8_high_rep $V/tb0.8/assets.mce high; cmp $I/tb0.8_high.ppm $I/tb0.8_high_rep.ppm && echo "repeat identical"
echo "strips: A = roughness 0.0 0.15 0.3 0.5 0.7, B = 0.4 0.5 0.6 0.7 0.8; fade 0.2"
for L in ta tb; do for q in high medium; do
  echo "== ladder $L $q (vs r.ssr 0)"
  python3 $R ladder $I/${L}_${q}_off.ppm $(for mr in 0.5 0.6 0.8; do echo $I/${L}${mr}_${q}.ppm $I/${L}${mr}_${q}_blur0.ppm; done)
done; done
# llvmpipe reference of the same ladder (B, 0.8, high) for the GPU-vs-software check
sw $P -pack $V/tb0.8/assets.mce -scene $TS -window 1920x1080 -capture_tick 180 -capture $I/tb0.8_high_sw.ppm -r.quality high -platform.error_box 0 \
   -r.vsync 0 -pause_on_focus_loss 0 -sim.fast_forward 1 -sim.max_catchup 32 > $I/sw.log 2>&1; echo "sw rc=$?"
python3 - <<'PY'
import numpy as np; from PIL import Image
a=np.asarray(Image.open('/work/img/tb0.8_high.ppm').convert('RGB'),dtype=np.int16); b=np.asarray(Image.open('/work/img/tb0.8_high_sw.ppm').convert('RGB'),dtype=np.int16)
d=np.abs(a-b).max(2); print(f"ladder B 0.8 high: GPU vs llvmpipe diff>12 {100*(d>12).mean():.2f}% mean|d| {np.abs(a-b).mean():.2f}")
PY
for n in tb0.5_medium tb0.6_medium tb0.8_medium tb0.8_medium_blur0 tb0.5_high tb0.8_high ta0.5_high ta0.8_high; do python3 $R jpeg $I/$n.ppm $n; done
