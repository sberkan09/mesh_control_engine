# soak job 10: build (job 1 again). The astcenc clone failed three times in a row and then worked by hand, so retry with pauses.
export GIT_TERMINAL_PROMPT=0
cd /work/repo
J=$(( $(nproc) < 32 ? $(nproc) : 32 ))
for try in 1 2 3 4 5 6; do
  rm -rf build/linux-gcc-release/_deps/astcenc-*
  T0=$(date +%s)
  cmake --preset linux-gcc-release > /work/cfg.log 2>&1; rc=$?; echo "configure try $try rc=$rc in $(( $(date +%s) - T0 )) s"
  [[ $rc == 0 ]] && break
  grep -E "CMake Error|fatal|error:" /work/cfg.log | head -4
  sleep 30
done
T0=$(date +%s)
cmake --build build/linux-gcc-release -j$J --target mce_player mce_cook kit3d_tour_content > /work/build.log 2>&1
echo "build rc=$? in $(( $(date +%s) - T0 )) s"; grep -E 'error|FAILED' /work/build.log | head -20; tail -2 /work/build.log
source .runpod-jobs/common.sh; echo "player=$P"; ls -la $PK
echo "soak job 10 done $(date -u +%H:%M:%S)"
