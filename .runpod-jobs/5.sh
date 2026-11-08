# r8 job 5: the reflect ctests' pass lines (job 2 cut them off).
cd /work/repo; source .runpod-jobs/common.sh
echo "=== ctest reflect [gl offscreen]"
( cd $B && SDL_VIDEODRIVER=offscreen XDG_RUNTIME_DIR=/tmp/xdg ctest -R 'reflect' --timeout 300 2>&1 | grep -E 'Test +#|tests passed|Failed' )
echo "=== ctest reflect [sdlgpu xvfb]"
( cd $B && MCE_RHI=sdlgpu VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json XDG_RUNTIME_DIR=/tmp/xdg "${XV[@]}" ctest -R 'reflect' --timeout 300 2>&1 | grep -E 'Test +#|tests passed|Failed' )
grep -h -m2 -E 'OpenGL|Vulkan|NVIDIA' /work/out/tour_high_on_1.log $B/Testing/Temporary/LastTest.log 2>/dev/null | head -4
