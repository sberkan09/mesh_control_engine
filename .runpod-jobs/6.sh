# r10 job 6: the sdlgpu failure of "the shader's visibility weight (gi_vis) is gi_visibility_weight": gpu and cpu values per failing configuration.
cd /work/repo; source .runpod-jobs/common.sh
TR=$B/tests/render_gl/test_render_gl
for be in gl vk lvp sw; do
  echo "=== gi_vis [$be]"
  $be $TR -tc='*gi_vis*' 2>&1 | grep -E 'configuration|worst|test cases|assertions|ERROR' | sed -e 's#.*/tests/#tests/#' | sort | uniq -c | sort -rn | head -40
done
