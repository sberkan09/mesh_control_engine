# r12 job 5: follow-ups. Dark pixels the pass brightens (wall reflection / rock contact edges; seen on llvmpipe in the session),
# on each renderer; the caustics golden block measure for every renderer's actual against both committed goldens.
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r12.py
for be in gl vk sw lvp; do python3 $R darkgain ${be}_on ${be}_off; done
python3 $R darkgain gl_med gl_medoff
GO=$B/tests/render_gl/golden_out; GD=tests/render_gl/golden
ls -la $GO/*/ 2>/dev/null | grep caustics
vk $TG -tc='*caustics*' > /dev/null 2>&1; cp $GO/linux-sdlgpu/caustics.actual.png /work/cap/g_vk.png 2>/dev/null
MCE_UPDATE_GOLDENS=1 gl $TG -tc='*caustics*' 2>&1 | grep -E 'golden written|test cases'
cp $GD/linux-gl/caustics.png /work/cap/g_gl.png; git checkout -- $GD/linux-gl/caustics.png
MCE_UPDATE_GOLDENS=1 lvp $TG -tc='*caustics*' 2>&1 | grep -E 'golden written'
cp $GD/linux-sdlgpu/caustics.png /work/cap/g_lvp.png; git checkout -- $GD/linux-sdlgpu/caustics.png
MCE_UPDATE_GOLDENS=1 sw $TG -tc='*caustics*' 2>&1 | grep -E 'golden written'
cp $GD/linux-gl/caustics.png /work/cap/g_sw.png; git checkout -- $GD/linux-gl/caustics.png
[[ -f /work/cap/g_vk.png ]] || { MCE_UPDATE_GOLDENS=1 vk $TG -tc='*caustics*' 2>&1 | grep -E 'golden written'; cp $GD/linux-sdlgpu/caustics.png /work/cap/g_vk.png; git checkout -- $GD/linux-sdlgpu/caustics.png; }
git status --short | head
python3 $R golden_blocks /work/cap/g_gl.png $GD/linux-gl/caustics.png "nvidia gl vs linux-gl golden"
python3 $R golden_blocks /work/cap/g_vk.png $GD/linux-sdlgpu/caustics.png "nvidia vk vs linux-sdlgpu golden"
python3 $R golden_blocks /work/cap/g_vk.png /work/cap/g_gl.png "nvidia vk vs nvidia gl"
python3 $R golden_blocks /work/cap/g_sw.png $GD/linux-gl/caustics.png "llvmpipe now vs linux-gl golden"
python3 $R golden_blocks /work/cap/g_lvp.png $GD/linux-sdlgpu/caustics.png "lavapipe now vs linux-sdlgpu golden"
python3 $R golden_blocks $GD/linux-sdlgpu/caustics.png $GD/linux-gl/caustics.png "sdlgpu golden vs gl golden"
echo "r12 done $(date -u +%H:%M:%S)"
