#!/usr/bin/env bash
# r7 job 3: test_golden_gl 3 runs on GL offscreen and 3 on sdlgpu under Xvfb.
cd /work/repo; B=build/linux-gcc-release; mkdir -p /work/logs /tmp/xdg; chmod 700 /tmp/xdg; export XDG_RUNTIME_DIR=/tmp/xdg
G=$(find $B -name test_golden_gl -type f -perm -u+x | head -1); echo "binary $G"
for i in 1 2 3; do
  SDL_VIDEODRIVER=offscreen $G > /work/logs/golden_gl_$i.log 2>&1; echo "GL run $i rc=$?"
  grep -E '^\[doctest\] (test cases|assertions)' /work/logs/golden_gl_$i.log; grep -E 'blocks over|ERROR|FAILED' /work/logs/golden_gl_$i.log | head -12
done
for i in 1 2 3; do
  MCE_RHI=sdlgpu VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json xvfb-run -a -s "-screen 0 1920x1080x24" $G > /work/logs/golden_vk_$i.log 2>&1; echo "sdlgpu run $i rc=$?"
  grep -E '^\[doctest\] (test cases|assertions)' /work/logs/golden_vk_$i.log; grep -E 'blocks over|ERROR|FAILED' /work/logs/golden_vk_$i.log | head -12
done
grep -h -i -E 'renderer|vendor|nvidia' /work/logs/golden_gl_1.log /work/logs/golden_vk_1.log | sort | uniq -c | head -6
