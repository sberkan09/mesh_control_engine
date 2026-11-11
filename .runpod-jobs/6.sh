# r12 job 6: the doctest summary of the water, underwater and cloud shadow cases on llvmpipe and lavapipe (job 5's head cut it).
cd /work/repo; source .runpod-jobs/common.sh
for be in sw lvp; do echo "=== water, underwater, cloud shadow cases [$be] summary"; $be $TR -tc='water*,underwater*,cloud shadow*' 2>&1 | grep -E '^\[doctest\] (test cases|assertions)|ERROR|FATAL|SIGSEGV'; done
echo "r12 job 6 done $(date -u +%H:%M:%S)"
