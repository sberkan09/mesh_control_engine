# soak job 4: why did configure fail?
grep -n -B2 -A12 -E "CMake Error|Could NOT|not found" /work/cfg.log | head -60
tail -15 /work/cfg.log
