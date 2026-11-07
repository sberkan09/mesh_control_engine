#!/usr/bin/env bash
# r7 job 6: the first lines of each failed test's output (from job 2's logs), and a backtrace of the sdlgpu segfaults.
cd /work/repo; B=build/linux-gcc-release; mkdir -p /tmp/xdg; chmod 700 /tmp/xdg; export XDG_RUNTIME_DIR=/tmp/xdg
for f in /work/logs/ct_gl.log /work/logs/ct_vk.log; do
  echo "== $f"
  python3 - "$f" <<'PY'
import re, sys
t = open(sys.argv[1], errors="replace").read().splitlines()
for i, l in enumerate(t):
    if re.search(r"\*\*\*(Failed|Exception|Timeout)", l):
        print(l.strip())
        body = []
        for m in t[i + 1:]:
            if re.match(r"\s*\d+/\d+ Test", m): break
            body.append(m)
        keep = [m for m in body if re.search(r"(?i)error|fail|fatal|require|segv|sdl_|vulkan|assert|abort", m)][:10]
        for m in keep or body[-8:]: print("    " + m[:220])
PY
done
apt-get install -y -qq --no-install-recommends gdb > /dev/null 2>&1
R=$(find $B -name test_rhi_sdlgpu -type f -perm -u+x | head -1); echo "== gdb $R under SDL offscreen (NVIDIA ICD)"
SDL_VIDEODRIVER=offscreen VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json timeout 120 gdb -q -batch -ex run -ex bt $R 2>&1 | grep -E '^#|SIGSEGV|error|Error' | head -25
