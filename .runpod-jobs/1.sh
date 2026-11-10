# r11 job 1: build what the M-CLOUDSHADOW checks need (RelWithDebInfo, MCE_GPU_TESTS=ON).
cd /work/repo
apt-get install -y -qq python3-numpy > /dev/null 2>&1; python3 -c 'import numpy, PIL; print("numpy", numpy.__version__)'
git log -1 --format='%H %s'
J=16; echo "jobs $J (nproc $(nproc))"
T0=$(date +%s)
cmake --preset linux-gcc-release -DMCE_GPU_TESTS=ON > /work/cfg.log 2>&1; echo "configure rc=$? in $(( $(date +%s) - T0 )) s"; tail -3 /work/cfg.log
T0=$(date +%s)
cmake --build build/linux-gcc-release -j$J --target mce_player mce_cook test_render_gl test_golden_gl kit3d_tour_content > /work/build.log 2>&1
echo "build rc=$? in $(( $(date +%s) - T0 )) s"; grep -E 'error|FAILED' /work/build.log | head -20; tail -2 /work/build.log
source .runpod-jobs/common.sh; echo "player=$P"; ls -la $PK $TR $TG
