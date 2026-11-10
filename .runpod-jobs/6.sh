# r10 job 6: the sdlgpu failure of "the shader's visibility weight (gi_vis) is gi_visibility_weight": gpu and cpu values per failing configuration.
cd /work/repo; source .runpod-jobs/common.sh
TR=$B/tests/render_gl/test_render_gl
for be in gl vk lvp sw; do
  echo "=== gi_vis [$be]"
  $be $TR -tc='*gi_vis*' 2>&1 | grep -E 'configuration|worst|test cases|assertions|ERROR' | sed -e 's#.*/tests/#tests/#' | sort | uniq -c | sort -rn | head -40
done
F='*gi*,*probe*,*irradiance*,*following volume*,*visibility*'
for i in 1 2 3 4 5 6; do
  echo "=== wide GI filter [vk] repeat $i"
  vk $TR -tc="$F" 2>&1 | grep -E 'DEVICE_LOST|FATAL|^\[doctest\] (test cases|assertions)|gi_visibility_gl.cpp:608' | sed -e 's#.*/tests/#tests/#' | sort | uniq -c | head -8
done
for i in 1 2; do
  echo "=== wide GI filter [gl] repeat $i"
  gl $TR -tc="$F" 2>&1 | grep -E 'DEVICE_LOST|FATAL|^\[doctest\] (test cases|assertions)' | head -4
done
nvidia-smi --query-gpu=temperature.gpu,clocks.sm,power.draw --format=csv
