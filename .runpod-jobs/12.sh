# soak job 3: leave the summary where the HTTP server (job 1) can serve it.
cd /work/repo
python3 .runpod-jobs/soak.py /work/out > /work/out/summary.txt 2>&1
grep -h -m1 -E 'rhi\.(gl|sdlgpu):' /work/out/*.log >> /work/out/summary.txt
for f in /work/out/*.log; do echo "## $f"; tail -2 "$f"; done >> /work/out/summary.txt
touch /work/out/DONE
echo "soak job 3 done $(date -u +%H:%M:%S)"
