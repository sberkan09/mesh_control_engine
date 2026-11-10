# r10 job 12: ASCII renders of the captured street views (the log is the only channel off the pod)
cd /work/repo; source .runpod-jobs/common.sh; R=.runpod-jobs/r10.py
python3 $R ascii p2_A_gl p2_B_gl p2_C_gl p2_D_gl a1_A_gl a1_B_gl a1_D_gl f1_A_gl f1_B_gl f1_D_gl
