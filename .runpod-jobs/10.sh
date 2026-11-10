# r10 job 10: the street captures as JPEG strips in the log (A = no GI, B = plain volume, D = visibility off by cvar, C = as committed).
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r10.py
ls /work/cap/*_gl.ppm | sed 's#.*/##' | tr '\n' ' '; echo
for k in p2 a1 a2 f1 f2 p1 q1 q2 q3 q4; do python3 $R montage $k A B C; done
python3 $R montage p2 B D C
