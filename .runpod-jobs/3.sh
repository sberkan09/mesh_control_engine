# r11 job 3: the sample_kit3d_tour_cloudshadow_gpu capture (tick 300, 1280x720, High) with -r.cloud_shadows 1 and 0 on GL, sdlgpu,
# llvmpipe and lavapipe; numbers, an ASCII look and a small JPEG strip (on | off | 4x difference) as base64 in the log.
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r11.py
cap() { # cap <be> <tag> [extra...]
  local be=$1 tag=$2; shift 2
  $be $P -pack $PK -scene $SC -window 1280x720 -capture_tick 300 -capture /work/cap/$tag.ppm -r.quality high -platform.error_box 0 \
     -r.vsync 0 -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0 "$@" > /work/cap/$tag.log 2>&1
  local rc=$?; [[ $rc != 0 || ! -f /work/cap/$tag.ppm ]] && { echo "!! $tag rc=$rc"; tail -3 /work/cap/$tag.log; }
  grep -m1 -E 'rhi\.(gl|sdlgpu):' /work/cap/$tag.log | sed "s/^/$tag: /"
}
for be in gl vk sw lvp; do cap $be ${be}_on -r.cloud_shadows 1; cap $be ${be}_off -r.cloud_shadows 0; done
cap gl gl_on2 -r.cloud_shadows 1; cap gl gl_med -r.quality medium -r.cloud_shadows 1
for be in gl vk sw lvp; do python3 $R cmp ${be}_on ${be}_off; done
python3 $R cmp gl_on gl_on2; python3 $R cmp gl_on vk_on; python3 $R cmp gl_on sw_on; python3 $R cmp gl_on lvp_on; python3 $R cmp vk_on lvp_on
python3 $R cmp gl_off sw_off; python3 $R cmp gl_med gl_on; python3 $R cmp gl_med gl_off
python3 $R shade gl_on gl_off; python3 $R shade gl_med gl_off; python3 $R shade vk_on vk_off; python3 $R shade sw_on sw_off
python3 $R ascii gl_on gl_off
python3 $R strip gl_on gl_off
