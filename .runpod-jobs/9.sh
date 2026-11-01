cd /work/repo
echo "== openworld by -frames 180, 2 runs (self-consistency)"
python3 scripts/ci/gpu_compare.py build/linux-gcc-release openworld_sample_player_gpu openworld_sample_player_gpu 2>&1 | grep -E "^##|gpu mean"
echo "== openworld pinned tick"
python3 scripts/ci/gpu_compare.py build/linux-gcc-release openworld_sample_player_gpu@120 openworld_sample_player_gpu@600 openworld_sample_player_gpu@1500 2>&1 | grep -E "^##|gpu mean|^   [a-z]"
