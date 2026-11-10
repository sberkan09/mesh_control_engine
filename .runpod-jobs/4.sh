# r10 job 4: before/after street captures (A29): 6 views x {C committed, B plain volume (novis scene), D -r.gi.visibility 0} on GL,
# the three first views of C on sdlgpu, and a 640x360 GL vs llvmpipe pair of one view (the probe sequence is frame-counted, so it matches).
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r10.py
cap() { # cap <be> <tag> <pack> <view> <WxH> <frames> [extra...]
  local be=$1 tag=$2 pk=$3 vw=$4 wh=$5 fr=$6; shift 6
  MCE_GI_TRACE=1 $be $P -pack $pk -scene $SC -window $wh -frames $fr -capture /work/cap/$tag.ppm -r.quality high -platform.error_box 0 \
     -r.vsync 0 -sim.fast_forward 1 -sim.max_catchup 1 -pause_on_focus_loss 0 -view.free $vw "$@" > /work/cap/$tag.log 2>&1
  local rc=$?; [[ $rc != 0 || ! -f /work/cap/$tag.ppm ]] && { echo "!! $tag rc=$rc"; tail -3 /work/cap/$tag.log; }
  grep -E 'GITRACE' /work/cap/$tag.log | tail -1 | sed "s/^/$tag last: /"
  grep -c 'gi=1' /work/cap/$tag.log | sed "s/^/$tag frames with gi=1: /"
}
declare -A V=( [p1]="-125,1.7,-8.3,-60,2.0,-8.3,70" [p2]="-125,1.7,-8.3,-118,7,-17,70" [a1]="-88,1.7,-12,-88,3,-70,70" [a2]="-88,1.7,-45,-88,3,-5,70" \
               [f1]="-50,1.7,-6,-50,3,21,70" [f2]="-20,1.7,10,-50,3,21,70" )
# llvmpipe reference of one view at 640x360 in the background (it takes a while), GL twin first
cap gl s_p1_gl $CITY ${V[p1]} 640x360 1000
( cap sw s_p1_sw $CITY ${V[p1]} 640x360 1000 > /work/cap/sw_bg.txt 2>&1 ) &
BG=$!
for k in p1 p2 a1 a2 f1 f2; do
  cap gl ${k}_C_gl $CITY ${V[$k]} 1280x720 1000
  cap gl ${k}_B_gl $NOVIS ${V[$k]} 1280x720 1000
  cap gl ${k}_D_gl $CITY ${V[$k]} 1280x720 1000 -r.gi.visibility 0
  cap gl ${k}_A_gl $CITY ${V[$k]} 1280x720 1000 -r.gi 0
done
for k in p1 a1 f1; do cap vk ${k}_C_vk $CITY ${V[$k]} 1280x720 1000; done
cap vk p1_B_vk $NOVIS ${V[p1]} 1280x720 1000
for k in p1 p2 a1 a2 f1 f2; do echo "--- $k"; python3 $R cmp ${k}_C_gl ${k}_B_gl; python3 $R cmp ${k}_C_gl ${k}_D_gl; python3 $R cmp ${k}_C_gl ${k}_A_gl; python3 $R cmp ${k}_B_gl ${k}_A_gl; done
for k in p1 a1 f1; do python3 $R cmp ${k}_C_gl ${k}_C_vk; done; python3 $R cmp p1_B_gl p1_B_vk
python3 $R cmp s_p1_gl s_p1_gl
echo "--- trace of p1_C_gl"; python3 $R trace /work/cap/p1_C_gl.log; echo "--- trace of p1_C_vk"; python3 $R trace /work/cap/p1_C_vk.log
echo "waiting for llvmpipe"; wait $BG; cat /work/cap/sw_bg.txt; python3 $R cmp s_p1_gl s_p1_sw; python3 $R trace /work/cap/s_p1_sw.log
ls -la /work/cap/*.ppm | head -50
