# r9 job 5: (a) where tour_river differs between the GPU and llvmpipe: repeats, effects off, water off, Medium (no TAA);
# (b) the VK_ERROR_DEVICE_LOST seen in one sdlgpu golden run: repeat the goldens on sdlgpu.
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r9.py
cap() { local be=$1 tag=$2 pk=$3 sc=$4 tk=$5; shift 5
  $be $P -pack $pk -scene $sc -window 1280x720 -capture_tick $tk -capture /work/cap/$tag.ppm -platform.error_box 0 \
     -r.vsync 0 -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0 "$@" > /work/cap/$tag.log 2>&1
  local rc=$?; [[ $rc != 0 || ! -f /work/cap/$tag.ppm ]] && { echo "!! $tag rc=$rc"; tail -3 /work/cap/$tag.log; }; }
for be in gl sw; do
  cap $be r5_hi_$be $TOUR $RS 120 -r.quality high; cap $be r5_hi2_$be $TOUR $RS 120 -r.quality high
  cap $be r5_hi_both0_$be $TOUR $RS 120 -r.quality high -r.water.refract 0 -r.water.reflect 0
  cap $be r5_hi_nowater_$be $TOUR $RS 120 -r.quality high -r.water 0
  cap $be r5_med_$be $TOUR $RS 120 -r.quality medium; cap $be r5_med_both0_$be $TOUR $RS 120 -r.quality medium -r.water.refract 0 -r.water.reflect 0
  cap $be r5_med_nowater_$be $TOUR $RS 120 -r.quality medium -r.water 0
  cap $be r5_low_$be $TOUR $RS 120 -r.quality low
done
python3 $R cmp r5_hi_gl r5_hi2_gl; python3 $R cmp r5_hi_sw r5_hi2_sw
for v in hi hi_both0 hi_nowater med med_both0 med_nowater low; do python3 $R cmp r5_${v}_gl r5_${v}_sw; python3 $R bands r5_${v}_gl r5_${v}_sw; done
python3 $R bands river_120_gl river_120_gl_both0
TG=$B/tests/render_gl/test_golden_gl
for i in 1 2 3 4; do echo "=== golden [vk] repeat $i"; vk $TG 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR|FATAL' | head -12; done
for i in 1 2 3 4 5 6 7 8; do vk $TG -tc='*heat_haze*,*lens_rain*,*underwater*,*sun_shafts*' 2>&1 | grep -E '^\[doctest\] test cases|ERROR|FATAL' | head -4; done
