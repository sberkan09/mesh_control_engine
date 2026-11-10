# r10 job 8: repeats of the sdlgpu wide GI filter (device lost?)
cd /work/repo; source .runpod-jobs/common.sh
TR=$B/tests/render_gl/test_render_gl
F='*gi*,*probe*,*irradiance*,*following volume*,*visibility*'
for i in 1 2 3; do
  echo "=== wide GI filter [vk] repeat $i"
  vk $TR -tc="$F" 2>&1 | grep -E 'DEVICE_LOST|FATAL|^\[doctest\] (test cases|assertions)|gi_visibility_gl.cpp:608' | sed -e 's#.*/tests/#tests/#' | sort | uniq -c | head -8
done
for i in 1; do
  echo "=== wide GI filter [gl] repeat $i"
  gl $TR -tc="$F" 2>&1 | grep -E 'DEVICE_LOST|FATAL|^\[doctest\] (test cases|assertions)' | head -4
done
nvidia-smi --query-gpu=temperature.gpu,clocks.sm,power.draw --format=csv
