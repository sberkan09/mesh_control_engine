# r10 job 7: the GL crash (rc=139) of the committed city at Medium, 1920x1080, found by job 3. Crash reports, repeats, variants.
cd /work/repo; source .runpod-jobs/common.sh
for f in $(ls -t /root/.local/share/MCE/mce_player/crashes/*.crash | head -3); do echo "=== $f"; head -c 6000 $f; echo; done
run() { local be=$1 tag=$2 pk=$3 wh=$4 q=$5; shift 5
  MCE_GI_TRACE=1 $be $P -pack $pk -scene $SC -window $wh -sim.fast_forward 1 -sim.max_catchup 1 -r.vsync 0 -frames 1600 -r.quality $q -platform.error_box 0 -pause_on_focus_loss 0 "$@" > /work/out/crash_${tag}.log 2>&1
  rc=$?; echo "$tag rc=$rc, last trace: $(grep GITRACE /work/out/crash_${tag}.log | tail -1 | cut -c1-120)"; }
run gl m1 $CITY 1920x1080 medium; run gl m2 $CITY 1920x1080 medium; run gl m3 $CITY 1920x1080 medium
run gl m_novis $NOVIS 1920x1080 medium; run gl m_vis0 $CITY 1920x1080 medium -r.gi.visibility 0
run gl m_720 $CITY 1280x720 medium; run gl h_1 $CITY 1920x1080 high
run vk m1 $CITY 1920x1080 medium
