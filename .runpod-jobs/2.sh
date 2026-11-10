# r11 job 2: correctness (A31). The cloud shadow cases of test_render_gl on the real GPU (GL, sdlgpu) and llvmpipe/lavapipe, the
# opt-in cost case (GL; sdlgpu has no timestamp queries), the cloud_shadow golden x3 per backend, the full goldens once per backend,
# the tour_cloudshadow ctests.
cd /work/repo; source .runpod-jobs/common.sh
nvidia-smi --query-gpu=name,driver_version,temperature.gpu,power.draw --format=csv,noheader
F='cloud shadow*'
for be in gl vk sw lvp; do
  echo "=== cloud shadow cases [$be]"
  $be $TR -tc="$F" 2>&1 | grep -E 'MESSAGE:|ERROR|FATAL|^\[doctest\] (test cases|assertions)|SIGSEGV|DEVICE_LOST|rhi\.(gl|sdlgpu):' \
    | sed -e 's#.*/tests/#tests/#' | cut -c1-240 | head -40
done
echo "=== cost case [gl] x3"
for i in 1 2 3; do MCE_CLOUD_SHADOW_COST=1 gl $TR -tc='*GPU cost per tier*' 2>&1 | grep -E 'cloud shadow cost|test cases|ERROR' | cut -c1-240; done
echo "=== cost case [vk] (expected: returns, no timestamp queries)"
MCE_CLOUD_SHADOW_COST=1 vk $TR -tc='*GPU cost per tier*' 2>&1 | grep -E 'cloud shadow cost|test cases|ERROR' | cut -c1-240
for be in gl vk; do for i in 1 2 3; do
  echo "=== golden cloud_shadow [$be] run $i"
  $be $TG -tc='*cloud_shadow*' 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR|FATAL|blocks|DEVICE_LOST' | head -8
done; done
for be in sw lvp; do echo "=== golden cloud_shadow [$be]"; $be $TG -tc='*cloud_shadow*' 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR|blocks' | head -5; done
for be in gl vk; do
  echo "=== golden full [$be]"
  T0=$(date +%s); $be $TG 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR|FATAL|blocks|DEVICE_LOST|TEST CASE' | head -20; echo "  $(( $(date +%s) - T0 )) s"
done
echo "=== ctest tour_cloudshadow [gl offscreen]"
( cd $B && SDL_VIDEODRIVER=offscreen XDG_RUNTIME_DIR=/tmp/xdg ctest -R 'cloudshadow' --timeout 300 2>&1 | tail -6 )
echo "=== ctest tour_cloudshadow [sdlgpu xvfb]"
( cd $B && MCE_RHI=sdlgpu VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json XDG_RUNTIME_DIR=/tmp/xdg "${XV[@]}" ctest -R 'cloudshadow' --timeout 300 2>&1 | tail -6 )
