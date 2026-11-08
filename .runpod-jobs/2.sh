# r8 job 2: correctness. M-REFLECT2 render3d_gl cases on the real GPU (GL, sdlgpu) and on llvmpipe/lavapipe, the
# whole test_render_gl, goldens x3 per backend, the reflect ctests.
cd /work/repo; source .runpod-jobs/common.sh
TR=$B/tests/render_gl/test_render_gl; TG=$B/tests/render_gl/test_golden_gl
for be in gl vk sw lvp; do
  echo "=== ssr cases [$be]"
  $be $TR -tc='*ssr*' 2>&1 | grep -E 'MESSAGE|ERROR|FATAL|test cases|assertions|SIGSEGV|crash' | sed -e 's#.*/tests/#tests/#' | cut -c1-260
done
gl glxinfo -B 2>/dev/null | head -3
for be in gl vk; do
  echo "=== test_render_gl full [$be]"
  $be $TR 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR:|FATAL|SIGSEGV' | head -20
done
for be in gl vk; do for i in 1 2 3; do
  echo "=== golden [$be] run $i"
  $be $TG 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR|FATAL|ssr_room|blocks' | head -12
done; done
echo "=== ctest reflect [gl offscreen]"
( cd $B && SDL_VIDEODRIVER=offscreen XDG_RUNTIME_DIR=/tmp/xdg ctest -R 'reflect' --timeout 300 2>&1 | tail -6 )
echo "=== ctest reflect [sdlgpu xvfb]"
( cd $B && MCE_RHI=sdlgpu VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json XDG_RUNTIME_DIR=/tmp/xdg "${XV[@]}" ctest -R 'reflect' --timeout 300 2>&1 | tail -6 )
echo "=== gpu_compare"
python3 scripts/ci/gpu_compare.py $B sample_kit3d_tour_reflect_gpu@120 sample_kit3d_tour_reflect_gpu@180 sample_kit3d_tour_reflect_gpu@300 2>&1 | tail -12
