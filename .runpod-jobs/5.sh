# soak job 5: job 1 again (the first configure died fetching astcenc), configure retried up to 3 times.
rm -f /work/out/DONE /work/out/summary.txt
cd /work/repo
J=$(( $(nproc) < 32 ? $(nproc) : 32 ))
for try in 1 2 3; do
  T0=$(date +%s)
  cmake --preset linux-gcc-release > /work/cfg.log 2>&1; rc=$?; echo "configure try $try rc=$rc in $(( $(date +%s) - T0 )) s"
  [[ $rc == 0 ]] && break
  grep -E "CMake Error|FAILED|fatal|error:" /work/cfg.log | head -8
done
T0=$(date +%s)
cmake --build build/linux-gcc-release -j$J --target mce_player mce_cook kit3d_tour_content > /work/build.log 2>&1
echo "build rc=$? in $(( $(date +%s) - T0 )) s"; grep -E 'error|FAILED' /work/build.log | head -20; tail -2 /work/build.log
source .runpod-jobs/common.sh; echo "player=$P"; ls -la $PK
echo "soak job 5 done $(date -u +%H:%M:%S)"
