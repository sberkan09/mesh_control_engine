cd /work/repo
B=build/linux-gcc-release
export XDG_RUNTIME_DIR=/tmp/xdg
export VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json
echo "== renderer strings seen in pass A"; grep -o -i -E "NVIDIA GeForce RTX 3070[^\"]{0,30}|llvmpipe[^\"]{0,20}|Mesa [0-9.]+" /work/gl_gpu.log | sort | uniq -c | head
grep -i -E "gl_renderer|renderer:|adapter|device:" /work/gl_gpu.log | sort | uniq -c | sort -rn | head -8
echo "== failed sdlgpu tests under offscreen: first error"
grep -E "^(217|220|222):.*(error|Error|FATAL)" /work/gl_gpu.log | head -6
echo "== PASS B: SDL GPU / Vulkan NVIDIA under Xvfb, whole gpu label with MCE_RHI=sdlgpu"
MCE_RHI=sdlgpu timeout 2400 xvfb-run -a -s "-screen 0 1920x1080x24" ctest --test-dir $B -L gpu -j4 --timeout 300 --output-on-failure > /work/vk_gpu.log 2>&1
grep -E "tests passed|tests failed" /work/vk_gpu.log
sed -n '/The following tests FAILED/,$p' /work/vk_gpu.log | head -60
grep -o -i -E "NVIDIA GeForce RTX 3070|llvmpipe|lavapipe" /work/vk_gpu.log | sort | uniq -c
