cd /work/repo
B=build/linux-gcc-release
export XDG_RUNTIME_DIR=/tmp/xdg VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json
apt-get install -y -qq gdb > /dev/null 2>&1
echo "== test_mcp_agent_gpu, MCE_RHI=sdlgpu, xvfb"
MCE_RHI=sdlgpu xvfb-run -a -s "-screen 0 1920x1080x24" ctest --test-dir $B -R '^test_mcp_agent_gpu$' -V 2>&1 | tail -25 | cut -c1-220
echo "== same, GL offscreen (pass A said ok)"
SDL_VIDEODRIVER=offscreen ctest --test-dir $B -R '^test_mcp_agent_gpu$' 2>&1 | tail -4
echo "== backtrace"
CMD=$(ctest --test-dir $B -R '^test_mcp_agent_gpu$' --show-only=json-v1 | python3 -c "import json,sys; print(' '.join(json.load(sys.stdin)['tests'][0]['command']))")
echo "$CMD"
MCE_RHI=sdlgpu xvfb-run -a -s "-screen 0 1920x1080x24" gdb -batch -ex run -ex bt --args $CMD 2>&1 | tail -30 | cut -c1-240
