#!/usr/bin/env bash
# r7 job 2: ctest -L gpu, milestone filter first, then the full label; GL offscreen and sdlgpu under Xvfb.
cd /work/repo; B=build/linux-gcc-release; mkdir -p /work/logs /tmp/xdg; chmod 700 /tmp/xdg; export XDG_RUNTIME_DIR=/tmp/xdg
F='lens_flare|lens_rain|lensrain|heat_haze|haze|underwater|translucen|sun_shafts|shafts|kit3d_tour|test_render3d|daynight_sunrise'
summ(){ grep -E 'tests passed|Total Test time' "$1"; grep -E '\*\*\*|Failed|Not Run|Timeout' "$1" | grep -v '^ *[0-9]*/[0-9]* Test.*Passed' | head -30; }
echo "== tests matched by the filter"; ctest --test-dir $B -L gpu -R "$F" -N | grep -E 'Test +#' | sed 's/^ *//' | tr '\n' ';'; echo
echo "== GL offscreen, filtered"
SDL_VIDEODRIVER=offscreen ctest --test-dir $B -L gpu -R "$F" -j2 --timeout 300 --output-on-failure > /work/logs/ct_gl_f.log 2>&1; summ /work/logs/ct_gl_f.log
echo "== sdlgpu Xvfb, filtered"
MCE_RHI=sdlgpu VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json xvfb-run -a -s "-screen 0 1920x1080x24" ctest --test-dir $B -L gpu -R "$F" -j2 --timeout 300 --output-on-failure > /work/logs/ct_vk_f.log 2>&1; summ /work/logs/ct_vk_f.log
echo "== GL offscreen, full label"
SDL_VIDEODRIVER=offscreen ctest --test-dir $B -L gpu -j3 --timeout 300 --output-on-failure > /work/logs/ct_gl.log 2>&1; summ /work/logs/ct_gl.log
echo "== sdlgpu Xvfb, full label"
MCE_RHI=sdlgpu VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json xvfb-run -a -s "-screen 0 1920x1080x24" ctest --test-dir $B -L gpu -j3 --timeout 300 --output-on-failure > /work/logs/ct_vk.log 2>&1; summ /work/logs/ct_vk.log
