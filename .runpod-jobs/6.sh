# r11 job 6: would the llvmpipe linux-gl golden hold for NVIDIA sdlgpu (a candidate for linux-sdlgpu-nvidia/)? Numbers only.
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r11.py; GD=tests/render_gl/golden
python3 $R golden_blocks /work/cap/vk_actual.png $GD/linux-gl/cloud_shadow.png "nvidia vk vs gl golden (llvmpipe)"
python3 $R golden_blocks /work/cap/lvp_actual.png /work/cap/gl_actual.png "lavapipe vs nvidia gl"
python3 -c "import hashlib;[print(p, hashlib.sha256(open(p,'rb').read()).hexdigest()[:16]) for p in ['/work/cap/vk_actual.png','/work/cap/gl_actual.png','$GD/linux-gl/cloud_shadow.png','$GD/linux-sdlgpu/cloud_shadow.png']]"
echo "r11 done $(date -u +%H:%M:%S)"
