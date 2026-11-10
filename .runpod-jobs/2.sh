# r10 job 2: correctness. The M-GI2 GL/SDL GPU cases of test_render_gl on the real GPU (GL, sdlgpu) and llvmpipe/lavapipe,
# the whole test_render_gl per backend, goldens x3 per backend (gi_thin_wall included), the city's GPU ctests.
cd /work/repo; source .runpod-jobs/common.sh
TR=$B/tests/render_gl/test_render_gl; TG=$B/tests/render_gl/test_golden_gl
F='*gi*,*probe*,*irradiance*,*following volume*,*visibility*'
for be in gl vk sw lvp; do
  echo "=== GI cases [$be]"
  $be $TR -tc="$F" -sf='*gi_*' 2>&1 | grep -E 'ERROR|FATAL|test cases|assertions|SIGSEGV|crash|rhi\.' | sed -e 's#.*/tests/#tests/#' | cut -c1-260 | head -40
  $be $TR -tc="$F" 2>&1 | grep -E 'ERROR|FATAL|^\[doctest\] (test cases|assertions)|SIGSEGV' | head -12
done
for be in gl vk; do
  echo "=== test_render_gl full [$be]"
  T0=$(date +%s); $be $TR 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR:|FATAL|SIGSEGV|DEVICE_LOST' | head -20; echo "  $(( $(date +%s) - T0 )) s"
done
for be in gl vk; do for i in 1 2 3; do
  echo "=== golden [$be] run $i"
  $be $TG 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR|FATAL|blocks|DEVICE_LOST' | head -12
done; done
echo "=== gi_thin_wall golden only"
for be in gl vk sw lvp; do echo "[$be]"; $be $TG -tc='*gi_thin_wall*' 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR|blocks' | head -5; done
echo "=== ctest city [gl offscreen]"
( cd $B && SDL_VIDEODRIVER=offscreen XDG_RUNTIME_DIR=/tmp/xdg ctest -R 'openworld' --timeout 600 2>&1 | tail -8 )
echo "=== ctest city [sdlgpu xvfb]"
( cd $B && MCE_RHI=sdlgpu VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json XDG_RUNTIME_DIR=/tmp/xdg "${XV[@]}" ctest -R 'openworld' --timeout 600 2>&1 | tail -8 )
