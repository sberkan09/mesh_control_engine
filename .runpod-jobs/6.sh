# r9 job 6: is the sdlgpu VK_ERROR_DEVICE_LOST new since r8? Build test_golden_gl at r8's master (747a0d43) next to this
# build (reusing the fetched deps) and run its goldens on sdlgpu, interleaved with this build's.
cd /work/repo; source .runpod-jobs/common.sh
T0=$(date +%s)
git fetch -q --depth 1 origin 747a0d434babf7d4c6471cca723ad7566e6685f7 && git worktree add -q --detach /work/r8 FETCH_HEAD; echo "worktree rc=$?"
cd /work/r8 && git log -1 --format='%H %s'
cmake --preset linux-gcc-release -DMCE_GPU_TESTS=ON -DFETCHCONTENT_BASE_DIR=$B/_deps -DFETCHCONTENT_FULLY_DISCONNECTED=ON > /work/cfg8.log 2>&1 || \
  cmake --preset linux-gcc-release -DMCE_GPU_TESTS=ON -DFETCHCONTENT_FULLY_DISCONNECTED=OFF > /work/cfg8.log 2>&1
echo "configure rc=$? in $(( $(date +%s) - T0 )) s"; tail -2 /work/cfg8.log
cmake --build build/linux-gcc-release -j8 --target test_golden_gl > /work/build8.log 2>&1; echo "build rc=$? in $(( $(date +%s) - T0 )) s"; tail -1 /work/build8.log
T8=/work/r8/build/linux-gcc-release/tests/render_gl/test_golden_gl; TG=$B/tests/render_gl/test_golden_gl
for i in 1 2 3 4; do
  echo "=== r8 golden [vk] $i"; vk $T8 2>&1 | grep -E '^\[doctest\] test cases|DEVICE_LOST|FATAL' | sort | uniq -c | head -6
  echo "=== r9 golden [vk] $i"; vk $TG 2>&1 | grep -E '^\[doctest\] test cases|DEVICE_LOST|FATAL' | sort | uniq -c | head -6
done
nvidia-smi --query-gpu=temperature.gpu,clocks.sm,power.draw,ecc.errors.uncorrected.volatile.total --format=csv
