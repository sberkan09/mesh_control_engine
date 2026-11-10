# r10 job 5: driving (A28). The player idles 1000 frames (the volume fills), gets in the car and holds the throttle for 1000 frames
# along the avenue (1 tick a frame). GITRACE gives camera position, probes, scrolls per frame; perf has the stage times.
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r10.py
python3 $R shift_input samples/09_openworld/drive.input /work/var/drive_long.input 1000 130 1130
run() { local be=$1 tag=$2 pk=$3 wh=$4; shift 4
  MCE_GI_TRACE=1 $be $P -pack $pk -scene $SC -input /work/var/drive_long.input -window $wh -perf_capture /work/out/drv_${tag}.mcperf -sim.fast_forward 1 -sim.max_catchup 1 \
     -r.vsync 0 -perf_warmup 0 -frames 2300 -r.quality high -platform.error_box 0 -pause_on_focus_loss 0 "$@" > /work/out/drv_${tag}.log 2>&1
  echo "rc=$? $tag"; }
run gl C_gl $CITY 1920x1080; run gl B_gl $NOVIS 1920x1080; run vk C_vk $CITY 1920x1080; run gl C_gl720 $CITY 1280x720
for t in C_gl B_gl C_vk C_gl720; do echo "--- $t"; python3 $R trace /work/out/drv_$t.log | head -60
  python3 $R perfwin /work/out/drv_$t.mcperf 1200 1500; python3 $R perfwin /work/out/drv_$t.mcperf 900 1000; python3 $R perfwin /work/out/drv_$t.mcperf 1500 2300; done
