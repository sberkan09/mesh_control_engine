# r10 job 1: build what the M-GI2 checks need (RelWithDebInfo, MCE_GPU_TESTS=ON) with a job-side GITRACE patch in the player.
cd /work/repo
apt-get install -y -qq python3-numpy > /dev/null 2>&1; python3 -c 'import numpy, PIL; print("numpy", numpy.__version__)'
git log -1 --format='%H %s'
python3 .runpod-jobs/r10.py patch_player src/player/src/player.cpp
J=8; echo "jobs $J (nproc $(nproc))"
T0=$(date +%s)
cmake --preset linux-gcc-release -DMCE_GPU_TESTS=ON > /work/cfg.log 2>&1; echo "configure rc=$? in $(( $(date +%s) - T0 )) s"; tail -3 /work/cfg.log
T0=$(date +%s)
cmake --build build/linux-gcc-release -j$J --target mce_player mce_cook test_render_gl test_golden_gl openworld_content > /work/build.log 2>&1
echo "build rc=$? in $(( $(date +%s) - T0 )) s"; grep -E 'error|FAILED' /work/build.log | head -20; tail -2 /work/build.log
source .runpod-jobs/common.sh; echo "player=$P cook=$C"; ls -la $CITY
python3 .runpod-jobs/r10.py variant samples/09_openworld/assets /work/var/city_novis scenes/city.mcscene enabled=0 && $C /work/var/city_novis --pack --quiet > /work/cook_nv.log 2>&1; echo "cook novis rc=$?"; ls -la $NOVIS; tail -3 /work/cook_nv.log
