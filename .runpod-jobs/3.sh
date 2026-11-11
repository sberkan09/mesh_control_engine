# r12 job 3: gpu_compare.py on sample_kit3d_tour_caustics_gpu (real GPU vs llvmpipe), then the capture at tick 120 (1280x720, High)
# with -r.caustics 1 and 0 on GL, sdlgpu, llvmpipe and lavapipe: numbers, the light the pass adds and an ASCII look.
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r12.py
echo "=== gpu_compare.py sample_kit3d_tour_caustics_gpu (as committed, -frames 120) and @120 (tick-pinned)"
GPUCMP_ASCII=1 python3 scripts/ci/gpu_compare.py $B sample_kit3d_tour_caustics_gpu sample_kit3d_tour_caustics_gpu@120
cap() { # cap <be> <tag> [extra...]
  local be=$1 tag=$2; shift 2
  $be $P -pack $PK -scene $SC -window 1280x720 -capture_tick 120 -capture /work/cap/$tag.ppm -r.quality high -platform.error_box 0 \
     -r.vsync 0 -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0 "$@" > /work/cap/$tag.log 2>&1
  local rc=$?; [[ $rc != 0 || ! -f /work/cap/$tag.ppm ]] && { echo "!! $tag rc=$rc"; tail -3 /work/cap/$tag.log; }
  grep -m1 -E 'rhi\.(gl|sdlgpu):' /work/cap/$tag.log | sed "s/^/$tag: /"
}
for be in gl vk sw lvp; do cap $be ${be}_on -r.caustics 1; cap $be ${be}_off -r.caustics 0; done
cap gl gl_on2 -r.caustics 1; cap gl gl_med -r.quality medium -r.caustics 1; cap gl gl_medoff -r.quality medium -r.caustics 0
cap sw sw_med -r.quality medium -r.caustics 1; cap gl gl_t300 -capture_tick 300 -r.caustics 1
for be in gl vk sw lvp; do python3 $R cmp ${be}_on ${be}_off; done
python3 $R cmp gl_on gl_on2; python3 $R cmp gl_on vk_on; python3 $R cmp gl_on sw_on; python3 $R cmp gl_on lvp_on; python3 $R cmp vk_on lvp_on
python3 $R cmp gl_off sw_off; python3 $R cmp gl_off vk_off; python3 $R cmp gl_med sw_med; python3 $R cmp gl_med gl_medoff; python3 $R cmp gl_on gl_t300
for p in "gl_on gl_off" "vk_on vk_off" "sw_on sw_off" "lvp_on lvp_off" "gl_med gl_medoff"; do python3 $R light $p; done
python3 $R ascii gl_on gl_off
