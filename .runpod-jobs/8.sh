cd /work/repo
echo "== 285 test_mcp_agent_gpu (vk)"; grep -E "^285:" /work/vk_gpu.log | tail -25 | cut -c1-240
echo "== 215 test_rhi_gl (vk pass, xvfb)"; grep -E "^215:" /work/vk_gpu.log | grep -i -E "FATAL|ERROR:|failed|llvmpipe|renderer" | head -8 | cut -c1-240
echo "== 14 player_capture_no_display"; grep -E "^14:" /work/vk_gpu.log | tail -8 | cut -c1-240
echo "== vulkan device in vk pass"; grep -h -i -E "sdlgpu.*(device|driver|adapter)|vulkan.*(device|nvidia)|NVIDIA GeForce" /work/vk_gpu.log | sort | uniq -c | head -5 | cut -c1-200
