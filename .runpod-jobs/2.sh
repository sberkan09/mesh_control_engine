# r9 job 2: correctness. Water / underwater / water-optics cases of test_render_gl on the real GPU (GL, sdlgpu) and on
# llvmpipe/lavapipe, the whole test_render_gl per backend, goldens x3 per backend, river + water ctests.
cd /work/repo; source .runpod-jobs/common.sh
TR=$B/tests/render_gl/test_render_gl; TG=$B/tests/render_gl/test_golden_gl
for be in gl vk sw lvp; do
  echo "=== water cases [$be]"
  $be $TR -tc='*water*' 2>&1 | grep -E 'MESSAGE|ERROR|FATAL|test cases|assertions|SIGSEGV|crash|rhi\.' | sed -e 's#.*/tests/#tests/#' | cut -c1-260 | head -60
done
for be in gl vk; do
  echo "=== test_render_gl full [$be]"
  T0=$(date +%s); $be $TR 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR:|FATAL|SIGSEGV' | head -20; echo "  $(( $(date +%s) - T0 )) s"
done
for be in gl vk; do for i in 1 2 3; do
  echo "=== golden [$be] run $i"
  $be $TG 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR|FATAL|blocks' | head -12
done; done
echo "=== ctest river+water [gl offscreen]"
( cd $B && SDL_VIDEODRIVER=offscreen XDG_RUNTIME_DIR=/tmp/xdg ctest -R 'river|sample_water' --timeout 300 2>&1 | tail -8 )
echo "=== ctest river+water [sdlgpu xvfb]"
( cd $B && MCE_RHI=sdlgpu VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json XDG_RUNTIME_DIR=/tmp/xdg "${XV[@]}" ctest -R 'river|sample_water' --timeout 300 2>&1 | tail -8 )
echo "=== gpu_compare (GL vs llvmpipe)"
python3 scripts/ci/gpu_compare.py $B sample_kit3d_tour_river_gpu@120 sample_kit3d_tour_river_gpu@300 sample_water_gpu@300 2>&1 | tail -16
