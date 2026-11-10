# r10 job 6: before/after street captures (A29), second try: the player stands at the spot (scene copies), -view.free crashed the UI text.
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r10.py
python3 $R spots samples/09_openworld/assets /work/var/city_spots && $C /work/var/city_spots --pack --quiet > /work/cook_sp.log 2>&1; echo "cook spots rc=$?"; tail -1 /work/cook_sp.log
SP=/work/var/city_spots/assets.mce
cap() { # cap <be> <tag> <scene> <WxH> <frames> [extra...]
  local be=$1 tag=$2 sc=$3 wh=$4 fr=$5; shift 5
  MCE_GI_TRACE=1 $be $P -pack $SP -scene scenes/$sc.mcscene -window $wh -frames $fr -capture /work/cap/$tag.ppm -r.quality high -platform.error_box 0 \
     -r.vsync 0 -sim.fast_forward 1 -sim.max_catchup 1 -pause_on_focus_loss 0 "$@" > /work/cap/$tag.log 2>&1
  local rc=$?; [[ $rc != 0 || ! -f /work/cap/$tag.ppm ]] && { echo "!! $tag rc=$rc"; grep -E 'signal|#[0-3] ' /work/cap/$tag.log | head -3; ls -t /root/.local/share/MCE/mce_player/crashes | head -1; }
  echo "$tag: $(grep GITRACE /work/cap/$tag.log | tail -1 | cut -c1-110) gi1=$(grep -c 'gi=1' /work/cap/$tag.log)"
}
for k in p1 p2 a1 a2 f1 f2; do
  cap gl ${k}_C_gl s_$k 1280x720 1000
  cap gl ${k}_B_gl s_${k}_nv 1280x720 1000
  cap gl ${k}_D_gl s_$k 1280x720 1000 -r.gi.visibility 0
  cap gl ${k}_A_gl s_$k 1280x720 1000 -r.gi 0
done
for k in p1 a1 f1; do cap vk ${k}_C_vk s_$k 1280x720 1000; done
cap vk p1_B_vk s_p1_nv 1280x720 1000
for k in p1 p2 a1 a2 f1 f2; do echo "--- $k"; python3 $R cmp ${k}_C_gl ${k}_B_gl; python3 $R cmp ${k}_C_gl ${k}_D_gl; python3 $R cmp ${k}_C_gl ${k}_A_gl; python3 $R cmp ${k}_B_gl ${k}_A_gl; done
for k in p1 a1 f1; do python3 $R cmp ${k}_C_gl ${k}_C_vk; done; python3 $R cmp p1_B_gl p1_B_vk
ls -la /work/cap/*.ppm | wc -l
