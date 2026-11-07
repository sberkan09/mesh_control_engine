#!/usr/bin/env bash
# r7 job 1: build master HEAD of the branch (RelWithDebInfo, GPU tests on).
cd /work/repo
git log -1 --format='commit %H %s'
apt-get install -y -qq --no-install-recommends python3-numpy > /tmp/apt2.log 2>&1; python3 -c 'import numpy; print("numpy", numpy.__version__)'
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader
J=$(nproc); (( J > 16 )) && J=16
echo "cores=$(nproc) using -j$J"
t0=$(date +%s)
cmake --preset linux-gcc-release -DMCE_GPU_TESTS=ON -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache > /work/cfg.log 2>&1
echo "configure rc=$?"; tail -3 /work/cfg.log
cmake --build build/linux-gcc-release -j$J > /work/build.log 2>&1; rc=$?
echo "build rc=$rc in $(( $(date +%s) - t0 )) s"
if (( rc != 0 )); then grep -m 20 -E 'error|FAILED' /work/build.log; cmake --build build/linux-gcc-release -j4 > /work/build2.log 2>&1; echo "retry rc=$?"; grep -m 20 -E 'error|FAILED' /work/build2.log; fi
tail -2 /work/build.log
