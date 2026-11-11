# soak job 1: serve /work/out over HTTP (port 8000), then build the player, the cook and the kit3d_tour pack.
mkdir -p /work/out; cd /work/out
setsid nohup python3 -m http.server 8000 --directory /work/out > /tmp/http.log 2>&1 &
cd /work/repo
git log -1 --format='%H %s'
J=$(( $(nproc) < 32 ? $(nproc) : 32 )); echo "jobs $J (nproc $(nproc))"
T0=$(date +%s)
cmake --preset linux-gcc-release > /work/cfg.log 2>&1; echo "configure rc=$? in $(( $(date +%s) - T0 )) s"; tail -3 /work/cfg.log
T0=$(date +%s)
cmake --build build/linux-gcc-release -j$J --target mce_player mce_cook kit3d_tour_content > /work/build.log 2>&1
echo "build rc=$? in $(( $(date +%s) - T0 )) s"; grep -E 'error|FAILED' /work/build.log | head -20; tail -2 /work/build.log
source .runpod-jobs/common.sh; echo "player=$P"; ls -la $PK
nvidia-smi --query-gpu=name,driver_version,memory.total,memory.used --format=csv
echo "soak job 1 done $(date -u +%H:%M:%S)"
