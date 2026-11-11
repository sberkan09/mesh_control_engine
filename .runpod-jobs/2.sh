# r12 job 2: correctness (M-CAUSTICS). The caustics cases of test_render_gl on the real GPU (GL, sdlgpu) and llvmpipe/lavapipe, the
# water, underwater and cloud shadow cases (same post chain), the caustics golden x3 per backend, the full goldens once per backend,
# the tour_caustics ctests.
cd /work/repo; source .runpod-jobs/common.sh
nvidia-smi --query-gpu=name,driver_version,temperature.gpu,power.draw --format=csv,noheader
for be in gl vk sw lvp; do
  echo "=== caustics cases [$be]"
  T0=$(date +%s)
  $be $TR -tc='caustics*' 2>&1 | grep -E 'MESSAGE:|ERROR|FATAL|^\[doctest\] (test cases|assertions)|SIGSEGV|DEVICE_LOST|rhi\.(gl|sdlgpu):|luma|worst|changed' \
    | sed -e 's#.*/tests/#tests/#' | cut -c1-260 | head -60
  echo "  $(( $(date +%s) - T0 )) s"
done
for be in gl vk sw lvp; do
  echo "=== water, underwater, cloud shadow cases [$be]"
  T0=$(date +%s)
  $be $TR -tc='water*,underwater*,cloud shadow*' 2>&1 | grep -E 'ERROR|FATAL|^\[doctest\] (test cases|assertions)|SIGSEGV|DEVICE_LOST|TEST CASE' \
    | sed -e 's#.*/tests/#tests/#' | cut -c1-240 | head -30
  echo "  $(( $(date +%s) - T0 )) s"
done
for be in gl vk; do for i in 1 2 3; do
  echo "=== golden caustics [$be] run $i"
  $be $TG -tc='*caustics*' 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR|FATAL|blocks|DEVICE_LOST' | head -8
done; done
for be in sw lvp; do echo "=== golden caustics [$be]"; $be $TG -tc='*caustics*' 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR|blocks' | head -5; done
for be in gl vk; do
  echo "=== golden full [$be]"
  T0=$(date +%s); $be $TG 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR|FATAL|blocks|DEVICE_LOST|TEST CASE' | head -20; echo "  $(( $(date +%s) - T0 )) s"
done
echo "=== ctest caustics [gl offscreen]"
( cd $B && SDL_VIDEODRIVER=offscreen XDG_RUNTIME_DIR=/tmp/xdg ctest -R 'tour_caustics' --timeout 300 2>&1 | tail -6 )
echo "=== ctest caustics [sdlgpu xvfb]"
( cd $B && MCE_RHI=sdlgpu VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json XDG_RUNTIME_DIR=/tmp/xdg "${XV[@]}" ctest -R 'tour_caustics' --timeout 300 2>&1 | tail -6 )
