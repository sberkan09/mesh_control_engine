# soak job 8: why can the pod not read github.com/ARM-software/astc-encoder?
rm -f /work/out/DONE /work/out/summary.txt
export GIT_TERMINAL_PROMPT=0
curl -sS -m 20 -o /dev/null -w "curl astc repo: %{http_code}\n" https://github.com/ARM-software/astc-encoder
curl -sS -m 20 -o /dev/null -w "curl astc info/refs: %{http_code}\n" "https://github.com/ARM-software/astc-encoder.git/info/refs?service=git-upload-pack"
curl -sS -m 20 -o /dev/null -w "curl box2d info/refs: %{http_code}\n" "https://github.com/erincatto/box2d.git/info/refs?service=git-upload-pack"
git ls-remote https://github.com/ARM-software/astc-encoder.git 5.6.0 2>&1 | head -3
cd /work/repo; grep -n "astcenc" /work/cfg.log | head -5
ls /work/repo/build/linux-gcc-release/_deps | head -30
