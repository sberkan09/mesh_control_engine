# r11 job 5: the cloud_shadow golden on NVIDIA sdlgpu (3 of 300 blocks over 0.5% in job 2): where the blocks are and whether the
# pass or the frame under it differs. NVIDIA GL's actual image is taken with MCE_UPDATE_GOLDENS=1 into the pod's clone (restored after).
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r11.py
GO=$B/tests/render_gl/golden_out; GD=tests/render_gl/golden
ls -la $GO/*/ 2>/dev/null | grep cloud_shadow
cp $GO/linux-sdlgpu/cloud_shadow.actual.png /work/cap/vk_actual.png
gl() { env -u MCE_RHI SDL_VIDEODRIVER=offscreen "$@"; }
MCE_UPDATE_GOLDENS=1 gl $TG -tc='*cloud_shadow*' 2>&1 | grep -E 'golden written|test cases'
cp $GD/linux-gl/cloud_shadow.png /work/cap/gl_actual.png; git checkout -- $GD/linux-gl/cloud_shadow.png
# a second sdlgpu run's actual (same image?) and lavapipe's (no vendor) for reference
vk $TG -tc='*cloud_shadow*' > /dev/null 2>&1; cp $GO/linux-sdlgpu/cloud_shadow.actual.png /work/cap/vk_actual2.png
MCE_UPDATE_GOLDENS=1 lvp $TG -tc='*cloud_shadow*' 2>&1 | grep -E 'golden written'
cp $GD/linux-sdlgpu/cloud_shadow.png /work/cap/lvp_actual.png; git checkout -- $GD/linux-sdlgpu/cloud_shadow.png
python3 $R golden_blocks /work/cap/vk_actual.png $GD/linux-sdlgpu/cloud_shadow.png "nvidia vk vs sdlgpu golden"
python3 $R golden_blocks /work/cap/vk_actual2.png /work/cap/vk_actual.png "nvidia vk run 2 vs run 1"
python3 $R golden_blocks /work/cap/gl_actual.png $GD/linux-gl/cloud_shadow.png "nvidia gl vs gl golden"
python3 $R golden_blocks /work/cap/vk_actual.png /work/cap/gl_actual.png "nvidia vk vs nvidia gl"
python3 $R golden_blocks /work/cap/lvp_actual.png $GD/linux-sdlgpu/cloud_shadow.png "lavapipe now vs sdlgpu golden"
python3 $R golden_blocks $GD/linux-sdlgpu/cloud_shadow.png $GD/linux-gl/cloud_shadow.png "sdlgpu golden vs gl golden"
python3 $R golden_strip $GD/linux-sdlgpu/cloud_shadow.png /work/cap/vk_actual.png /work/cap/gl_actual.png
