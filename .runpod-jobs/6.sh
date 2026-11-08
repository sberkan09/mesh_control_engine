# r8 job 6: job 3's summary again, one tag per row (job 3's glob folded the _blur0 runs into the mr rows).
cd /work/repo; source .runpod-jobs/common.sh
for f in $(ls /work/out/*_1.mcperf | sort); do t=$(basename $f _1.mcperf); printf '%-28s ' $t; python3 .runpod-jobs/r8.py perf /work/out/${t}_[0-9].mcperf | sed -e 's/ clusters.*ssao/ ssao/' -e 's/ gi 0.001 volumetrics 0.001//' -e 's/ fxaa 0.001//'; done
