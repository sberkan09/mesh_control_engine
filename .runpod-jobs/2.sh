cd /work/repo
echo "== vulkan devices"; vulkaninfo --summary 2>/dev/null | grep -E "deviceName|driverName|apiVersion|driverInfo" 
echo "== glx under xvfb"; xvfb-run -a -s "-screen 0 1280x1024x24" glxinfo -B 2>&1 | grep -E "renderer|OpenGL version|vendor" | head
echo "== egl"; ls /usr/share/glvnd/egl_vendor.d/ 2>/dev/null
nohup bash -c 'cmake --preset linux-gcc-release -DMCE_GPU_TESTS=ON && cmake --build build/linux-gcc-release -j32 && echo BUILD_DONE' > /work/build.log 2>&1 &
echo "build started"
