cd /work/repo
B=build/linux-gcc-release
export XDG_RUNTIME_DIR=/tmp/xdg VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json
S=$B/samples/06_showcase/sample_06_showcase
echo "== GL"; SDL_VIDEODRIVER=offscreen $S -sheet /work/s.ppm -sheet.w 1920 -sheet.h 1080 -platform.error_box 0 -r.vsync 0 2>&1 | grep -E "^06_showcase: sheet|^  \| (stage|Forward|scale|SSAO|shadows|GI|volumetric|main pass|MSAA|velocity|post|bloom|tonemap|FXAA|3D frame|---)" | head -20 | cut -c1-140
echo "== VK"; MCE_RHI=sdlgpu xvfb-run -a -s "-screen 0 1920x1080x24" $S -sheet /work/s.ppm -sheet.w 1920 -sheet.h 1080 -platform.error_box 0 -r.vsync 0 2>&1 | grep -E "^  \| (stage|Forward|scale|SSAO|shadows|GI|volumetric|main pass|MSAA|velocity|post|bloom|tonemap|FXAA|3D frame)" | cut -c1-140
