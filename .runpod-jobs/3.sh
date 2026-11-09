# r9 job 3: river / 13_water captures at pinned ticks, 1280x720, High: GL and sdlgpu against llvmpipe (and lavapipe), and
# the effects' on/off share on the real GPU.
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r9.py
cap() { # cap <be> <tag> <pack> <scene> <tick> [extra...]
  local be=$1 tag=$2 pk=$3 sc=$4 tk=$5; shift 5
  $be $P -pack $pk -scene $sc -window 1280x720 -capture_tick $tk -capture /work/cap/$tag.ppm -r.quality high -platform.error_box 0 \
     -r.vsync 0 -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0 "$@" > /work/cap/$tag.log 2>&1
  local rc=$?; [[ $rc != 0 || ! -f /work/cap/$tag.ppm ]] && { echo "!! $tag rc=$rc"; tail -3 /work/cap/$tag.log; }
}
for t in 120 300; do
  for be in gl vk sw lvp; do cap $be river_${t}_$be $TOUR $RS $t; done
  for v in "refract0:-r.water.refract 0" "reflect0:-r.water.reflect 0" "both0:-r.water.refract 0 -r.water.reflect 0"; do
    cap gl river_${t}_gl_${v%%:*} $TOUR $RS $t ${v#*:}; cap vk river_${t}_vk_${v%%:*} $TOUR $RS $t ${v#*:}; done
done
for be in gl vk sw lvp; do cap $be wssao_300_$be $WSSAO $WS 300; cap $be water_300_$be $WATER $WS 300; done
cap gl wssao_300_gl_both0 $WSSAO $WS 300 -r.water.refract 0 -r.water.reflect 0
cap vk wssao_300_vk_both0 $WSSAO $WS 300 -r.water.refract 0 -r.water.reflect 0
grep -h -E 'rhi\.(gl|sdlgpu):' /work/cap/river_120_gl.log /work/cap/river_120_vk.log /work/cap/river_120_sw.log /work/cap/river_120_lvp.log | cut -c1-160
for t in 120 300; do
  python3 $R cmp river_${t}_gl river_${t}_sw; python3 $R cmp river_${t}_vk river_${t}_sw; python3 $R cmp river_${t}_vk river_${t}_lvp; python3 $R cmp river_${t}_sw river_${t}_lvp
  for v in refract0 reflect0 both0; do python3 $R cmp river_${t}_gl river_${t}_gl_$v; python3 $R cmp river_${t}_vk river_${t}_vk_$v; done
done
for s in wssao water; do python3 $R cmp ${s}_300_gl ${s}_300_sw; python3 $R cmp ${s}_300_vk ${s}_300_sw; python3 $R cmp ${s}_300_vk ${s}_300_lvp; done
python3 $R cmp wssao_300_gl wssao_300_gl_both0; python3 $R cmp wssao_300_vk wssao_300_vk_both0; python3 $R cmp water_300_gl wssao_300_gl_both0
python3 $R cmp river_120_gl river_120_gl  # self-check 0
